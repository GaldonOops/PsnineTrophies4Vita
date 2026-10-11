// Rendering base: framebuffer + freetype text, extracted from the M0 demo with baseline conversion fixed
#include "render.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <psp2/display.h>
#include <psp2/gxm.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/sysmem.h>

#include <stdlib.h>
#include <string.h>

#define FB_PITCH RENDER_PITCH /* pitch in pixels matching the GXM sample; common dialog renders at this pitch */
#define FB_SIZE (FB_PITCH * SCREEN_H * 4)

static uint32_t *fb_pages[2]; // double-buffered VRAM (uncached): fb_pages[fb_cur] is synced this frame, shown next
static uint32_t *fb; // current draw target: always the canvas at runtime, temporarily the row-strip buffer when baking strips at startup
static int fb_cur;
static SceUID fb_block;
static void *fb_base; // for sceGxmMapMemory to register with the GPU
static SceGxmSyncObject *fb_sync[2]; // for common dialog overlay rendering
static int gxm_ok;
static uint32_t *canvas; // compositing canvas in cached main memory; all drawing writes here, DMA'd into VRAM on flip
static SceGxmColorSurface fb_surf[2]; // GXM view of the VRAM pages: PadHeartbeat must be issued per page
static void *font_data; // FT_New_Memory_Face does not copy; the font buffer must live until process end
static FT_Library ft_lib;
static FT_Face ft_face;

/* ---------------- large memory pool (see render.h notes) ---------------- */
static char *arena_p, *arena_end, *arena_base;
static size_t arena_total;
static size_t arena_ovf; /* bytes fallen back to malloc when the pool is insufficient */

static void arena_init(void) {
	static const size_t try_sizes[5] = { 64u << 20, 40u << 20, 32u << 20, 24u << 20, 16u << 20 };
	for (int i = 0; i < 5; i++) {
		SceUID uid = sceKernelAllocMemBlock("arena", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_RW,
		                                    try_sizes[i], NULL);
		if (uid < 0) continue;
		void *base = NULL;
		if (sceKernelGetMemBlockBase(uid, &base) < 0) {
			sceKernelFreeMemBlock(uid);
			continue;
		}
		arena_p = arena_base = base;
		arena_end = (char *)base + try_sizes[i];
		arena_total = try_sizes[i];
		return;
	}
}

void *render_alloc(size_t n) {
	n = (n + 15u) & ~(size_t)15u;
	if (arena_p && n <= (size_t)(arena_end - arena_p)) {
		void *p = arena_p;
		arena_p += n;
		return p;
	}
	arena_ovf += n;
	return malloc(n);
}

void *render_alloc0(size_t n) {
	void *p = render_alloc(n);
	if (p) memset(p, 0, n);
	return p;
}

size_t render_arena_total(void) { return arena_total; }
size_t render_arena_used(void) { return arena_p ? (size_t)(arena_p - arena_base) : 0; }
size_t render_arena_overflow(void) { return arena_ovf; }

static void *read_whole_file(const char *path, SceOff *size) {
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0) return NULL;
	SceOff sz = sceIoLseek(fd, 0, SCE_SEEK_END);
	sceIoLseek(fd, 0, SCE_SEEK_SET);
	void *buf = render_alloc((size_t)sz);
	if (!buf || sceIoRead(fd, buf, (size_t)sz) != sz) {
		sceIoClose(fd);
		return NULL;
	}
	sceIoClose(fd);
	*size = sz;
	return buf;
}

static unsigned int utf8_next(const char **s) {
	const unsigned char *p = (const unsigned char *)*s;
	unsigned int cp;
	int extra;
	if (p[0] < 0x80) { cp = p[0]; extra = 0; }
	else if ((p[0] >> 5) == 0x6) { cp = p[0] & 0x1f; extra = 1; }
	else if ((p[0] >> 4) == 0xe) { cp = p[0] & 0x0f; extra = 2; }
	else { cp = p[0] & 0x07; extra = 3; }
	for (int i = 1; i <= extra; i++)
		cp = (cp << 6) | (p[i] & 0x3f);
	*s += extra + 1;
	return cp;
}

static int cur_px; // cached current pixel size, avoids Set_Pixel_Sizes per character

