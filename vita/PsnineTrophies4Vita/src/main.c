// Native trophy-app-style UI: data comes from real psnine scraping (data.c background thread), assets extracted from vs0
// All layout sizes measured pixel-by-pixel from native screenshots on real hardware (row height 90, list start 128, icon slot 154x84...)
// Performance strategy: text baked into whole-row textures cached on demand, network icons composited separately once they arrive, runtime only does memcpy and NEON blending,
//           selection mask eases at 60fps; scrolling and mask movement reach the screen within a few milliseconds
#include "render.h"
#include "data.h"

#include <psp2/apputil.h>
#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/system_param.h>
#include <psp2/touch.h>
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/libime.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arm_neon.h>

/* Colors are always 0xAABBGGRR (memory byte order R,G,B,A) */
#define C(r, g, b)   ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16))
#define CA(r, g, b, a) (((uint32_t)(a) << 24) | C(r, g, b))

/* ---------------- Minimal logging for crash diagnosis (file only, not on screen) ---------------- */
static int log_fd = -1;

void dlog(const char *fmt, ...) {
	if (log_fd < 0) return;
	char line[160];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
	va_end(ap);
	if (n <= 0) return;
	line[n++] = '\n';
	sceIoWrite(log_fd, line, n);
}

static void diag_open(void) {
	sceIoMkdir("ux0:data/PSNIN0003", 0777);
	log_fd = sceIoOpen("ux0:data/PSNIN0003/trophy.log",
	                   SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
}

/* ---------------- Layout constants (measured from native) ---------------- */
#define ROW_H   90             /* Row height: separators in the screenshot sit at 128/218/308/398/488 */
#define LT_TOP  128            /* Level-1 list start; levels 2/3 shift down one row for the game info band */
#define ICON_X  93             /* Icon slot 154x84; covers are scaled proportionally and centered in this box */
#define ICON_W  154
#define ICON_H  84
#define TXT_X   262            /* Title baseline = row top + 44 */
#define TXT_W   378            /* Title drawn up to x=640 at most; the count panel follows */
#define BAR_X   260
#define BAR_Y   56             /* Progress bar 210x8, row top + 56 */
#define BAR_W   210
#define BAR_H   8
#define PCT_X   476
#define BOX_X   650            /* Count panel 217x70, row top + 10 */
#define BOX_Y   10
#define BOX_W   217
#define BOX_H   70
#define DIG_X   684            /* Centers of the four digit columns, spacing 50, vertically aligned with the top-bar trophy art */
#define INFO_X  660            /* Right side of trophy rows: rarity + Tips count */
#define INFO_W  280
#define SB_X    941            /* Scrollbar */
#define SB_W    5
#define SRCH_X  8              /* Top-left search box: takes the spot of the native PS Vita tab */
#define INFOBAR_H 32           /* System status bar occupies y0..31; top-bar elements can only be centered within [32,LT_TOP] */
#define SRCH_Y  56             /* 48-high search box centered in 32..128: (96-48)/2+32 */
#define SB_W2   260
#define SB_H2   48
#define HINT_X  288            /* Control hints to the right of the search box */
#define SPN_X   858            /* Top-right spinning loader: right of the legend bronze cup */
#define SPN_Y   64             /* 32px spinner centered in 32..128: right of the legend bronze cup */
#define SPN_S   32
#define SEL_H   92             /* Selection mask texture height (1px taller than the row on top and bottom) */
#define MAX_ROW 6
#define MAX_CACHE 12           /* Row texture cache slots: only bake the few visible rows */

/* ---------------- Screen state ----------------
   Five levels: games -> trophy groups -> trophies -> Tips -> single Tip body.
   All data comes from the data.c background thread; want_* records the pressed target,
   and lists stay empty (spinner) until the page returns, so the previous game's stale table is never shown. */
enum { SCR_GAMES, SCR_GROUPS, SCR_TROPHIES, SCR_TIPS, SCR_TIPVIEW, SCR_N };
static int screen_id;
static int curv[SCR_N], topv[SCR_N];
static unsigned int ok_btn = SCE_CTRL_CROSS, back_btn = SCE_CTRL_CIRCLE;
static int list_want;    /* Last level-1 list page requested; 0 = none yet */
static char query[96];    /* △ search term; empty = latest list */
static long want_game;    /* Game id confirmed with ○; ready once dt_head_id catches up */
static long want_trophy;  /* Trophy id confirmed with ○; ready once dt_tips_for catches up */
static int tip_top;       /* Level-5 body scroll amount (lines) */
int peel_req;             /* Whether to page-turn this frame: +1 enter, -1 back; also used by search level jumps */
static long long retry_at; /* Resend timestamp after a request failure; avoids resending every frame */

static int lt_of(int scr) { return scr == SCR_GAMES ? LT_TOP : LT_TOP + ROW_H; }
/* Earliest start allowed for snapshot/mask: the mask sits 1px above the row top, and that row must be restorable too */
static int snap0_of(int scr) { return lt_of(scr) - 1; }
/* Fully visible rows: when the last row is half cut by the screen, selecting it must scroll (native does this) */
static int rows_vis(int scr) { return (SCREEN_H - lt_of(scr)) / ROW_H; }
/* Rows to draw: the partial trailing row is drawn too; the list is cut off by the screen's bottom edge.
   Native shows 4 and a half rows; rounding to whole rows earlier left a 50+px gap at the bottom. */
static int rows_draw(int scr) {
	int h = SCREEN_H - lt_of(scr), v = h / ROW_H;
	return v + (h % ROW_H ? 1 : 0);
}

static int game_ready(void) { return want_game != 0 && dt_head_id == want_game; }
static int tips_ready(void) { return want_trophy != 0 && dt_tips_for == want_trophy; }
static int cur_group(void) {
	int g = curv[SCR_GROUPS];
	return g >= 0 && g < dt_group_n ? g : 0;
}
/* Index of the level-3 selection within the whole trophy table */
static int trophy_sel(void) {
	int i = dt_group[cur_group()].t0 + curv[SCR_TROPHIES];
	return i >= 0 && i < dt_trophy_n ? i : 0;
}
/* The level-1 list is append-style: any fetched game makes it drawable; a new page is appended below */
static int list_ready(void) { return dt_list_n > 0; }
/* Trust the data layer's real in-flight state: tracking page numbers ourselves would misjudge "sent but failed" as "still running",
   so one failure at startup would leave the list empty forever */
static int list_pending(void) { return data_pending(DP_LIST); }

static int cnt_of(int scr) {
	switch (scr) {
	case SCR_GAMES: return list_ready() ? dt_list_n : 0;
	case SCR_GROUPS: return game_ready() ? dt_group_n : 0;
	case SCR_TROPHIES: return game_ready() ? dt_group[cur_group()].tn : 0;
	case SCR_TIPS: return tips_ready() ? dt_tip_n : 0;
	default: return 1; /* Level 5 is a full-page body, no list */
	}
}
static int item_of(int scr, int idx) {
	return scr == SCR_TROPHIES ? dt_group[cur_group()].t0 + idx : idx;
}
/* Version of the row-texture cache: when data changes generation, all baked rows are invalidated */
static int gen_of(int scr) {
	return scr == SCR_GAMES ? dt_gen[0] : scr == SCR_TIPS ? dt_gen[2] : dt_gen[1];
}
static int cur_of(int scr) { return curv[scr]; }
static int top_of(int scr) { return topv[scr]; }
static int *cur_pp(int scr) { return &curv[scr]; }
static int *top_pp(int scr) { return &topv[scr]; }

static int cup_of(char t) { /* 'p' platinum, 'g' gold, 's' silver, otherwise bronze */
	switch (t) {
	case 'p': return 0;
	case 'g': return 1;
	case 's': return 2;
	default: return 3;
	}
}

static void reveal(int *c, int *t, int scr) {
	int v = rows_vis(scr);
	if (*c < *t) *t = *c;
	if (*c >= *t + v) *t = *c - v + 1;
	if (*t < 0) *t = 0;
}
/* ---------------- Texture primitives ---------------- */
typedef struct { uint32_t *p; int w, h; } Spr;

static uint32_t *bg; /* 960x544 native radial-ray background (pre-darkened as the base) */

static Spr S_sel, S_rowbg, S_legend;          /* Native RCO assets */
static Spr S_selw, S_bub_tl, S_bub_w;         /* Recolor-baked at startup: white row mask / teal bubble mask / white bubble mask */
static Spr S_field;                           /* Top-left search box base */
static Spr C_cup26[5];                         /* Native 26px trophy badges (large icons always come from PSN) */
static Spr S_track, S_fill, S_box, S_play, S_dlc;
static Spr S_back;   /* Bottom-left back bubble: system highlight orb recolored + livearea's ↩ glyphs */

static int spr_alloc(Spr *s, int w, int h) {
	s->p = render_alloc0((size_t)w * h * 4);
	s->w = s->p ? w : 0;
	s->h = s->p ? h : 0;
	return s->p ? 0 : -1;
}

/* Bake-canvas pixel: store directly if the target is transparent, otherwise alpha-over composite */
static void s_px(Spr *s, int x, int y, uint32_t c) {
	if (x < 0 || x >= s->w || y < 0 || y >= s->h) return;
	uint32_t *d = &s->p[(size_t)y * s->w + x];
	unsigned as = (c >> 24) & 0xff, ad = (*d >> 24) & 0xff;
	if (ad == 0 || as >= 255) { *d = c; return; }
	unsigned na = as + (ad * (255 - as) >> 8);
	if (na == 0) { *d = 0; return; }
	unsigned r = (((c & 0xff) * as) + ((*d & 0xff) * ad * (255 - as) >> 8)) / na;
	unsigned g = ((((c >> 8) & 0xff) * as) + (((*d >> 8) & 0xff) * ad * (255 - as) >> 8)) / na;
	unsigned b = ((((c >> 16) & 0xff) * as) + (((*d >> 16) & 0xff) * ad * (255 - as) >> 8)) / na;
	*d = (na << 24) | (b << 16) | (g << 8) | r;
}

static int rr_inset(int t, int r) { /* Rounded-row inset: t = row number from the top (or bottom) edge */
	if (t >= r) return 0;
	int dy = r - t;
	return r - (int)(0.5f + sqrtf((float)(r * r - dy * dy)));
}

static void s_rect(Spr *s, int x, int y, int w, int h, uint32_t c) {
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++)
			s_px(s, x + i, y + j, c);
}

static void s_rrect(Spr *s, int x, int y, int w, int h, int r, uint32_t c) {
	for (int j = 0; j < h; j++) {
		int inset = 0;
		if (j < r) inset = rr_inset(j, r);
		else if (j >= h - r) inset = rr_inset(h - 1 - j, r);
		for (int i = inset; i < w - inset; i++)
			s_px(s, x + i, y + j, c);
	}
}

static void s_rrect_border(Spr *s, int x, int y, int w, int h, int r, uint32_t c) {
	for (int j = 0; j < h; j++) {
		int inset = 0;
		if (j < r) inset = rr_inset(j, r);
		else if (j >= h - r) inset = rr_inset(h - 1 - j, r);
		if (inset) { s_px(s, x + inset, y + j, c); s_px(s, x + w - 1 - inset, y + j, c); }
		else if (j == 0 || j == h - 1) s_rect(s, x + inset, y + j, w - 2 * inset, 1, c);
		else { s_px(s, x, y + j, c); s_px(s, x + w - 1, y + j, c); }
	}
}

/* Runtime blending: NEON, 8 pixels at a time. Measured on real hardware, scalar -O3 costs 37 instructions per pixel,
   one 960x92 mask band takes 8ms, so easing animations must be vectorized.
   Weights sum to a fixed 255: od*(255-a) + cd*a peaks at 65025, fitting exactly in uint16;
   the earlier int16 form od + ((cd-od)*a >> 8) wrapped negative — that was the source of the screen-full of noise.
   (t + (t>>8) + 128) >> 8 is fixed-point rounding of t/255; exact source color when a=255 */
