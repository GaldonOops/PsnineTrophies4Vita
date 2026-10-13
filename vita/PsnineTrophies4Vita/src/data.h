#ifndef DATA_H
#define DATA_H

#include <stdint.h>

#include "parse.h"

/* Real data + fully async fetching middle layer:
   background threads only parse into staging; the main thread collects via data_poll() and only then swaps in live data,
   so the UI always reads complete arrays at any moment, never a half-written state. */

/* change bits returned by data_poll */
#define DC_LIST  1  /* level 1: game list */
#define DC_GAME  2  /* levels 2-3: game header + trophy groups + trophy table */
#define DC_TIPS  4  /* level 4: tips list */
#define DC_ICON  8  /* an icon finished decoding */
#define DC_STATE 16 /* in-flight count or error code changed */

/* icon kinds: determine cache slot size and decode cap */
enum { IC_COVER = 0, IC_CUP = 1, IC_KIND_N };
#define IC_W 154 /* outer box of both slot kinds; actual pixels are scaled proportionally and centered */
#define IC_CUP_W 90

int data_init(void);

/* Page-level requests are idempotent: nothing is re-sent when the target matches what is loaded/in flight.
   page starts at 1; game_id / trophy_id are psnine numeric ids.
   The list page is append-style: page>1 results append to the existing list tail, page==1 replaces the whole table. */
void data_request_list(int page);
void data_request_search(const char *kw); /* search results replace the level-1 list wholesale */
void data_request_game(long game_id);
void data_request_tips(long trophy_id);

/* Get icon pixels: on cache hit returns the real image, otherwise registers an async request and returns the placeholder.
   The returned pointer targets a fixed cache slot; usable for the whole frame, but do not keep it across frames. */
const uint32_t *data_icon(int kind, const char *url, int *w, int *h);

/* icon state: query first if you want to swap the placeholder for a different fallback image */
enum { DI_NONE = 0, DI_WAIT, DI_READY, DI_FAIL };
int data_icon_state(int kind, const char *url);

int data_poll(void); /* returns DC_* bits, 0 = nothing happened */
int data_busy(void);
int data_err(void); /* latest error code, 0=OK */
void data_icons_paused(int on); /* pause icon download/decode while the IME is up; page requests unaffected */

/* whether a page request of this kind is queued/running/result not yet collected: drives the re-post when the list is empty */
enum { DP_LIST = 0, DP_GAME = 1, DP_TIPS = 2 };
int data_pending(int kind);

/* live data (read-only from the main thread) */
#define DT_LIST_MAX 256 /* cap for scroll-append: 8 pages; beyond that, memory and the scrollbar both become pointless */
extern PsGame dt_list[DT_LIST_MAX];
extern int dt_list_n, dt_list_page;
extern int dt_list_mode;  /* 0=latest list (appendable) 1=search results (not appendable) */
extern int dt_list_more;  /* whether another page can still be pulled down */
extern PsGame dt_head;
extern PsGroup dt_group[PS_GROUPS];
extern int dt_group_n;
extern PsTrophy dt_trophy[PS_TROPHIES];
extern int dt_trophy_n;
extern PsTip dt_tip[PS_TIPS];
extern int dt_tip_n;
extern long dt_head_id;   /* currently loaded game */
extern long dt_tips_for;  /* trophy whose tips are loaded */

#endif