/* When baking row textures at startup the target is a transparent scratch canvas, so coverage must be composited
   into alpha (non-premultiplied source-over). At runtime the canvas composites opaque, so this flag is off for the original fast path. */
static int bake_alpha;

void render_set_bake_alpha(int on) { bake_alpha = on; }

static uint32_t over_px(uint32_t dst, uint32_t src, unsigned a) {
	unsigned ad = (dst >> 24) & 0xff;
	unsigned na = a + (ad * (255 - a) >> 8);
	unsigned r = (((src & 0xff) * a) + ((dst & 0xff) * ad * (255 - a) >> 8)) / na;
	unsigned g = ((((src >> 8) & 0xff) * a) + (((dst >> 8) & 0xff) * ad * (255 - a) >> 8)) / na;
	unsigned b = ((((src >> 16) & 0xff) * a) + (((dst >> 16) & 0xff) * ad * (255 - a) >> 8)) / na;
	return (na << 24) | (b << 16) | (g << 8) | r;
}

static int set_size(int px) {
	if (px == cur_px) return 0;
	if (FT_Set_Pixel_Sizes(ft_face, 0, px)) {
		cur_px = 0; // failure (usually out of memory): retry next time, caller skips this glyph
		return -1;
	}
	cur_px = px;
	return 0;
}

/* glyph cache: CJK rasterization is expensive, each (codepoint, size) rendered once; direct-mapped, overwrite on conflict */
#define GC_SLOTS 1024
#define GC_SLOT_BYTES 2048 /* must fit a 42px CJK glyph (~1900B), else large glyphs re-rasterize every frame */
typedef struct {
	unsigned int cp;
	int px, advance, left, top, rows, width, pitch;
	int valid;
	unsigned char *buf; /* points to a fixed gc_pool slot; NULL for blank glyphs */
} GCache;
static GCache gc[GC_SLOTS];
static unsigned char *gc_pool;

static GCache *get_glyph(unsigned int cp, int px) {
	if (px <= 0 || set_size(px) < 0) return NULL;
	GCache *g = &gc[(cp * 2654435761u >> 22) & (GC_SLOTS - 1)];
	if (g->valid && g->cp == cp && g->px == px) return g;
	if (FT_Load_Char(ft_face, cp, FT_LOAD_RENDER)) {
		g->valid = 0;
		return NULL;
	}
	FT_GlyphSlot s = ft_face->glyph;
	g->valid = 1;
	g->cp = cp;
	g->px = px;
	g->advance = s->advance.x >> 6; // 26.6 fixed-point
	g->left = s->bitmap_left;
	g->top = s->bitmap_top;
	g->rows = s->bitmap.rows;
	g->width = s->bitmap.width;
	g->pitch = s->bitmap.pitch;
	if (!g->rows || !s->bitmap.buffer || g->rows * g->pitch > GC_SLOT_BYTES) {
		g->buf = NULL; /* space or oversized glyph: keep only advance */
		g->rows = 0;
		return g;
	}
	g->buf = gc_pool + (g - gc) * GC_SLOT_BYTES;
	memcpy(g->buf, s->bitmap.buffer, (size_t)g->rows * g->pitch);
	return g;
}

/* draw a single glyph at (x, baseline), return its advance; -1 on failure */
static int blit_char(int x, int baseline_y, unsigned int cp, uint32_t color) {	GCache *g = get_glyph(cp, cur_px);
	if (!g) return -1;
	for (int row = 0; row < g->rows; row++) {
		// font y up, screen y down: glyph top is 'top' above baseline, so first row = baseline - top
		int py = baseline_y - g->top + row;
		if (py < 0 || py >= SCREEN_H) continue;
		for (int col = 0; col < g->width; col++) {
			int sx = x + g->left + col;
			if (sx < 0 || sx >= SCREEN_W) continue;
			unsigned char a = g->buf[row * g->pitch + col];
			if (!a) continue;
			uint32_t *pixel = &fb[py * FB_PITCH + sx];
			uint32_t bg = *pixel;
			if (bake_alpha) { *pixel = over_px(bg, color, a); continue; }
			uint32_t r = (((bg & 0xff) * (255 - a)) + ((color & 0xff) * a)) >> 8;
			uint32_t gg = (((bg & 0xff00) * (255 - a)) + ((color & 0xff00) * a)) >> 8;
			uint32_t b = ((((bg >> 16) & 0xff) * (255 - a)) + (((color >> 16) & 0xff) * a)) >> 8;
			*pixel = (b << 16) | (gg & 0xff00) | r;
		}
	}
	return g->advance;
}

