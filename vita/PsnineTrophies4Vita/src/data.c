// Real data engine: one page thread handles psnine requests + gumbo parsing,
// while several icon threads download and decode in parallel (32 covers per page; serial would take far too long).
// The main thread only posts requests and polls the "done" flags, then swaps staging wholesale into live,
// so every rendered frame reads a complete, consistent array; a slow network only delays text by a frame, never stalls the screen.
//
// Icon formats measured (2026-10-11, captured directly on Mac):
//   psnobj.prod.dl.playstation.net covers return JPEG only, regardless of the Accept header;
//   image.api.playstation.com trophy images default to AVIF; must append ?format=png to get PNG.
// Plain HTTP still answers 200 on both CDNs, so icon urls keep their https -> http rewrite;
// psnine itself has moved to https only (it 302s plain http), and pages follow it there.
#include "data.h"
#include "net.h"
#include "gpu.h"

#include <psp2/kernel/threadmgr.h>

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>
#include <png.h>

extern void dlog(const char *fmt, ...);

#define FETCH_CAP (512 * 1024)
#define SCRATCH_PX (512u * 512u) /* decode scratch cap: 512x512 RGBA = 1MB */
#define COVER_W IC_W
#define COVER_H 84
#define CUP_W IC_CUP_W
#define CUP_H IC_CUP_W
#define SLOT_N 48 /* 32 covers per page + trophy icon rotation; spare room means fewer evictions */
#define IQ_N 160 /* icon job queue: one full screen of covers + a prefetched page */
#define W_N 3    /* thread count: #0 runs pages and icons, 1..N run icons only */
/* icon receive buffer: psnobj trophy PNGs measured 285~346KB (512x512 RGBA lossless);
   320KB would truncate the image and libpng reports "read beyond end of data" */
#define ICON_CAP (512 * 1024)

enum { ST_FREE = 0, ST_WAIT, ST_READY, ST_FAIL };
enum { J_LIST = 0, J_GAME = 1, J_TIPS = 2, J_ICON = 3 };

typedef struct {
	uint64_t hash;
	unsigned char state;
	unsigned char tries; /* failure retries: an occasional CDN drop shouldn't leave a cover black forever */
	short w, h;
	uint32_t *px;
	unsigned age;
} Slot;

typedef struct {
	int kind;
	long id;
	int seq;
	char url[256];
} Job;

static SceUID mtx, tid[W_N];
static int widx[W_N]; /* thread entry args can only be pointers; park the index here */
static Job hi[3];        /* page-level requests: keep only the latest one per kind */
static int hi_wait[3];   /* posted but not yet picked up by a worker */
static int hi_seq[3];
static Job iq[IQ_N];
static int ihead, itail;
static Slot slots[IC_KIND_N][SLOT_N];
static unsigned aged;

static char *w_fbuf[W_N];              /* per-thread private receive buffer (HTML or image) */
static uint32_t *w_scratch[W_N];       /* per-thread private full-size decode buffer */
static void *w_cmap[W_N];              /* landing buffer for 256-color palette of palettized PNGs */
static uint32_t *w_tmpic[W_N][IC_KIND_N];
static uint32_t *phic[IC_KIND_N];
static int slot_px[IC_KIND_N];

/* parse results land in staging first; the main thread swaps them into live only after validating seq */
static PsGame st_list[PS_GAMES];
static PsGame st_head;
static PsGroup st_group[PS_GROUPS];
static int st_group_n;
static PsTrophy st_trophy[PS_TROPHIES];
static PsTip st_tip[PS_TIPS];

/* worker -> main thread: one page result slot + icon completion counter */
static volatile int pg_done = -1; /* J_LIST/J_GAME/J_TIPS, -1 = empty */
static int pg_run = -1;           /* kind the worker is currently fetching; set when the job is taken, leaving no idle window */
static long pg_id;
static int pg_seq, pg_n, pg_ng, pg_err;
static volatile int icon_done;
static int ic_fly; /* icons dequeued and currently downloading/decoding (multi-threaded; lock before touching) */
static int ic_pause; /* stop draining the icon queue while the IME is up: decoding and scaling saturate both cores, making the keyboard drop frames */

