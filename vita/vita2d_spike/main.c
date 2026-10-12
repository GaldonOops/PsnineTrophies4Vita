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
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/sysmodule.h>
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/libime.h>
#include <psp2/apputil.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

/* stage logger: the crash happens before any frame is visible, so mirror progress to a
   file we can pull over FTP (proven faster than decoding psp2core dumps on this project) */
static SceUID logfd = -1;
static void slog(const char *tag, int code) {
	if (logfd < 0) {
		sceIoMkdir("ux0:data", 0777);
		logfd = sceIoOpen("ux0:data/spike.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0644);
		if (logfd < 0) return;
	}
	char line[128];
	int n = snprintf(line, sizeof(line), "%s %d\n", tag, code);
	sceIoWrite(logfd, line, n);
	sceIoSync("ux0:", 0);
}

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
	unsigned stride = vita2d_texture_get_stride(t);   /* bytes per row (960-wide A8B8G8R8 reports 3840) */
	static unsigned char line[4 * 4096];              /* static: keep off the small main stack */
	for (int y = 0; y < h; y++) {
		if (sceIoRead(fd, line, (unsigned)(w * 4)) != w * 4) break;
		memcpy((unsigned char *)dst + (size_t)y * stride, line, (size_t)w * 4);
	}
	sceIoClose(fd);
	return t;
}

/* read a whole file into a dedicated memblock (NOT the heap): the 22MB font.ttf must not
   come out of the ~28MB app heap, or FreeType's internal allocations fail and crash */
static SceUID font_block = -1;
static void *read_whole_memblock(const char *path, unsigned *size) {
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0) return NULL;
	SceOff sz = sceIoLseek(fd, 0, SCE_SEEK_END);
	sceIoLseek(fd, 0, SCE_SEEK_SET);
	/* Memblock sizes must be a multiple of 64KB: the raw 22,489,228-byte TTF otherwise returns
	   SCE_KERNEL_ERROR_ILLEGAL_MEMBLOCK_SIZE (0x80024b05), which looks like an out-of-memory failure. */
	SceUID blk = -1;
	void *base = NULL;
	const size_t alloc_sz = ((size_t)sz + 0xFFFFu) & ~(size_t)0xFFFFu;
	slog("font_alloc_kb", (int)(alloc_sz >> 10));
	SceKernelMemBlockType types[2] = { SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_RW,
	                                   SCE_KERNEL_MEMBLOCK_TYPE_USER_RW };
	for (int i = 0; i < 2; i++) {
		blk = sceKernelAllocMemBlock("font", types[i], alloc_sz, NULL);
		if (blk >= 0) break;
	}
	if (blk < 0) { sceIoClose(fd); slog("font_block", (int)blk); return NULL; }
	if (sceKernelGetMemBlockBase(blk, &base) < 0) {
		sceKernelFreeMemBlock(blk);
		slog("font_base", -1);
		return NULL;
	}
	unsigned char *p = base;
	SceOff got = 0;
	while (got < sz) {                       /* IoRead can return short counts on large buffers */
		int rd = sceIoRead(fd, p + got, (unsigned)(sz - got));
		if (rd <= 0) break;
		got += rd;
	}
	sceIoClose(fd);
	if (got != sz) { sceKernelFreeMemBlock(blk); slog("font_read", (int)got); return NULL; }
	font_block = blk;
	*size = (unsigned)sz;
	return base;
}

/* IME coexistence probe (mirrors the working params in the main app) */
static SceWChar16 ime_buf[128];
static int ime_active, ime_loaded, ime_probe;

static void ime_open(void) {
	if (!ime_loaded) {
		int r = sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
		slog("ime_load", r);
		if (r < 0) return;
		ime_loaded = 1;
	}
	ime_buf[0] = 0;
	SceImeDialogParam p;
	sceImeDialogParamInit(&p);
	p.supportedLanguages = SCE_IME_LANGUAGE_SIMPLIFIED_CHINESE | SCE_IME_LANGUAGE_ENGLISH;
	p.languagesForced = SCE_FALSE;
	p.type = SCE_IME_TYPE_DEFAULT;
	p.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
	p.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
	p.title = (SceWChar16 *)u"共存验证";
	p.maxTextLength = 32;
	p.inputTextBuffer = ime_buf;
	p.enterLabel = SCE_IME_ENTER_LABEL_SEARCH;
	int r = sceImeDialogInit(&p);
	slog("ime_init", r);
	if (r < 0) return;
	ime_active = 1;
}

static void ime_poll(void) {
	if (!ime_active) return;
	SceCommonDialogStatus st = sceImeDialogGetStatus();
	if (st < 0) { slog("ime_status", st); sceImeDialogTerm(); ime_active = 0; ime_probe = 0; return; }
	if (st == SCE_COMMON_DIALOG_STATUS_FINISHED) {
		slog("ime_done", 0);
		sceImeDialogTerm();
		ime_active = 0;
		ime_probe = 0;
	}
}

