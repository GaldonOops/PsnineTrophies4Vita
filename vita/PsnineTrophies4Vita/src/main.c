// Native trophy-app-style UI: data comes from real psnine scraping (data.c background thread), assets extracted from vs0
// All layout sizes measured pixel-by-pixel from native screenshots on real hardware (row height 90, list start 128, icon slot 154x84...)
// Rendering: every vsync repaints the whole screen as GPU textured quads through gpu.c (libvita2d),
//            so scrolling, the selection mask and the page turn all animate at a steady 60fps.
#include "gpu.h"
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

#define SCREEN_W GPU_W
#define SCREEN_H GPU_H

/* Colors are always 0xAABBGGRR; on the GPU the alpha byte is a real blend weight, so C() is opaque
   and translucency has to be asked for with CA(). */
#define C(r, g, b)   (0xff000000u | ((uint32_t)(r)) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16))
#define CA(r, g, b, a) (((uint32_t)(a) << 24) | ((uint32_t)(r)) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16))


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
static int cur_of(int scr) { return curv[scr]; }
static int top_of(int scr) { return topv[scr]; }
static int *cur_pp(int scr) { return &curv[scr]; }
static int *top_pp(int scr) { return &topv[scr]; }

/* Is the level-1 list showing its last row? Either the cursor sits on it (d-pad) or the finger has dragged
   the viewport to the end -- touch deliberately leaves the cursor alone, so watching only the cursor made
   paging work with buttons and never with a scroll. */
static int list_at_end(void) {
	return curv[SCR_GAMES] + 1 >= dt_list_n || top_of(SCR_GAMES) + rows_vis(SCR_GAMES) >= dt_list_n;
}

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
/* ---------------- Sprites ----------------
   Native RCO art ships as raw .rgba and is uploaded once; the recolored and procedural pieces are
   still baked on the CPU (s_px below writes straight-alpha pixels) and uploaded as textures.
   Nothing is composited per pixel at runtime any more. */
typedef struct { uint32_t *p; int w, h; } Spr;

static vita2d_texture *T_bg;                    /* 960x544 native radial-ray background */
static vita2d_texture *T_sel, *T_rowbg, *T_legend;   /* Native RCO assets */
static vita2d_texture *T_selw, *T_bub_tl, *T_bub_w;  /* Recolored at startup: white row mask / teal + white bubble mask */
static vita2d_texture *T_field;                 /* Top-left search box base */
static vita2d_texture *T_cup26[5];               /* Native 26px trophy badges (large icons always come from PSN) */
static vita2d_texture *T_track, *T_fill, *T_box, *T_dlc;
static vita2d_texture *T_back;                  /* Bottom-left back bubble: system highlight orb recolored + livearea's ↩ glyphs */
static vita2d_texture *T_spin[8];                /* Spinner frames, positioned like the loading animation on the native PSN page */

static int spr_alloc(Spr *s, int w, int h) {
	s->p = gpu_arena_alloc0((size_t)w * h * 4);
	s->w = s->p ? w : 0;
	s->h = s->p ? h : 0;
	return s->p ? 0 : -1;
}

/* Bake a CPU-drawn sprite into a GPU texture; the staging pixels stay in the pool. */
static vita2d_texture *spr_tex(Spr *s) {
	vita2d_texture *t = gpu_tex_upload(s->p, s->w, s->h);
	if (!t) dlog("spr tex %dx%d fail", s->w, s->h);
	return t;
}