PsGame dt_list[DT_LIST_MAX];
int dt_list_n, dt_list_page, dt_list_mode, dt_list_more;
PsGame dt_head;
PsGroup dt_group[PS_GROUPS];
int dt_group_n;
PsTrophy dt_trophy[PS_TROPHIES];
int dt_trophy_n;
PsTip dt_tip[PS_TIPS];
int dt_tip_n;
long dt_head_id, dt_tips_for;

static int live_err;

static void lock(void) { sceKernelLockMutex(mtx, 1, NULL); }
static void unlock(void) { sceKernelUnlockMutex(mtx, 1); }

static uint64_t url_hash(const char *s) {
	uint64_t h = 1469598103934665603ull;
	for (; *s; s++) {
		h ^= (unsigned char)*s;
		h *= 1099511628211ull;
	}
	return h;
}

/* ---------- request posting ---------- */
static void post(int kind, long id, const char *url) {
	lock();
	hi_seq[kind]++;
	hi[kind].kind = kind;
	hi[kind].id = id;
	hi[kind].seq = hi_seq[kind];
	snprintf(hi[kind].url, sizeof(hi[kind].url), "%s", url);
	hi_wait[kind] = 1;
	unlock();
}

void data_request_list(int page) {
	char url[128];
	snprintf(url, sizeof(url), "https://www.psnine.com/psngame?page=%d", page < 1 ? 1 : page);
	post(J_LIST, page, url);
}

/* the site search box is just /psngame?title=keyword, same template as the list page; id 0 means replace the whole result table */
void data_request_search(const char *kw) {
	char enc[200], url[240];
	net_url_encode(kw, enc, sizeof(enc));
	snprintf(url, sizeof(url), "https://www.psnine.com/psngame?title=%s", enc);
	post(J_LIST, 0, url);
}

void data_request_game(long game_id) {
	char url[128];
	snprintf(url, sizeof(url), "https://www.psnine.com/psngame/%ld", game_id);
	post(J_GAME, game_id, url);
}

void data_request_tips(long trophy_id) {
	char url[128];
	snprintf(url, sizeof(url), "https://www.psnine.com/trophy/%ld", trophy_id);
	post(J_TIPS, trophy_id, url);
}

/* ---------- icon cache ---------- */
static int slot_find(int kind, uint64_t hs) {
	for (int i = 0; i < SLOT_N; i++)
		if (slots[kind][i].hash == hs && slots[kind][i].state != ST_FREE) return i;
	return -1;
}

/* pick a free slot first; only evict least-recently-used when full */
static int slot_victim(int kind) {
	Slot *a = slots[kind];
	for (int i = 0; i < SLOT_N; i++)
		if (a[i].state == ST_FREE) return i;
	int v = 0;
	for (int i = 1; i < SLOT_N; i++)
		if (a[i].age < a[v].age) v = i;
	return v;
}

/* on queue overflow, drop the oldest queued request: its slot must go back to free, otherwise the image never arrives */
static void icon_drop_job(Job *j) {
	int k = (int)j->id;
	if (k < 0 || k >= IC_KIND_N) return;
	uint64_t hs = url_hash(j->url);
	int i = slot_find(k, hs);
	if (i >= 0) slots[k][i].state = ST_FREE;
}

/* enqueue writes at the tail, dequeue reads the head: writing at the head would overwrite pending entries with an empty URL */
static void iq_push(int kind, const char *url) {
	if ((itail + 1) % IQ_N == ihead) {
		Job *old = &iq[ihead];
		icon_drop_job(old);
		ihead = (ihead + 1) % IQ_N;
	}
	Job *j = &iq[itail];
	j->kind = 0;
	j->id = kind; /* queue item id stores the icon kind */
	snprintf(j->url, sizeof(j->url), "%s", url);
	itail = (itail + 1) % IQ_N;
}