int render_init(void) {
	arena_init(); /* must be first: font/background/sprites all draw from the pool, heap left for FreeType */
	/* VRAM must be uncached (USER_CDRAM_RW): switching to L1WBWA garbles text in testing --
	   dirty rows are still in cache, not flushed to DRAM, when the display controller reads them, and userland has no cache-clean API.
	   Uncached per-pixel writes are notoriously slow (~15MB/s), so we instead
	   "draw into a cached main-memory canvas -> CPU-copy only the changed rows into VRAM each frame". */
	fb_block = sceKernelAllocMemBlock("fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
	                                  2 * FB_SIZE, NULL);
	if (fb_block < 0) return fb_block;
	void *base = NULL;
	if (sceKernelGetMemBlockBase(fb_block, &base) < 0) return -1;
	fb_pages[0] = (uint32_t *)base;
	fb_pages[1] = (uint32_t *)((char *)base + FB_SIZE);
	fb_base = base;
	fb_cur = 1; /* pages[0] is the display page first, first flip writes pages[1] */
	canvas = render_alloc0(FB_SIZE); /* same layout as VRAM (1024 pitch), can DMA the whole block */
	if (!canvas) return -5;
	fb = canvas; /* all drawing writes the canvas; on flip only changed rows are DMA'd into the back page */

	SceOff font_size = 0;
	font_data = read_whole_file("app0:font.ttf", &font_size);
	if (!font_data) return -1;
	if (FT_Init_FreeType(&ft_lib)) return -2;
	if (FT_New_Memory_Face(ft_lib, (const FT_Byte *)font_data, (FT_Long)font_size, 0, &ft_face))
		return -3;
	gc_pool = render_alloc0(GC_SLOTS * GC_SLOT_BYTES); // glyph cache pool
	if (!gc_pool) return -4;

	render_clear();
	/* at boot copy the empty canvas to the display page first, to avoid showing leftover VRAM data before the first frame */
	memcpy(fb_pages[0], canvas, FB_SIZE);
	SceDisplayFrameBuf frame = {
		.size = sizeof(frame),
		.base = fb_pages[0],
		.pitch = FB_PITCH,
		.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8,
		.width = SCREEN_W,
		.height = SCREEN_H,
	};
	int rr = sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
	fb = canvas;
	return rr;
}

void render_clear(void) {
	for (int i = 0; i < FB_PITCH * SCREEN_H; i++)
		fb[i] = RENDER_BG;
}

void render_text(int x, int baseline_y, const char *utf8, uint32_t color, int px) {
	set_size(px);
	while (*utf8) {
		unsigned int cp = utf8_next(&utf8);
		int adv = blit_char(x, baseline_y, cp, color);
		if (adv >= 0) x += adv;
	}
}

int render_text_width(const char *utf8, int px) {
	set_size(px);
	int w = 0;
	while (*utf8) {
		unsigned int cp = utf8_next(&utf8);
		GCache *g = get_glyph(cp, px);
		if (g) w += g->advance;
	}
	return w;
}

int render_text_clip(int x, int baseline_y, const char *utf8, uint32_t color, int px, int max_w) {
	set_size(px);
	int total = render_text_width(utf8, px);
	if (total <= max_w) {
		render_text(x, baseline_y, utf8, color, px);
		return total;
	}
	int ell = 0;
	GCache *ge = get_glyph(0x2026, px);
	if (ge) ell = ge->advance;
	int w = 0;
	const char *p = utf8;
	while (*p) {
		const char *next = p;
		unsigned int cp = utf8_next(&next);
		GCache *g = get_glyph(cp, px);
		if (!g) { p = next; continue; }
		if (w + g->advance > max_w - ell) break;
		blit_char(x + w, baseline_y, cp, color);
		w += g->advance;
		p = next;
	}
	blit_char(x + w, baseline_y, 0x2026, color);
	return max_w;
}