static void blend_row(uint32_t *dp, const uint32_t *sp, int n) {
	int i = 0;
	for (; i + 8 <= n; i += 8) {
		uint8x8x4_t s = vld4_u8((const uint8_t *)(sp + i));
		uint8x8x4_t o = vld4_u8((const uint8_t *)(dp + i));
		uint8x8_t ia = vsub_u8(vdup_n_u8(255), s.val[3]);
		uint8x8x4_t r;
		for (int k = 0; k < 3; k++) {
			uint16x8_t t = vaddq_u16(vmull_u8(o.val[k], ia), vmull_u8(s.val[k], s.val[3]));
			t = vaddq_u16(t, vshrq_n_u16(t, 8));
			t = vaddq_u16(t, vdupq_n_u16(128));
			r.val[k] = vshrn_n_u16(t, 8);
		}
		r.val[3] = o.val[3];
		vst4_u8((uint8_t *)(dp + i), r);
	}
	for (; i < n; i++) {
		uint32_t c = sp[i], o = dp[i];
		int a = (int)((c >> 24) & 0xff);
		if (!a) continue;
		uint32_t rr = (o & 0xff) + ((((int)(c & 0xff)) - (int)(o & 0xff)) * a >> 8);
		uint32_t gg = ((o >> 8) & 0xff) + ((((int)((c >> 8) & 0xff)) - (int)((o >> 8) & 0xff)) * a >> 8);
		uint32_t bb = ((o >> 16) & 0xff) + ((((int)((c >> 16) & 0xff)) - (int)((o >> 16) & 0xff)) * a >> 8);
		dp[i] = (o & 0xff000000) | ((bb & 0xff) << 16) | ((gg & 0xff) << 8) | (rr & 0xff);
	}
}

/* Bake mode only: the staging canvas is transparent, so alpha must be composited too; otherwise the baked row comes out A=0
   and blending it to screen at runtime draws nothing. Runs only at startup, scalar is fine. */
static void blend_row_bake(uint32_t *dp, const uint32_t *sp, int n) {
	for (int i = 0; i < n; i++) {
		uint32_t c = sp[i], o = dp[i];
		unsigned as = (c >> 24) & 0xff, ad = (o >> 24) & 0xff;
		if (!as) continue;
		unsigned na = as + (ad * (255 - as) >> 8);
		unsigned r = (((c & 0xff) * as) + ((o & 0xff) * ad * (255 - as) >> 8)) / na;
		unsigned g = ((((c >> 8) & 0xff) * as) + (((o >> 8) & 0xff) * ad * (255 - as) >> 8)) / na;
		unsigned b = ((((c >> 16) & 0xff) * as) + (((o >> 16) & 0xff) * ad * (255 - as) >> 8)) / na;
		dp[i] = (na << 24) | (b << 16) | (g << 8) | r;
	}
}

static int baking; /* 1 = currently baking a row texture into the transparent staging canvas */

static void blend(uint32_t *dp, const uint32_t *sp, int n) {
	if (baking) blend_row_bake(dp, sp, n);
	else blend_row(dp, sp, n);
}

/* Blend with strength factor k (0..254): scale the source alpha by k, then the same over formula.
   The selection mask's breathing animation relies on it; k>=255 falls back to the exact blend_row. */
static void blend_row_k(uint32_t *dp, const uint32_t *sp, int n, int k) {
	int i = 0;
	uint8x8_t kv = vdup_n_u8((uint8_t)k);
	for (; i + 8 <= n; i += 8) {
		uint8x8x4_t s = vld4_u8((const uint8_t *)(sp + i));
		uint8x8x4_t o = vld4_u8((const uint8_t *)(dp + i));
		s.val[3] = vshrn_n_u16(vaddq_u16(vmull_u8(s.val[3], kv), vdupq_n_u16(127)), 8);
		uint8x8_t ia = vsub_u8(vdup_n_u8(255), s.val[3]);
		uint8x8x4_t r;
		for (int c = 0; c < 3; c++) {
			uint16x8_t t = vaddq_u16(vmull_u8(o.val[c], ia), vmull_u8(s.val[c], s.val[3]));
			t = vaddq_u16(t, vshrq_n_u16(t, 8));
			t = vaddq_u16(t, vdupq_n_u16(128));
			r.val[c] = vshrn_n_u16(t, 8);
		}
		r.val[3] = o.val[3];
		vst4_u8((uint8_t *)(dp + i), r);
	}
	for (; i < n; i++) {
		uint32_t c = sp[i];
		int a = (int)((c >> 24) & 0xff) * k / 255;
		if (!a) continue;
		uint32_t o = dp[i];
		uint32_t rr = (o & 0xff) + ((((int)(c & 0xff)) - (int)(o & 0xff)) * a >> 8);
		uint32_t gg = ((o >> 8) & 0xff) + ((((int)((c >> 8) & 0xff)) - (int)((o >> 8) & 0xff)) * a >> 8);
		uint32_t bb = ((o >> 16) & 0xff) + ((((int)((c >> 16) & 0xff)) - (int)((o >> 16) & 0xff)) * a >> 8);
		dp[i] = (o & 0xff000000) | ((bb & 0xff) << 16) | ((gg & 0xff) << 8) | (rr & 0xff);
	}
}

static void blend_k(uint32_t *dp, const uint32_t *sp, int n, int k) {
	if (k >= 255) blend_row(dp, sp, n);
	else blend_row_k(dp, sp, n, k);
}

/* Strength version of blend_into: runtime only (mask, reflections); baking never goes through here */
static int blend_clip_y; /* When the list splits at the top/bottom, row content is clipped to the list area's top edge so it doesn't smear onto the top bar */
static void blend_into_k(uint32_t *dst, int dst_h, int x, int y, const Spr *s, int k) {
	if (!s->p) return;
	int x0 = x < 0 ? -x : 0, y0 = y < blend_clip_y ? blend_clip_y - y : 0;
	int x1 = s->w, y1 = s->h;
	if (x + x1 > SCREEN_W) x1 = SCREEN_W - x;
	if (y + y1 > dst_h) y1 = dst_h - y;
	for (int j = y0; j < y1; j++)
		blend_k(dst + (size_t)(y + j) * RENDER_PITCH + (x + x0),
		        s->p + (size_t)j * s->w + x0, x1 - x0, k);
}

/* Blend a texture into an arbitrary destination buffer (full-screen canvas or a single row band); dst_h is the destination row count.
   blend_clip_y: on top/bottom overscroll, row content is clipped to the list area's top edge so it doesn't smear onto the top bar */
static void blend_into(uint32_t *dst, int dst_h, int x, int y, const Spr *s) {
	if (!s->p) return;
	int x0 = x < 0 ? -x : 0, y0 = y < blend_clip_y ? blend_clip_y - y : 0;
	int x1 = s->w, y1 = s->h;
	if (x + x1 > SCREEN_W) x1 = SCREEN_W - x;
	if (y + y1 > dst_h) y1 = dst_h - y;
	for (int j = y0; j < y1; j++)
		blend(dst + (size_t)(y + j) * RENDER_PITCH + (x + x0),
		      s->p + (size_t)j * s->w + x0, x1 - x0);
}

static void blit(int x, int y, const Spr *s) {
	blend_into(render_cur_fb(), baking ? ROW_H : SCREEN_H, x, y, s);
}

/* Blend only the left fw columns of the texture (progress bar truncated by percentage) */
static void blit_w(int x, int y, const Spr *s, int fw) {
	if (!s->p || fw <= 0) return;
	if (fw > s->w) fw = s->w;
	uint32_t *fb = render_cur_fb();
	int rows = s->h, lim = baking ? ROW_H : SCREEN_H;
	if (y + rows > lim) rows = lim - y;
	for (int j = 0; j < rows; j++)
		blend(fb + (size_t)(y + j) * RENDER_PITCH + x,
		      s->p + (size_t)j * s->w, fw);
}

/* Network icons: pixels live in data.c's fixed cache slots; a placeholder block until the download finishes */
static void blit_icon(int x, int y, int kind, const char *url) {
	int w, h;
	const uint32_t *px = data_icon(kind, url, &w, &h);
	Spr s = { (uint32_t *)px, w, h };
	blend_into(render_cur_fb(), baking ? ROW_H : SCREEN_H, x, y, &s);
}

static void blit_bg(void) {
	uint32_t *f = render_cur_fb();
	for (int y = 0; y < SCREEN_H; y++)
		memcpy(f + (size_t)y * RENDER_PITCH, bg + (size_t)y * SCREEN_W, SCREEN_W * 4);
}

/* ---------------- Native asset loading (.rgba under app0:assets) ---------------- */
static uint32_t *load_rgba(const char *path, int w, int h) {
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0) { dlog("open fail %s 0x%x", path, fd); return NULL; }
	uint32_t *buf = render_alloc((size_t)w * h * 4);
	if (!buf) { dlog("alloc fail %s", path); sceIoClose(fd); return NULL; }
	size_t need = (size_t)w * h * 4, got = 0;
	while (got < need) {
		int n = sceIoRead(fd, (char *)buf + got, need - got);
		if (n <= 0) break;
		got += n;
	}
	sceIoClose(fd);
	if (got < need) { dlog("read fail %s got=%d/%d", path, (int)got, (int)need); return NULL; }
	return buf;
}

static int load_one(Spr *s, const char *path, int w, int h) {
	s->p = load_rgba(path, w, h);
	s->w = s->p ? w : 0;
	s->h = s->p ? h : 0;
	return s->p ? 0 : -1;
}

static int load_sprites(void) {
	bg = load_rgba("app0:assets/bg.rgba", SCREEN_W, SCREEN_H);
	if (!bg) return -1;
	if (load_one(&S_sel, "app0:assets/sel.rgba", 960, SEL_H)) return -1;
	if (load_one(&S_rowbg, "app0:assets/rowbg.rgba", 960, SEL_H)) return -1;
	if (load_one(&S_legend, "app0:assets/legend.rgba", 182, 66)) return -1;
	static const char *c26[5] = { "app0:assets/cup26_0.rgba", "app0:assets/cup26_1.rgba",
		"app0:assets/cup26_2.rgba", "app0:assets/cup26_3.rgba", "app0:assets/cup26_4.rgba" };
	for (int k = 0; k < 5; k++)
		if (load_one(&C_cup26[k], c26[k], 26, 26)) return -1;
	/* If the back bubble can't be fetched, fall back to not drawing it; it must not stop the whole app from starting */
	load_one(&S_back, "app0:assets/back.rgba", 110, 110);
	return 0;
}

/* White row mask / teal bubble mask / white bubble mask: baked at startup by recoloring existing assets (the ↩ glyph stays white) */
static void bake_hl(void) {
	if (S_sel.p && spr_alloc(&S_selw, S_sel.w, S_sel.h) == 0)
		for (int i = 0; i < S_sel.w * S_sel.h; i++)
			S_selw.p[i] = (S_sel.p[i] & 0xff000000u) | 0x00ffffffu;
	if (!S_back.p) return;
	if (spr_alloc(&S_bub_tl, S_back.w, S_back.h) < 0) return;
	if (spr_alloc(&S_bub_w, S_back.w, S_back.h) < 0) return;
	for (int i = 0; i < S_back.w * S_back.h; i++) {
		uint32_t c = S_back.p[i];
		int r = c & 255, g = (c >> 8) & 255, b = (c >> 16) & 255, a = (c >> 24) & 255;
		S_bub_w.p[i] = ((uint32_t)a << 24) | 0x00ffffffu;
		if (r > 200 && g > 200) { S_bub_tl.p[i] = c; continue; }
		int t = (r * 30 + g * 59 + b * 11) / 100;
		S_bub_tl.p[i] = ((uint32_t)a << 24) |
		                ((uint32_t)(84 + (255 - 84) * t / 255) << 16) |
		                ((uint32_t)(64 + (212 - 64) * t / 255) << 8) |
		                (uint32_t)(4 + (18 - 4) * t / 255);
	}
}

