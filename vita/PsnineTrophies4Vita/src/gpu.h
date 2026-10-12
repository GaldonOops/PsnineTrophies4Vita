#ifndef PSNINE_GPU_H
#define PSNINE_GPU_H

/* GPU rendering layer on libvita2d, replacing the v0.9 CPU canvas.
   The whole frame is repainted from scratch every vsync, so there are no dirty rows,
   snapshots or baked row textures any more -- drawing order *is* the compositing order.

   Two conventions this layer enforces:
   - Colors are 0xAABBGGRR, same as the old canvas, but the alpha byte is meaningful now:
     an alpha of 0 means fully transparent, so opaque colors must carry 0xff.
   - vita2d blends with (SRC_ALPHA, ONE_MINUS_SRC_ALPHA), i.e. plain *straight* alpha, exactly
     like the old CPU canvas: no premultiplication anywhere. The tint color modulates all four
     channels, so fading alpha and squashing rgb need different helpers (see gpu_draw_a). */

#include <stddef.h>
#include <stdint.h>
#include <vita2d.h>

#define GPU_W 960
#define GPU_H 544
#define GPU_BG 0xff201820u /* the v0.9 backdrop color, with an opaque alpha byte */

/* arena + vita2d + font; <0 on failure */
int  gpu_init(void);
/* Frame tail order is fixed by the common dialog: end_drawing, then the dialog update
   (it needs a closed scene), then the swap. gpu_frame_end() does all three. */
void gpu_frame_begin(void);
void gpu_frame_end(void);

/* big memory pool (the app heap is only ~28MB and FreeType needs most of it) */
void *gpu_arena_alloc(size_t n);
void *gpu_arena_alloc0(size_t n);
size_t gpu_arena_total(void);
size_t gpu_arena_used(void);
size_t gpu_arena_overflow(void);

/* textures */
uint32_t *gpu_read_file(const char *path, size_t n);                /* arena copy of a raw asset, NULL on failure */
vita2d_texture *gpu_tex_file(const char *path, int w, int h);      /* raw w*h RGBA file from app0 assets */
vita2d_texture *gpu_tex_upload(const uint32_t *px, int w, int h);  /* straight-alpha pixels, copied verbatim */
vita2d_texture *gpu_tex_rt(int w, int h);                           /* render target for the page turn */
/* Paint a whole page into a render target: the target binds its own framebuffer, so the
   default page is untouched and presentation keeps working afterwards. */
void gpu_rt_begin(vita2d_texture *rt);
void gpu_rt_end(void);
/* icon textures decoded by data.c, cached here as textures keyed by url */
vita2d_texture *gpu_icon(int kind, const char *url, int *w, int *h);

void gpu_draw(int x, int y, const vita2d_texture *t);
/* alpha scaled to k/255 (touch flash, breathing selection mask, bubble focus) */
void gpu_draw_a(int x, int y, const vita2d_texture *t, int k);
void gpu_draw_part(int x, int y, const vita2d_texture *t, int tx, int ty, int tw, int th);
/* sub-rect with alpha scaled to k/255: masks that have to be clamped to a band */
void gpu_draw_part_a(int x, int y, const vita2d_texture *t, int tx, int ty, int tw, int th, int k);
void gpu_draw_part_shade_scale(const vita2d_texture *t, int x, int y, int tx, int ty, int tw,
                               int th, float y_scale, int shade);
/* filled rectangle; the color's alpha byte is the blend weight (scrollbar, bars) */
void gpu_rect(int x, int y, int w, int h, uint32_t color);

/* text: y is the baseline, same convention as before (font coords, y up) */
int  gpu_text(int x, int baseline_y, const char *utf8, uint32_t color, int px);
int  gpu_text_w(const char *utf8, int px);
/* overflow is replaced by an ellipsis; returns the drawn width */
int  gpu_text_clip(int x, int baseline_y, const char *utf8, uint32_t color, int px, int max_w);
/* \n breaks a line, over-wide text wraps, at most max_lines lines (last one ellipsized);
   returns the baseline y available for the next run */
int  gpu_text_wrap(int x, int baseline_y, const char *utf8, uint32_t color, int px,
                   int max_w, int max_lines);
/* reading page: draws max_lines lines after skipping skip, *total_lines gets the full line count */
int  gpu_text_scroll(int x, int baseline_y, const char *utf8, uint32_t color, int px,
                     int max_w, int max_lines, int skip, int *total_lines);

#endif