int render_text_wrap(int x, int baseline_y, const char *utf8, uint32_t color, int px,
                    int max_w, int max_lines) {
	set_size(px);
	int pitch = px + px / 4;
	int ell = 0;
	GCache *ge = get_glyph(0x2026, px);
	if (ge) ell = ge->advance;
	const char *p = utf8;
	int y = baseline_y;
	for (int line = 0; line < max_lines && *p; line++) {
		int last = (line == max_lines - 1);
		int avail = max_w;
		const char *end = p;
		int w = 0;
		while (*end && *end != '\n') {
			const char *next = end;
			unsigned int cp = utf8_next(&next);
			GCache *g = get_glyph(cp, px);
			if (!g) { end = next; continue; }
			if (w + g->advance > avail) break;
			w += g->advance;
			end = next;
		}
		int more = (*end == '\n') ? end[1] != '\0' : *end != '\0';
		if (last && more) {
			// last line with remaining content: redo the greedy pass reserving ellipsis width
			avail = max_w - ell;
			end = p; w = 0;
			while (*end && *end != '\n') {
				const char *next = end;
				unsigned int cp = utf8_next(&next);
				GCache *g = get_glyph(cp, px);
				if (!g) { end = next; continue; }
				if (w + g->advance > avail) break;
				w += g->advance;
				end = next;
			}
		}
		const char *q = p;
		int dx = 0;
		while (q < end) {
			unsigned int cp = utf8_next(&q);
			int adv = blit_char(x + dx, y, cp, color);
			if (adv >= 0) dx += adv;
		}
		if (last && more) blit_char(x + dx, y, 0x2026, color);
		if (*end == '\n') end++;
		p = end;
		y += pitch;
	}
	return y;
}

/* For reading pages: after skipping 'skip' lines draws max_lines lines, without clipping the last line (clipping would lose body text);
   whether more content remains is reported via the return value. total_lines returns the full laid-out line count.
   Every line measures its advance, but all are FreeType cache hits, so 2KB of body text takes under 1ms. */
int render_text_scroll(int x, int baseline_y, const char *utf8, uint32_t color, int px,
                       int max_w, int max_lines, int skip, int *total_lines) {
	set_size(px);
	int pitch = px + px / 4;
	const char *p = utf8;
	int line = 0, drawn = 0, y = baseline_y;
	while (*p) {
		const char *end = p;
		int w = 0;
		while (*end && *end != '\n') {
			const char *next = end;
			unsigned int cp = utf8_next(&next);
			GCache *g = get_glyph(cp, px);
			if (!g) { end = next; continue; }
			if (w + g->advance > max_w) break;
			w += g->advance;
			end = next;
		}
		if (line >= skip && drawn < max_lines) {
			const char *q = p;
			int dx = 0;
			while (q < end) {
				unsigned int cp = utf8_next(&q);
				int adv = blit_char(x + dx, y, cp, color);
				if (adv >= 0) dx += adv;
			}
			y += pitch;
			drawn++;
		}
		if (*end == '\n') end++;
		p = end;
		line++;
	}
	if (total_lines) *total_lines = line;
	return drawn;
}

uint32_t *render_cur_fb(void) { return fb; }

void render_rect(int x, int y, int w, int h, uint32_t color) {
	for (int row = y; row < y + h; row++) {
		if (row < 0 || row >= SCREEN_H) continue;
		for (int col = x; col < x + w; col++) {
			if (col < 0 || col >= SCREEN_W) continue;
			fb[row * FB_PITCH + col] = color;
		}
	}
}

void render_border(int x, int y, int w, int h, uint32_t color) {
	render_rect(x, y, w, 1, color);
	render_rect(x, y + h - 1, w, 1, color);
	render_rect(x, y, 1, h, color);
	render_rect(x + w - 1, y, 1, h, color);
}

/* ---------------- canvas -> VRAM: dirty-row sync + page flip ----------------
   Each VRAM page tracks a row range that "lags the canvas": this frame merges the debt with the canvas's newly changed rows and DMAs
   them in one go to page p (about to be shown); those rows then become the other page's new debt (it becomes the back page next),
   so flipping back and forth never reveals stale content. */
static int db0[2] = { 0, 0 }, db1[2] = { SCREEN_H, SCREEN_H };
static int pz0 = SCREEN_H, pz1 = 0; /* row range changed in the canvas this frame; empty when pz0>=pz1 */

