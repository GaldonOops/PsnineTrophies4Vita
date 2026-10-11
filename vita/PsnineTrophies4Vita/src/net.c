// SceHttp wrapper: NET sysmodule init + blocking GET + URL encoding
// Conclusion carried over from task 1a: SceHttp works over plain HTTP, TLS does not (see memory vitasdk-dev-env)
// One page = 32 covers = 32 requests; creating a new template+connection each time pays DNS+TCP again, which was tens of seconds slow on device,
// so tpl/conn are cached per host to reuse keep-alive; on error the connection for that host is dropped and rebuilt next time.
#include "net.h"

#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/net/http.h>
#include <psp2/sysmodule.h>
#include <psp2/kernel/processmgr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NET_POOL  (2 * 1024 * 1024)
#define HTTP_POOL (2 * 1024 * 1024)
#define UA        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) psnine-vita"
#define CONN_N    3            /* hosts cached per lane: psnine + two CDNs, enough */
#define LANE_N    4            /* connection lanes: one for the page thread, the rest for background icon threads */

static void *net_memory;
static int net_inited;
static int http_inited;

typedef struct {
	char host[104];
	SceUID tpl, conn;
} Conn;

/* a lane is used by exactly one thread: libhttp connections are not thread-safe,
   so parallel downloads must each use their own connection; this is why pages and icons use separate lanes. */
static Conn conns[LANE_N][CONN_N];

int net_init(void) {
	if (net_inited) return 0;
	if (sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0) return -1;
	/* libhttp is provided by the HTTPS module; loading only NET makes sceHttpInit crash on a null jump table (C2-12828-1) */
	if (sceSysmoduleLoadModule(SCE_SYSMODULE_HTTPS) < 0) return -2;
	net_memory = malloc(NET_POOL);
	if (!net_memory) return -2;
	SceNetInitParam param = { .memory = net_memory, .size = NET_POOL, .flags = 0 };
	if (sceNetInit(&param) < 0) return -3;
	if (sceNetCtlInit() < 0) return -4;
	if (sceHttpInit(HTTP_POOL) < 0) return -5;
	net_inited = 1;
	http_inited = 1;
	return 0;
}

/* extract host from scheme://host/... (no port; sufficient) */
static int url_host(const char *url, char *out, int cap) {
	const char *p = strstr(url, "://");
	if (!p) return -1;
	p += 3;
	int n = 0;
	while (p[n] && p[n] != '/' && p[n] != '?' && p[n] != ':' && n < cap - 1) n++;
	if (!n) return -1;
	memcpy(out, p, n);
	out[n] = 0;
	return 0;
}

static void conn_drop(Conn *c) {
	if (c->conn >= 0) sceHttpDeleteConnection(c->conn);
	if (c->tpl >= 0) sceHttpDeleteTemplate(c->tpl);
	c->conn = c->tpl = -1;
	c->host[0] = 0;
}

static Conn *conn_get(int lane, const char *base) {
	Conn *band = conns[lane];
	Conn *free_slot = NULL;
	for (int i = 0; i < CONN_N; i++) {
		if (band[i].host[0] && strcmp(band[i].host, base) == 0) return &band[i];
		if (!band[i].host[0] && !free_slot) free_slot = &band[i];
	}
	if (!free_slot) free_slot = &band[0]; /* when full, evict the first entry; just rebuild it */
	conn_drop(free_slot);
	free_slot->tpl = sceHttpCreateTemplate(UA, SCE_HTTP_VERSION_1_1, 1);
	if (free_slot->tpl < 0) { conn_drop(free_slot); return NULL; }
	sceHttpSetResolveTimeOut(free_slot->tpl, 10 * 1000 * 1000);
	sceHttpSetConnectTimeOut(free_slot->tpl, 10 * 1000 * 1000);
	sceHttpSetSendTimeOut(free_slot->tpl, 10 * 1000 * 1000);
	sceHttpSetRecvTimeOut(free_slot->tpl, 10 * 1000 * 1000);
	free_slot->conn = sceHttpCreateConnectionWithURL(free_slot->tpl, base, 0);
	if (free_slot->conn < 0) { conn_drop(free_slot); return NULL; }
	snprintf(free_slot->host, sizeof(free_slot->host), "%s", base);
	return free_slot;
}

int net_fetch_bin(const char *url, void *dst, int cap) {
	return net_fetch_lane(url, dst, cap, 0);
}

/* the caller guarantees lane belongs to its own thread; pages use 0, each icon worker takes one of the rest */
int net_fetch_lane(const char *url, void *dst, int cap, int lane) {
	if (lane < 0 || lane >= LANE_N) lane = 0;
	if (!net_inited) {
		int r = net_init();
		if (r < 0) return r;
	}
	char *buf = (char *)dst;
	char host[96], base[104];
	if (url_host(url, host, sizeof(host)) < 0) return SCE_HTTP_ERROR_INVALID_URL;
	snprintf(base, sizeof(base), "http://%s", host); /* plain HTTP only for now (TLS: see task #9) */

	for (int attempt = 0; attempt < 2; attempt++) {
		Conn *c = conn_get(lane, base);
		if (!c) return SCE_HTTP_ERROR_OUT_OF_MEMORY;
		SceUID req = sceHttpCreateRequestWithURL(c->conn, SCE_HTTP_METHOD_GET, url, 0);
		if (req < 0) { conn_drop(c); continue; } /* server closed the connection: rebuild and retry once */
		int ret = sceHttpSendRequest(req, NULL, 0);
		int total = 0;
		if (ret >= 0) {
			for (;;) {
				int n = sceHttpReadData(req, buf + total, cap - 1 - total);
				if (n < 0) { ret = n; break; }
				if (n == 0) break;
				total += n;
				if (total >= cap - 1) break;
			}
		}
		sceHttpDeleteRequest(req);
		if (ret < 0) { conn_drop(c); continue; }
		buf[total] = '\0';
		return total;
	}
	return SCE_HTTP_ERROR_NETWORK;
}

int net_fetch(const char *url, char *buf, int cap) {
	int n = net_fetch_bin(url, buf, cap);
	if (n >= 0 && n < cap) buf[n] = '\0';
	return n;
}

void net_url_encode(const char *s, char *out, int cap) {
	static const char hex[] = "0123456789ABCDEF";
	int o = 0;
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		unsigned char c = *p;
		int alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
		if (alnum) {
			if (o + 1 >= cap) break;
			out[o++] = (char)c;
		} else {
			if (o + 3 >= cap) break;
			out[o++] = '%';
			out[o++] = hex[c >> 4];
			out[o++] = hex[c & 0xf];
		}
	}
	if (o < cap) out[o] = '\0';
}
