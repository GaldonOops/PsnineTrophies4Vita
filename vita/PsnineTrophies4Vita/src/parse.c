// psnine HTML -> flat data structs. Parsing points match scripts/scraper.mjs 1:1 (selectors ported directly)
// The site has two coexisting templates at the same URL (trophy tips: div.post / ul.list>li); both are supported.
#include "gumbo.h"
#include "parse.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- DOM helpers ---------- */

static const char *attr(GumboNode *el, const char *name) {
	GumboAttribute *a = gumbo_get_attribute(&el->v.element.attributes, name);
	return a ? a->value : NULL;
}

/* Whether class="a b c" contains a whole token (text-platinum has hyphens, plain strstr won't do) */
static int has_token(const char *s, const char *tok) {
	if (!s) return 0;
	size_t n = strlen(tok);
	for (const char *p = s; (p = strstr(p, tok)); p += n) {
		if (p != s && !isspace((unsigned char)p[-1])) continue;
		if (p[n] && !isspace((unsigned char)p[n])) continue;
		return 1;
	}
	return 0;
}

typedef struct {
	int tag;          /* -1 = any tag */
	const char *cls;  /* NULL = no class restriction */
} Want;

static int pred_want(GumboNode *n, void *arg) {
	Want *w = arg;
	if (n->type != GUMBO_NODE_ELEMENT) return 0;
	if (w->tag >= 0 && (int)n->v.element.tag != w->tag) return 0;
	if (w->cls && !has_token(attr(n, "class"), w->cls)) return 0;
	return 1;
}

/* Depth-first collection of matching elements; DOCUMENT root doesn't match but is descended into */
static void walk_collect(GumboNode *n, Want *w, GumboNode **out, int max, int *count) {
	if (n->type == GUMBO_NODE_ELEMENT) {
		if (pred_want(n, w) && *count < max) out[(*count)++] = n;
		for (unsigned i = 0; i < n->v.element.children.length; i++)
			walk_collect(n->v.element.children.data[i], w, out, max, count);
	} else if (n->type == GUMBO_NODE_DOCUMENT) {
		for (unsigned i = 0; i < n->v.document.children.length; i++)
			walk_collect(n->v.document.children.data[i], w, out, max, count);
	}
}

/* First matching element, or NULL if none */
static GumboNode *find_one(GumboNode *root, int tag, const char *cls) {
	Want w = { tag, cls };
	GumboNode *one[1];
	int n = 0;
	walk_collect(root, &w, one, 1, &n);
	return n ? one[0] : NULL;
}

/* Direct children of the element with the given tag (picks a tr's tds without descending into nested tables) */
static int child_tds(GumboNode *tr, GumboNode **out, int max) {
	int n = 0;
	GumboVector *k = &tr->v.element.children;
	for (unsigned i = 0; i < k->length && n < max; i++) {
		GumboNode *c = k->data[i];
		if (c->type == GUMBO_NODE_ELEMENT && c->v.element.tag == GUMBO_TAG_TD) out[n++] = c;
	}
	return n;
}

/* ---------- Text extraction ---------- */

typedef struct {
	char *buf;
	size_t cap;
	size_t len;
	int keep_newlines; /* convert <br> to \n in tip bodies */
	GumboNode *skip;   /* if non-NULL, skip this subtree (date in meta must exclude the author link) */
} Txt;

static void txt_put(Txt *t, char c) {
	if (c == '\t' || c == '\r') c = ' ';
	if (c == '\n' && !t->keep_newlines) c = ' ';
	if (c == ' ' || c == '\n') {
		if (t->len == 0) return; /* drop leading whitespace */
		char last = t->buf[t->len - 1];
		if (last == ' ' || last == '\n') {
			if (c == '\n' && last == ' ') t->buf[t->len - 1] = '\n'; /* upgrade space to newline */
			return;
		}
		t->buf[t->len++] = c;
		return;
	}
	if (t->len + 1 < t->cap) t->buf[t->len++] = c;
}

