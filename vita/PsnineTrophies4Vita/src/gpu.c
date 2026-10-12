// GPU rendering layer on libvita2d: textured draws, UTF-8 text with width measurement,
// network-icon textures and the render target used by the page turn.
//
// vita2d here is the VGL backend: it blends with (SRC_ALPHA, ONE_MINUS_SRC_ALPHA), so both
// textures and colors keep plain straight alpha exactly like the old CPU canvas, and the tint
// color modulates all four channels (out.rgb = tex.rgb*color.rgb, out.a = tex.a*color.a).
// That is why dimming alpha and dimming rgb are two different helpers below.
#include "gpu.h"
#include "data.h"

#include <psp2/io/fcntl.h>
#include <psp2/kernel/sysmem.h>

#include <stdlib.h>
#include <string.h>

void dlog(const char *fmt, ...);

/* ---------------- big memory pool ----------------
   The app heap is only ~28MB and the 23MB font eats most of it, so buffers are carved out of
   a dedicated memblock; malloc is the fallback when the pool runs dry. */
static char *arena_p, *arena_end, *arena_base;
static size_t arena_total;
static size_t arena_ovf;

static void arena_init(void) {
	static const size_t try_sizes[5] = { 64u << 20, 48u << 20, 32u << 20, 24u << 20, 16u << 20 };
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

void *gpu_arena_alloc(size_t n) {
	n = (n + 15u) & ~(size_t)15u;
	if (arena_p && n <= (size_t)(arena_end - arena_p)) {
		void *p = arena_p;
		arena_p += n;
		return p;
	}
	arena_ovf += n;
	return malloc(n);
}

void *gpu_arena_alloc0(size_t n) {
	void *p = gpu_arena_alloc(n);
	if (p) memset(p, 0, n);
	return p;
}

size_t gpu_arena_total(void) { return arena_total; }
size_t gpu_arena_used(void) { return arena_p ? (size_t)(arena_p - arena_base) : 0; }
size_t gpu_arena_overflow(void) { return arena_ovf; }

/* ---------------- textures ---------------- */
uint32_t *gpu_read_file(const char *path, size_t n) {
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0) { dlog("open fail %s 0x%x", path, fd); return NULL; }
	uint32_t *buf = gpu_arena_alloc(n);
	if (!buf) { dlog("alloc fail %s", path); sceIoClose(fd); return NULL; }
	size_t got = 0;
	while (got < n) { /* sceIoRead may return a short count */
		int r = sceIoRead(fd, (char *)buf + got, n - got);
		if (r <= 0) break;
		got += r;
	}
	sceIoClose(fd);
	if (got < n) { dlog("read fail %s got=%d/%d", path, (int)got, (int)n); return NULL; }
	return buf;
}

