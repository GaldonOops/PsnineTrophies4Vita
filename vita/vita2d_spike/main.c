/* Phase-0 feasibility spike: prove libvita2d GPU rendering is smooth on real
   hardware using this project's actual assets (bg.rgba / rowbg.rgba / sel.rgba /
   cup26_*.rgba / font.ttf). It renders a scrolling trophy-style list with inertial
   touch scroll, an on-screen FPS counter, and opens the IME on Start to prove
   vita2d + common-dialog coexistence. This is throwaway validation code, not the
   shipped UI. */
#include <vita2d.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/libime.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define W 960
#define H 544
#define ROW_H 92
#define LT_TOP 132                 /* first row's top y; list viewport is [LT_TOP, H) */
#define LT_H (H - LT_TOP)

/* one mock row, shaped like a real trophy entry */
typedef struct {
	int rank;                      /* 0 plat 1 gold 2 silver 3 bronze */
	const char *name;
	const char *desc;
	const char *rarity;
	const char *tips;
} Row;

static const Row ROWS[] = {
	{ 0, "利贝尔的英雄",   "获得了所有奖杯。",             "2.80% 极为珍贵", "14 Tips" },
	{ 1, "新手游击士",     "完成了在巴鲁斯塔尔水道的训练。", "63.30% 一般",   "2 Tips"  },
	{ 2, "无敌的黄金搭档", "达成了所有奖励。",             "4.50% 极为珍贵", "2 Tips"  },
	{ 3, "A 级游击士",     "游击士等级达到了 A 级。",       "16.90% 珍贵",   "4 Tips"  },
	{ 0, "空之轨迹的羁绊", "与所有同伴建立羁绊。",         "3.10% 极为珍贵", "6 Tips"  },
	{ 1, "结晶桥大师",     "击败了结晶桥的敌人。",         "28.4% 一般",    "1 Tips"  },
	{ 2, "狩猎达人",       "讨伐了 100 只敌人。",          "9.70% 珍贵",    "3 Tips"  },
	{ 3, "最初的胜利",     "赢得了第一场战斗。",           "71.2% 一般",    "0 Tips"  },
};
#define NROWS (int)(sizeof(ROWS) / sizeof(ROWS[0]))
/* we scroll a virtual list that repeats NROWS many times to have room to fling */
#define TOTAL (NROWS * 8)

static vita2d_texture *bg, *rowbg, *sel, *cup[5];
static vita2d_font *font;
static char fontbuf_path[] = "app0:font.ttf";

/* load a raw RGBA8888 file (memory order R,G,B,A == SCE_GXM A8B8G8R8) into a GPU texture */
static vita2d_texture *load_rgba(const char *path, int w, int h) {
	vita2d_texture *t = vita2d_create_empty_texture_format(w, h, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
	if (!t) return NULL;
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0) { vita2d_free_texture(t); return NULL; }
	void *dst = vita2d_texture_get_datap(t);
	unsigned stride = vita2d_texture_get_stride(t);   /* pixels per row in the texture */
	for (int y = 0; y < h; y++) {
		unsigned char line[4 * 4096];
		if (sceIoRead(fd, line, (unsigned)(w * 4)) != w * 4) break;
		memcpy((unsigned char *)dst + (size_t)y * stride * 4, line, (size_t)w * 4);
	}
	sceIoClose(fd);
	return t;
}

static void *read_whole(const char *path, unsigned *size) {
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0) return NULL;
	SceOff sz = sceIoLseek(fd, 0, SCE_SEEK_END);
	sceIoLseek(fd, 0, SCE_SEEK_SET);
	void *buf = malloc((size_t)sz);
	if (!buf || sceIoRead(fd, buf, (unsigned)sz) != sz) { sceIoClose(fd); free(buf); return NULL; }
	sceIoClose(fd);
	*size = (unsigned)sz;
	return buf;
}

/* IME coexistence probe (mirrors the working params in the main app) */
static SceWChar16 ime_buf[128];
static int ime_active, ime_loaded;

static void ime_open(void) {
	if (!ime_loaded) {
		if (sceSysmoduleLoadModule(SCE_SYSMODULE_IME) < 0) return;
		ime_loaded = 1;
	}
	ime_buf[0] = 0;
	SceImeDialogParam p;
	sceImeDialogParamInit(&p);
	p.supportedLanguages = SCE_IME_LANGUAGE_SIMPLIFIED_CHINESE | SCE_IME_LANGUAGE_ENGLISH;
	p.languagesForced = SCE_FALSE;
	p.type = SCE_IME_TYPE_DEFAULT;
	p.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
	p.title = (SceWChar16 *)u"共存验证";
	p.maxTextLength = 32;
	p.inputTextBuffer = ime_buf;
	p.enterLabel = SCE_IME_ENTER_LABEL_SEARCH;
	if (sceImeDialogInit(&p) < 0) return;
	ime_active = 1;
}

static void ime_poll(void) {
	if (!ime_active) return;
	SceCommonDialogStatus st = sceImeDialogGetStatus();
	if (st == SCE_COMMON_DIALOG_STATUS_FINISHED) {
		sceImeDialogTerm();
		ime_active = 0;
	}
}

/* inertial scroll state */
static float off;            /* current scroll offset in px */
static float vel;            /* px per frame */
static int dragging;
static float drag_start_off, drag_start_y, last_touch_y;