void render_mark_dirty(int y0, int h) {
	if (y0 < pz0) pz0 = y0;
	if (y0 + h > pz1) pz1 = y0 + h;
}

void render_set_target(uint32_t *t) { fb = t ? t : canvas; }

static void sync_rows(uint32_t *page, int y0, int y1) {
	char *d = (char *)page + (size_t)y0 * FB_PITCH * 4;
	const char *s = (const char *)canvas + (size_t)y0 * FB_PITCH * 4;
	memcpy(d, s, (size_t)(y1 - y0) * FB_PITCH * 4);
	/* CPU copy only: the DMAC reads physical memory, but rows just written to the canvas are still in cache;
	   fetching stale data garbles the screen (same pitfall as L1WBWA VRAM), and userland has no cache-flush API */
}

static int fb_pending = -1; /* page whose dirty rows are already laid out, waiting for the dialog to finish drawing before switching on-screen */
static int dq_n, dq_retry, dq_skip, dq_wait; /* dialog flip counters: skip = queue full, didn't dare switch; wait = target page still on screen */

/* only lay the canvas's dirty rows onto the next page, without switching the screen */
static int flush_rows(void) {
	int p = fb_cur; /* next page to be shown */
	int lo = pz0 < db0[p] ? pz0 : db0[p];
	int hi = pz1 > db1[p] ? pz1 : db1[p];
	if (hi > lo) sync_rows(fb_pages[p], lo, hi);
	/* Only record this frame's newly drawn rows as the other page's debt. Previously it recorded lo/hi including old debt;
	   after one full-screen redraw every frame carries a full page of debt, forever copying the whole screen at 12ms */
	if (pz1 > pz0) {
		int q = 1 - p;
		if (pz0 < db0[q]) db0[q] = pz0;
		if (pz1 > db1[q]) db1[q] = pz1;
	}
	db0[p] = SCREEN_H;
	db1[p] = 0;
	pz0 = SCREEN_H;
	pz1 = 0;
	return p;
}

static void set_frame(int p, int mode) {
	SceDisplayFrameBuf frame = {
		.size = sizeof(frame),
		.base = fb_pages[p],
		.pitch = FB_PITCH,
		.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8,
		.width = SCREEN_W,
		.height = SCREEN_H,
	};
	sceDisplaySetFrameBuf(&frame, mode);
}

static void present_page(int p, int paced) {
	/* The back page is already a complete frame, so normal running flips immediately: no vsync wait, keypress to on-screen is one scan.
	   But dialogs (IME) are drawn onto this page asynchronously by the GPU; flipping immediately lands on the frame where
	   "the keyboard isn't finished", which looks like constant flicker -- that path goes through render_compose_end's display queue. */
	set_frame(p, paced ? SCE_DISPLAY_SETBUF_NEXTFRAME : SCE_DISPLAY_SETBUF_IMMEDIATE);
	fb_cur ^= 1;
	fb = canvas;
}

void render_flip_now(void) {
	present_page(flush_rows(), 0);
}

/* GPU-overlay dialogs like the IME must "draw onto that page first, then switch it on-screen":
   drawing after the switch shows a frame without the keyboard, which looks like constant flicker. */
void *render_pending_page(void) { return fb_pending >= 0 ? fb_pages[fb_pending] : NULL; }
void *render_pending_sync(void) { return fb_pending >= 0 ? fb_sync[fb_pending] : NULL; }

/* Before the CPU fills a page we must confirm it's not on screen: the display queue registers the flip in the vsync callback as
   NEXTFRAME, so it may take effect one vsync later. In the meantime we're already laying the bare canvas (no keyboard) onto
   "the page still being scanned", so the screen shows "a flash of the game list then switches back" --
   the log flickers even with retry=0 skip=0, meaning it's not the queue filling up but this write path. */
static void wait_page_offscreen(int p) {
	for (int i = 0; i < 2; i++) {
		SceDisplayFrameBuf cur;
		memset(&cur, 0, sizeof(cur));
		cur.size = sizeof(cur);
		if (sceDisplayGetFrameBuf(&cur, 0) < 0) return;
		if (cur.base != (void *)fb_pages[p]) return;
		sceDisplayWaitVblankStart();
		dq_wait++;
	}
}