/* Bake-canvas pixel: straight alpha, the GPU does the compositing */
static void s_px(Spr *s, int x, int y, uint32_t c) {
	if (x < 0 || x >= s->w || y < 0 || y >= s->h) return;
	s->p[(size_t)y * s->w + x] = c;
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

/* Repaint a full-width band of the background. Rows sliding up must not smear onto the chrome above
   them, and repainting the band they invade does that without depending on a scissor rectangle. */
static void bg_band(int y, int h) { gpu_draw_part(0, y, T_bg, 0, y, SCREEN_W, h); }


/* ---------------- Native asset loading (.rgba under app0:assets) ----------------
   Every asset uploads into a GPU texture once; raw pixels are dropped except the two pieces a
   recolour is derived from (kept in the pool, a few hundred KB at most). */
static uint32_t *sel_px, *back_px;

static vita2d_texture *load_tex(const char *path, int w, int h) {
	vita2d_texture *t = gpu_tex_file(path, w, h);
	if (!t) dlog("tex %s fail", path);
	return t;
}

static int load_sprites(void) {
	T_bg = load_tex("app0:assets/bg.rgba", SCREEN_W, SCREEN_H);
	T_rowbg = load_tex("app0:assets/rowbg.rgba", 960, SEL_H);
	T_legend = load_tex("app0:assets/legend.rgba", 182, 66);
	static const char *c26[5] = { "app0:assets/cup26_0.rgba", "app0:assets/cup26_1.rgba",
		"app0:assets/cup26_2.rgba", "app0:assets/cup26_3.rgba", "app0:assets/cup26_4.rgba" };
	for (int k = 0; k < 5; k++) T_cup26[k] = load_tex(c26[k], 26, 26);
	if (!T_bg || !T_rowbg || !T_legend || !T_cup26[0]) return -1;

	sel_px = gpu_read_file("app0:assets/sel.rgba", (size_t)960 * SEL_H * 4);
	if (sel_px) T_sel = gpu_tex_upload(sel_px, 960, SEL_H);
	/* If the back bubble can't be fetched, fall back to not drawing it; it must not stop the whole app from starting */
	back_px = gpu_read_file("app0:assets/back.rgba", 110 * 110 * 4);
	if (back_px) T_back = gpu_tex_upload(back_px, 110, 110);
	return T_sel ? 0 : -1;
}

/* White row mask / teal bubble mask / white bubble mask: baked at startup by recolouring the native art (the ↩ glyph stays white) */
static void bake_hl(void) {
	Spr s;
	if (sel_px && spr_alloc(&s, 960, SEL_H) == 0) {
		for (int i = 0; i < 960 * SEL_H; i++) s.p[i] = (sel_px[i] & 0xff000000u) | 0x00ffffffu;
		T_selw = spr_tex(&s);
	}
	if (!back_px) return;
	int n = 110 * 110;
	if (spr_alloc(&s, 110, 110) == 0) {
		for (int i = 0; i < n; i++) s.p[i] = (back_px[i] & 0xff000000u) | 0x00ffffffu;
		T_bub_w = spr_tex(&s);
	}
	Spr tl;
	if (spr_alloc(&tl, 110, 110) < 0) return;
	for (int i = 0; i < n; i++) {
		uint32_t c = back_px[i];
		int r = c & 255, g = (c >> 8) & 255, b = (c >> 16) & 255, a = (c >> 24) & 255;
		if (r > 200 && g > 200) { tl.p[i] = c; continue; }
		int t = (r * 30 + g * 59 + b * 11) / 100;
		tl.p[i] = ((uint32_t)a << 24) |
	                ((uint32_t)(84 + (255 - 84) * t / 255) << 16) |
	                ((uint32_t)(64 + (212 - 64) * t / 255) << 8) |
	                (uint32_t)(4 + (18 - 4) * t / 255);
	}
	T_bub_tl = spr_tex(&tl);
}

/* ---------------- Startup parts ---------------- */
static void bake_parts(void) {
	/* Progress bar: native empty slot is light gray, earned portion is white */
	Spr s;
	if (spr_alloc(&s, BAR_W, BAR_H) == 0) {
		s_rrect(&s, 0, 0, BAR_W, BAR_H, BAR_H / 2, CA(205, 205, 205, 110));
		T_track = spr_tex(&s);
	}
	if (spr_alloc(&s, BAR_W, BAR_H) == 0) {
		s_rrect(&s, 0, 0, BAR_W, BAR_H, BAR_H / 2, CA(255, 255, 255, 235));
		T_fill = spr_tex(&s);
	}
	/* Count panel: native is just a rounded base + 1px bright border, no separators between the four columns */
	if (spr_alloc(&s, BOX_W, BOX_H) == 0) {
		s_rrect(&s, 0, 0, BOX_W, BOX_H, 10, CA(16, 18, 24, 120));
		s_rrect_border(&s, 0, 0, BOX_W, BOX_H, 10, CA(255, 255, 255, 40));
		T_box = spr_tex(&s);
	}
	if (spr_alloc(&s, 24, 24) == 0) {
		for (int j = -11; j <= 11; j++)
			for (int i = -11; i <= 11; i++)
				if (i * i + j * j <= 121) s_px(&s, 12 + i, 12 + j, C(96, 98, 104));
		for (int k = -7; k <= 7; k++) {
			s_px(&s, 12, 12 + k, C(245, 245, 245));
			s_px(&s, 12 + k, 12, C(245, 245, 245));
		}
		T_dlc = spr_tex(&s);
	}
	/* Top-left search box: replaces the native PS Vita tab's spot, same height as the original tab */
	if (spr_alloc(&s, SB_W2, SB_H2) == 0) {
		s_rrect(&s, 0, 0, SB_W2, SB_H2, 12, CA(18, 20, 26, 150));
		s_rrect_border(&s, 0, 0, SB_W2, SB_H2, 12, CA(190, 200, 215, 70));
		T_field = spr_tex(&s);
	}
}

/* ---------------- Row content ----------------
   Rows are drawn straight into the frame every vsync: a textured quad per sprite and one draw call
   per string is cheaper on the GPU than the old CPU bake-and-memcpy round trip, so the row-texture
   cache and the whole clean-snapshot machinery are gone. */
static void fmt_pct(int p100, char *buf, int cap) {
	snprintf(buf, cap, "%d.%02d%%", p100 / 100, p100 % 100);
}

static void row_counts(int y, const int cnt[4]) {
	gpu_draw(BOX_X, y + BOX_Y, T_box);
	for (int k = 0; k < 4; k++) {
		char d[8];
		snprintf(d, sizeof(d), "%d", cnt[k]);
		gpu_text(DIG_X + k * 50 - gpu_text_w(d, 28) / 2, y + 54, d,
		         cnt[k] ? C(245, 245, 245) : C(120, 120, 120), 28);
	}
}

/* Perfect-rate progress bar: list pages only carry this number when sorted by perfect rate; skip drawing when absent */
static void row_bar(int y, int p100) {
	if (p100 <= 0) return;
	int fw = BAR_W * p100 / 10000;
	gpu_draw(BAR_X, y + BAR_Y, T_track);
	if (fw > 0) gpu_draw_part(BAR_X, y + BAR_Y, T_fill, 0, 0, fw, BAR_H);
	char b[16];
	fmt_pct(p100, b, sizeof(b));
	gpu_text(PCT_X, y + 66, b, C(235, 235, 235), 20);
}

/* Levels 1-2: games / trophy groups */
static void draw_row_set(int y, const char *name, const char *sub, int p100,
                         const int cnt[4], int dlc) {
	gpu_text_clip(TXT_X, y + 40, name, C(245, 245, 245), 26, TXT_W);
	if (sub && sub[0]) gpu_text_clip(TXT_X, y + 66, sub, C(150, 150, 150), 16, TXT_W);
	else row_bar(y, p100);
	row_counts(y, cnt);
	if (dlc) gpu_draw(ICON_X + 6, y + 58, T_dlc);
}

/* Level 3: trophies. The 90x90 trophy image on the left is a network icon drawn separately. */
static void draw_row_trophy(int y, const PsTrophy *t) {
	gpu_draw(TXT_X, y + 14, T_cup26[cup_of(t->type)]);
	gpu_text_clip(TXT_X + 36, y + 40, t->cn[0] ? t->cn : t->name, C(245, 245, 245), 22,
	              TXT_W - 36);
	if (t->detail[0])
		gpu_text_clip(TXT_X + 36, y + 66, t->detail, C(160, 160, 160), 15, TXT_W - 36);
	char b[64];
	if (t->rarity100 > 0) {
		char pc[16];
		fmt_pct(t->rarity100, pc, sizeof(pc));
		snprintf(b, sizeof(b), "%s %s", pc, t->rarity);
	} else {
		snprintf(b, sizeof(b), "%s", t->rarity);
	}
	gpu_text_clip(INFO_X, y + 40, b, C(150, 210, 150), 16, INFO_W);
	snprintf(b, sizeof(b), "%d Tips", t->tips_count);
	gpu_text_clip(INFO_X, y + 66, b,
	              t->tips_count ? C(120, 190, 255) : C(110, 110, 110), 16, INFO_W);
}

/* Level 4: one tip's header + two-line body summary */
static void draw_row_tip(int y, const PsTip *t) {
	char head[176];
	snprintf(head, sizeof(head), "%s  ·  %s", t->author, t->date);
	gpu_text_clip(110, y + 26, head, C(150, 200, 255), 16, 800);
	gpu_text_wrap(110, y + 52, t->content, C(225, 225, 225), 18, 800, 2);
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

/* Full-width SEL_H mask (selection band / white flash) clamped to the list area: a row sliding out
   at the top must not smear its mask over the header, and the GPU has no per-row dirty bookkeeping
   to lean on here. */
static void row_mask(int lt, vita2d_texture *tex, int y, int k) {
	if (!tex || k <= 0) return;
	int ty = y < lt ? lt : y;
	int by = y + SEL_H > SCREEN_H ? SCREEN_H : y + SEL_H;
	if (by <= ty) return;
	if (k >= 255) gpu_draw_part(0, ty, tex, 0, ty - y, SCREEN_W, by - ty);
	else gpu_draw_part_a(0, ty, tex, 0, ty - y, SCREEN_W, by - ty, k);
}

/* ---------------- Overscroll separation (native row gaps when pulled past the top) ----------------
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
static int glow_y, glow_from, glow_to, glow_anim;
static int glow_k = 255;
static long long glow_t0;
static int bk_focus;
static long long bub_t0 = -1;

/* ---------------- Top bar / hints / top-right spinner ---------------- */

static void bake_spinner(void) {
	Spr s;
	for (int f = 0; f < 8; f++) {
		if (spr_alloc(&s, SPN_S, SPN_S) < 0) return;
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
				s_px(&s, x, y, CA(235, 240, 245, al));
				s_px(&s, x + 1, y, CA(235, 240, 245, al * 2 / 3));
				s_px(&s, x, y + 1, CA(235, 240, 245, al * 2 / 3));
			}
		}
		T_spin[f] = spr_tex(&s);
	}
}

