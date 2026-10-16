// SceHttp wrapper: NET/SSL sysmodule init + blocking GET + URL encoding
// HTTPS does work on device: sceSslInit plus a PEM root anchor registered at boot (net_trust_root).
// Task 1a concluded "SceHttp does not do TLS" only because that init call was missing.
// One page = 32 covers = 32 requests; creating a new template+connection each time pays DNS+TCP again, which was tens of seconds slow on device,
// so tpl/conn are cached per scheme://host to reuse keep-alive; on error the connection for that host is dropped and rebuilt next time.
#include "net.h"

#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/net/http.h>
#include <psp2/libssl.h>
#include <psp2/sysmodule.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NET_POOL  (2 * 1024 * 1024)
#define HTTP_POOL (2 * 1024 * 1024)
#define SSL_POOL  (1 * 1024 * 1024)
#define UA        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) psnine-vita"
#define CONN_N    3            /* hosts cached per lane: psnine + two CDNs, enough */
#define LANE_N    4            /* connection lanes: one for the page thread, the rest for background icon threads */

extern void dlog(const char *fmt, ...);

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

/* psnine's chain is WoTrus DV -> Sectigo R46 -> USERTrust RSA and the Vita's root store (frozen around 2011)
   knows none of them: the handshake completes, then dies with detail 0x20 (SSL_UNKNOWN_CA). Registering the
   root the server itself sends fixes that, so verification stays on -- no need to disable it.
   The anchor has to be PEM: handing libhttp the same certificate as DER returns SCE_HTTP_ERROR_SSL. */
#define ROOT_PEM "app0:certs/usertrust_rsa.pem"

static void net_trust_root(void) {
	SceUID fd = sceIoOpen(ROOT_PEM, SCE_O_RDONLY, 0);
	if (fd < 0) { dlog("cert open %s =0x%x", ROOT_PEM, fd); return; }
	SceOff sz = sceIoLseek(fd, 0, SCE_SEEK_END);
	sceIoLseek(fd, 0, SCE_SEEK_SET);
	if (sz <= 0 || sz > 64 * 1024) { dlog("cert size %d", (int)sz); sceIoClose(fd); return; }
	char *buf = malloc((size_t)sz);
	if (!buf) { sceIoClose(fd); return; }
	if (sceIoRead(fd, buf, (int)sz) != (int)sz) { free(buf); sceIoClose(fd); dlog("cert short read"); return; }
	sceIoClose(fd);
	/* libhttp wants a list of pointers, not an array of structs; buf stays referenced by the ssl layer */
	SceHttpsData ca = { .ptr = buf, .size = (unsigned int)sz };
	const SceHttpsData *ca_list[1] = { &ca };
	dlog("cert anchor ret=%x", (unsigned)sceHttpsLoadCert(1, ca_list, NULL, NULL));
}

int net_init(void) {
	if (net_inited) return 0;
	if (sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0) return -1;
	/* libhttp is provided by the HTTPS module; loading only NET makes sceHttpInit crash on a null jump table (C2-12828-1) */
	if (sceSysmoduleLoadModule(SCE_SYSMODULE_HTTPS) < 0) return -2;
	/* the SSL module backs libhttp's https; without sceSslInit every TLS request dies before it dials */
	int ssl_mod = sceSysmoduleLoadModule(SCE_SYSMODULE_SSL);
	int ssl_r = sceSslInit(SSL_POOL);
	dlog("net ssl mod=%d init=%x", ssl_mod, (unsigned)ssl_r);
	net_memory = malloc(NET_POOL);
	if (!net_memory) return -2;
	SceNetInitParam param = { .memory = net_memory, .size = NET_POOL, .flags = 0 };
	if (sceNetInit(&param) < 0) return -3;
	if (sceNetCtlInit() < 0) return -4;
	if (sceHttpInit(HTTP_POOL) < 0) return -5;
	net_trust_root();
	net_inited = 1;
	http_inited = 1;
	return 0;
}

/* extract scheme://host from scheme://host/... (no port; sufficient) -- the connection base has to
   carry the scheme, because a plain http connection can never follow a 301 to https */
static int url_base(const char *url, char *out, int cap) {
	const char *sep = strstr(url, "://");
	if (!sep) return -1;
	const char *p = sep + 3;
	while (*p && *p != '/' && *p != '?' && *p != ':') p++;
	int n = (int)(p - url);
	if (n <= 0 || n >= cap) return -1;
	memcpy(out, url, n);
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

/* the failure code handed back to data.c is our own NETWORK sentinel, which hides what libhttp actually
   said -- and "the site started redirecting to https" is precisely the case worth seeing on device.
   Only trouble gets logged, and the count is capped so a failing CDN can't flood the file. */
static int dbg_n;
static void net_dbg(const char *stage, SceUID req, int code, int status, int total, int lane, const char *url) {
	if (code >= 0 && status < 300) return;
	if (dbg_n++ >= 16) return;
	/* detail is a bitfield (SceHttpsSslErrorCode): 0x20 unknown CA, 0x04 common name, 0x01 internal */
	int en = 0;
	unsigned int det = 0;
	if (req >= 0) sceHttpsGetSslError(req, &en, &det);
	dlog("net %s lane%d code=%x status=%d n=%d ssl=%x/%x %.40s", stage, lane, (unsigned)code,
	     status, total, (unsigned)en, det, url);
}

/* the caller guarantees lane belongs to its own thread; pages use 0, each icon worker takes one of the rest */
int net_fetch_lane(const char *url, void *dst, int cap, int lane) {
	if (lane < 0 || lane >= LANE_N) lane = 0;
	if (!net_inited) {
		int r = net_init();
		if (r < 0) return r;
	}
	char *buf = (char *)dst;
	char base[104];
	if (url_base(url, base, sizeof(base)) < 0) return SCE_HTTP_ERROR_INVALID_URL;

	for (int attempt = 0; attempt < 2; attempt++) {
		Conn *c = conn_get(lane, base);
		if (!c) return SCE_HTTP_ERROR_OUT_OF_MEMORY;
		SceUID req = sceHttpCreateRequestWithURL(c->conn, SCE_HTTP_METHOD_GET, url, 0);
		if (req < 0) { net_dbg("req", req, (int)req, 0, 0, lane, url); conn_drop(c); continue; } /* server closed the connection: rebuild and retry once */
		int ret = sceHttpSendRequest(req, NULL, 0);
		int status = 0;
		if (ret >= 0) sceHttpGetStatusCode(req, &status);
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
		net_dbg("get", req, (int)ret, status, total, lane, url);
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
