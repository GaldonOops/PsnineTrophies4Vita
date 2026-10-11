#ifndef PARSE_H
#define PARSE_H

/* Flat data parsed from psnine pages: fixed-length fields, fixed capacity, no malloc
   inside the parser; arrays allocated once by the caller (app heap on device is only ~28MB). */

#define PS_GAMES    32   /* 32 entries per game-list page, measured */
#define PS_GROUPS   16
#define PS_TROPHIES 256
#define PS_TIPS     64
#define PS_URL      176  /* Sony CDN icon URL measured at 118 chars */

typedef struct {
	char name[128];      /* Chinese game name */
	char icon[PS_URL];   /* cover URL (91x91 or 91x50 on list page) */
	char platform[16];   /* e.g. PS5 / PS4/PSV */
	long game_id;        /* /psngame/<id> */
	int cnt[4];          /* platinum gold silver bronze counts */
	int pct100;          /* completion rate 10.09% -> 1009 */
	int players;         /* number of players */
} PsGame;

/* Trophy group (base game + DLC): t0 = start index in the PsTrophy array, tn = count */
typedef struct {
	char name[128];
	int cnt[4];
	int t0, tn;
	int dlc;             /* groups after the first count as DLC */
} PsGroup;

typedef struct {
	char name[160];      /* original name (JP/EN) */
	char cn[160];        /* Chinese translated name, may be empty */
	char detail[1024];   /* unlock-condition description */
	char rarity[32];     /* ultra rare / very rare / common / rare */
	char icon[PS_URL];   /* trophy icon 54x54 */
	long trophy_id;      /* /trophy/<id> */
	int rarity100;       /* 7.90% -> 790 */
	int tips_count;      /* number of tips */
	char type;           /* 'p' platinum 'g' gold 's' silver 'b' bronze */
} PsTrophy;

typedef struct {
	char author[64];     /* PSN account */
	char date[48];       /* post date (may include province) */
	char content[2048];  /* tip body, lines split by \n */
} PsTip;

/* Returns: number of parsed entries; <0 = parse failure. Arrays allocated by caller */
int ps_parse_games(const char *html, PsGame *out, int max);
/* Game page: parses header, trophy groups and trophy table in one pass. head/groups may be NULL */
int ps_parse_game(const char *html, PsGame *head, PsGroup *groups, int maxg, int *ng,
                  PsTrophy *out, int max, int *nt);
int ps_parse_trophies(const char *html, PsTrophy *out, int max);
int ps_parse_tips(const char *html, PsTip *out, int max);

/* e.g. "Platinum1 Gold13 Silver24 Bronze99", drawn directly by the 3-column layout */
int ps_fmt_counts(const int cnt[4], char *buf, int cap);

#endif