/* f<0: nothing to show; the whole screen is repainted each frame, so no background restore is needed */
static int spin_shown = -1;

static void draw_spinner(int f) {
	if (f >= 0) gpu_draw(SPN_X, SPN_Y, T_spin[f]);
}

static void draw_topbar(void) {
	gpu_draw(SRCH_X, SRCH_Y, T_field);
	if (query[0]) {
		char b[128];
		snprintf(b, sizeof(b), "搜索：%s", query);
		gpu_text_clip(SRCH_X + 14, 88, b, C(235, 238, 242), 18, SB_W2 - 28);
	} else {
		gpu_text_clip(SRCH_X + 14, 88, "搜索游戏（按 △）", C(150, 155, 165), 18, SB_W2 - 28);
	}
	gpu_draw(668, 47, T_legend); /* Native texture: the four trophies include their own reflection, 66 high, centered in 32..128 */
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
		gpu_text(HINT_X, 94, buf, C(150, 150, 150), 16);
		return;
	}
	/* List pages: hint moves up one notch, source attribution on the line below */
	gpu_text(HINT_X, 68, buf, C(150, 150, 150), 16);
	gpu_text(HINT_X, 94, "数据实时查询自 psnine.com", C(110, 115, 122), 14);
}

/* ---------------- Bottom-left back button (non-level-1 pages) ----------------
   Like native, a circular bubble partly clipped by the bottom-left corner: measured from native screenshots,
   center (30,522), radius 55, with the ↩ arrow sitting slightly above the middle of the visible bubble area.
   The negative x is fine on the GPU: the quad simply extends past the left edge and the hardware clips it. */