const uint32_t *data_icon(int kind, const char *url, int *w, int *h) {
	int dw = kind == IC_COVER ? COVER_W : CUP_W;
	int dh = kind == IC_COVER ? COVER_H : CUP_H;
	*w = dw;
	*h = dh;
	if (kind < 0 || kind >= IC_KIND_N || !url || !url[0]) return phic[IC_COVER];
	uint64_t hs = url_hash(url);
	lock();
	int i = slot_find(kind, hs);
	if (i >= 0) {
		Slot *s = &slots[kind][i];
		s->age = ++aged;
		if (s->state == ST_READY) {
			*w = s->w;
			*h = s->h;
			uint32_t *px = s->px;
			unlock();
			return px;
		}
		/* give a failed slot another chance: one timeout must not leave this image black forever */
		if (s->state == ST_FAIL && s->tries < 2) {
			s->tries++;
			s->state = ST_WAIT;
			iq_push(kind, url);
		}
		unlock(); /* WAIT=still in flight, FAIL=placeholder awaiting retry */
		return phic[kind];
	}
	int v = slot_victim(kind);
	Slot *s = &slots[kind][v];
	s->hash = hs;
	s->state = ST_WAIT;
	s->tries = 0;
	s->age = ++aged;
	s->w = (short)dw;
	s->h = (short)dh;
	iq_push(kind, url);
	unlock();
	return phic[kind];
}

/* state values map one-to-one to internal ST_*; no entry means DI_NONE (never requested) */
int data_icon_state(int kind, const char *url) {
	if (kind < 0 || kind >= IC_KIND_N || !url || !url[0]) return DI_NONE;
	lock();
	int i = slot_find(kind, url_hash(url));
	int st = i < 0 ? DI_NONE : slots[kind][i].state;
	unlock();
	return st;
}

/* ---------- scaling ---------- */
static void halve(uint32_t *px, int pitch, int *w, int *h) {
	int dw = *w / 2, dh = *h / 2;
	if (dw < 1 || dh < 1) return;
	for (int y = 0; y < dh; y++) {
		uint32_t *d = px + (size_t)y * pitch;
		const uint32_t *r0 = px + (size_t)(2 * y) * pitch;
		const uint32_t *r1 = px + (size_t)(2 * y + 1) * pitch;
		for (int x = 0; x < dw; x++) {
			uint32_t a = r0[2 * x], b = r0[2 * x + 1], c = r1[2 * x], e = r1[2 * x + 1];
			uint32_t rr = ((a & 0xff) + (b & 0xff) + (c & 0xff) + (e & 0xff)) >> 2;
			uint32_t gg = (((a >> 8) & 0xff) + ((b >> 8) & 0xff) + ((c >> 8) & 0xff) + ((e >> 8) & 0xff)) >> 2;
			uint32_t bb = (((a >> 16) & 0xff) + ((b >> 16) & 0xff) + ((c >> 16) & 0xff) + ((e >> 16) & 0xff)) >> 2;
			uint32_t aa = (((a >> 24) & 0xff) + ((b >> 24) & 0xff) + ((c >> 24) & 0xff) + ((e >> 24) & 0xff)) >> 2;
			d[x] = rr | (gg << 8) | (bb << 16) | (aa << 24);
		}
	}
	*w = dw;
	*h = dh;
}

/* rounded-corner mask: covers are JPEG without alpha; applying the native icon rounding keeps them from looking like a pasted-on patch */
static void round_alpha(uint32_t *px, int w, int h, int r) {
	for (int y = 0; y < h; y++) {
		int dy = y < r ? r - y : (y >= h - r ? y - h + r + 1 : 0);
		if (dy <= 0) continue;
		for (int x = 0; x < w; x++) {
			int dx = x < r ? r - x : (x >= w - r ? x - w + r + 1 : 0);
			if (dx <= 0) continue;
			if (dx * dx + dy * dy > r * r) px[(size_t)y * w + x] = 0;
		}
	}
}