static void txt_walk(GumboNode *n, Txt *t) {
	if (t->skip && n == t->skip) return;
	switch (n->type) {
	case GUMBO_NODE_TEXT:
	case GUMBO_NODE_WHITESPACE:
	case GUMBO_NODE_CDATA:
		for (const char *s = n->v.text.text; *s; s++) txt_put(t, *s);
		break;
	case GUMBO_NODE_ELEMENT:
		if (n->v.element.tag == GUMBO_TAG_BR) txt_put(t, t->keep_newlines ? '\n' : ' ');
		for (unsigned i = 0; i < n->v.element.children.length; i++)
			txt_walk(n->v.element.children.data[i], t);
		break;
	default:
		break; /* comments and the document root are handled by the caller */
	}
}

static void text_of(GumboNode *n, char *buf, size_t cap, int keep_nl) {
	if (!n || cap == 0) { if (cap) buf[0] = 0; return; }
	Txt t = { buf, cap, 0, keep_nl, NULL };
	txt_walk(n, &t);
	t.buf[t.len] = 0;
	while (t.len && (t.buf[t.len - 1] == ' ' || t.buf[t.len - 1] == '\n'))
		t.buf[--t.len] = 0;
}

/* Text of n, excluding sub (a subtree inside n) */
static void text_of_skip(GumboNode *n, GumboNode *sub, char *buf, size_t cap) {
	if (!n || cap == 0) { if (cap) buf[0] = 0; return; }
	Txt t = { buf, cap, 0, 0, sub };
	txt_walk(n, &t);
	t.buf[t.len] = 0;
	while (t.len && (t.buf[t.len - 1] == ' ' || t.buf[t.len - 1] == '\n'))
		t.buf[--t.len] = 0;
}

static void copy_txt(char *dst, int cap, const char *src) {
	if (cap <= 0) return;
	int i = 0;
	if (src) for (; i < cap - 1 && src[i]; i++) dst[i] = src[i];
	dst[i] = 0;
}

/* ---------- Number extraction ---------- */

/* Number after mark in a URL, e.g. "trophy/22724001" -> 22724001 */
static long id_after(const char *s, const char *mark) {
	const char *p = s ? strstr(s, mark) : NULL;
	if (!p) return 0;
	p += strlen(mark);
	long v = 0;
	while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
	return v;
}

/* First run of digits in the text (skips preceding label characters and whitespace) */
static int first_num(const char *s) {
	while (*s && (*s < '0' || *s > '9')) s++;
	int v = 0;
	while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
	return v;
}

/* "10.09% perfect" -> 1009; "7.90%" -> 790; integer part x100, two decimal digits */
static int parse_pct100(const char *s) {
	while (*s && (*s < '0' || *s > '9')) s++;
	if (!*s) return 0;
	long ip = 0;
	while (*s >= '0' && *s <= '9') ip = ip * 10 + (*s++ - '0');
	int fp = 0, dig = 0;
	if (*s == '.') {
		s++;
		while (*s >= '0' && *s <= '9' && dig < 2) { fp = fp * 10 + (*s++ - '0'); dig++; }
	}
	while (dig < 2) { fp *= 10; dig++; }
	return (int)(ip * 100 + fp);
}

/* Number/percent immediately before mark: in "points1980 12235 players" take 12235, not 1980 */
static const char *back_to_num(const char *s, const char *mark, int pct) {
	if (!mark) return NULL;
	const char *p = mark;
	while (p > s && ((p[-1] >= '0' && p[-1] <= '9') || (pct && (p[-1] == '.' || p[-1] == '%')))) p--;
	return p;
}

static int int_before(const char *s, const char *mark) {
	const char *p = back_to_num(s, mark, 0);
	if (!p) return 0;
	int v = 0;
	for (; p < mark; p++) v = v * 10 + (*p - '0');
	return v;
}

static int pct_before(const char *s, const char *mark) {
	const char *p = back_to_num(s, mark, 1);
	return p ? parse_pct100(p) : 0;
}