/* ---------------- Startup baking ---------------- */
static void bake_parts(void) {
	/* Progress bar: native empty slot is light gray, earned portion is white */
	if (spr_alloc(&S_track, BAR_W, BAR_H) == 0)
		s_rrect(&S_track, 0, 0, BAR_W, BAR_H, BAR_H / 2, CA(205, 205, 205, 110));
	if (spr_alloc(&S_fill, BAR_W, BAR_H) == 0)
		s_rrect(&S_fill, 0, 0, BAR_W, BAR_H, BAR_H / 2, CA(255, 255, 255, 235));
	/* Count panel: native is just a rounded base + 1px bright border, no separators between the four columns */
	if (spr_alloc(&S_box, BOX_W, BOX_H) == 0) {
		s_rrect(&S_box, 0, 0, BOX_W, BOX_H, 10, CA(16, 18, 24, 120));
		s_rrect_border(&S_box, 0, 0, BOX_W, BOX_H, 10, CA(255, 255, 255, 40));
	}
	if (spr_alloc(&S_play, 15, 28) == 0)
		for (int j = 0; j < 28; j++) {
			int len = 14 - abs(j - 13);
			for (int k = 0; k <= len; k++) s_px(&S_play, k, j, CA(235, 235, 235, 220));
		}
	if (spr_alloc(&S_dlc, 24, 24) == 0) {
		for (int j = -11; j <= 11; j++)
			for (int i = -11; i <= 11; i++)
				if (i * i + j * j <= 121) s_px(&S_dlc, 12 + i, 12 + j, C(96, 98, 104) | 0xff000000);
		for (int k = -7; k <= 7; k++) {
			s_px(&S_dlc, 12, 12 + k, C(245, 245, 245) | 0xff000000);
			s_px(&S_dlc, 12 + k, 12, C(245, 245, 245) | 0xff000000);
		}
	}
	/* Top-left search box: replaces the native PS Vita tab's spot, same height as the original tab */
	if (spr_alloc(&S_field, SB_W2, SB_H2) == 0) {
		s_rrect(&S_field, 0, 0, SB_W2, SB_H2, 12, CA(18, 20, 26, 150));
		s_rrect_border(&S_field, 0, 0, SB_W2, SB_H2, 12, CA(190, 200, 215, 70));
	}
}

/* ---------------- Row content (selected rows keep white text too, as in native) ----------------
   y is the row top; baked into a screen-wide transparent staging canvas, cropped to 960x90 as the row texture.
   Network icons are not baked in: they can arrive at any time and one bake would invalidate the whole batch, so they are blended as a separate layer at runtime. */
static void fmt_pct(int p100, char *buf, int cap) {
	snprintf(buf, cap, "%d.%02d%%", p100 / 100, p100 % 100);
}

static void row_counts(int y, const int cnt[4]) {
	blit(BOX_X, y + BOX_Y, &S_box);
	for (int k = 0; k < 4; k++) {
		char d[8];
		snprintf(d, sizeof(d), "%d", cnt[k]);
		render_text(DIG_X + k * 50 - render_text_width(d, 28) / 2, y + 54, d,
		            cnt[k] ? C(245, 245, 245) : C(120, 120, 120), 28);
	}
}

/* Perfect-rate progress bar: list pages only carry this number when sorted by perfect rate; skip drawing when absent */
static void row_bar(int y, int p100) {
	if (p100 <= 0) return;
	int fw = BAR_W * p100 / 10000;
	blit(BAR_X, y + BAR_Y, &S_track);
	if (fw > 0) blit_w(BAR_X, y + BAR_Y, &S_fill, fw < BAR_H ? BAR_H : fw);
	char b[16];
	fmt_pct(p100, b, sizeof(b));
	render_text(PCT_X, y + 66, b, C(235, 235, 235), 20);
}

/* Levels 1-2: games / trophy groups */
static void draw_row_set(int y, const char *name, const char *sub, int p100,
                         const int cnt[4], int dlc) {
	render_text_clip(TXT_X, y + 40, name, C(245, 245, 245), 26, TXT_W);
	if (sub && sub[0]) render_text_clip(TXT_X, y + 66, sub, C(150, 150, 150), 16, TXT_W);
	else row_bar(y, p100);
	row_counts(y, cnt);
	if (dlc) blit(ICON_X + 6, y + 58, &S_dlc);
}

/* Level 3: trophies. The 90x90 trophy image on the left is composited at runtime; here we only bake the text and the right-side info */
static void draw_row_trophy(int y, const PsTrophy *t) {
	blit(TXT_X, y + 14, &C_cup26[cup_of(t->type)]);
	render_text_clip(TXT_X + 36, y + 40, t->cn[0] ? t->cn : t->name, C(245, 245, 245), 22,
	                 TXT_W - 36);
	if (t->detail[0])
		render_text_clip(TXT_X + 36, y + 66, t->detail, C(160, 160, 160), 15, TXT_W - 36);
	char b[64];
	if (t->rarity100 > 0) {
		char pc[16];
		fmt_pct(t->rarity100, pc, sizeof(pc));
		snprintf(b, sizeof(b), "%s %s", pc, t->rarity);
	} else {
		snprintf(b, sizeof(b), "%s", t->rarity);
	}
	render_text_clip(INFO_X, y + 40, b, C(150, 210, 150), 16, INFO_W);
	snprintf(b, sizeof(b), "%d Tips", t->tips_count);
	render_text_clip(INFO_X, y + 66, b,
	                 t->tips_count ? C(120, 190, 255) : C(110, 110, 110), 16, INFO_W);
}

/* Level 4: one tip's header + two-line body summary */
static void draw_row_tip(int y, const PsTip *t) {
	char head[176];
	snprintf(head, sizeof(head), "%s  ·  %s", t->author, t->date);
	render_text_clip(110, y + 26, head, C(150, 200, 255), 16, 800);
	render_text_wrap(110, y + 52, t->content, C(225, 225, 225), 18, 800, 2);
}

static void draw_row(int scr, int item, int y) {
	char sub[64];
	sub[0] = 0;
	switch (scr) {
	case SCR_GAMES: {
		const PsGame *g = &dt_list[item];
		snprintf(sub, sizeof(sub), "%s · %d 人玩过", g->platform, g->players);
		draw_row_set(y, g->name, sub, g->pct100, g->cnt, 0);
		break;
	}
	case SCR_GROUPS: {
		const PsGroup *g = &dt_group[item];
		draw_row_set(y, g->name, g->dlc ? "DLC" : "本体", 0, g->cnt, g->dlc);
		break;
	}
	case SCR_TROPHIES:
		draw_row_trophy(y, &dt_trophy[item]);
		break;
	default:
		draw_row_tip(y, &dt_tip[item]);
		break;
	}
}

/* ---------------- Row texture cache ----------------
   Baking a batch at startup no longer makes sense (data waits on the network), so bake on demand:
   key = (screen, item, data generation); a data generation change auto-invalidates old rows, and only the visible rows are baked. */
typedef struct {
	uint32_t *px;
	int scr, item, gen;
	unsigned age;
} RowSlot;

static RowSlot rc[MAX_CACHE];
static unsigned rc_age;
static uint32_t *bake_buf; /* ROW_H x RENDER_PITCH transparent staging canvas */

static uint32_t *row_tex(int scr, int item) {
	if (!bake_buf || item < 0) return NULL;
	int gen = gen_of(scr);
	for (int i = 0; i < MAX_CACHE; i++) {
		if (rc[i].px && rc[i].scr == scr && rc[i].item == item && rc[i].gen == gen) {
			rc[i].age = ++rc_age;
			return rc[i].px;
		}
	}
	int v = -1;
	for (int i = 0; i < MAX_CACHE; i++) {
		if (!rc[i].px) { v = i; break; }
		if (v < 0 || rc[i].age < rc[v].age) v = i;
	}
	if (!rc[v].px) rc[v].px = render_alloc0((size_t)ROW_H * SCREEN_W * 4);
	if (!rc[v].px) return NULL;
	uint32_t *d = rc[v].px;
	memset(d, 0, (size_t)ROW_H * SCREEN_W * 4);
	render_set_target(bake_buf);
	baking = 1;
	render_set_bake_alpha(1);
	draw_row(scr, item, 0);
	render_set_bake_alpha(0);
	baking = 0;
	render_set_target(NULL);
	for (int j = 0; j < ROW_H; j++)
		memcpy(d + (size_t)j * SCREEN_W, bake_buf + (size_t)j * RENDER_PITCH, SCREEN_W * 4);
	memset(bake_buf, 0, (size_t)ROW_H * RENDER_PITCH * 4);
	rc[v].scr = scr;
	rc[v].item = item;
	rc[v].gen = gen;
	rc[v].age = ++rc_age;
	return d;
}

/* ---------------- Row-band base images and list snapshot ----------------
   bandbg: bg slice + native unselected-row texture, baked at startup, memcpy'd as a whole block while scrolling.
   clean : snapshot of the whole list after the foreground is drawn (no selection mask). Each frame the mask first
           restores the rows it swept over to clean before overlaying; otherwise blending the same translucent mask onto the same row keeps brightening it. */
static uint32_t *bandbg[SCR_N][MAX_ROW];
static uint32_t *clean[SCR_N];

/* ---------------- Top/bottom overscroll separation (native row gaps when pulled past the top) ----------------
   ov_e is the current split amount (positive = pulled past top, negative = pushed past bottom); while tracking the finger it equals ov_tgt directly,
   and after release ov_tick springs back to 0 over 260ms with cubic easing.
   The gap is uniform: the anchored end stays put and the offset spreads linearly with the row index (native screenshots measure
   about a 22px gap with the top row fixed), so sep = ov_e * i / vis. */
#define OV_MAX   130
#define OV_BACK  260000LL

static int ov_e, ov_tgt, ov_from, ov_anim;
static long long ov_t0;

static int sep_of(int i, int vis) {
	if (!ov_e || vis <= 1) return 0;
	if (ov_e > 0) return ov_e * i / vis;               /* Pulled past top: top row anchored */
	return ov_e * (vis - 1 - i) / vis;                 /* Pushed past bottom: bottom row anchored */
}

static int ov_tick(long long now) {
	if (!ov_anim) return 0;
	long long t = now - ov_t0;
	if (t >= OV_BACK) {
		ov_e = 0;
		ov_anim = 0;
		return 1;
	}
	float u = 1.0f - (float)t / (float)OV_BACK;
	ov_e = (int)((float)ov_from * u * u * u);
	return 1;
}

static void ov_release(long long now) {
	if (!ov_tgt) return;
	ov_from = ov_e = ov_tgt;
	ov_tgt = 0;
	ov_t0 = now;
	ov_anim = 1;
}

/* ---------------- Pixel-level viewport (finger-tracking scroll + snap slide) ----------------
   viewpx is the list viewport's pixel position; at rest it always equals top*ROW_H.
   Touch dragging modifies viewpx directly (1:1 with the finger) and slides to the nearest row on release; d-pad paging uses
   a 130ms ease-out slide, the native row-by-row sliding feel. */
#define SLIDE_US 130000LL

static int viewpx[SCR_N];
static int sl_from, sl_to, sl_anim, sl_us, sl_cube, sl_fling;
static long long sl_t0;

static void view_slide_to(int scr, int tgt, long long now);
static int view_max(int scr);

static int view_tick(long long now, int scr) {
	if (!sl_anim) return 0;
	long long t = now - sl_t0;
	if (t >= sl_us) {
		viewpx[scr] = sl_to;
		sl_anim = 0;
		if (sl_fling) {   /* After the fling, settle onto the nearest row */
			sl_fling = 0;
			int vmax = view_max(scr);
			int tgt = (viewpx[scr] + ROW_H / 2) / ROW_H * ROW_H;
			if (tgt > vmax) tgt = vmax;
			if (tgt != viewpx[scr]) view_slide_to(scr, tgt, now);
		}
		return 1;
	}
	float u = (float)t / (float)sl_us;
	float e = sl_cube ? 1.0f - (1.0f - u) * (1.0f - u) * (1.0f - u)
	                  : 1.0f - (1.0f - u) * (1.0f - u);
	viewpx[scr] = sl_from + (int)((float)(sl_to - sl_from) * e);
	return 1;
}

static void view_slide_to(int scr, int tgt, long long now) {
	if (tgt == viewpx[scr]) return;
	sl_from = viewpx[scr];
	sl_to = tgt;
	sl_t0 = now;
	sl_us = SLIDE_US;
	sl_cube = 0;
	sl_fling = 0;
	sl_anim = 1;
}

/* Release inertia: keep sliding at the release velocity (cubic ease-out), then settle */
#define FLING_US  320000LL
#define FLING_VMIN 400