#define BUB_X   -25
#define BUB_Y   467
#define BUB_HIT_X 92
#define BUB_HIT_Y 462

static void draw_back_btn(int scr) {
	if (scr == SCR_GAMES || !T_back) return;
	gpu_draw(BUB_X, BUB_Y, T_back);
	if (bub_t0 >= 0) {   /* Touching the bubble: white mask plays from opaque to transparent */
		long long t = sceKernelGetProcessTimeWide() - bub_t0;
		int k = 255 - (int)(t * 255 / BUB_FLASH_US);
		gpu_draw_a(BUB_X, BUB_Y, T_bub_w, k);
	} else if (bk_focus) {
		gpu_draw(BUB_X, BUB_Y, T_bub_tl);
	}
}

/* Scrollbar: track drawn in the two segments around the thumb, so the translucent layers never stack */
static void draw_scrollbar(int scr) {
	int lt = lt_of(scr), n = cnt_of(scr), vh = rows_vis(scr);
	int th = SCREEN_H - 8 - lt;
	if (n <= vh || th <= 0) return;
	int dh = th * vh / n;
	if (dh < 24) dh = 24;
	int dy = lt + (th - dh) * top_of(scr) / (n - vh);
	if (dy < lt) dy = lt;
	if (dy + dh > lt + th) dy = lt + th - dh;
	const uint32_t trk = CA(140, 140, 140, 30), hub = CA(200, 200, 200, 160), cap = CA(200, 200, 200, 70);
	gpu_rect(SB_X, lt, SB_W, dy - lt, trk);
	gpu_rect(SB_X, dy + dh, SB_W, lt + th - dy - dh, trk);
	gpu_rect(SB_X, dy, SB_W, dh, hub);
	gpu_rect(SB_X, dy, SB_W, 1, cap);
	gpu_rect(SB_X, dy + dh - 1, SB_W, 1, cap);
}

/* One-line note for empty lists: "loading" while in flight; on failure show the error code and press ○ to retry */
static void draw_empty_note(int scr) {
	char b[64];
	if (data_err() < 0) {
		snprintf(b, sizeof(b), "加载失败 %x，按 %s 重试", (unsigned)(-data_err()),
		         ok_btn == SCE_CTRL_CIRCLE ? "○" : "×");
		gpu_text(120, lt_of(scr) + 40, b, C(255, 120, 120), 20);
	} else if (data_busy()) {
		snprintf(b, sizeof(b), "正在加载…");
		gpu_text(120, lt_of(scr) + 40, b, C(150, 150, 150), 20);
	} else if (scr == SCR_TIPS) {
		gpu_text(120, lt_of(scr) + 40, "这条奖杯还没有人写过 Tips", C(150, 150, 150), 20);
	}
}

/* ---------------- List drawing ----------------
   Covers and trophy balls are PSN images cached as textures by gpu.c. gpu_icon has to be called on
   every pass even while an icon is still missing: the download registration and retries live inside it. */
static void draw_icon_at(int x, int y, int kind, const char *url, int show_ph) {
	int w, h;
	vita2d_texture *t = gpu_icon(kind, url, &w, &h);
	if (!t) return;
	/* Every icon slot sits at x>=ICON_X, so a smaller x is a bug worth a log line: one appeared
	   over the search box in a device screenshot and no call site can account for it. */
	if (x < 60) dlog("icon@%d,%d %dx%d k=%d %.48s", x, y, w, h, kind, url);
	int st = data_icon_state(kind, url);
	if (st == DI_READY) {
		gpu_draw(x, y, t);
		return;
	}
	/* Trophy balls only ever show PSN's art: no gray placeholder box */
	if (show_ph) gpu_draw(x, y, t);
	/* Still in flight: spin the same loader in the middle of the slot, so an empty icon reads as
	   "downloading" rather than "this game has no art". A failure keeps the static placeholder. */
	if (st != DI_FAIL && spin_shown >= 0 && w >= SPN_S && h >= SPN_S)
		gpu_draw(x + (w - SPN_S) / 2, y + (h - SPN_S) / 2, T_spin[spin_shown]);
}

static void row_icon(int scr, int item, int y) {
	if (scr == SCR_GAMES) draw_icon_at(ICON_X, y + 3, IC_COVER, dt_list[item].icon, 1);
	else if (scr == SCR_GROUPS) draw_icon_at(ICON_X, y + 3, IC_COVER, dt_head.icon, 1);
	else if (scr == SCR_TROPHIES) draw_icon_at(ICON_X + 32, y, IC_CUP, dt_trophy[item].icon, 0);
}