/* scale proportionally into a dw x dh box and center; source is stored with pitch row stride, halved in place first, then box-averaged */
static void fit_into(uint32_t *src, int sw, int sh, int pitch, uint32_t *dst,
                     int dw, int dh, int round) {
	memset(dst, 0, (size_t)dw * dh * 4);
	if (sw <= 0 || sh <= 0) return;
	int w = sw, h = sh;
	while ((w > dw * 2 || h > dh * 2) && w > 2 && h > 2) halve(src, pitch, &w, &h);
	int ow, oh;
	if ((long)w * dh > (long)h * dw) {
		oh = dh;
		ow = (int)((long)dh * w / h);
	} else {
		ow = dw;
		oh = (int)((long)dw * h / w);
	}
	if (ow > dw) ow = dw;
	if (oh > dh) oh = dh;
	if (ow < 1 || oh < 1) return;
	int ox = (dw - ow) / 2, oy = (dh - oh) / 2;
	for (int y = 0; y < oh; y++) {
		int sy0 = (int)((long)y * h / oh), sy1 = (int)((long)(y + 1) * h / oh);
		if (sy1 <= sy0) sy1 = sy0 + 1;
		if (sy1 > h) sy1 = h;
		for (int x = 0; x < ow; x++) {
			int sx0 = (int)((long)x * w / ow), sx1 = (int)((long)(x + 1) * w / ow);
			if (sx1 <= sx0) sx1 = sx0 + 1;
			if (sx1 > w) sx1 = w;
			unsigned long r = 0, g = 0, b = 0, a = 0, n = 0;
			for (int yy = sy0; yy < sy1; yy++) {
				const uint32_t *row = src + (size_t)yy * pitch;
				for (int xx = sx0; xx < sx1; xx++) {
					uint32_t p = row[xx];
					unsigned pa = (p >> 24) & 0xff;
					a += pa;
					r += (unsigned long)(p & 0xff) * pa;
					g += (unsigned long)((p >> 8) & 0xff) * pa;
					b += (unsigned long)((p >> 16) & 0xff) * pa;
					n++;
				}
			}
			if (!a) continue; /* fully transparent: leave empty */
			dst[(size_t)(y + oy) * dw + x + ox] =
			    (uint32_t)(r / a) | ((uint32_t)(g / a) << 8) | ((uint32_t)(b / a) << 16) |
			    ((uint32_t)(a / n) << 24);
		}
	}
	if (round) round_alpha(dst, dw, dh, 8);
}

/* ---------- decode ---------- */
typedef struct {
	struct jpeg_error_mgr pub;
	jmp_buf jb;
} Jerr;

static void jerr_exit(j_common_ptr c) {
	longjmp(((Jerr *)c->err)->jb, 1);
}

static int decode_jpeg(const char *bytes, int len, uint32_t *out, int *ow, int *oh) {
	struct jpeg_decompress_struct ci;
	Jerr je;
	ci.err = jpeg_std_error(&je.pub);
	if (setjmp(je.jb)) {
		jpeg_destroy_decompress(&ci);
		return -1;
	}
	jpeg_create_decompress(&ci);
	je.pub.error_exit = jerr_exit;
	jpeg_mem_src(&ci, (const unsigned char *)bytes, (unsigned long)len);
	if (jpeg_read_header(&ci, TRUE) != JPEG_HEADER_OK) {
		jpeg_destroy_decompress(&ci);
		return -2;
	}
	unsigned long px = (unsigned long)ci.image_width * ci.image_height;
	int d = 1;
	while (d < 8 && (px / (unsigned)d > SCRATCH_PX || ci.image_width / d > 2048)) d *= 2;
	ci.scale_num = 1;
	ci.scale_denom = d;
	jpeg_start_decompress(&ci);
	int w = (int)ci.output_width, h = (int)ci.output_height;
	if ((unsigned long)w * h > SCRATCH_PX) {
		jpeg_destroy_decompress(&ci);
		return -3;
	}
	unsigned char rowb[2048 * 3];
	JSAMPROW rp = rowb;
	for (int y = 0; y < h; y++) {
		jpeg_read_scanlines(&ci, &rp, 1);
		uint32_t *drow = out + (size_t)y * w;
		for (int x = w - 1; x >= 0; x--)
			drow[x] = rowb[x * 3] | ((uint32_t)rowb[x * 3 + 1] << 8) |
			          ((uint32_t)rowb[x * 3 + 2] << 16) | 0xff000000u;
	}
	jpeg_finish_decompress(&ci);
	jpeg_destroy_decompress(&ci);
	*ow = w;
	*oh = h;
	return 0;
}