static void view_fling(int scr, int v_px_s, long long now) {
	int vmax = view_max(scr);
	int dist = v_px_s * 18 / 100;          /* Distance to decelerate to 0 ≈ v*0.18s */
	if (dist > 8 * ROW_H) dist = 8 * ROW_H;
	if (dist < -8 * ROW_H) dist = -8 * ROW_H;
	int tgt = viewpx[scr] + dist;
	if (tgt < 0) tgt = 0;
	if (tgt > vmax) tgt = vmax;
	if (tgt == viewpx[scr]) return;
	sl_from = viewpx[scr];
	sl_to = tgt;
	sl_t0 = now;
	sl_us = FLING_US;
	sl_cube = 1;
	sl_fling = 1;
	sl_anim = 1;
}

static int view_fr(int scr) { return viewpx[scr] - top_of(scr) * ROW_H; }

static int view_max(int scr) {
	int n = cnt_of(scr), vh = rows_vis(scr);
	return n > vh ? (n - vh) * ROW_H : 0;
}

/* ---------------- Touch white flash / on-demand teal mask / back-bubble focus ----------------
   Natively, entering a list shows no mask; the breathing teal mask lights up only on d-pad presses and goes off after a few idle seconds.
   Whichever row the finger holds flashes white once; LEFT gives the back bubble the teal mask (focus),
   and touching the bubble plays a white-mask animation before going back. */
#define FLASH_US    260000LL
#define GLOW_HOLD   3000000LL
#define BUB_FLASH_US 200000LL

static int flash_idx = -1, flash_k;
static long long flash_t0;
static int glow_on;
static long long glow_last;
static int bk_focus;
static long long bub_t0 = -1;

static int band_h(int scr, int pos) {
	int h = SCREEN_H - (lt_of(scr) + pos * ROW_H);
	return h > ROW_H ? ROW_H : h;
}

static void bake_bands(void) {
	for (int scr = 0; scr < SCR_N; scr++) {
		/* The level-5 reading page draws no list and has no selection mask; row bands and snapshots are unused */
		if (scr == SCR_TIPVIEW) continue;
		int s0 = snap0_of(scr);
		clean[scr] = render_alloc((size_t)(SCREEN_H - s0) * RENDER_PITCH * 4);
		if (!clean[scr]) dlog("clean alloc fail");
		for (int p = 0; p < rows_draw(scr); p++) {
			int y = lt_of(scr) + p * ROW_H, h = band_h(scr, p);
			uint32_t *d = render_alloc((size_t)h * RENDER_PITCH * 4);
			bandbg[scr][p] = d;
			if (!d) { dlog("band alloc fail"); continue; }
			for (int j = 0; j < h; j++)
				memcpy(d + (size_t)j * RENDER_PITCH, bg + (size_t)(y + j) * SCREEN_W,
				       SCREEN_W * 4);
			blend_into(d, h, 0, -1, &S_rowbg); /* Texture is 1px taller than the row band on top and bottom */
		}
	}
}

/* ---------------- Top bar / hints / top-right spinner ---------------- */
static Spr S_spin[8]; /* Eight spinner frames, positioned like the loading animation on the native PSN page */

static void bake_spinner(void) {
	for (int f = 0; f < 8; f++) {
		if (spr_alloc(&S_spin[f], SPN_S, SPN_S) < 0) return;
		Spr *s = &S_spin[f];
		int cx = SPN_S / 2, cy = SPN_S / 2;
		for (int i = 0; i < 8; i++) {
			int a = (i + 8 - f) & 7;           /* Newer tail segments are brighter */
			int al = 40 + a * 26;
			float ang = (float)i * 0.785398f;
			int r0 = 8, r1 = 14;
			for (int t = r0 * 4; t <= r1 * 4; t++) {
				float rr = (float)t / 4.f;
				int x = cx + (int)(rr * cosf(ang));
				int y = cy + (int)(rr * sinf(ang));
				if (x < 0 || x >= s->w || y < 0 || y >= s->h) continue;
				s_px(s, x, y, CA(235, 240, 245, al));
				s_px(s, x + 1, y, CA(235, 240, 245, al * 2 / 3));
				s_px(s, x, y + 1, CA(235, 240, 245, al * 2 / 3));
			}
		}
	}
}

static int spin_shown = -1;

/* f<0: restore this area to the background (spinner finished) */
static void draw_spinner(int f) {
	if (f == spin_shown) return;
	spin_shown = f;
	uint32_t *fb = render_cur_fb();
	int y0 = SPN_Y - 4, y1 = SPN_Y + SPN_S + 4;
	for (int y = y0; y < y1; y++)
		memcpy(fb + (size_t)y * RENDER_PITCH + SPN_X - 4, bg + (size_t)y * SCREEN_W + SPN_X - 4,
		       (SPN_S + 8) * 4);
	if (f >= 0 && S_spin[f].p) blend_into(fb, SCREEN_H, SPN_X, SPN_Y, &S_spin[f]);
	render_mark_dirty(y0, y1 - y0);
}

static void draw_topbar(void) {
	blit(SRCH_X, SRCH_Y, &S_field);
	if (query[0]) {
		char b[128];
		snprintf(b, sizeof(b), "搜索：%s", query);
		render_text_clip(SRCH_X + 14, 88, b, C(235, 238, 242), 18, SB_W2 - 28);
	} else {
		render_text_clip(SRCH_X + 14, 88, "搜索游戏（按 △）", C(150, 155, 165), 18, SB_W2 - 28);
	}
	blit(668, 47, &S_legend); /* Native texture: the four trophies include their own reflection, 66 high, centered in 32..128 */
}

static void draw_hint(int scr) {
	const char *ok = ok_btn == SCE_CTRL_CIRCLE ? "○" : "×";
	const char *bk = back_btn == SCE_CTRL_CIRCLE ? "○" : "×";
	char buf[96];
	if (scr == SCR_GAMES)
		snprintf(buf, sizeof(buf), "%s 打开  上下选择  △ 搜索  □ 取消搜索", ok);
	else if (scr == SCR_GROUPS) snprintf(buf, sizeof(buf), "%s 进入   %s 返回", ok, bk);
	else if (scr == SCR_TROPHIES) snprintf(buf, sizeof(buf), "%s 看 Tips   %s 返回", ok, bk);
	else if (scr == SCR_TIPS) snprintf(buf, sizeof(buf), "%s 读全文   %s 返回", ok, bk);
	else snprintf(buf, sizeof(buf), "上下翻正文   %s 返回", bk);
	if (scr == SCR_TIPVIEW) {
		render_text(HINT_X, 94, buf, C(150, 150, 150), 16);
		return;
	}
	/* List pages: hint moves up one notch, source attribution on the line below */
	render_text(HINT_X, 68, buf, C(150, 150, 150), 16);
	render_text(HINT_X, 94, "数据实时查询自 psnine.com", C(110, 115, 122), 14);
}

/* ---------------- Bottom-left back button (non-level-1 pages) ----------------
   Like native, a circular bubble partly clipped by the bottom-left corner: measured from native screenshots,
   center (30,522), radius 55, with the ↩ arrow sitting slightly above the middle of the visible bubble area. */
#define BUB_X   -25
#define BUB_Y   467
#define BUB_HIT_X 92
#define BUB_HIT_Y 462

static void draw_back_btn(int scr) {
	if (scr == SCR_GAMES || !S_back.p) return;
	blit(BUB_X, BUB_Y, &S_back);
	uint32_t *fb = render_cur_fb();
	if (bub_t0 >= 0) {   /* Touching the bubble: white mask plays from opaque to transparent */
		long long t = sceKernelGetProcessTimeWide() - bub_t0;
		int k = 255 - (int)(t * 255 / BUB_FLASH_US);
		if (k > 0) blend_into_k(fb, SCREEN_H, BUB_X, BUB_Y, &S_bub_w, k);
	} else if (bk_focus) {
		blend_into_k(fb, SCREEN_H, BUB_X, BUB_Y, &S_bub_tl, 255);
	}
}

/* Scrollbar: draw only the part overlapping [y0,y1); animation doesn't dirty the whole screen */
static void draw_scrollbar(int scr, int y0, int y1) {
	int lt = lt_of(scr), n = cnt_of(scr), vh = rows_vis(scr);
	int th = SCREEN_H - 8 - lt;
	if (n <= vh || th <= 0) return;
	int dh = th * vh / n;
	if (dh < 24) dh = 24;
	int dy = lt + (th - dh) * top_of(scr) / (n - vh);
	if (y0 < lt) y0 = lt;
	if (y1 > lt + th) y1 = lt + th;
	if (y1 <= y0) return;
	uint32_t trk[SB_W], hub[SB_W], cap[SB_W];
	for (int i = 0; i < SB_W; i++) {
		trk[i] = CA(140, 140, 140, 30);
		hub[i] = CA(200, 200, 200, 160);
		cap[i] = CA(200, 200, 200, 70);
	}
	uint32_t *fb = render_cur_fb();
	for (int r = y0; r < y1; r++) {
		int k = r - dy;
		const uint32_t *c = (k < 0 || k >= dh) ? trk : (k == 0 || k == dh - 1) ? cap : hub;
		blend_row(fb + (size_t)r * RENDER_PITCH + SB_X, c, SB_W);
	}
	render_mark_dirty(y0, y1 - y0);
}

/* One-line note for empty lists: "loading" while in flight; on failure show the error code and press ○ to retry */
static void draw_empty_note(int scr) {
	char b[64];
	if (data_err() < 0) {
		snprintf(b, sizeof(b), "加载失败 %x，按 %s 重试", (unsigned)(-data_err()),
		         ok_btn == SCE_CTRL_CIRCLE ? "○" : "×");
		render_text(120, lt_of(scr) + 40, b, C(255, 120, 120), 20);
	} else if (data_busy()) {
		snprintf(b, sizeof(b), "正在加载…");
		render_text(120, lt_of(scr) + 40, b, C(150, 150, 150), 20);
	} else if (scr == SCR_TIPS) {
		render_text(120, lt_of(scr) + 40, "这条奖杯还没有人写过 Tips", C(150, 150, 150), 20);
	}
	render_mark_dirty(snap0_of(scr), SCREEN_H - snap0_of(scr));
}

/* ---------------- List drawing ---------------- */
/* Trophy icons only trust PSN's: leave blank until fetched, no native placeholder cup.
   But data_icon must still be called — download registration/retries live inside it; only checking state means the request never gets sent */
static void blit_cup(int x, int y, const PsTrophy *t) {
	if (!t->icon[0]) return;
	int w, h;
	const uint32_t *px = data_icon(IC_CUP, t->icon, &w, &h);
	if (data_icon_state(IC_CUP, t->icon) != DI_READY) return;
	Spr s = { (uint32_t *)px, w, h };
	blend_into(render_cur_fb(), baking ? ROW_H : SCREEN_H, x, y, &s);
}

static void row_icon(int scr, int item, int y) {
	if (scr == SCR_GAMES) blit_icon(ICON_X, y + 3, IC_COVER, dt_list[item].icon);
	else if (scr == SCR_GROUPS) blit_icon(ICON_X, y + 3, IC_COVER, dt_head.icon);
	else if (scr == SCR_TROPHIES) blit_cup(ICON_X + 32, y, &dt_trophy[item]);
}

/* Lay the row-band base back over the whole visible area. The last row is shorter than ROW_H and band_h already clipped it to the screen edge,
   so the base fills all the way to SCREEN_H with no gap below (old thumbs no longer get scorched into the gap). */
static void rows_bg(int scr) {
	int lt = lt_of(scr), vh = rows_draw(scr), fr = view_fr(scr);
	uint32_t *fb = render_cur_fb();
	if (ov_e || fr) { /* Splitting/sliding: lay the whole list area back to the background first, then paste row bands at their offsets */
		for (int r = lt; r < SCREEN_H; r++)
			memcpy(fb + (size_t)r * RENDER_PITCH, bg + (size_t)r * SCREEN_W, SCREEN_W * 4);
	}
	for (int i = -1; i <= vh; i++) {
		int y = lt + i * ROW_H - fr + sep_of(i, vh), h = band_h(scr, i);
		int so = 0;                    /* Row pokes above the list area's top edge: clip off the protruding part */
		if (y < lt) { so = lt - y; y = lt; }
		if (y + h > SCREEN_H) h = SCREEN_H - y;
		h -= so;
		if (h <= 0 || i < 0 || i >= vh || !bandbg[scr][i]) continue;
		memcpy(fb + (size_t)y * RENDER_PITCH, bandbg[scr][i] + (size_t)so * RENDER_PITCH,
		       (size_t)h * RENDER_PITCH * 4);
	}
	/* Row count includes partial rows (rows_draw); bandbg already fills to SCREEN_H, no gap patching needed */
}