static void paint_list(int scr) {
	int top = top_of(scr), n = cnt_of(scr), lt = lt_of(scr), vh = rows_draw(scr);
	int fr = view_fr(scr);
	/* One pass per visible row: the native band, then its content, then the network icon.
	   Sliding and the overscroll gap are just row positions, and anything that leaves the list area
	   gets painted over by the chrome bands afterwards. */
	for (int i = -1; i <= vh; i++) {
		int idx = top + i;
		if (idx < 0 || idx >= n) continue;
		int y = lt + i * ROW_H - fr + sep_of(i, vh);
		if (y + ROW_H <= lt || y >= SCREEN_H) continue;
		int item = item_of(scr, idx);
		gpu_draw(0, y - 1, T_rowbg);
		draw_row(scr, item, y);
		row_icon(scr, item, y);
	}
	if (!n) draw_empty_note(scr);
	if (flash_idx >= 0 && flash_k > 0)   /* Flash the touch-start row white once */
		row_mask(lt, T_selw, lt + (flash_idx - top) * ROW_H - fr + sep_of(flash_idx - top, vh) - 1,
		         flash_k);
	if (glow_on && n > 0) row_mask(lt, T_sel, glow_y, glow_k);
}

/* ---------------- Selection mask easing + breathing ----------------
   With the whole frame repainted each vsync the mask is simply one more overlay: no snapshot to
   restore and no accumulation to undo, so this section only tracks a position and an intensity. */
#define GLOW_US   140000LL
#define GLOW_CUBE (GLOW_US * GLOW_US * GLOW_US)
#define PULSE_US  1250000LL    /* One full cycle of mask depth breathing */
#define PULSE_MID 195          /* Center intensity (255 = texture as-is) */
#define PULSE_AMP 60           /* Swing: 135~255, the teal light brightening and dimming */

/* The native mask is a blue-green light band whose brightness periodically deepens and lightens */
static int pulse_k(long long now) {
	float ph = (float)(now % PULSE_US) * (6.2831853f / (float)PULSE_US);
	return PULSE_MID + (int)((float)PULSE_AMP * sinf(ph));
}

static int glow_target(int scr) {
	return lt_of(scr) + (cur_of(scr) - top_of(scr)) * ROW_H - 1 - view_fr(scr) +
	       sep_of(cur_of(scr) - top_of(scr), rows_draw(scr));
}

/* Jump onto the row without animating: used when the mask first lights up, so it never slides in from a stale position */
static void glow_snap(int y) {
	glow_y = y;
	glow_anim = 0;
}

static void glow_tick(long long now, int scr) {
	glow_k = pulse_k(now);
	int want = glow_target(scr);
	if (!glow_anim && want != glow_y) {
		glow_from = glow_y;
		glow_to = want;
		glow_t0 = now;
		glow_anim = 1;
	}
	if (!glow_anim) return;
	long long e = now - glow_t0;
	if (e >= GLOW_US) {
		glow_y = glow_to;
		glow_anim = 0;
	} else {
		long long d = (long long)glow_to - glow_from;
		long long rem = GLOW_US - e;
		glow_y = glow_to - (int)(d * rem * rem * rem / GLOW_CUBE); /* Cubic ease-out */
	}
}

/* ---------------- Page turn: the old page flips outward around a vertical edge, narrowing in perspective ----------------
   When native enters level 2/3, the previous page is flipped away like a sheet of paper: the right edge (left edge when going back)
   translates all the way to the opposite side while the page is squashed near-large-far-small, flipping until invisible as the new page is revealed underneath.
   GPU approach: the outgoing page is painted once per frame into a render target, then sampled as vertical strips -- each strip a
   textured quad whose height is scaled by the perspective factor. Two full page paints per turn frame, at 30fps for 420ms. */
#define PEEL_US     420000LL
#define PEEL_STRIP  8          /* Destination columns sampled together; a strip is one quad */
#define PEEL_Y0     LT_TOP     /* The top bar doesn't participate in the page turn */
#define PEEL_CY     320        /* Perspective centerline; the page's top and bottom edges converge toward it */
#define PEEL_TURN   1.15f      /* Max rotation angle (radians, about 66 degrees) */
#define PEEL_DEPTH  2.6f       /* Strength of the near-large-far-small effect */

static vita2d_texture *T_peel;
static int peel_on, peel_dir, peel_scr;
static long long peel_t0;

static void peel_init(void) {
	T_peel = gpu_tex_rt(SCREEN_W, SCREEN_H);
	if (!T_peel) dlog("peel rt fail: page turn disabled"); /* degrade to a plain cut, never block the app */
}

/* Called when the screen changes: dir +1 forward, -1 back; scr is the page being left */
static void peel_begin(int dir, int scr, long long now) {
	if (!T_peel) return;
	peel_scr = scr;
	peel_dir = dir;
	peel_t0 = now;
	peel_on = 1;
}