static void load_assets(void) {
	bg     = load_rgba("app0:assets/bg.rgba", W, H);
	rowbg  = load_rgba("app0:assets/rowbg.rgba", W, ROW_H);
	sel    = load_rgba("app0:assets/sel.rgba", W, ROW_H);
	for (int i = 0; i < 5; i++) {
		char p[64]; snprintf(p, sizeof(p), "app0:assets/cup26_%d.rgba", i);
		cup[i] = load_rgba(p, 26, 26);
	}
	unsigned fsz = 0;
	void *fb = read_whole(fontbuf_path, &fsz);
	if (fb) { font = vita2d_load_font_mem(fb, fsz); /* font keeps a copy; leak ok for spike */ }
}

static void draw_row(const Row *r, int y) {
	vita2d_draw_texture(rowbg, 0, y);
	if (cup[r->rank]) vita2d_draw_texture(cup[r->rank], 250, y + 33);
	if (font) {
		vita2d_font_draw_text(font, 300, y + 40, 0xffffffff, 22, r->name);
		vita2d_font_draw_text(font, 300, y + 68, 0xff9aa0aa, 16, r->desc);
		vita2d_font_draw_text(font, 700, y + 40, 0xff6fd0a0, 16, r->rarity);
		vita2d_font_draw_text(font, 700, y + 68, 0xff4aa0ff, 16, r->tips);
	}
}

int main(void) {
	vita2d_init();
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
	load_assets();

	SceCtrlData pad;
	SceTouchData touch;
	int fps_frames = 0, fps_shown = 0;
	unsigned long long fps_t0 = sceKernelGetProcessTimeWide();

	float content_h = (float)TOTAL * ROW_H;

	while (1) {
		sceCtrlPeekBufferPositive(0, &pad, 1);
		sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);

		/* --- input: drag + fling --- */
		if (touch.reportNum > 0) {
			float tx = touch.report[0].x / 2.0f;   /* raw is 2x the 960x544 space */
			float ty = touch.report[0].y / 2.0f;
			if (!dragging && tx > 40) {           /* ignore left edge so back tap is free */
				dragging = 1; vel = 0;
				drag_start_off = off; drag_start_y = last_touch_y = ty;
			}
			if (dragging) {
				float dy = (ty - drag_start_y) / LT_H * content_h * 0.18f;
				off = drag_start_off - dy;
				vel = (last_touch_y - ty) * 0.9f;   /* px/frame proxy from finger delta */
				last_touch_y = ty;
			}
		} else if (dragging) {
			dragging = 0;                          /* release keeps vel for fling */
		}
		if (pad.buttons & SCE_CTRL_START && !ime_active) ime_open();
		ime_poll();

		/* --- physics: friction + clamp with soft overscroll --- */
		if (!dragging) {
			off += vel;
			vel *= 0.94f;
			if (vel > 0 && off < 0)         { off += (0 - off) * 0.2f;         vel *= 0.5f; }
			if (vel < 0 && off > content_h - LT_H) { off += (content_h - LT_H - off) * 0.2f; vel *= 0.5f; }
		}
		if (off < -40) off = -40;
		if (off > content_h - LT_H + 40) off = content_h - LT_H + 40;

		/* --- render --- */
		vita2d_start_drawing();
		vita2d_clear_screen();

		vita2d_draw_texture(bg, 0, 0);

		/* list viewport clip so rows never bleed onto the top bar */
		vita2d_enable_clipping();
		vita2d_set_clip_rectangle(0, LT_TOP, W, H - 1);

		int first = (int)(off / ROW_H);
		if (first < 0) first = 0;
		int vis = LT_H / ROW_H + 2;
		for (int i = first; i < first + vis && i < TOTAL; i++) {
			int y = (int)(i * ROW_H - off);
			if (y + ROW_H < LT_TOP || y > H) continue;
			const Row *r = &ROWS[i % NROWS];
			int focused = (i == first + 1);
			if (focused && sel) vita2d_draw_texture(sel, 0, y);
			draw_row(r, y);
		}
		vita2d_disable_clipping();

		/* top bar text + FPS */
		if (font) {
			vita2d_font_draw_text(font, 20, 70, 0xffffffff, 20, "搜索：轨迹");
			vita2d_font_draw_text(font, 20, 100, 0xff8890a0, 14, "vita2d GPU spike — 拖动滚动 / Start 开输入法验证共存");
			char f[32]; snprintf(f, sizeof(f), "%d FPS", fps_shown);
			vita2d_font_draw_text(font, 860, 40, fps_shown >= 55 ? 0xff60ff60 : 0xff60ffff, 20, f);
		}

		vita2d_common_dialog_update();   /* renders the IME on top, same GXM context */
		vita2d_end_drawing();
		vita2d_swap_buffers();           /* vsync-paced */

		/* fps accounting */
		fps_frames++;
		unsigned long long now = sceKernelGetProcessTimeWide();
		if (now - fps_t0 >= 1000000ULL) { fps_shown = fps_frames; fps_frames = 0; fps_t0 = now; }

		if (pad.buttons & SCE_CTRL_SELECT) break;
	}

	vita2d_fini();
	sceKernelExitProcess(0);
	return 0;
}