static GumboNode *closest(GumboNode *n, GumboTag tag) {
	while (n) {
		if (n->type == GUMBO_NODE_ELEMENT && n->v.element.tag == tag) return n;
		n = n->parent;
	}
	return NULL;
}

/* Platinum/gold/silver/bronze four spans: first number in the span text is the count */
static void read_counts(GumboNode *root, int cnt[4]) {
	static const char *cs[] = { "text-platinum", "text-gold", "text-silver", "text-bronze" };
	for (int k = 0; k < 4; k++) {
		GumboNode *e = find_one(root, GUMBO_TAG_SPAN, cs[k]);
		char tmp[32];
		cnt[k] = 0;
		if (!e) continue;
		text_of(e, tmp, sizeof(tmp), 0);
		cnt[k] = first_num(tmp);
	}
}

/* ---------- Game list page: /psngame?page=N and /psngame?title=xxx share one template ---------- */

int ps_parse_games(const char *html, PsGame *out, int max) {
	GumboOutput *doc = gumbo_parse(html);
	if (!doc) return -1;
	int count = 0;

	Want wtd = { GUMBO_TAG_TD, "title" };
	GumboNode *tds[PS_GAMES * 2];
	int ntds = 0;
	walk_collect(doc->root, &wtd, tds, (int)(sizeof(tds) / sizeof(tds[0])), &ntds);

	for (int i = 0; i < ntds && count < max; i++) {
		GumboNode *td = tds[i];
		GumboNode *tr = closest(td, GUMBO_TAG_TR);
		if (!tr) continue;

		Want wa = { GUMBO_TAG_A, NULL };
		GumboNode *as[4];
		int nas = 0;
		walk_collect(td, &wa, as, 4, &nas);
		if (!nas) continue;
		long gid = id_after(attr(as[0], "href"), "psngame/");
		if (!gid) continue;

		PsGame *g = &out[count];
		memset(g, 0, sizeof(*g));
		g->game_id = gid;
		text_of(as[0], g->name, sizeof(g->name), 0);
		if (!g->name[0]) continue;

		/* Cover: first img in the row (commented-out placeholders never reach the DOM) */
		GumboNode *img = find_one(tr, GUMBO_TAG_IMG, NULL);
		if (img) copy_txt(g->icon, sizeof(g->icon), attr(img, "src"));

		read_counts(tr, g->cnt);

		static const char *pfs[] = { "pf_ps5", "pf_ps4", "pf_ps3", "pf_psv", "pf_pc" };
		size_t off = 0;
		for (int k = 0; k < 5 && off < sizeof(g->platform) - 1; k++) {
			GumboNode *sp = find_one(tr, GUMBO_TAG_SPAN, pfs[k]);
			if (!sp) continue;
			char tmp[16];
			text_of(sp, tmp, sizeof(tmp), 0);
			off += snprintf(g->platform + off, sizeof(g->platform) - off, "%s%s",
			                off ? "/" : "", tmp);
		}

		/* Columns 3/4: difficulty + completion rate, player count */
		GumboNode *row_td[6];
		int ntd = child_tds(tr, row_td, 6);
		for (int k = 2; k < ntd; k++) {
			char tmp[64];
			text_of(row_td[k], tmp, sizeof(tmp), 0);
			if (!tmp[0]) continue;
			char *mp = strstr(tmp, "完美"), *pp = strstr(tmp, "人玩过");
			if (!g->pct100 && mp) g->pct100 = pct_before(tmp, mp);
			else if (!g->players && pp) g->players = int_before(tmp, pp);
		}
		count++;
	}
	gumbo_destroy_output(&kGumboDefaultOptions, doc);
	return count;
}

/* ---------- Trophy row ---------- */

