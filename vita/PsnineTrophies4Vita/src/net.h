#ifndef NET_H
#define NET_H

/* init NET sysmodule + SceNet/SceHttp + request template; <0 on failure */
int net_init(void);

/* blocking GET, body written into buf (cap bytes, including trailing NUL), returns length; <0 is an error code */
int net_fetch(const char *url, char *buf, int cap);

/* binary GET (icons and other non-text resources): only guarantees capacity suffices; the extra trailing NUL is harmless */
int net_fetch_bin(const char *url, void *dst, int cap);

/* same as above, but uses connection lane `lane` (0..NET_LANE_N-1).
   Keep-alive connections are stored per lane, so multiple background threads can truly download in parallel. */
int net_fetch_lane(const char *url, void *dst, int cap, int lane);

/* percent-encode a UTF-8 query parameter */
void net_url_encode(const char *s, char *out, int cap);

#endif