static void clean_snap(int scr) {
	/* Rows are displaced while splitting/sliding; writing them into the snapshot would make mask restoration pull the displaced rows back: record only once settled */
	if (!clean[scr] || ov_e || view_fr(scr)) return;
	int s0 = snap0_of(scr);
	uint32_t *fb = render_cur_fb();
	for (int r = s0; r < SCREEN_H; r++)
		memcpy(clean[scr] + (size_t)(r - s0) * RENDER_PITCH, fb + (size_t)r * RENDER_PITCH,
		       SCREEN_W * 4);
}

static void paint_list(int scr) {
	int top = top_of(scr), n = cnt_of(scr), lt = lt_of(scr), vh = rows_draw(scr);
	int fr = view_fr(scr);
	uint32_t *fb = render_cur_fb();
	rows_bg(scr);
	if (!n) {
		draw_empty_note(scr);
		draw_back_btn(scr);
		clean_snap(scr);
		render_mark_dirty(snap0_of(scr), SCREEN_H - snap0_of(scr));
		return;
	}
	blend_clip_y = lt; /* On top/bottom overscroll split or slide, row content is clipped inside the list area */
	for (int i = -1; i <= vh; i++) {
		int idx = top + i;
		if (idx < 0 || idx >= n) continue;
		int y = lt + i * ROW_H - fr + sep_of(i, vh);
		if (y + ROW_H <= lt || y >= SCREEN_H) continue;
		int item = item_of(scr, idx);
		if (i < 0 || i >= vh) blend_into(fb, SCREEN_H, 0, y - 1, &S_rowbg); /* Sliding-in rows have no row band */
		uint32_t *tex = row_tex(scr, item);
		if (tex) {
			Spr s = { tex, SCREEN_W, ROW_H };
			blend_into(fb, SCREEN_H, 0, y, &s);
		}
		row_icon(scr, item, y);
	}
	if (flash_idx >= 0 && flash_k > 0) {   /* Flash the touch-start row white once */
		int i = flash_idx - top;
		if (i >= -1 && i <= vh)
			blend_into_k(fb, SCREEN_H, 0, lt + i * ROW_H - fr + sep_of(i, vh) - 1, &S_selw, flash_k);
	}
	blend_clip_y = 0;
	/* The scrollbar must be scorched into the snapshot: the thumb only moves with top, and top stays put while the cursor moves within a page,
	   so restoring rows during breathing frames won't leave an old thumb in place or stack the same gray layer over and over. */
	draw_scrollbar(scr, 0, SCREEN_H);
	draw_back_btn(scr);
	clean_snap(scr);
	render_mark_dirty(snap0_of(scr), SCREEN_H - snap0_of(scr));
}

/* ---------------- Selection mask easing + breathing ---------------- */
#define GLOW_US   140000LL
#define GLOW_CUBE (GLOW_US * GLOW_US * GLOW_US)
#define FRAME_US  16667LL
#define PULSE_US  1250000LL    /* One full cycle of mask depth breathing */
#define PULSE_MID 195          /* Center intensity (255 = texture as-is) */
#define PULSE_AMP 60           /* Swing: 135~255, the teal light brightening and dimming */
#define PULSE_FRAME 33333LL    /* Breathing only needs 30fps, saving half the cost of the movement animation */

static int glow_y, glow_from, glow_to, glow_anim;
static int glow_k = 255;
static long long glow_t0, glow_next;

/* The native mask is a blue-green light band whose brightness periodically deepens and lightens */
static int pulse_k(long long now) {
	float ph = (float)(now % PULSE_US) * (6.2831853f / (float)PULSE_US);
	return PULSE_MID + (int)((float)PULSE_AMP * sinf(ph));
}

static int glow_target(int scr) {
	return lt_of(scr) + (cur_of(scr) - top_of(scr)) * ROW_H - 1 - view_fr(scr) +
	       sep_of(cur_of(scr) - top_of(scr), rows_draw(scr));
}

static void glow_at(int y, int scr, int r0, int r1) {
	uint32_t *fb = render_cur_fb();
	/* Don't lay the selection band while the list is still empty: in native, with no selection the row is just background; we earlier made it breathe.
	   No need to redraw the scrollbar here: it's already scorched into the clean snapshot and comes back naturally when rows are restored */
	if (cnt_of(scr) > 0) {
		blend_clip_y = lt_of(scr) - 1; /* The mask is inherently 1px taller than the row (snap0); clip to here when pushed past the bottom */
		blend_into_k(fb, SCREEN_H, 0, y, &S_sel, glow_k);
		blend_clip_y = 0;
	}
	render_mark_dirty(r0, r1 - r0);
}

static void glow_snap(int y, int scr) {
	int s0 = snap0_of(scr);
	int r0 = y < s0 ? s0 : y;
	int r1 = y + SEL_H > SCREEN_H ? SCREEN_H : y + SEL_H;
	glow_y = y;
	glow_anim = 0;
	glow_at(y, scr, r0, r1);
}

static void glow_start(int to, long long now) {
	glow_from = glow_y;
	glow_to = to;
	glow_t0 = now;
	glow_anim = 1;
}

/* Restore [r0,r1) to the clean mask-less snapshot, then overlay the mask at y with the current intensity */
static void glow_redraw(int scr, int r0, int r1, int y) {
	uint32_t *fb = render_cur_fb();
	int s0 = snap0_of(scr);
	if (clean[scr])
		for (int r = r0; r < r1; r++)
			memcpy(fb + (size_t)r * RENDER_PITCH,
			       clean[scr] + (size_t)(r - s0) * RENDER_PITCH, SCREEN_W * 4);
	glow_at(y, scr, r0, r1);
}

static void glow_band_rows(int scr, int *r0, int *r1) {
	int s0 = snap0_of(scr);
	*r0 = glow_y < s0 ? s0 : glow_y;
	*r1 = glow_y + SEL_H > SCREEN_H ? SCREEN_H : glow_y + SEL_H;
}

/* Idle breathing frame: redraw only the few rows the mask swept over */
static int pulse_tick(long long now, int scr) {
	if (now < glow_next) return -1;
	glow_next = now + PULSE_FRAME;
	int k = pulse_k(now);
	if (k == glow_k) return -1;
	glow_k = k;
	int r0, r1;
	glow_band_rows(scr, &r0, &r1);
	glow_redraw(scr, r0, r1, glow_y);
	return 1;
}

/* Returns 1 = screen updated, 0 = in place with no change, -1 = next frame not due yet */
static int glow_step(long long now, int scr) {
	if (now < glow_next) return -1;
	glow_next = now + FRAME_US;
	glow_k = pulse_k(now);
	int y;
	long long e = now - glow_t0;
	if (e >= GLOW_US) {
		y = glow_to;
		glow_anim = 0;
	} else {
		long long d = (long long)glow_to - glow_from;
		long long rem = GLOW_US - e;
		y = glow_to - (int)(d * rem * rem * rem / GLOW_CUBE); /* Cubic ease-out */
	}
	if (y == glow_y) return glow_anim ? -1 : 0;
	int s0 = snap0_of(scr);
	int lo = glow_y < y ? glow_y : y;
	int hi = glow_y > y ? glow_y : y;
	int r0 = lo < s0 ? s0 : lo;
	int r1 = hi + SEL_H > SCREEN_H ? SCREEN_H : hi + SEL_H;
	glow_redraw(scr, r0, r1, y);
	glow_y = y;
	return 1;
}

/* ---------------- Page turn: the old page flips outward around a vertical edge, narrowing in perspective ----------------
   When native enters level 2/3, the previous page is flipped away like a sheet of paper: the right edge (left edge when going back)
   translates all the way to the opposite side while the page is squashed near-large-far-small, flipping until invisible as the new page is revealed underneath.
   Approach: save the whole old screen before switching pages, save again after the new page is drawn; each frame first restore the column range
   covered last frame from the new page, then paste the old page back column by column with perspective. */
#define PEEL_US     420000LL
#define PEEL_FRAME  33333LL    /* 30fps is enough; the saved time goes to per-column sampling */
#define PEEL_Y0     LT_TOP     /* The top bar doesn't participate in the page turn */
#define PEEL_CY     320        /* Perspective centerline; the page's top and bottom edges converge toward it */
#define PEEL_TURN   1.15f      /* Max rotation angle (radians, about 66 degrees) */
#define PEEL_DEPTH  2.6f       /* Strength of the near-large-far-small effect */

static uint32_t *peel_src, *peel_new;
static int peel_ready, peel_on, peel_dir, peel_x0, peel_x1;
static long long peel_t0, peel_next;

static void peel_alloc(void) {
	size_t n = (size_t)SCREEN_H * RENDER_PITCH * 4;
	peel_src = render_alloc(n);
	peel_new = render_alloc(n);
	peel_ready = peel_src && peel_new;
	if (!peel_ready) dlog("peel alloc fail");
}

/* Called before switching pages: save the old page still on screen */
static void peel_begin(int dir, long long now) {
	if (!peel_ready) return;
	memcpy(peel_src, render_cur_fb(), (size_t)SCREEN_H * RENDER_PITCH * 4);
	peel_dir = dir;
	peel_t0 = now;
	peel_next = now; /* Must draw this frame: the old page still fills the screen */
	peel_x0 = 0;
	peel_x1 = SCREEN_W; /* Frame 0: old page fills the screen, restore full-width first */
	peel_on = 1;
}

/* Called after the new page is drawn: save it as the base during the page turn */
static void peel_capture(void) {
	memcpy(peel_new, render_cur_fb(), (size_t)SCREEN_H * RENDER_PITCH * 4);
}

/* Returns 1 = screen updated, -1 = next frame not due yet */
static int peel_frame(long long now) {
	if (now < peel_next) return -1;
	peel_next = now + PEEL_FRAME;
	uint32_t *fb = render_cur_fb();
	float t = (float)(now - peel_t0) / (float)PEEL_US;
	if (t > 1.f) t = 1.f;
	float e = t * t * (3.f - 2.f * t); /* smoothstep: slow at both ends, fast in the middle */
	if (peel_x1 > peel_x0) {
		size_t nb = (size_t)(peel_x1 - peel_x0) * 4;
		for (int y = PEEL_Y0; y < SCREEN_H; y++)
			memcpy(fb + (size_t)y * RENDER_PITCH + peel_x0,
			       peel_new + (size_t)y * RENDER_PITCH + peel_x0, nb);
	}
	peel_x0 = SCREEN_W;
	peel_x1 = 0;
	if (e >= 1.f) {
		peel_on = 0;
		render_mark_dirty(PEEL_Y0, SCREEN_H - PEEL_Y0);
		return 1;
	}
	float cs = cosf(e * PEEL_TURN);
	float D = PEEL_DEPTH * e;
	int hinge = (int)(SCREEN_W * (1.f - e) + 0.5f); /* Hinge; moves right to left in mirrored space */
	int shade = 255 - (int)(60.f * e);              /* Darken slightly while flipping away */
	for (int xp = hinge; xp >= 0; xp--) {
		float v = (float)(hinge - xp) / (960.f * cs); /* v = u/(1+D*u) */
		float den = 1.f - D * v;
		if (den < 0.02f) break;
		float u = v / den;
		if (u > 1.f) break;                        /* Past the paper edge; nothing left on the left */
		float S = 1.f / (1.f + D * u);             /* Vertical scale of this column */
		int sc = (int)(u * 959.f + 0.5f);
		int scol = peel_dir > 0 ? SCREEN_W - 1 - sc : sc;
		int X = peel_dir > 0 ? xp : SCREEN_W - 1 - xp;
		if (X < 0 || X >= SCREEN_W) continue;
		int y0 = PEEL_CY + (int)((PEEL_Y0 - PEEL_CY) * S);
		int y1 = PEEL_CY + (int)((SCREEN_H - PEEL_CY) * S);
		if (y0 < PEEL_Y0) y0 = PEEL_Y0;
		if (y1 > SCREEN_H) y1 = SCREEN_H;
		if (y1 <= y0) continue;
		int s256 = (int)(256.f / S); /* Fixed point: destination row -> source row */
		for (int y = y0; y < y1; y++) {
			int sy = PEEL_CY + (((y - PEEL_CY) * s256) >> 8);
			if (sy < PEEL_Y0) sy = PEEL_Y0;
			else if (sy >= SCREEN_H) sy = SCREEN_H - 1;
			uint32_t p = peel_src[(size_t)sy * RENDER_PITCH + scol];
			uint32_t r = ((p & 0xff) * shade) >> 8;
			uint32_t g = (((p >> 8) & 0xff) * shade) >> 8;
			uint32_t b = (((p >> 16) & 0xff) * shade) >> 8;
			fb[(size_t)y * RENDER_PITCH + X] = (p & 0xff000000) | (b << 16) | (g << 8) | r;
		}
		if (X < peel_x0) peel_x0 = X;
		if (X + 1 > peel_x1) peel_x1 = X + 1;
	}
	render_mark_dirty(PEEL_Y0, SCREEN_H - PEEL_Y0);
	return 1;
}

