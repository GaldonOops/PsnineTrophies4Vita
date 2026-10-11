#ifndef RENDER_H
#define RENDER_H

#include <stddef.h>
#include <stdint.h>

#define SCREEN_W 960
#define SCREEN_H 544
#define RENDER_PITCH 1024 /* framebuffer pitch in pixels, required by common dialog */
/* pixel value is 0xAABBGGRR (memory byte order R,G,B,A), matching A8B8G8R8 format */
#define RENDER_BG     0x00201820
#define RENDER_WHITE  0x00ffffff
#define RENDER_GRAY   0x00999999
#define RENDER_GREEN  0x0000ff00
#define RENDER_RED    0x000000ff

int  render_init(void);   /* allocates VRAM + loads app0:font.ttf, <0 on failure */
/* Large blocks: the app heap is only ~28MB, nearly gone after the 22.7MB font loads;
   FreeType's internal malloc then fails, leaving a null table pointer that crashes on access.
   Draw from a separate memblock pool here, leaving the heap for FreeType; fall back to malloc when exhausted. */
void *render_alloc(size_t n);   /* uninitialized */
void *render_alloc0(size_t n);  /* zeroed */
size_t render_arena_total(void);
/* bytes used in the pool / overflowed to malloc; startup log uses these to check memory budget */
size_t render_arena_used(void);
size_t render_arena_overflow(void);
void render_clear(void);  /* fill whole screen with background color */
/* draw one line of UTF-8 text at (x, baseline_y); baseline uses font coords with y-axis up */
void render_text(int x, int baseline_y, const char *utf8, uint32_t color, int px);
/* text width in pixels, used for layout and truncation */
int  render_text_width(const char *utf8, int px);
/* draw text but no wider than max_w; overflow is clipped with an ellipsis; returns drawn width */
int  render_text_clip(int x, int baseline_y, const char *utf8, uint32_t color, int px, int max_w);
/* wrapped text: \n forces a break, over-wide text wraps, up to max_lines lines (last line
   overflow gets ...). Returns the baseline_y available for the next text run. */
int  render_text_wrap(int x, int baseline_y, const char *utf8, uint32_t color, int px,
                      int max_w, int max_lines);
/* reading page: draws max_lines lines after skipping skip lines, total_lines returns full line count */
int  render_text_scroll(int x, int baseline_y, const char *utf8, uint32_t color, int px,
                        int max_w, int max_lines, int skip, int *total_lines);
void render_rect(int x, int y, int w, int h, uint32_t color); /* filled rectangle */
uint32_t *render_cur_fb(void); /* current draw target (canvas, pitch RENDER_PITCH) */
void render_set_target(uint32_t *t); /* switch to a temp canvas (used to bake row strips at startup) */
/* Bake mode: target is a transparent scratch canvas, so text must composite coverage into the alpha channel,
   otherwise the baked row texture is fully transparent and can't blend back at runtime. Only on during startup. */
void render_set_bake_alpha(int on);
void render_border(int x, int y, int w, int h, uint32_t color); /* 1px border */
/* canvas changes must be marked: on page turns only dirty rows are DMA'd into VRAM (VRAM is uncached, per-pixel writes are slow) */
void render_mark_dirty(int y0, int h);
void render_flip_now(void); /* DMA dirty rows + flip immediately */

/* common dialog (IME etc.) requires GXM to be initialized and hands our back draw page to it for overlay rendering.
   During a dialog you must use the two-step compose path below, not render_flip_now:
   that would contend with the GPU's compositing for the same page, giving a white screen with torn old rows. */
int   render_dialog_init(void);        /* <0 on failure */
void *render_backbuf(void);            /* page currently being drawn, valid until render_present */
void *render_backbuf_sync(void);       /* SceGxmSyncObject* for that page */

/* Two-step page turn for overlaid dialogs: begin lays the canvas into the next page and registers it with GXM (PadHeartbeat),
   the dialog draws onto the pending page, end swaps that page on-screen via the display queue at vsync.
   Used together they make the dialog appear one frame late, causing constant flicker. */
void render_compose_begin(void);
void render_compose_end(void);
void *render_pending_page(void);       /* valid after begin */
void *render_pending_sync(void);

#endif