/* inertial scroll state */
static float off;            /* current scroll offset in px */
static float vel;            /* px per frame */
static int dragging;
static float drag_start_off, drag_start_y, last_touch_y;

static void load_assets(void) {
	bg     = load_rgba("app0:assets/bg.rgba", W, H);      slog("bg", !!bg);
	rowbg  = load_rgba("app0:assets/rowbg.rgba", W, ROW_H); slog("rowbg", !!rowbg);
	sel    = load_rgba("app0:assets/sel.rgba", W, ROW_H);   slog("sel", !!sel);
	for (int i = 0; i < 5; i++) {
		char p[64]; snprintf(p, sizeof(p), "app0:assets/cup26_%d.rgba", i);
		cup[i] = load_rgba(p, 26, 26);
	}
	slog("cups", cup[0] && cup[4]);
	unsigned fsz = 0;
	void *fb = read_whole_memblock(fontbuf_path, &fsz);
	slog("fontbuf", fb ? (int)(fsz >> 20) : -1);
	if (fb) font = vita2d_load_font_mem(fb, fsz);
	slog("font_mem", !!font);
	if (!font) font = vita2d_load_font_file(fontbuf_path);
	slog("font", !!font);
}

static void draw_row(const Row *r, int y) {
	if (rowbg) vita2d_draw_texture(rowbg, 0, y);
	if (cup[r->rank]) vita2d_draw_texture(cup[r->rank], 250, y + 33);
	if (font) {
		vita2d_font_draw_text(font, 300, y + 40, 0xffffffff, 22, r->name);
		vita2d_font_draw_text(font, 300, y + 68, 0xff9aa0aa, 16, r->desc);
		vita2d_font_draw_text(font, 700, y + 40, 0xff6fd0a0, 16, r->rarity);
		vita2d_font_draw_text(font, 700, y + 68, 0xff4aa0ff, 16, r->tips);
	}
}

int main(void) {
	slog("start", 0);
	/* Let vita2d initialize GXM itself: its own sceGxmInitialize params carry the display-queue
	   callback that performs the actual sceDisplaySetFrameBuf flip. Initializing GXM earlier with
	   our own params steals that hook and the screen stays black while the loop keeps running. */
	int ir = vita2d_init();
	slog("vita2d_init", ir);
	slog("gxm_context", (int)(uintptr_t)vita2d_get_context());

	/* The common-dialog family (IME included) needs the app registered through apputil and its
	   config param set once, after GXM is up. Without it sceImeDialogInit/Update never get a
	   dialog process to talk to and the frame loop blocks forever. Same order as the shipped app. */
	slog("apputil_load", sceSysmoduleLoadModule(SCE_SYSMODULE_APPUTIL));
	SceAppUtilInitParam aup = {0};
	SceAppUtilBootParam abp = {0};
	slog("apputil_init", sceAppUtilInit(&aup, &abp));
	SceCommonDialogConfigParam cdp = {0};
	slog("cdlg_config", sceCommonDialogSetConfigParam(&cdp));

	SceKernelFreeMemorySizeInfo mi;
	memset(&mi, 0, sizeof(mi));
	mi.size = sizeof(mi);
	sceKernelGetFreeMemorySize(&mi);
	slog("free_cdram", mi.size_cdram >> 20);
	slog("free_user", mi.size_user >> 20);

	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
	load_assets();
	slog("assets_done", 0);

	SceCtrlData pad;
	SceTouchData touch;
	int fps_frames = 0, fps_shown = 0, logged_first = 0;
	unsigned long long fps_t0 = sceKernelGetProcessTimeWide();

	float content_h = (float)TOTAL * ROW_H;

	while (1) {
		sceCtrlPeekBufferPositive(0, &pad, 1);
		sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);

		/* Liveness probe while the dialog is up: two lines per second. If they keep growing, our
		   loop is fine and the frozen panel means the dialog never reaches the display; if they
		   stop, the hang is inside the frame and ime_swapped tells us which half. */
		if (ime_active) {
			ime_probe++;
			if ((ime_probe % 30) == 1) {
				slog("ime_frame", ime_probe);
				slog("ime_status", (int)sceImeDialogGetStatus());
				slog("ime_touch", touch.reportNum);
			}
		}

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

		if (bg) vita2d_draw_texture(bg, 0, 0);

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

		vita2d_end_drawing();
		/* Must run with NO vita2d scene open: start/end_drawing bracket a sceGxmBeginScene/
		   EndScene pair, and the dialog's own render passes nested inside ours hang the GPU a few
		   frames later (once its process actually starts drawing), not on the first call. */
		vita2d_common_dialog_update();
		if (ime_active && (ime_probe % 30) == 1) slog("ime_swapped", ime_probe);
		vita2d_swap_buffers();           /* vsync-paced */
		if (fps_frames == 0 && !logged_first) { logged_first = 1; slog("frame1", 0); }

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