static int decode_png(const char *bytes, int len, uint32_t *out, int *ow, int *oh,
                      void *cmap) {
	png_image im;
	memset(&im, 0, sizeof(im));
	/* the simplified API requires filling version first; leaving it 0 makes begin_read fail outright (hit this on real hardware) */
	im.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_memory(&im, bytes, (size_t)len)) return -1;
	unsigned w = im.width, h = im.height;
	if (!w || !h || (unsigned long)w * h > SCRATCH_PX) return -2;
	im.format = PNG_FORMAT_RGBA;
	/* PSN trophy icons are 256-color palette PNGs: palettized images must be given a landing buffer;
	   passing NULL makes finish_read fail outright (root cause of icon decode -3 on device).
	   256 entries x RGBA = 1024 bytes, enough for any output format. */
	if (!png_image_finish_read(&im, NULL, out, (png_int_32)(w * 4), cmap)) {
		dlog("png err %s", im.message);
		return -3;
	}
	*ow = (int)w;
	*oh = (int)h;
	return 0;
}

static void icon_mark(uint64_t hs, int kind, int ready) {
	lock();
	int i = slot_find(kind, hs);
	if (i < 0) {
		i = slot_victim(kind);
		slots[kind][i].hash = hs;
		slots[kind][i].tries = 0; /* slot now holds a new image; reset the retry count */
		slots[kind][i].w = (short)(kind == IC_COVER ? COVER_W : CUP_W);
		slots[kind][i].h = (short)(kind == IC_COVER ? COVER_H : CUP_H);
	}
	slots[kind][i].state = (unsigned char)(ready ? ST_READY : ST_FAIL);
	slots[kind][i].age = ++aged;
	unlock();
}

/* me = index of the working thread, which also selects the keep-alive lane (threads must not share a connection) */
static void load_icon(int kind, const char *url, int me) {
	char src[280];
	char *fbuf = w_fbuf[me];
	uint32_t *scratch = w_scratch[me];
	if (strncmp(url, "https://", 8) == 0) snprintf(src, sizeof(src), "http://%s", url + 8);
	else snprintf(src, sizeof(src), "%s", url);
	/* image.api.playstation.com serves AVIF/WebP by default; explicitly requesting PNG yields something libpng can decode */
	if (strstr(src, "image.api.playstation.com") && !strstr(src, "?")) {
		int n = (int)strlen(src);
		if (n + 12 < (int)sizeof(src)) snprintf(src + n, sizeof(src) - n, "?format=png");
	}
	uint64_t hs = url_hash(url);
	int len = net_fetch_lane(src, fbuf, ICON_CAP, me);
	if (len <= 0) {
		dlog("icon fetch %x %s", (unsigned)len, src);
		icon_mark(hs, kind, 0);
		return;
	}
	int w = 0, h = 0;
	int r = -10;
	const unsigned char *b = (const unsigned char *)fbuf;
	if (len > 8 && b[0] == 0x89 && b[1] == 'P') r = decode_png(fbuf, len, scratch, &w, &h, w_cmap[me]);
	else if (len > 3 && b[0] == 0xff && b[1] == 0xd8) r = decode_jpeg(fbuf, len, scratch, &w, &h);
	if (r < 0) {
		dlog("icon decode %d len=%d type=%02x%02x%02x", r, len, b[0], b[1], b[2]);
		icon_mark(hs, kind, 0);
		return;
	}
	int dw = kind == IC_COVER ? COVER_W : CUP_W;
	int dh = kind == IC_COVER ? COVER_H : CUP_H;
	fit_into(scratch, w, h, w, w_tmpic[me][kind], dw, dh, kind == IC_COVER);
	lock();
	int i = slot_find(kind, hs);
	if (i < 0) i = slot_victim(kind);
	Slot *s = &slots[kind][i];
	memcpy(s->px, w_tmpic[me][kind], (size_t)dw * dh * 4);
	s->hash = hs;
	s->w = (short)dw;
	s->h = (short)dh;
	s->state = ST_READY;
	s->age = ++aged;
	icon_done++;
	unlock();
}