/* ---------------- Info band (from level 2 on, the strip above the list) ----------------
   No texture baking: it's drawn only once per screen/generation change, so drawing straight into the canvas is simpler and can't pick up stale data. */
static void draw_header(int scr) {
	int y = LT_TOP;
	uint32_t *fb = render_cur_fb();
	for (int j = 0; j < ROW_H; j++)
		memcpy(fb + (size_t)(y + j) * RENDER_PITCH, bg + (size_t)(y + j) * SCREEN_W,
		       SCREEN_W * 4);
	blend_into(fb, SCREEN_H, 0, y - 1, &S_rowbg);
	char sub[80];
	sub[0] = 0;
	if (scr == SCR_GROUPS || scr == SCR_TROPHIES) {
		if (scr == SCR_GROUPS) {
			draw_row_set(y, dt_head.name, NULL, dt_head.pct100, dt_head.cnt, 0);
		} else {
			const PsGroup *g = &dt_group[cur_group()];
			snprintf(sub, sizeof(sub), "%d 个奖杯%s", g->tn, g->dlc ? " · DLC" : "");
			draw_row_set(y, g->name, sub, 0, g->cnt, 0);
		}
		blit_icon(ICON_X, y + 3, IC_COVER, dt_head.icon);
	} else {
		const PsTrophy *t = &dt_trophy[trophy_sel()];
		blit_cup(ICON_X + 32, y, t);
		render_text_clip(TXT_X, y + 40, t->cn[0] ? t->cn : t->name, C(245, 245, 245), 24, TXT_W);
		if (scr == SCR_TIPVIEW && tips_ready() && curv[SCR_TIPS] < dt_tip_n) {
			const PsTip *p = &dt_tip[curv[SCR_TIPS]];
			char hb[176];
			snprintf(hb, sizeof(hb), "%s · %s", p->author, p->date);
			render_text_clip(TXT_X, y + 66, hb, C(150, 200, 255), 16, TXT_W);
		} else {
			render_text_clip(TXT_X, y + 66, t->detail, C(150, 150, 150), 15, TXT_W);
		}
	}
	render_mark_dirty(y - 1, ROW_H + 1);
}

/* ---------------- Level 5: full-page body ----------------
   Line spacing is fixed by the font size (px + px/4), so scrolling is by "line"; the last line isn't clipped, no body character is lost. */
#define TV_X 60
#define TV_W 840
#define TV_PX 20
#define TV_LH (TV_PX + TV_PX / 4)
static int tv_top_max, tv_total;

static void paint_tipview(void) {
	int y0 = LT_TOP + ROW_H + TV_LH;
	/* Reserve the bottom strip for the back button so the last body line isn't covered by the key art */
	int vis = (SCREEN_H - 52 - y0) / TV_LH;
	if (vis < 1) vis = 1;
	/* Lay the body area's background back first: scrolling uses the fast path that only redraws the list area, so the previous frame's text is still on the canvas,
	   and overlaying directly would ghost (the full-screen redraw path has blit_bg; only here was it missed) */
	uint32_t *fb = render_cur_fb();
	for (int y = LT_TOP + ROW_H; y < SCREEN_H; y++)
		memcpy(fb + (size_t)y * RENDER_PITCH, bg + (size_t)y * SCREEN_W, SCREEN_W * 4);
	if (!tips_ready() || !dt_tip_n) {
		if (data_busy()) render_text(TV_X, y0 + 20, "正在加载 Tips…", C(150, 150, 150), 20);
		else render_text(TV_X, y0 + 20, "这条奖杯还没有 Tips", C(150, 150, 150), 20);
		render_mark_dirty(LT_TOP + ROW_H, SCREEN_H - LT_TOP - ROW_H);
		return;
	}
	const PsTip *p = &dt_tip[curv[SCR_TIPS] < dt_tip_n ? curv[SCR_TIPS] : 0];
	int total = 0;
	render_text_scroll(TV_X, y0, p->content, C(225, 225, 225), TV_PX, TV_W, 0, 0, &total);
	tv_total = total;
	tv_top_max = total > vis ? total - vis : 0;
	if (tip_top > tv_top_max) tip_top = tv_top_max;
	if (tip_top < 0) tip_top = 0;
	render_text_scroll(TV_X, y0, p->content, C(225, 225, 225), TV_PX, TV_W, vis, tip_top, NULL);
	char b[40];
	snprintf(b, sizeof(b), "%d / %d 行", tip_top + 1, total);
	render_text(SCREEN_W - 210, SCREEN_H - 12, b, C(140, 140, 140), 14);
	if (tip_top < tv_top_max) render_text(SCREEN_W - 90, SCREEN_H - 14, "下 ▾", C(140, 140, 140), 16);
	draw_back_btn(SCR_TIPVIEW);
	render_mark_dirty(LT_TOP + ROW_H, SCREEN_H - LT_TOP - ROW_H);
}

/* ---------------- Full screen ---------------- */
static void paint_screen(int scr) {
	blit_bg();
	draw_topbar();
	draw_hint(scr);
	spin_shown = -1; /* After a full-screen redraw the spinner must be redrawn too */
	if (scr == SCR_GAMES) {
		paint_list(scr);
	} else {
		draw_header(scr);
		if (scr == SCR_TIPVIEW) paint_tipview();
		else paint_list(scr);
	}
	/* The top bar and bottom hints differ per screen, but blit_bg/draw_topbar don't mark dirty themselves:
	   without marking the full screen dirty once, the top bar would still show the previous screen's hint after a page turn */
	render_mark_dirty(0, SCREEN_H);
}

/* Redraw only the list area (icon arrived, cursor scrolled, loading note changed) */
static void repaint_list(int scr) {
	if (scr == SCR_TIPVIEW) paint_tipview();
	else paint_list(scr);
}

/* ---------------- System buttons: HK/CN consoles use ○ confirm × back; JP/US consoles are reversed ---------------- */
static void init_buttons(void) {
	int lm = sceSysmoduleLoadModule(SCE_SYSMODULE_APPUTIL);
	dlog("apputil load=%x", (unsigned)lm);
	/* Pass all-zero structs like the psnine main app: workBufSize is the system's work buffer size for apputil;
	   filling in sizeof(param struct) earlier was plain wrong and made init fail */
	SceAppUtilInitParam ip = {0};
	SceAppUtilBootParam bp = {0};
	int ai = sceAppUtilInit(&ip, &bp);
	dlog("apputil init=%x", (unsigned)ai);
	if (ai < 0) return;
	int v = SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
	int gi = sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &v);
	dlog("enter_param=%x v=%d", (unsigned)gi, v);
	if (gi < 0) return;
	if (v == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE) {
		ok_btn = SCE_CTRL_CIRCLE;
		back_btn = SCE_CTRL_CROSS;
	}
}

/* ---------------- System status bar (infobar) ----------------
   Composited by the shell, overlaid on y0..31 (our top bar starts at y=37, so it isn't blocked); doesn't shift the layout.
   Always shown: the native trophy app also carries this black status bar the whole time. */
static void infobar_set(int on) {
	int r = sceAppMgrSetInfobarState(
	    on ? SCE_APPMGR_INFOBAR_VISIBILITY_VISIBLE : SCE_APPMGR_INFOBAR_VISIBILITY_INVISIBLE,
	    SCE_APPMGR_INFOBAR_COLOR_BLACK, SCE_APPMGR_INFOBAR_TRANSPARENCY_OPAQUE);
	dlog("infobar on=%d -> %x", on, (unsigned)r);
}

/* Whether the data a screen needs is missing when entering it; if missing, send the request */
static void want_screen(int scr) {
	if (scr == SCR_GAMES) {
		if (!list_ready() && !list_pending()) {
			list_want = 1;
			data_request_list(1);
		}
	} else if (scr == SCR_GROUPS || scr == SCR_TROPHIES) {
		if (!game_ready() && want_game) data_request_game(want_game);
	} else if (scr == SCR_TIPS || scr == SCR_TIPVIEW) {
		if (!tips_ready() && want_trophy) data_request_tips(want_trophy);
	}
}

/* ---------------- △ search: system IME (common dialog overlaid on our background page) ---------------- */
static SceWChar16 ime_text[128];
static int ime_active, ime_loaded;

static void u16_to_u8(const SceWChar16 *w, char *out, int cap) {
	int o = 0;
	for (; *w && o + 4 < cap; w++) {
		unsigned c = *w;
		if (c < 0x80) out[o++] = (char)c;
		else if (c < 0x800) {
			out[o++] = (char)(0xC0 | (c >> 6));
			out[o++] = (char)(0x80 | (c & 0x3F));
		} else {
			out[o++] = (char)(0xE0 | (c >> 12));
			out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
			out[o++] = (char)(0x80 | (c & 0x3F));
		}
	}
	out[o] = 0;
}

/* Search term applied to the level-1 list: results replace the whole table, and we page back to level 1 from other levels */
static void run_search(void) {
	list_want = 1;
	curv[SCR_GAMES] = topv[SCR_GAMES] = 0;
	data_request_search(query);
	if (screen_id != SCR_GAMES) {
		peel_req = -1;
		screen_id = SCR_GAMES;
	}
}

static void open_ime(void) {
	if (!ime_loaded) {
		int r = sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
		dlog("ime load=%x", (unsigned)r);
		if (r < 0) return;
		ime_loaded = 1;
	}
	ime_text[0] = 0;
	SceImeDialogParam p;
	sceImeDialogParamInit(&p);
	p.supportedLanguages = SCE_IME_LANGUAGE_SIMPLIFIED_CHINESE |
	                       SCE_IME_LANGUAGE_TRADITIONAL_CHINESE |
	                       SCE_IME_LANGUAGE_ENGLISH;
	/* Don't force a language mask: if the console's system language lacks the forced one, Init fails silently */
	p.languagesForced = SCE_FALSE;
	p.type = SCE_IME_TYPE_DEFAULT;
	p.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
	p.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
	p.title = (SceWChar16 *)u"搜索游戏";
	p.maxTextLength = 32;
	p.inputTextBuffer = ime_text;
	p.enterLabel = SCE_IME_ENTER_LABEL_SEARCH;
	int r = sceImeDialogInit(&p);
	dlog("ime init=%x", (unsigned)r);
	if (r < 0) return;
	ime_active = 1;
	/* Icon download+decode eats two cores, and the keyboard is exactly when finger-tracking matters: pause while the dialog is open;
	   page requests (tips/details) are unaffected, and it resumes as soon as the dialog closes */
	data_icons_paused(1);
}

/* Returns 1 = the dialog has ended (text captured or exited on failure) */
static int pump_ime(void) {
	SceCommonDialogStatus st = sceImeDialogGetStatus();
	if (st < 0) { /* Don't get stuck in the IME branch when Init succeeded but the dialog isn't running */
		dlog("ime status=%x", (unsigned)st);
		sceImeDialogTerm();
		ime_active = 0;
		data_icons_paused(0);
		return 1;
	}
	if (st != SCE_COMMON_DIALOG_STATUS_FINISHED) return 0;
	SceImeDialogResult r;
	memset(&r, 0, sizeof(r));
	sceImeDialogGetResult(&r);
	sceImeDialogTerm();
	ime_active = 0;
	data_icons_paused(0);
	if (r.button == SCE_IME_DIALOG_BUTTON_ENTER) {
		char kw[sizeof(query)];
		u16_to_u8(ime_text, kw, sizeof(kw));
		if (kw[0]) {
			snprintf(query, sizeof(query), "%s", kw);
			run_search();
		}
	}
	return 1;
}

