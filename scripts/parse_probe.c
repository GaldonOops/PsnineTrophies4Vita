/* Offline cross-check: run parse.c over scripts/fixtures/*.html and compare with the scraper.mjs patrol */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parse.h"

static char *slurp(const char *path, long *len) {
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); return NULL; }
	fseek(f, 0, SEEK_END);
	*len = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = malloc((size_t)*len + 1);
	if (!buf || fread(buf, 1, (size_t)*len, f) != (size_t)*len) { fclose(f); free(buf); return NULL; }
	buf[*len] = 0;
	fclose(f);
	return buf;
}

int main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: parse_probe <games|game|tips> <htmlfile>\n");
		return 2;
	}
	long n = 0;
	char *html = slurp(argv[2], &n);
	if (!html) return 2;

	if (!strcmp(argv[1], "games")) {
		static PsGame g[PS_GAMES];
		int c = ps_parse_games(html, g, PS_GAMES);
		printf("games=%d\n", c);
		if (c > 0)
			printf("  first id=%ld name=%s cnt=%d/%d/%d/%d pct100=%d players=%d icon=%s\n",
			       g[0].game_id, g[0].name, g[0].cnt[0], g[0].cnt[1], g[0].cnt[2],
			       g[0].cnt[3], g[0].pct100, g[0].players, g[0].icon);
	} else if (!strcmp(argv[1], "game")) {
		static PsGame head;
		static PsGroup gr[PS_GROUPS];
		static PsTrophy t[PS_TROPHIES];
		int ng = 0, nt = 0;
		int c = ps_parse_game(html, &head, gr, PS_GROUPS, &ng, t, PS_TROPHIES, &nt);
		printf("trophies=%d groups=%d head=%s\n", c, ng, head.name);
		for (int i = 0; i < ng; i++)
			printf("  group[%d] %s t0=%d tn=%d dlc=%d\n", i, gr[i].name, gr[i].t0, gr[i].tn, gr[i].dlc);
		if (c > 0)
			printf("  first type=%c id=%ld name=%s cn=%s tips=%d rarity=%s icon=%s\n",
			       t[0].type, t[0].trophy_id, t[0].name, t[0].cn, t[0].tips_count,
			       t[0].rarity, t[0].icon);
	} else {
		static PsTip tp[PS_TIPS];
		int c = ps_parse_tips(html, tp, PS_TIPS);
		printf("tips=%d\n", c);
		if (c > 0)
			printf("  first author=%s date=%s content=%.40s\n", tp[0].author, tp[0].date, tp[0].content);
	}
	free(html);
	return 0;
}