/* ---------- worker ---------- */
static void run_page(Job *j) {
	char *fbuf = w_fbuf[0];
	int err = 0, n = 0, ng = 0;
	int len = net_fetch(j->url, fbuf, FETCH_CAP);
	if (len <= 0) err = len;
	else if (j->kind == J_LIST) {
		n = ps_parse_games(fbuf, st_list, PS_GAMES);
		if (n < 0) err = -900;
	} else if (j->kind == J_GAME) {
		memset(&st_head, 0, sizeof(st_head));
		st_group_n = 0;
		n = ps_parse_game(fbuf, &st_head, st_group, PS_GROUPS, &ng, st_trophy, PS_TROPHIES, NULL);
		if (n < 0) err = -901;
	} else {
		n = ps_parse_tips(fbuf, st_tip, PS_TIPS);
		if (n < 0) err = -902;
	}
	lock();
	pg_done = j->kind;
	pg_id = j->id;
	pg_seq = j->seq;
	pg_n = n;
	pg_ng = ng;
	pg_err = err;
	unlock();
	dlog("page kind=%d id=%ld n=%d err=%x", j->kind, j->id, n, (unsigned)err);
}

static int worker(SceSize args, void *argp) {
	(void)args;
	int me = 0;
	if (argp) memcpy(&me, argp, sizeof(me)); /* thread #0 does pages + icons, the rest take icons only */
	for (;;) {
		Job j;
		int got = 0;
		lock();
		if (me == 0 && pg_done < 0) { /* don't parse a new page until the previous result is consumed, avoiding staging corruption */
			for (int k = J_TIPS; k >= J_LIST; k--) {
				if (!hi_wait[k]) continue;
				j = hi[k];
				hi_wait[k] = 0;
				pg_run = k; /* the gap between unlocking and actually starting also counts as in-flight, so the main thread doesn't misjudge it as idle */
				got = 1;
				break;
			}
		}
		if (!got && !ic_pause && ihead != itail) {
			j = iq[ihead];
			ihead = (ihead + 1) % IQ_N;
			j.kind = J_ICON;
			got = 1;
		}
		unlock();
		if (!got) {
			sceKernelDelayThread(15 * 1000);
			continue;
		}
		if (j.kind == J_ICON) {
			lock();
			ic_fly++;
			unlock();
			load_icon((int)j.id, j.url, me);
			lock();
			ic_fly--;
			unlock();
		} else {
			dlog("get k=%d id=%ld", j.kind, j.id);
			run_page(&j);
			lock();
			pg_run = -1;
			unlock();
		}
	}
	return 0;
}

int data_busy(void) {
	lock();
	int n = 0;
	for (int k = 0; k < 3; k++) n += hi_wait[k];
	n += (itail - ihead + IQ_N) % IQ_N + ic_fly;
	n += pg_done >= 0 || pg_run >= 0 ? 1 : 0;
	unlock();
	return n;
}

/* whether a page request of this kind is already queued/running: the main thread uses it to decide whether to post again,
   so the first request is never missed and a single failure won't leave the list empty forever */
int data_pending(int kind) {
	lock();
	int n = hi_wait[kind] || pg_run == kind || (pg_done == kind);
	unlock();
	return n;
}

int data_err(void) { return live_err; }

void data_icons_paused(int on) {
	lock();
	ic_pause = on ? 1 : 0;
	unlock();
}