/* □ cancel search: clear the keyword, return to the latest list */
static void cancel_search(void) {
	query[0] = 0;
	list_want = 1;
	curv[SCR_GAMES] = topv[SCR_GAMES] = 0;
	data_request_list(1);
	if (screen_id != SCR_GAMES) {
		peel_req = -1;
		screen_id = SCR_GAMES;
	}
}

static long long spin_next;
static int spin_f;

static long long prof_sum, prof_max;
static int prof_n;

/* Top-right spinner: 8fps is enough; it marks its own rows dirty */
static int spin_tick(long long now) {
	if (now < spin_next) return 0;
	spin_next = now + 125000;
	if (data_busy()) draw_spinner(spin_f = (spin_f + 1) & 7);
	else if (spin_shown >= 0) draw_spinner(-1);
	else return 0;
	return 1;
}

/* ---------------- Touch ----------------
   The list follows the finger row by row: every full row height the finger travels moves the cursor one slot, and reveal() drags the viewport along.
   No pixel-level smooth scrolling — row-band textures and the clean snapshot are all ROW_H-aligned; a half-row offset
   would require rewriting the whole compositing chain, not worth the gain.
   Dragging past the bounds doesn't hard-stop: it feeds overscroll, rows split apart (native's pull-past-top separation animation),
   springing back 260ms after release.
   On lift, total displacement <12px and hold <300ms counts as a tap. */
#define TAP_MOVE 12
#define TAP_US   300000LL

static int tr_down, tr_x, tr_y, tr_last_y, tr_acc, tr_moved;
static long long tr_t0, tr_last_t;
static int tr_v;        /* Finger velocity px/s, used for release inertia */
static int drag_pos;    /* Raw drag position including overscroll allowance; the gap doesn't self-close while held */
static int tg_n, tg_step, tg_tap; /* On-device diagnostics: touches received, slots scrolled, taps landed */

/* Returns 1 = redraw needed; tap: >=0 row index, -1 none, -2 back button, -3 search box */
static int touch_step(long long now, int scr, int *tap) {
	SceTouchData td;
	*tap = -1;
	if (sceTouchRead(SCE_TOUCH_PORT_FRONT, &td, 1) < 0) return 0;

	if (td.reportNum == 0) {
		if (!tr_down) return 0;
		tr_down = 0;
		if (now - tr_last_t > 80000) tr_v = 0; /* Stopping before lifting doesn't count as a fling */
		if (ov_tgt || ov_e) ov_release(now);          /* Release: gap springs back */
		else if (scr != SCR_TIPVIEW) {
			if (tr_v > FLING_VMIN || tr_v < -FLING_VMIN) view_fling(scr, -tr_v, now);
			else if (viewpx[scr] != top_of(scr) * ROW_H)
				view_slide_to(scr, top_of(scr) * ROW_H, now); /* On release, settle onto the nearest row */
		}
		tr_v = 0;
		if (peel_on || now - tr_t0 > TAP_US || tr_moved > TAP_MOVE) return 1;
		if (scr != SCR_GAMES && tr_y >= BUB_HIT_Y && tr_x < BUB_HIT_X) {
			*tap = -2;
			tg_tap++;
			return 0;
		}
		if (tr_x >= SRCH_X && tr_x < SRCH_X + SB_W2 && tr_y >= SRCH_Y &&
		    tr_y < SRCH_Y + SB_H2) {
			*tap = -3;
			tg_tap++;
			return 0;
		}
		if (scr == SCR_TIPVIEW) return 0;
		int n = cnt_of(scr);
		if (n <= 0 || tr_y < lt_of(scr)) return 0;
		int idx = (viewpx[scr] + tr_y - lt_of(scr)) / ROW_H;
		if (idx < n) {
			*tap = idx;
			tg_tap++;
		}
		return 0;
	}

	/* Front panel raw coords 0..1919 x 0..1087, halved proportionally to 960x544 */
	int px = (int)td.report[0].x / 2, py = (int)td.report[0].y / 2;
	if (!tr_down) {
		tr_down = 1;
		tr_x = px;
		tr_y = py;
		tr_last_y = py;
		tr_acc = tr_moved = 0;
		tr_t0 = tr_last_t = now;
		tr_v = 0;
		ov_anim = 0; /* Pressing immediately interrupts the spring-back animation and tracks the finger directly */
		sl_anim = 0; /* The settle slide also yields to the finger */
		drag_pos = viewpx[scr] - ov_e; /* Start from the current visual position, folding the gap amount in */
		ov_e = ov_tgt = 0;
		tg_n++;
		if (tg_n == 1) dlog("touch first %d,%d", px, py);
		if (bk_focus) { bk_focus = 0; return 1; }
		int in_bub = (scr != SCR_GAMES && py >= BUB_HIT_Y && px < BUB_HIT_X);
		int in_srch = (px >= SRCH_X && px < SRCH_X + SB_W2 &&
		               py >= SRCH_Y && py < SRCH_Y + SB_H2);
		if (scr != SCR_TIPVIEW && !peel_on && !in_bub && !in_srch) {   /* Whichever row is held flashes white once */
			int n = cnt_of(scr);
			if (n > 0 && py >= lt_of(scr)) {
				int idx = (viewpx[scr] + py - lt_of(scr)) / ROW_H;
				if (idx < n) { flash_idx = idx; flash_t0 = now; return 1; }
			}
		}
		return 0;
	}
	int dy = py - tr_last_y;
	tr_last_y = py;
	tr_moved += dy < 0 ? -dy : dy;
	if (!dy) return 0;
	{   /* Instantaneous finger velocity, smoothed over two samples, for release inertia */
		long long dt = now - tr_last_t;
		tr_last_t = now;
		if (dt > 0 && dt < 200000) {
			int inst = (int)(dy * 1000000LL / dt);
			tr_v = (tr_v + inst) / 2;
		}
	}
	if (peel_on) return 0;

	if (scr == SCR_TIPVIEW) {
		tr_acc += dy;
		int k = tr_acc / TV_LH;
		if (!k) return 0;
		tr_acc -= k * TV_LH;
		int old = tip_top;
		tip_top -= k; /* Finger moving down = viewing the lines above */
		if (tip_top > tv_top_max) tip_top = tv_top_max;
		if (tip_top < 0) tip_top = 0;
		if (tip_top != old) tg_step++;
		return tip_top != old;
	}

	int *tp2 = top_pp(scr);
	int vmax = view_max(scr);
	drag_pos -= dy;   /* Finger down = content down = viewport up */
	if (drag_pos < 0) {              /* Past top: viewport stops, extra pull feeds the separation */
		viewpx[scr] = 0;
		ov_tgt = ov_e = -drag_pos > OV_MAX ? OV_MAX : -drag_pos;
	} else if (drag_pos > vmax) {   /* Past bottom: bottom row anchored */
		viewpx[scr] = vmax;
		int ex = drag_pos - vmax > OV_MAX ? OV_MAX : drag_pos - vmax;
		ov_tgt = ov_e = -ex;
	} else {                        /* Within bounds: gap stays 0, no spring-back */
		viewpx[scr] = drag_pos;
		ov_tgt = ov_e = 0;
	}
	ov_anim = 0; /* While held, the gap fully tracks the finger with no time decay */
	/* The logical viewport rounds along (scrollbar/mask slots); the cursor doesn't move */
	int nt = (viewpx[scr] + ROW_H / 2) / ROW_H;
	if (nt > vmax / ROW_H) nt = vmax / ROW_H;
	if (nt != *tp2) *tp2 = nt;
	tg_step++;
	return 1;
}