static void fill_trophy(GumboNode *tr, PsTrophy *t) {
	memset(t, 0, sizeof(*t));
	t->type = 'b';

	/* Type: class t1..t4 of the first td (the rarity td also carries t*, only the first td counts) */
	GumboNode *row_td[6];
	int ntd = child_tds(tr, row_td, 6);
	if (ntd) {
		const char *cls = attr(row_td[0], "class");
		if (has_token(cls, "t1")) t->type = 'p';
		else if (has_token(cls, "t2")) t->type = 'g';
		else if (has_token(cls, "t3")) t->type = 's';
		else t->type = 'b';
		GumboNode *img = find_one(row_td[0], GUMBO_TAG_IMG, NULL);
		if (img) copy_txt(t->icon, sizeof(t->icon), attr(img, "src"));
	}

	const char *namecls = t->type == 'p' ? "text-platinum" : t->type == 'g' ? "text-gold"
	                      : t->type == 's' ? "text-silver" : "text-bronze";
	Want wa = { GUMBO_TAG_A, NULL };
	GumboNode *as[8];
	int nas = 0;
	walk_collect(tr, &wa, as, 8, &nas);
	for (int k = 0; k < nas; k++) {
		if (!t->trophy_id) t->trophy_id = id_after(attr(as[k], "href"), "trophy/");
		if (!t->name[0] && has_token(attr(as[k], "class"), namecls))
			text_of(as[k], t->name, sizeof(t->name), 0);
	}

	/* An em wrapped in full-width parens is the Chinese translated name; other ems hold the original unlock condition */
	static const char *BRK = "\xEF\xBC\x88"; /* full-width left paren */
	Want wem = { GUMBO_TAG_EM, NULL };
	GumboNode *ems[8];
	int nems = 0;
	walk_collect(tr, &wem, ems, 8, &nems);
	char orig[512] = "";
	for (int k = 0; k < nems; k++) {
		const char *cls = attr(ems[k], "class");
		char tmp[512];
		if (has_token(cls, "alert-success")) {
			text_of(ems[k], tmp, sizeof(tmp), 0);
			t->tips_count = first_num(tmp); /* "22 Tips" */
			continue;
		}
		text_of(ems[k], tmp, sizeof(tmp), 0);
		if (!tmp[0]) continue;
		if (!t->cn[0] && !strncmp(tmp, BRK, 3)) {
			char *e = strstr(tmp + 3, "\xEF\xBC\x89"); /* full-width right paren */
			if (e) {
				size_t n = (size_t)(e - tmp - 3);
				if (n >= sizeof(t->cn)) n = sizeof(t->cn) - 1;
				memcpy(t->cn, tmp + 3, n);
				t->cn[n] = 0;
			}
		} else if (!orig[0] && !has_token(cls, "h-p") && !has_token(cls, "r")
		           && !has_token(cls, "text-gray")) {
			copy_txt(orig, sizeof(orig), tmp);
		}
	}

	/* Unlock condition: Chinese text preferred (div.text-strong / em.text-gray), else keep the original */
	GumboNode *d = find_one(tr, GUMBO_TAG_DIV, "text-strong");
	if (!d) d = find_one(tr, GUMBO_TAG_EM, "text-gray");
	if (d) text_of(d, t->detail, sizeof(t->detail), 0);
	if (!t->detail[0]) copy_txt(t->detail, sizeof(t->detail), orig);

	/* Rarity: td.twoge holds e.g. "7.90% very rare" */
	for (int k = 1; k < ntd; k++) {
		if (!has_token(attr(row_td[k], "class"), "twoge")) continue;
		char tmp[64];
		text_of(row_td[k], tmp, sizeof(tmp), 0);
		if (!tmp[0]) break;
		t->rarity100 = parse_pct100(tmp);
		char *p = tmp;
		while (*p && (isdigit((unsigned char)*p) || *p == '.' || *p == '%' || *p == ' ')) p++;
		copy_txt(t->rarity, sizeof(t->rarity), p);
		break;
	}
}

/* ---------- Game detail page /psngame/<id>: header + trophy groups + trophy table ---------- */