int data_poll(void) {
	int ch = 0, ic = 0;
	lock();
	static int seen_icons;
	if (icon_done != seen_icons) {
		seen_icons = icon_done;
		ic = DC_ICON;
	}
	int kind = pg_done;
	if (kind < 0) {
		unlock();
		return ic;
	}
	long id = pg_id;
	int seq = pg_seq, n = pg_n, ng = pg_ng, err = pg_err;
	pg_done = -1;
	unlock();

	if (seq != hi_seq[kind]) return ic | 0; /* superseded by a newer request */
	live_err = err;
	ch |= DC_STATE;
	if (err < 0) return ch;

	if (kind == J_LIST) {
		int p = (int)id; /* 0 = search results */
		int searching = p <= 0;
		int at = 0;
		/* a page pulled at the bottom appends to the tail; page 1 and search results replace the whole table */
		if (!searching && p > 1 && !dt_list_mode && dt_list_page == p - 1 && dt_list_n > 0 &&
		    dt_list_n < DT_LIST_MAX)
			at = dt_list_n;
		int room = DT_LIST_MAX - at;
		int cnt = n < room ? n : room;
		memcpy(dt_list + at, st_list, (size_t)cnt * sizeof(PsGame));
		dt_list_n = at + cnt;
		dt_list_page = searching ? 1 : p;
		dt_list_mode = searching;
		/* less than a full page means the site is exhausted; also stop at the 256-entry cap */
		dt_list_more = !searching && n >= PS_GAMES && dt_list_n < DT_LIST_MAX;
		ch |= DC_LIST;
	} else if (kind == J_GAME) {
		dt_head = st_head;
		memcpy(dt_group, st_group, sizeof(dt_group));
		memcpy(dt_trophy, st_trophy, sizeof(dt_trophy));
		dt_group_n = ng;
		dt_trophy_n = n;
		dt_head_id = id;
		ch |= DC_GAME;
	} else {
		memcpy(dt_tip, st_tip, sizeof(dt_tip));
		dt_tip_n = n > PS_TIPS ? PS_TIPS : n;
		dt_tips_for = id;
		ch |= DC_TIPS;
	}
	return ch | ic;
}

int data_init(void) {
	w_fbuf[0] = gpu_arena_alloc(FETCH_CAP);
	if (!w_fbuf[0]) return -1;
	for (int i = 0; i < W_N; i++) {
		w_scratch[i] = gpu_arena_alloc((size_t)SCRATCH_PX * 4);
		if (!w_scratch[i]) return -1;
		w_cmap[i] = gpu_arena_alloc(256 * 4);
		if (!w_cmap[i]) return -1;
		if (i > 0) {
			w_fbuf[i] = gpu_arena_alloc(ICON_CAP);
			if (!w_fbuf[i]) return -1;
		}
	}
	slot_px[IC_COVER] = COVER_W * COVER_H;
	slot_px[IC_CUP] = CUP_W * CUP_H;
	for (int k = 0; k < IC_KIND_N; k++) {
		phic[k] = gpu_arena_alloc0((size_t)slot_px[k] * 4);
		if (!phic[k]) return -2;
		for (int i = 0; i < W_N; i++) {
			w_tmpic[i][k] = gpu_arena_alloc((size_t)slot_px[k] * 4);
			if (!w_tmpic[i][k]) return -2;
		}
		int dw = k == IC_COVER ? COVER_W : CUP_W;
		int dh = k == IC_COVER ? COVER_H : CUP_H;
		for (int i = 0; i < slot_px[k]; i++) phic[k][i] = 0x90343438u; /* neutral gray placeholder, so it doesn't look like a black block */
		round_alpha(phic[k], dw, dh, 8);
		for (int i = 0; i < SLOT_N; i++) {
			slots[k][i].px = gpu_arena_alloc0((size_t)slot_px[k] * 4);
			slots[k][i].state = ST_FREE;
			slots[k][i].w = (short)dw;
			slots[k][i].h = (short)dh;
			if (!slots[k][i].px) return -3;
		}
	}
	mtx = sceKernelCreateMutex("datamtx", 0, 0, NULL);
	if (mtx < 0) return -4;
	/* initialize networking before starting threads: net_init has no concurrency protection of its own */
	int nr = net_init();
	dlog("net init=%x", (unsigned)nr);
	if (nr < 0) return -6;
	for (int i = 0; i < W_N; i++) {
		widx[i] = i;
		tid[i] = sceKernelCreateThread("dataworker", worker, 0x10000100, 192 * 1024, 0, 0, NULL);
		if (tid[i] < 0) return -5;
		sceKernelStartThread(tid[i], sizeof(int), &widx[i]);
	}
	return 0;
}