/* Drawn over the fresh new page: the flipping sheet as a row of narrowing strips */
static void peel_strips(long long now) {
	float t = (float)(now - peel_t0) / (float)PEEL_US;
	if (t >= 1.f) { peel_on = 0; return; }
	float e = t * t * (3.f - 2.f * t); /* smoothstep: slow at both ends, fast in the middle */
	float cs = cosf(e * PEEL_TURN);
	float D = PEEL_DEPTH * e;
	int hinge = (int)(SCREEN_W * (1.f - e) + 0.5f); /* Hinge; moves right to left in mirrored space */
	int shade = 255 - (int)(60.f * e);              /* Darken slightly while flipping away */
	for (int xa = hinge; xa >= 0; xa -= PEEL_STRIP) {
		float v = (float)(hinge - xa) / (960.f * cs); /* v = u/(1+D*u) */
		float den = 1.f - D * v;
		if (den < 0.02f) break;
		float u = v / den;
		if (u > 1.f) break;                        /* Past the paper edge; nothing left on the left */
		float S = 1.f / (1.f + D * u);             /* Vertical scale of this strip */
		int sw = xa + 1 < PEEL_STRIP ? xa + 1 : PEEL_STRIP;
		int scol = (int)(u * 959.f + 0.5f);
		int src = peel_dir > 0 ? SCREEN_W - 1 - (scol + sw - 1) : scol;
		int destX = peel_dir > 0 ? xa - sw + 1 : SCREEN_W - 1 - xa;
		/* Both edges shrink toward PEEL_CY, so a strip can never leave the page area: no clipping needed */
		int y0 = PEEL_CY + (int)((PEEL_Y0 - PEEL_CY) * S);
		gpu_draw_part_shade_scale(T_peel, destX, y0, src, PEEL_Y0, sw, SCREEN_H - PEEL_Y0, S, shade);
	}
}

/* ---------------- Info band (from level 2 on, the strip above the list) ---------------- */
static void draw_header(int scr) {
	int y = LT_TOP;
	gpu_draw(0, y - 1, T_rowbg);
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
		draw_icon_at(ICON_X, y + 3, IC_COVER, dt_head.icon, 1);
	} else {
		const PsTrophy *t = &dt_trophy[trophy_sel()];
		draw_icon_at(ICON_X + 32, y, IC_CUP, t->icon, 0);
		gpu_text_clip(TXT_X, y + 40, t->cn[0] ? t->cn : t->name, C(245, 245, 245), 24, TXT_W);
		if (scr == SCR_TIPVIEW && tips_ready() && curv[SCR_TIPS] < dt_tip_n) {
			const PsTip *p = &dt_tip[curv[SCR_TIPS]];
			char hb[176];
			snprintf(hb, sizeof(hb), "%s · %s", p->author, p->date);
			gpu_text_clip(TXT_X, y + 66, hb, C(150, 200, 255), 16, TXT_W);
		} else {
			gpu_text_clip(TXT_X, y + 66, t->detail, C(150, 150, 150), 15, TXT_W);
		}
	}
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
	if (!tips_ready() || !dt_tip_n) {
		if (data_busy()) gpu_text(TV_X, y0 + 20, "正在加载 Tips…", C(150, 150, 150), 20);
		else gpu_text(TV_X, y0 + 20, "这条奖杯还没有 Tips", C(150, 150, 150), 20);
		return;
	}
	const PsTip *p = &dt_tip[curv[SCR_TIPS] < dt_tip_n ? curv[SCR_TIPS] : 0];
	/* First pass only counts the laid-out lines (all advance lookups are cache hits), then the visible window is drawn */
	int total = 0;
	gpu_text_scroll(TV_X, y0, p->content, C(225, 225, 225), TV_PX, TV_W, 0, 0, &total);
	tv_total = total;
	tv_top_max = total > vis ? total - vis : 0;
	if (tip_top > tv_top_max) tip_top = tv_top_max;
	if (tip_top < 0) tip_top = 0;
	gpu_text_scroll(TV_X, y0, p->content, C(225, 225, 225), TV_PX, TV_W, vis, tip_top, NULL);
	char b[40];
	snprintf(b, sizeof(b), "%d / %d 行", tip_top + 1, total);
	gpu_text(SCREEN_W - 210, SCREEN_H - 12, b, C(140, 140, 140), 14);
	if (tip_top < tv_top_max) gpu_text(SCREEN_W - 90, SCREEN_H - 14, "下 ▾", C(140, 140, 140), 16);
}

/* ---------------- Full screen ----------------
   Drawing order is the compositing order. Chrome comes last on purpose: a row that slides past the
   top of the list area is erased by repainting the background band under the top bar and info band,
   so nothing depends on a scissor rectangle behaving as expected. */