int ps_parse_game(const char *html, PsGame *head, PsGroup *groups, int maxg, int *ng,
                  PsTrophy *out, int max, int *nt) {
	GumboOutput *doc = gumbo_parse(html);
	if (!doc) return -1;
	int ntr = 0, ngrp = 0;
	if (ng) *ng = 0;
	if (nt) *nt = 0;

	/* Header band: <div class="box pd10">, holds cover, h1, counts, difficulty and completion rate */
	if (head) {
		memset(head, 0, sizeof(*head));
		GumboNode *box = NULL;
		Want wb = { GUMBO_TAG_DIV, "pd10" };
		GumboNode *cand[4];
		int nc = 0;
		walk_collect(doc->root, &wb, cand, 4, &nc);
		for (int k = 0; k < nc; k++)
			if (has_token(attr(cand[k], "class"), "box")) { box = cand[k]; break; }
		if (box) {
			GumboNode *img = find_one(box, GUMBO_TAG_IMG, NULL);
			if (img) copy_txt(head->icon, sizeof(head->icon), attr(img, "src"));
			GumboNode *h1 = find_one(box, GUMBO_TAG_H1, NULL);
			char title[160];
			title[0] = 0;
			if (h1) text_of(h1, title, sizeof(title), 0);
			/* Title like "<game> Chinese trophy list" -> take the name inside the book-title marks */
			char *a = strstr(title, "\xE3\x80\x8A");
			char *b = a ? strstr(a + 3, "\xE3\x80\x8B") : NULL;
			if (a && b && b > a + 3) {
				size_t n = (size_t)(b - a - 3);
				if (n >= sizeof(head->name)) n = sizeof(head->name) - 1;
				memcpy(head->name, a + 3, n);
				head->name[n] = 0;
			} else {
				copy_txt(head->name, sizeof(head->name), title);
			}
			read_counts(box, head->cnt);
			GumboNode *sp = find_one(box, GUMBO_TAG_SPAN, "pf_ps5");
			if (!sp) sp = find_one(box, GUMBO_TAG_SPAN, "pf_ps4");
			if (!sp) sp = find_one(box, GUMBO_TAG_SPAN, "pf_ps3");
			if (!sp) sp = find_one(box, GUMBO_TAG_SPAN, "pf_psv");
			if (sp) text_of(sp, head->platform, sizeof(head->platform), 0);
			/* "points1980 12235 players" is bare text scattered in the div, not inside em; scan the whole block */
			char all[256];
			text_of(box, all, sizeof(all), 0);
			char *mp = strstr(all, "完美"), *pp = strstr(all, "人玩过");
			if (mp) head->pct100 = pct_before(all, mp);
			if (pp) head->players = int_before(all, pp);
		}
	}

	/* Each trophy group is its own <table class="list">: header row colspan=4 holds the group name and counts */
	Want wtb = { GUMBO_TAG_TABLE, "list" };
	GumboNode *tabs[PS_GROUPS];
	int ntabs = 0;
	walk_collect(doc->root, &wtb, tabs, PS_GROUPS, &ntabs);
	if (!ntabs) { gumbo_destroy_output(&kGumboDefaultOptions, doc); return -1; }

	Want wtr = { GUMBO_TAG_TR, NULL };
	GumboNode *trs[PS_TROPHIES + 4];

	for (int ti = 0; ti < ntabs; ti++) {
		int ntrs = 0;
		walk_collect(tabs[ti], &wtr, trs, (int)(sizeof(trs) / sizeof(trs[0])), &ntrs);
		for (int i = 0; i < ntrs; i++) {
			GumboNode *tr = trs[i];
			GumboNode *row_td[6];
			int ntd = child_tds(tr, row_td, 6);
			int is_group = 0;
			for (int k = 0; k < ntd; k++) if (attr(row_td[k], "colspan")) is_group = 1;

			if (is_group) {
				if (!groups || ngrp >= maxg) continue;
				PsGroup *gp = &groups[ngrp];
				memset(gp, 0, sizeof(*gp));
				GumboNode *p = find_one(tr, GUMBO_TAG_P, NULL);
				if (p) text_of(p, gp->name, sizeof(gp->name), 0);
				read_counts(tr, gp->cnt);
				gp->t0 = ntr;
				gp->dlc = ti > 0;
				ngrp++;
				continue;
			}
			if (!has_token(attr(tr, "class"), "trophy")) continue;
			if (!out || ntr >= max) continue;

			fill_trophy(tr, &out[ntr]);
			if (!out[ntr].trophy_id && !out[ntr].name[0]) continue;
			ntr++;
			if (ngrp) groups[ngrp - 1].tn = ntr - groups[ngrp - 1].t0;
		}
	}
	/* Games with no group rows (rare): whole table counts as one group, named from the header */
	if (!ngrp && ntr && groups && maxg > 0) {
		memset(&groups[0], 0, sizeof(groups[0]));
		if (head) copy_txt(groups[0].name, sizeof(groups[0].name), head->name);
		read_counts(tabs[0], groups[0].cnt);
		groups[0].t0 = 0;
		groups[0].tn = ntr;
		ngrp = 1;
	}
	if (head && !head->name[0] && ngrp) copy_txt(head->name, sizeof(head->name), groups[0].name);
	if (ng) *ng = ngrp;
	if (nt) *nt = ntr;
	gumbo_destroy_output(&kGumboDefaultOptions, doc);
	return ntr;
}