void render_compose_begin(void) {
	/* first confirm the page to be filled with the canvas isn't on screen, then proceed */
	if (gxm_ok) wait_page_offscreen(fb_cur);
	render_mark_dirty(0, SCREEN_H); /* the dialog repaints the whole page, so the other page's old content can't be reused */
	fb_pending = flush_rows();
}

void render_compose_end(void) {
	if (fb_pending < 0) return;
	int p = fb_pending;
	fb_pending = -1;
	if (!gxm_ok) {
		present_page(p, 1);
		return;
	}
	/* The heartbeat must come after the dialog finishes drawing: it tells GXM "this frame's work is handed off".
	   If placed at begin, the keyboard's GPU commands are still queued and unflushed, so the display queue waits on
	   a sync that's already signaled but whose keyboard isn't actually drawn -- this is the flicker while typing */
	sceGxmPadHeartbeat(&fb_surf[p], fb_sync[p]);
	/* Hand the flip to the display queue at vsync: it first waits for the frontSync page to finish drawing and stop being scanned,
	   then switches pages in our vsync callback, so CPU writes and GPU compositing are truly staggered.
	   Each keystroke makes the IME spend tens of ms processing pinyin, so that frame exceeds one vsync and the queue fills up;
	   never call SetFrameBuf directly then -- the dialog's GPU commands aren't finished, and flipping shows the bare list without
	   the keyboard, exactly "a flash of the game list then switching back". Wait a frame and retry; if still failing, skip this frame's
	   flip and re-lay the same page next frame to self-heal. */
	int r = -1, retry = 0;
	for (int try = 0; try < 2 && r < 0; try++) {
		r = sceGxmDisplayQueueAddEntry(fb_sync[1 - p], fb_sync[p], &fb_pages[p]);
		if (r < 0) {
			retry++;
			sceDisplayWaitVblankStart();
		}
	}
	dq_retry += retry;
	dq_skip += (r < 0);
	if (++dq_n >= 120) {
		extern void dlog(const char *fmt, ...);
		dlog("ime flip n=%d retry=%d skip=%d wait=%d", dq_n, dq_retry, dq_skip, dq_wait);
		dq_n = dq_retry = dq_skip = dq_wait = 0;
	}
	if (r < 0) return; /* already waited a frame in the loop, so skip the flip here */
	fb_cur ^= 1;
	fb = canvas;
	sceDisplayWaitVblankStart(); /* one flip per frame, otherwise the queue fills immediately */
}

/* the display queue calls back at vsync and does the flip for us (callback_data = &fb_pages[p]) */
static void vsync_flip(const void *callback_data) {
	SceDisplayFrameBuf frame = {
		.size = sizeof(frame),
		.base = *(void *const *)callback_data,
		.pitch = FB_PITCH,
		.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8,
		.width = SCREEN_W,
		.height = SCREEN_H,
	};
	sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

int render_dialog_init(void) {
	if (gxm_ok) return 0;
	/* The common dialog family (including IME) strictly requires GXM to be initialized; error 0x80020436 means this step is missing.
	   A vsync callback is required here so sceGxmDisplayQueueAddEntry has somewhere to queue */
	int r = sceGxmInitialize(&(SceGxmInitializeParams){0, 1, vsync_flip, sizeof(void *),
	                                                   SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE});
	if (r < 0) return r;
	/* fb must be registered with the GPU: dialogs writing to unmapped memory via the GPU trigger an SGX fault that hangs the whole system */
	r = sceGxmMapMemory(fb_base, 2 * FB_SIZE, SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
	if (r < 0) return r;
	for (int i = 0; i < 2; i++) {
		r = sceGxmColorSurfaceInit(&fb_surf[i], SCE_GXM_COLOR_FORMAT_A8B8G8R8,
		                           SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
		                           SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, SCREEN_W, SCREEN_H, FB_PITCH,
		                           fb_pages[i]);
		if (r < 0) return r;
		r = sceGxmSyncObjectCreate(&fb_sync[i]);
		if (r < 0) return r;
	}
	gxm_ok = 1;
	return 0;
}

/* the dialog is overlaid directly by the GPU, targeting "the page currently shown", not the canvas */
void *render_backbuf(void) { return fb_pages[1 - fb_cur]; }
void *render_backbuf_sync(void) { return fb_sync[1 - fb_cur]; }