static void paint_screen(int scr) {
	gpu_draw(0, 0, T_bg);
	if (scr == SCR_TIPVIEW) paint_tipview();
	else paint_list(scr);
	int lt = lt_of(scr);
	bg_band(0, lt);
	draw_topbar();
	draw_hint(scr);
	draw_spinner(spin_shown);
	if (scr != SCR_GAMES) draw_header(scr);
	draw_scrollbar(scr);
	draw_back_btn(scr);
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
	int st = (int)sceImeDialogGetStatus(); /* the enum's underlying type is unsigned: keep the error bit visible */
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

static long long prof_sum, prof_max;
static int prof_n;

/* Top-right spinner: 8fps is enough; paint_screen just draws the current frame index */
static void spin_tick(long long now) {
	if (now < spin_next) return;
	spin_next = now + 125000;
	if (data_busy()) spin_shown = (spin_shown + 1) & 7;
	else spin_shown = -1;
}

/* ---------------- Touch ----------------
   The list tracks the finger one pixel at a time: dragging moves the pixel viewport (viewpx) directly and the
   cursor follows the rounded viewport, so a half-row offset is normal and needs no special case now that the
   GPU composites each row at its own position.
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
	/* vita2d must bring GXM up by itself: its sceGxmInitialize registers the display-queue callback
	   that performs the actual sceDisplaySetFrameBuf flip. Initialising GXM first with our own params
	   steals that hook and the screen stays black while the loop keeps running. */
	if (gpu_init() < 0) sceKernelExitProcess(1);
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
	/* The IME soft keyboard is touch-only and so is list dragging: the front panel must be sampled either way */
	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
	init_buttons(); /* apputil: the common-dialog family needs the app registered before any dialog */
	SceCommonDialogConfigParam cdp = {0};
	sceCommonDialogSetConfigParam(&cdp);
	infobar_set(1);
	if (load_sprites() < 0) sceKernelExitProcess(1);
	bake_hl();
	bake_parts();
	bake_spinner();
	peel_init();
	if (data_init() < 0) dlog("data init fail");
	want_screen(SCR_GAMES);
	/* vitasdk's printf doesn't know %zu; writing %zu prints "zu" verbatim */
	dlog("ready pool=%dMB used=%dMB ovf=%dKB", (int)(gpu_arena_total() >> 20),
	     (int)(gpu_arena_used() >> 20), (int)(gpu_arena_overflow() >> 10));

	unsigned int prev = 0;
	long long up_t = 0, down_t = 0;
	int up_rep = 0, down_rep = 0;
	int snap_scr = -1;
	int first = 1, pad_resync = 0;
	/* Frame period while the IME is up: should stay near 16.7ms; well above that means the dialog is costing us frames */
	long long ime_t = 0, ime_sum = 0;
	int ime_n = 0;

	/* One iteration = one displayed frame: collect background results -> read input -> advance the
	   animations -> repaint the whole screen -> vsync-paced swap. Nothing sleeps here any more,
	   swap_buffers is what sets the tempo, and there is no dirty-row bookkeeping left to keep in sync. */
	for (;;) {
		int dc = data_poll();
		if (dc & DC_GAME) {
			/* Only one trophy group (no DLC) means there is no level 2 to look at: drop straight to the trophy list */
			if (screen_id == SCR_GROUPS && dt_group_n <= 1) screen_id = SCR_TROPHIES;
		}
		if (dc & DC_LIST) list_want = dt_list_page; /* continue fetching from the page actually received */

		long long now = sceKernelGetProcessTimeWide();
		int scr = screen_id;
		int *cp = cur_pp(scr), *tp2 = top_pp(scr);
		int n = cnt_of(scr);
		unsigned int hit = 0;
		int tap = -1;

		/* The dialog owns the screen and the input while it is open, but the base page keeps being
		   repainted underneath: the shell composites it against the keyboard every swap. */
		if (ime_active) {
			tr_down = 0; /* the lift happened on the soft keyboard, not on our list */
			if (pump_ime()) pad_resync = 1;
		} else {
			SceCtrlData pad;
			sceCtrlReadBufferPositive(0, &pad, 1);
			hit = pad.buttons & ~prev;
			prev = pad.buttons;
			if (pad_resync) { hit = 0; pad_resync = 0; } /* don't act on the key the dialog just consumed */

			if (hit & SCE_CTRL_START) sceKernelExitProcess(0);

			/* Touch: dragging tracks the finger pixel by pixel; on lift, hitting the search box / back bubble /
			   a row turns into the matching key, so all the navigation below is reused as-is and touch feels like keys */
			touch_step(now, scr, &tap);
			if (tap == -3) hit |= SCE_CTRL_TRIANGLE;
			else if (tap == -2 && bub_t0 < 0) bub_t0 = now; /* touching the bubble plays the white mask first */
			else if (tap >= 0 && !peel_on) {
				*cp = tap;
				reveal(cp, tp2, scr);
				sl_anim = 0;
				viewpx[scr] = top_of(scr) * ROW_H; /* tapping a row settles at once; the page turn takes over */
				hit |= ok_btn;
			}
			if (bub_t0 >= 0 && now - bub_t0 >= BUB_FLASH_US) {
				bub_t0 = -1;
				hit |= back_btn; /* only really go back after the white mask has finished */
			}

			/* Native: only the d-pad lights the breathing teal mask; LEFT hands focus to the back bubble (teal mask),
			   confirming on that focus goes back, the other d-pad directions drop the focus */
			if (!peel_on && scr != SCR_TIPVIEW) {
				if (hit & SCE_CTRL_LEFT) {
					if (scr != SCR_GAMES && !bk_focus) bk_focus = 1;
				} else if (hit & (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_RIGHT)) {
					bk_focus = 0;
				}
				if (bk_focus && (hit & ok_btn)) {
					hit &= ~ok_btn;
					hit |= back_btn;
					bk_focus = 0;
				}
				if (hit & (SCE_CTRL_UP | SCE_CTRL_DOWN)) {
					if (!glow_on) glow_snap(glow_target(scr)); /* light up on its own row, never slide in from a stale one */
					glow_on = 1;
					glow_last = now;
				}
			}
			int top_before = top_of(scr);

			/* This page is still empty and no request is in flight = the last one failed: resend every 3 seconds,
			   otherwise the user is stuck forever on an empty list or "loading" (happens when WiFi comes up after start) */
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
				if (posted) retry_at = now + 3000000;
			}

			/* Timed auto-repeat: the first press steps once; only after 350ms of holding does it speed up to one step every 70ms */
			if (scr == SCR_TIPVIEW) { /* level 5 scrolls body lines */
				if (!peel_on && (pad.buttons & SCE_CTRL_UP)) {
					if (hit & SCE_CTRL_UP) { if (tip_top > 0) tip_top--; up_t = now; up_rep = 0; }
					else if (now - up_t >= (up_rep ? 25000 : 350000)) {
						if (tip_top > 0) tip_top--;
						up_t = now;
						up_rep = 1;
					}
				} else up_rep = 0;
				if (!peel_on && (pad.buttons & SCE_CTRL_DOWN)) {
					if (hit & SCE_CTRL_DOWN) { tip_top++; down_t = now; down_rep = 0; }
					else if (now - down_t >= (down_rep ? 25000 : 350000)) { tip_top++; down_t = now; down_rep = 1; }
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

			/* Level 1 pulled to the bottom: fetch the next page and append it (native list behaviour, no left/right paging) */
			if (scr == SCR_GAMES && dt_list_more && !list_pending() && !peel_on && list_at_end()) {
				list_want = dt_list_page + 1;
				data_request_list(list_want);
			}

			/* △ opens the IME search, □ cancels it back to the latest list: pressable on every level */
			if (!peel_on && (hit & SCE_CTRL_TRIANGLE)) open_ime();
			if (!peel_on && (hit & SCE_CTRL_SQUARE)) cancel_search();

			if (!peel_on && (hit & (ok_btn | back_btn))) {
				int old = scr;
				if (hit & ok_btn) {
					if (n <= 0) want_screen(scr); /* the empty-page note reports a failure; ○ retries */
					else if (scr == SCR_GAMES) {
						want_game = dt_list[curv[SCR_GAMES]].game_id;
						curv[SCR_GROUPS] = topv[SCR_GROUPS] = 0;
						/* If we already know this game has no DLC, skip the empty level and go to level 3 */
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

			/* D-pad / search-cancel moved the viewport: slide it over. Touch drag owns viewpx itself, don't fight it. */
			int nt2 = top_of(scr);
			if (nt2 != top_before && !tr_down && tap < 0) view_slide_to(scr, nt2 * ROW_H, now);
		}

		scr = screen_id;

		ov_tick(now);              /* the overscroll gap springs back */
		view_tick(now, scr);       /* settle / fling slides */
		if (flash_idx >= 0) {      /* touch white-flash fade-out */
			long long ft = now - flash_t0;
			if (ft >= FLASH_US) flash_idx = -1;
			else flash_k = 255 - (int)(ft * 255 / FLASH_US);
		}
		if (glow_on && now - glow_last > GLOW_HOLD) glow_on = 0;
		if (glow_on) glow_tick(now, scr); /* slide to the next row, then keep breathing */
		spin_tick(now);

		if (first || scr != snap_scr) { /* page entry: a fresh viewport, nothing carried over from the page before */
			ov_e = ov_tgt = 0;
			ov_anim = 0;   /* zero the split when switching pages */
			glow_on = 0;   /* native shows no mask on entry; the d-pad lights it on press */
			bk_focus = 0;
			bub_t0 = -1;
			flash_idx = -1;
			sl_anim = 0;
			viewpx[scr] = top_of(scr) * ROW_H;
			if (!first && peel_req) peel_begin(peel_req > 0 ? 1 : -1, snap_scr, now);
			peel_req = 0;
			snap_scr = scr;
			first = 0;
		}

		/* The page being flipped away is repainted once per turn frame into the render target,
		   then sampled as vertical strips drawn over the new page. */
		long long t_draw = sceKernelGetProcessTimeWide();
		if (peel_on) {
			gpu_rt_begin(T_peel);
			paint_screen(peel_scr);
			gpu_rt_end();
		}
		gpu_frame_begin();
		paint_screen(scr);
		if (peel_on) peel_strips(now);
		/* Measured before the swap so the vsync wait doesn't pollute the number */
		long long cost = sceKernelGetProcessTimeWide() - t_draw;
		gpu_frame_end(); /* end_drawing -> common_dialog_update -> swap_buffers, in that order */

		if (ime_active) {
			long long t1 = sceKernelGetProcessTimeWide();
			if (ime_t) ime_sum += t1 - ime_t;
			ime_t = t1;
			if (++ime_n >= 120) {
				dlog("ime n=%d avg=%dus", ime_n, (int)(ime_sum / ime_n));
				ime_sum = 0;
				ime_n = 0;
			}
		} else {
			ime_t = 0;
			ime_n = 0;
		}

		/* Quantifying the feel: report the paint cost every 60 displayed frames */
		if (cost > prof_max) prof_max = cost;
		prof_sum += cost;
		if (++prof_n >= 60) {
			dlog("draw n=%d avg=%dus max=%dus used=%dMB ovf=%dKB", prof_n,
			     (int)(prof_sum / prof_n), (int)prof_max, (int)(gpu_arena_used() >> 20),
			     (int)(gpu_arena_overflow() >> 10));
			prof_sum = 0; prof_n = 0; prof_max = 0;
		}
		/* Touch-chain on-device diagnostics: when a slide misbehaves these three numbers say whether the
		   touch was never received, scrolled nothing, or never landed a tap */
		if (tg_n || tg_step || tg_tap) {
			dlog("touch down=%d step=%d tap=%d", tg_n, tg_step, tg_tap);
			tg_n = tg_step = tg_tap = 0;
		}
	}
	return 0;
}