int ps_parse_trophies(const char *html, PsTrophy *out, int max) {
	return ps_parse_game(html, NULL, NULL, 0, NULL, out, max, NULL);
}

/* ---------- Tips page /trophy/<id>: new template div.post, old template ul.list>li ---------- */

static void fill_tip(GumboNode *scope, PsTip *t) {
	memset(t, 0, sizeof(*t));
	GumboNode *a = find_one(scope, GUMBO_TAG_A, "psnnode");
	if (a) text_of(a, t->author, sizeof(t->author), 0);

	GumboNode *c = find_one(scope, GUMBO_TAG_DIV, "content");
	if (c) text_of(c, t->content, sizeof(t->content), 1);

	/* Date: what remains in meta after removing the author link (new template also has province) */
	Want wm = { GUMBO_TAG_DIV, "meta" };
	GumboNode *me[4];
	int nme = 0;
	walk_collect(scope, &wm, me, 4, &nme);
	if (nme) text_of_skip(me[nme - 1], a, t->date, sizeof(t->date));
}

int ps_parse_tips(const char *html, PsTip *out, int max) {
	GumboOutput *doc = gumbo_parse(html);
	if (!doc) return -1;
	int count = 0;

	Want wpost = { GUMBO_TAG_DIV, "post" };
	GumboNode *ps[PS_TIPS];
	int nps = 0;
	walk_collect(doc->root, &wpost, ps, max > PS_TIPS ? PS_TIPS : max, &nps);
	for (int i = 0; i < nps && count < max; i++) {
		/* Reply posts are also div.post but their body sits in div.content; skip anything without body text */
		fill_tip(ps[i], &out[count]);
		if (!out[count].content[0]) continue;
		count++;
	}
	if (count) { gumbo_destroy_output(&kGumboDefaultOptions, doc); return count; }

	GumboNode *ul = find_one(doc->root, GUMBO_TAG_UL, "list");
	if (ul) {
		GumboVector *lis = &ul->v.element.children;
		for (unsigned i = 0; i < lis->length && count < max; i++) {
			GumboNode *li = lis->data[i];
			if (li->type != GUMBO_NODE_ELEMENT || li->v.element.tag != GUMBO_TAG_LI) continue;
			fill_tip(li, &out[count]);
			if (!out[count].content[0]) continue;
			count++;
		}
	}
	gumbo_destroy_output(&kGumboDefaultOptions, doc);
	return count;
}

int ps_fmt_counts(const int cnt[4], char *buf, int cap) {
	static const char *lbl[4] = { "白", "金", "银", "铜" };
	int off = 0;
	for (int k = 0; k < 4 && off < cap - 8; k++)
		off += snprintf(buf + off, (size_t)(cap - off), "%s%d ", lbl[k], cnt[k]);
	while (off > 0 && buf[off - 1] == ' ') buf[--off] = 0;
	return off;
}