vita2d_texture *gpu_tex_upload(const uint32_t *px, int w, int h) {
	if (!px || w <= 0 || h <= 0) return NULL;
	vita2d_texture *t = vita2d_create_empty_texture_format(w, h, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
	if (!t) { dlog("tex alloc %dx%d fail", w, h); return NULL; }
	uint32_t *dst = vita2d_texture_get_datap(t);
	unsigned stride = vita2d_texture_get_stride(t) / 4; /* stride is in BYTES */
	for (int y = 0; y < h; y++)
		memcpy(dst + (size_t)y * stride, px + (size_t)y * w, (size_t)w * 4);
	return t;
}

vita2d_texture *gpu_tex_file(const char *path, int w, int h) {
	uint32_t *px = gpu_read_file(path, (size_t)w * h * 4);
	if (!px) return NULL;
	return gpu_tex_upload(px, w, h);
}

vita2d_texture *gpu_tex_rt(int w, int h) {
	return vita2d_create_empty_texture_rendertarget(w, h, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
}

void gpu_rt_begin(vita2d_texture *rt) {
	/* Binds the target's own framebuffer; the flags argument belongs to the GXM backend and is
	   ignored here. The default target is restored by the next gpu_frame_begin(). */
	vita2d_start_drawing_advanced(rt, 0);
	vita2d_clear_screen();
}

void gpu_rt_end(void) {
	vita2d_end_drawing();
	vita2d_wait_rendering_done(); /* the strips sample this texture right after */
}

/* ---------------- draws ---------------- */
void gpu_draw(int x, int y, const vita2d_texture *t) {
	if (t) vita2d_draw_texture(t, (float)x, (float)y);
}

/* fade a texture out by scaling its alpha: white tint, alpha k */
void gpu_draw_a(int x, int y, const vita2d_texture *t, int k) {
	if (!t || k <= 0) return;
	if (k >= 255) { vita2d_draw_texture(t, (float)x, (float)y); return; }
	vita2d_draw_texture_tint(t, (float)x, (float)y, RGBA8(255, 255, 255, k));
}

/* darken a texture without touching its alpha: gray k, opaque alpha */
void gpu_draw_shade(int x, int y, const vita2d_texture *t, int k) {
	if (!t) return;
	vita2d_draw_texture_tint(t, (float)x, (float)y, RGBA8(k, k, k, 255));
}

void gpu_draw_part(int x, int y, const vita2d_texture *t, int tx, int ty, int tw, int th) {
	if (!t || tw <= 0 || th <= 0) return;
	vita2d_draw_texture_part(t, (float)x, (float)y, (float)tx, (float)ty, (float)tw, (float)th);
}

void gpu_draw_part_a(int x, int y, const vita2d_texture *t, int tx, int ty, int tw, int th, int k) {
	if (!t || tw <= 0 || th <= 0 || k <= 0) return;
	if (k >= 255) {
		vita2d_draw_texture_part(t, (float)x, (float)y, (float)tx, (float)ty, (float)tw, (float)th);
		return;
	}
	vita2d_draw_texture_tint_part(t, (float)x, (float)y, (float)tx, (float)ty, (float)tw,
	                              (float)th, RGBA8(255, 255, 255, k));
}

/* one perspective column of the flipping page: vertically squashed by y_scale, darkened by shade */
void gpu_draw_part_shade_scale(const vita2d_texture *t, int x, int y, int tx, int ty, int tw,
                               int th, float y_scale, int shade) {
	if (!t || tw <= 0 || th <= 0) return;
	vita2d_draw_texture_tint_part_scale(t, (float)x, (float)y, (float)tx, (float)ty, (float)tw,
	                                    (float)th, 1.0f, y_scale, RGBA8(shade, shade, shade, 255));
}

/* fill: colors carry their own alpha and blend like everything else */
void gpu_rect(int x, int y, int w, int h, uint32_t color) {
	if (w <= 0 || h <= 0) return;
	vita2d_draw_rectangle((float)x, (float)y, (float)w, (float)h, color);
}

/* ---------------- text ---------------- */
static vita2d_font *font;

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

static int utf8_enc(unsigned int cp, char *out) {
	unsigned char *p = (unsigned char *)out;
	if (cp < 0x80) { p[0] = (unsigned char)cp; return 1; }
	if (cp < 0x800) {
		p[0] = (unsigned char)(0xc0 | (cp >> 6));
		p[1] = (unsigned char)(0x80 | (cp & 0x3f));
		return 2;
	}
	if (cp < 0x10000) {
		p[0] = (unsigned char)(0xe0 | (cp >> 12));
		p[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
		p[2] = (unsigned char)(0x80 | (cp & 0x3f));
		return 3;
	}
	p[0] = (unsigned char)(0xf0 | (cp >> 18));
	p[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3f));
	p[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
	p[3] = (unsigned char)(0x80 | (cp & 0x3f));
	return 4;
}

/* Per-codepoint advance, measured once through vita2d_font_text_width -- which is the very same
   layout path vita2d_font_draw_text uses (both call generic_font_draw_text), so a truncated
   string always ends where the drawn one would. CJK text has hundreds of distinct glyphs, so the
   table is sized for the whole visible set at every font size in use. */
#define ADV_SLOTS 4096
typedef struct { unsigned int key; int adv; } AdvSlot;
static AdvSlot advc[ADV_SLOTS];

static int adv_of(unsigned int cp, int px) {
	unsigned int key = cp * 128u + (unsigned int)px; /* px < 128 */
	unsigned int i = (key * 2654435761u) >> 20;
	for (int n = 0; n < ADV_SLOTS; n++, i = (i + 1) & (ADV_SLOTS - 1)) {
		if (advc[i].key == key) return advc[i].adv;
		if (advc[i].key == 0) {
			char b[8];
			b[utf8_enc(cp, b)] = 0; /* vita2d measures to NUL: an unterminated buffer counts stack junk as extra glyphs */
			advc[i].adv = vita2d_font_text_width(font, (unsigned int)px, b);
			advc[i].key = key;
			return advc[i].adv;
		}
	}
	return 0;
}

int gpu_text(int x, int baseline_y, const char *utf8, uint32_t color, int px) {
	if (!font || !utf8 || !*utf8) return 0;
	return vita2d_font_draw_text(font, x, baseline_y, color, (unsigned int)px, utf8);
}

int gpu_text_w(const char *utf8, int px) {
	int w = 0;
	while (utf8 && *utf8) {
		unsigned int cp = utf8_next(&utf8);
		w += adv_of(cp, px);
	}
	return w;
}

/* vita2d draws a NUL-terminated string, so a measured span is copied out to draw it */
static char span_buf[2048];

static void draw_span(int x, int y, const char *s, const char *e, uint32_t color, int px) {
	size_t n = (size_t)(e - s);
	if (!n) return;
	if (n >= sizeof(span_buf)) n = sizeof(span_buf) - 1;
	memcpy(span_buf, s, n);
	span_buf[n] = 0;
	vita2d_font_draw_text(font, x, y, color, (unsigned int)px, span_buf);
}

#define ELL 0x2026u

int gpu_text_clip(int x, int baseline_y, const char *utf8, uint32_t color, int px, int max_w) {
	int total = gpu_text_w(utf8, px);
	if (total <= max_w) {
		gpu_text(x, baseline_y, utf8, color, px);
		return total;
	}
	int ell = adv_of(ELL, px);
	const char *p = utf8, *end = utf8;
	int w = 0;
	while (*p) {
		const char *next = p;
		unsigned int cp = utf8_next(&next);
		int a = adv_of(cp, px);
		if (w + a > max_w - ell) break;
		w += a;
		p = next;
		end = p;
	}
	draw_span(x, baseline_y, utf8, end, color, px);
	gpu_text(x + w, baseline_y, "\xe2\x80\xa6", color, px);
	return max_w;
}

int gpu_text_wrap(int x, int baseline_y, const char *utf8, uint32_t color, int px,
                  int max_w, int max_lines) {
	int pitch = px + px / 4;
	int ell = adv_of(ELL, px);
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
			int a = adv_of(cp, px);
			if (w + a > avail) break;
			w += a;
			end = next;
		}
		int more = (*end == '\n') ? end[1] != '\0' : *end != '\0';
		if (last && more) { /* the closing line shows an ellipsis, so re-measure reserving its width */
			avail = max_w - ell;
			end = p;
			w = 0;
			while (*end && *end != '\n') {
				const char *next = end;
				unsigned int cp = utf8_next(&next);
				int a = adv_of(cp, px);
				if (w + a > avail) break;
				w += a;
				end = next;
			}
		}
		draw_span(x, y, p, end, color, px);
		if (last && more) {
			gpu_text(x + w, y, "\xe2\x80\xa6", color, px);
			y += pitch;
			return y; /* nothing left to show */
		}
		if (*end == '\n') end++;
		p = end;
		y += pitch;
	}
	return y;
}

int gpu_text_scroll(int x, int baseline_y, const char *utf8, uint32_t color, int px,
                    int max_w, int max_lines, int skip, int *total_lines) {
	int pitch = px + px / 4;
	const char *p = utf8;
	int line = 0, drawn = 0, y = baseline_y;
	while (*p) {
		const char *end = p;
		int w = 0;
		while (*end && *end != '\n') {
			const char *next = end;
			unsigned int cp = utf8_next(&next);
			int a = adv_of(cp, px);
			if (w + a > max_w) break;
			w += a;
			end = next;
		}
		if (line >= skip && drawn < max_lines) {
			draw_span(x, y, p, end, color, px);
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

/* ---------------- network icon textures ----------------
   data.c owns decode and cache slots; a decoded image is uploaded to a GPU texture once here and
   kept by url hash. The gray placeholder becomes a texture too, so a still-loading icon looks the
   same as before instead of leaving a hole. */
#define ICON_SLOTS 48
typedef struct {
	uint64_t key;
	int kind;
	vita2d_texture *tex;
	int is_ph;
	int w, h;
	unsigned age;
} IconSlot;
static IconSlot icons[ICON_SLOTS];
static unsigned icon_age;

static uint64_t url_key(int kind, const char *url) {
	uint64_t h = 1469598103934665603ull;
	for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
		h ^= *p;
		h *= 1099511628211ull;
	}
	return h ^ (uint64_t)kind * 0x9e3779b97f4a7c15ull;
}

vita2d_texture *gpu_icon(int kind, const char *url, int *w, int *h) {
	*w = *h = 0;
	if (!url || !url[0]) return NULL;
	uint64_t key = url_key(kind, url);
	int slot = -1;
	for (int i = 0; i < ICON_SLOTS; i++) {
		if (!icons[i].tex || icons[i].key != key || icons[i].kind != kind) continue;
		/* A cached placeholder stays valid only while the real image is still missing;
		   once data.c has decoded it, this same slot is refilled below. */
		if (!icons[i].is_ph || data_icon_state(kind, url) != DI_READY) {
			icons[i].age = ++icon_age;
			*w = icons[i].w;
			*h = icons[i].h;
			return icons[i].tex;
		}
		slot = i;
		break;
	}
	if (slot < 0) {
		int free_i = -1, victim = 0;
		for (int i = 0; i < ICON_SLOTS; i++) {
			if (!icons[i].tex) { free_i = i; break; }
			if (icons[i].age < icons[victim].age) victim = i;
		}
		slot = free_i >= 0 ? free_i : victim;
	}

	/* the call also registers the download when the url is new, so it must run every frame */
	int iw, ih;
	const uint32_t *px = data_icon(kind, url, &iw, &ih);
	int ready = data_icon_state(kind, url) == DI_READY;
	if (!ready) { /* placeholder, shared by every still-loading icon of this kind */
		for (int j = 0; j < ICON_SLOTS; j++) {
			if (icons[j].tex && icons[j].is_ph && icons[j].kind == kind) {
				icons[j].age = ++icon_age;
				*w = icons[j].w;
				*h = icons[j].h;
				return icons[j].tex;
			}
		}
	}
	if (icons[slot].tex) vita2d_free_texture(icons[slot].tex);
	icons[slot].tex = NULL;
	icons[slot].key = key;
	icons[slot].kind = kind;
	icons[slot].is_ph = !ready;
	icons[slot].w = iw;
	icons[slot].h = ih;
	icons[slot].tex = gpu_tex_upload(px, iw, ih);
	if (!icons[slot].tex) { dlog("icon tex fail %dx%d", iw, ih); return NULL; }
	icons[slot].age = ++icon_age;
	*w = iw;
	*h = ih;
	return icons[slot].tex;
}

/* ---------------- init ---------------- */
int gpu_init(void) {
	arena_init(); /* first: font, sprites and icon buffers all draw from the pool */

	/* Always returns 1; whether the GPU is really up shows as the first painted frame.
	   Never call sceGxmInitialize() before this -- it would take over the display queue. */
	vita2d_init();
	vita2d_set_clear_color(GPU_BG);

	SceUID fd = sceIoOpen("app0:font.ttf", SCE_O_RDONLY, 0);
	if (fd < 0) { dlog("font open 0x%x", fd); return -1; }
	SceOff sz = sceIoLseek(fd, 0, SCE_SEEK_END);
	sceIoLseek(fd, 0, SCE_SEEK_SET);
	uint8_t *fbuf = gpu_arena_alloc((size_t)sz);
	if (!fbuf) { sceIoClose(fd); return -2; }
	size_t got = 0;
	while (got < (size_t)sz) {
		int r = sceIoRead(fd, fbuf + got, (size_t)sz - got);
		if (r <= 0) break;
		got += r;
	}
	sceIoClose(fd);
	if (got != (size_t)sz) { dlog("font read %d/%d", (int)got, (int)sz); return -3; }

	/* FreeType keeps this pointer for the process lifetime, so it lives in the pool, not on the stack */
	font = vita2d_load_font_mem(fbuf, (unsigned int)sz);
	if (!font) { dlog("font load fail"); return -4; }
	dlog("gpu ready font=%dkb arena=%dMB used=%dMB ovf=%dKB", (int)(sz >> 10),
	     (int)(arena_total >> 20), (int)(gpu_arena_used() >> 20), (int)(arena_ovf >> 10));
	return 0;
}

void gpu_frame_begin(void) {
	vita2d_start_drawing();
	vita2d_clear_screen();
}

void gpu_frame_end(void) {
	vita2d_end_drawing();
	/* Must run with NO scene open: the common dialog issues its own render passes, and nesting
	   them inside an open scene hangs the GPU a few frames later (measured on device). */
	vita2d_common_dialog_update();
	vita2d_swap_buffers(); /* vsync-paced */
}