int main(void) {
	diag_open();
	if (render_init() < 0) sceKernelExitProcess(1);
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
	/* The IME soft keyboard can only be tapped; the main UI doesn't consume touch, but front-panel sampling must stay on for the dialog */
	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
	init_buttons();
	SceCommonDialogConfigParam cdp = {0};
	sceCommonDialogSetConfigParam(&cdp);
	int gx = render_dialog_init();
	dlog("dialog gxm=%x", (unsigned)gx);
	infobar_set(1);
	if (load_sprites() < 0) sceKernelExitProcess(1);
	bake_hl();
	bake_buf = render_alloc0((size_t)ROW_H * RENDER_PITCH * 4);
	bake_parts();
	bake_spinner();
	bake_bands();
	peel_alloc();
	if (data_init() < 0) dlog("data init fail");
	want_screen(SCR_GAMES);
	/* vitasdk's printf doesn't know %zu; writing %zu prints "zu" verbatim */
	dlog("ready pool=%dMB used=%dMB ovf=%dKB", (int)(render_arena_total() >> 20),
	     (int)(render_arena_used() >> 20), (int)(render_arena_overflow() >> 10));

	unsigned int prev = 0;
	long long up_t = 0, down_t = 0;
	int up_rep = 0, down_rep = 0;
	int snap_scr = -1, snap_top = -1, snap_gen = -1;
	int snap_tip = -1, snap_grp = -1;
	int need = 0;     /* Redraw list only: icon arrived, loading state changed */
	int first = 1;
	/* Frame interval while the IME is up: should be ≈16.7ms after waiting for vsync; significantly larger means it can't keep up */
	long long ime_t = 0, ime_sum = 0;
	int ime_n = 0;
	/* Each round: collect background results -> read keys -> locate changes -> repaint only affected rows -> DMA to screen.
	   Networking and parsing are all in data.c's thread; the worst slowdown here is baking one row. */
	for (;;) {
		/* IME running: don't redraw a single character of the base; just lay the canvas onto the next page and overlay the dialog */
		if (ime_active) {
			tr_down = 0; /* The lift was consumed by the soft keyboard; don't treat it as one of our taps when we come back */
			int ended = pump_ime();
			if (ime_active) {
				/* Icons keep arriving, and paint_screen costs 120ms per frame — that's "lag + flicker".
				   The keyboard covers the base, so here we only drain in-flight results; the screen gets a full redraw after the dialog closes.
				   The full-page relayout can't be skipped: the dialog overlays a translucent base on that page every frame, and skipping it gets darker and darker */
				(void)data_poll();
				render_compose_begin();
				SceCommonDialogUpdateParam up;
				memset(&up, 0, sizeof(up));
				up.renderTarget.depthSurfaceData = NULL;
				up.renderTarget.colorSurfaceData = render_pending_page();
				up.renderTarget.surfaceType = SCE_GXM_COLOR_SURFACE_LINEAR;
				up.renderTarget.colorFormat = SCE_GXM_COLOR_FORMAT_A8B8G8R8;
				up.renderTarget.width = SCREEN_W;
				up.renderTarget.height = SCREEN_H;
				up.renderTarget.strideInPixels = RENDER_PITCH;
				up.displaySyncObject = render_pending_sync();
				sceCommonDialogUpdate(&up);
				render_compose_end(); /* Waits one vsync frame internally; no need to sleep here */
				long long t1 = sceKernelGetProcessTimeWide();
				if (ime_t) ime_sum += t1 - ime_t;
				ime_t = t1;
				if (++ime_n >= 120) {
					dlog("ime n=%d avg=%dus", ime_n, (int)(ime_sum / ime_n));
					ime_sum = 0;
					ime_n = 0;
				}
			} else if (ended) {
				ime_t = 0;
				first = 1; /* The dialog is gone and scribbled over the base; redo the whole screen once */
			}
			continue;
		}

		SceCtrlData pad;
		sceCtrlReadBufferPositive(0, &pad, 1);
		unsigned int hit = pad.buttons & ~prev;
		prev = pad.buttons;
		long long now = sceKernelGetProcessTimeWide();

		if (hit & SCE_CTRL_START) sceKernelExitProcess(0);

		int dc = data_poll();
		if (dc) need = 1;
		if (dc & DC_GAME) {
			/* Only one trophy group (no DLC) means no level 2 to view; drop straight to the trophy list */
			if (screen_id == SCR_GROUPS && dt_group_n <= 1) screen_id = SCR_TROPHIES;
		}
		if (dc & DC_LIST) list_want = dt_list_page; /* Continue fetching based on the page actually received */

		int scr = screen_id;
		int *cp = cur_pp(scr), *tp2 = top_pp(scr);
		int n = cnt_of(scr);

		/* Touch: dragging tracks the finger pixel by pixel; on lift, hitting the search box/back button/a row is converted into the corresponding key,
		   so the whole navigation logic below is reused as-is and touch feels identical to keys */
		int tap;
		if (touch_step(now, scr, &tap)) need = 1;
		if (tap == -3) hit |= SCE_CTRL_TRIANGLE;
		else if (tap == -2 && bub_t0 < 0) { bub_t0 = now; need = 1; } /* Touching the bubble: play the white mask first */
		else if (tap >= 0 && !peel_on) {
			*cp = tap;
			reveal(cp, tp2, scr);
			sl_anim = 0;
			viewpx[scr] = top_of(scr) * ROW_H; /* Tapping a row settles immediately; the page-turn animation takes over */
			hit |= ok_btn;
		}
		if (bub_t0 >= 0 && now - bub_t0 >= BUB_FLASH_US) {
			bub_t0 = -1;
			hit |= back_btn;   /* Only actually go back after the white mask finishes */
		}

		/* Native: only the d-pad lights the breathing teal mask; LEFT gives focus to the back bubble (teal mask),
		   pressing confirm on the focus = go back, other d-pad directions cancel the focus */
		if (!peel_on && scr != SCR_TIPVIEW) {
			if (hit & SCE_CTRL_LEFT) {
				if (scr != SCR_GAMES && !bk_focus) { bk_focus = 1; need = 1; }
			} else if (hit & (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_RIGHT)) {
				if (bk_focus) { bk_focus = 0; need = 1; }
			}
			if (bk_focus && (hit & ok_btn)) {
				hit &= ~ok_btn;
				hit |= back_btn;
				bk_focus = 0;
				need = 1;
			}
			if (hit & (SCE_CTRL_UP | SCE_CTRL_DOWN)) { glow_on = 1; glow_last = now; }
		}
		int top_before = top_of(scr);

		/* This page is still empty yet no request is in flight = the last request failed: resend every 3 seconds,
		   otherwise the user is stuck forever on an empty list or "loading" (happens when WiFi isn't connected yet at startup) */
		if (!peel_on && now >= retry_at) {
			int posted = 0;
			if (scr == SCR_GAMES && !list_ready() && !data_pending(DP_LIST)) {
				list_want = 1;
				data_request_list(1);
				posted = 1;
			} else if ((scr == SCR_GROUPS || scr == SCR_TROPHIES) && want_game &&
			           !game_ready() && !data_pending(DP_GAME)) {
				data_request_game(want_game);
				posted = 1;
			} else if ((scr == SCR_TIPS || scr == SCR_TIPVIEW) && want_trophy &&
			           !tips_ready() && !data_pending(DP_TIPS)) {
				data_request_tips(want_trophy);
				posted = 1;
			}
			if (posted) {
				retry_at = now + 3000000;
				need = 1;
			}
		}

		/* Timed auto-repeat: the first press steps once; only after holding 350ms does it accelerate to one step every 70ms */
		if (scr == SCR_TIPVIEW) { /* Level 5 scrolls body lines */
			if (!peel_on && (pad.buttons & SCE_CTRL_UP)) {
				if (hit & SCE_CTRL_UP) { if (tip_top > 0) { tip_top--; need = 1; } up_t = now; up_rep = 0; }
				else if (now - up_t >= (up_rep ? 25000 : 350000)) {
					if (tip_top > 0) { tip_top--; need = 1; }
					up_t = now;
					up_rep = 1;
				}
			} else up_rep = 0;
			if (!peel_on && (pad.buttons & SCE_CTRL_DOWN)) {
				if (hit & SCE_CTRL_DOWN) { tip_top++; need = 1; down_t = now; down_rep = 0; }
				else if (now - down_t >= (down_rep ? 25000 : 350000)) { tip_top++; need = 1; down_t = now; down_rep = 1; }
			} else down_rep = 0;
		} else {
			if (!peel_on && (pad.buttons & SCE_CTRL_UP)) {
				if (hit & SCE_CTRL_UP) { if (*cp > 0) (*cp)--; up_t = now; up_rep = 0; }
				else if (now - up_t >= (up_rep ? 70000 : 350000)) {
					if (*cp > 0) (*cp)--;
					up_t = now;
					up_rep = 1;
				}
				reveal(cp, tp2, scr);
			} else up_rep = 0;
			if (!peel_on && (pad.buttons & SCE_CTRL_DOWN)) {
				if (hit & SCE_CTRL_DOWN) { if (*cp + 1 < n) (*cp)++; down_t = now; down_rep = 0; }
				else if (now - down_t >= (down_rep ? 70000 : 350000)) {
					if (*cp + 1 < n) (*cp)++;
					down_t = now;
					down_rep = 1;
				}
				reveal(cp, tp2, scr);
			} else down_rep = 0;
		}

		/* Level-1 pulled to the bottom: auto-fetch the next page and append it (native list behavior, no more left/right paging) */
		if (scr == SCR_GAMES && dt_list_more && !list_pending() && !peel_on &&
		    curv[SCR_GAMES] + 1 >= dt_list_n) {
			list_want = dt_list_page + 1;
			data_request_list(list_want);
			need = 1; /* Scrollbar and top-right spinner reflect the in-flight state immediately */
		}

		/* △ brings up the IME search, □ cancels the search back to the latest list: pressable at any level */
		if (!peel_on && (hit & SCE_CTRL_TRIANGLE)) open_ime();
		if (!peel_on && (hit & SCE_CTRL_SQUARE)) cancel_search();

		if (!peel_on && (hit & (ok_btn | back_btn))) {
			int old = scr;
			if (hit & ok_btn) {
				if (n <= 0) want_screen(scr); /* The note above reported a failure; ○ retries */
				else if (scr == SCR_GAMES) {
					want_game = dt_list[curv[SCR_GAMES]].game_id;
					curv[SCR_GROUPS] = topv[SCR_GROUPS] = 0;
					/* If we already know this game has no DLC, go straight to level 3 and skip an empty level */
					screen_id = game_ready() && dt_group_n <= 1 ? SCR_TROPHIES : SCR_GROUPS;
				} else if (scr == SCR_GROUPS) {
					curv[SCR_TROPHIES] = topv[SCR_TROPHIES] = 0;
					screen_id = SCR_TROPHIES;
				} else if (scr == SCR_TROPHIES) {
					want_trophy = dt_trophy[trophy_sel()].trophy_id;
					curv[SCR_TIPS] = topv[SCR_TIPS] = 0;
					screen_id = SCR_TIPS;
				} else if (scr == SCR_TIPS) {
					tip_top = 0;
					screen_id = SCR_TIPVIEW;
				}
			} else if (scr == SCR_TIPVIEW) screen_id = SCR_TIPS;
			else if (scr == SCR_TIPS) screen_id = SCR_TROPHIES;
			else if (scr == SCR_TROPHIES)
				screen_id = game_ready() && dt_group_n <= 1 ? SCR_GAMES : SCR_GROUPS;
			else if (scr == SCR_GROUPS) screen_id = SCR_GAMES;
			if (screen_id != old) {
				peel_req = screen_id > old ? 1 : -1;
				want_screen(screen_id);
			}
		}

		/* D-pad/search-cancel changed the viewport: slide over; touch drag manages viewpx itself, don't interfere with the animation */
		int nt2 = top_of(scr);
		if (nt2 != top_before && !tr_down && tap < 0) view_slide_to(scr, nt2 * ROW_H, now);

		scr = screen_id;
		long long t_draw = sceKernelGetProcessTimeWide();
		if (ov_tick(now)) need = 1; /* The spring-back animation needs the list redrawn every frame */
		if (view_tick(now, scr)) need = 1;
		if (flash_idx >= 0) {       /* Touch white-flash fade-out */
			long long ft = now - flash_t0;
			if (ft >= FLASH_US) flash_idx = -1;
			else {
				flash_k = 255 - (int)(ft * 255 / FLASH_US);
				need = 1;
			}
		}
		if (bub_t0 >= 0) need = 1;  /* Bubble white-mask animation redraws every frame */
		if (glow_on && now - glow_last > GLOW_HOLD) { glow_on = 0; need = 1; }
		int top = top_of(scr);
		int want = glow_target(scr);
		int act = 0;

		if (first || scr != snap_scr) {
			ov_e = ov_tgt = 0;
			ov_anim = 0; /* Zero the split amount when switching pages; don't carry the previous page's gap over */
			glow_on = 0; /* Native shows no mask when entering a page; the d-pad lights it on press */
			bk_focus = 0;
			bub_t0 = -1;
			flash_idx = -1;
			sl_anim = 0;
			viewpx[scr] = top_of(scr) * ROW_H;
			if (!first && peel_req > 0) peel_begin(1, now);
			else if (!first && peel_req < 0) peel_begin(-1, now);
			peel_req = 0;
			need = 0;
			paint_screen(scr);
			if (scr != SCR_TIPVIEW && glow_on) glow_snap(want, scr);
			snap_scr = scr;
			snap_top = top;
			snap_gen = gen_of(scr);
			snap_grp = curv[SCR_GROUPS];
			snap_tip = curv[SCR_TIPS];
			first = 0;
			if (peel_on) { peel_capture(); peel_frame(now); } /* Frame 0: old page fills the screen */
			act = 1;
		} else if (peel_on) {
			act = peel_frame(now);
		} else if (need || gen_of(scr) != snap_gen || curv[SCR_GROUPS] != snap_grp ||
		           curv[SCR_TIPS] != snap_tip) {
			/* Data generation changed (full-screen redraw, info band follows) or an icon arrived (patch only those list rows) */
			need = 0;
			int gen_changed = gen_of(scr) != snap_gen || curv[SCR_GROUPS] != snap_grp ||
			                  curv[SCR_TIPS] != snap_tip;
			if (gen_changed) paint_screen(scr);
			else repaint_list(scr);
			if (scr != SCR_TIPVIEW && glow_on) glow_snap(want, scr);
			snap_gen = gen_of(scr);
			snap_grp = curv[SCR_GROUPS];
			snap_tip = curv[SCR_TIPS];
			snap_top = top;
			act = 1;
		} else if (top != snap_top) {   /* Scrolling: redraw only the list; the top bar and info band stay put */
			paint_list(scr);
			if (glow_on) glow_snap(want, scr);
			snap_top = top;
			act = 1;
		} else if (scr == SCR_TIPVIEW) {
			act = 0; /* The reading page has no selection mask */
		} else if (glow_on && (glow_anim || want != glow_y)) {
			if (!glow_anim) glow_start(want, now);
			act = glow_step(now, scr);
		} else if (glow_on) {
			act = pulse_tick(now, scr);   /* Even when idle, let the mask breathe */
		} else {
			act = 0;
		}

		if (spin_tick(now) && act <= 0) act = 1;

		if (act <= 0) {
			long long w = (peel_on ? peel_next : glow_next) - now;
			if (data_busy() && spin_next - now < w) w = spin_next - now;
			if (w < 2000) w = 2000;
			sceKernelDelayThread((unsigned)w);
			continue;
		}
		render_flip_now();

		/* Quantifying the feel: report draw + page-turn cost every 60 displayed frames */
		long long cost = sceKernelGetProcessTimeWide() - t_draw;
		if (cost > prof_max) prof_max = cost;
		prof_sum += cost;
		if (++prof_n >= 60) {
			dlog("draw n=%d avg=%dus max=%dus used=%dMB ovf=%dKB", prof_n,
			     (int)(prof_sum / prof_n), (int)prof_max, (int)(render_arena_used() >> 20),
			     (int)(render_arena_overflow() >> 10));
			prof_sum = 0; prof_n = 0; prof_max = 0;
		}
		/* Touch-chain on-device diagnostics: when sliding misbehaves, these three numbers tell whether it's not received, not scrolling, or not tapping */
		if (tg_n || tg_step || tg_tap) {
			dlog("touch down=%d step=%d tap=%d", tg_n, tg_step, tg_tap);
			tg_n = tg_step = tg_tap = 0;
		}
	}
	return 0;
}