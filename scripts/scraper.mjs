// psnine.com scraper (read-only, no login): game search, trophy list, trophy tips.
// Doubles as a patrol script: `npm run patrol` asserts the live DOM still parses
// with the current selectors.
import * as cheerio from "cheerio";
import { connect } from "node:tls";
import { mkdirSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const BASE = "https://psnine.com";
const UA =
  "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0 Safari/537.36";

async function getHtml(url) {
  const res = await fetch(url, {
    headers: { "User-Agent": UA, "Accept-Language": "zh-CN,zh;q=0.9" },
    redirect: "follow",
  });
  if (!res.ok) throw new Error(`HTTP ${res.status} for ${url}`);
  return await res.text();
}

const idFromUrl = (href) => {
  const m = /\/(?:psngame|trophy)\/(\d+)/.exec(href || "");
  return m ? m[1] : null;
};

// --- 1) fuzzy game search: GET /psngame?title=<q> ---
export async function searchGames(title) {
  const url = `${BASE}/psngame?title=${encodeURIComponent(title)}`;
  const html = await getHtml(url);
  const $ = cheerio.load(html);
  const totalTxt = $.root().text().match(/共(\d+)条/);
  const seen = new Set();
  const games = [];
  const clean = (s) => (s || "").replace(/[ \t]+/g, " ").trim();
  $("td.title a").each((_, a) => {
    const gid = idFromUrl($(a).attr("href"));
    const name = clean($(a).text());
    if (!gid || !name || seen.has(gid)) return;
    seen.add(gid);
    const row = $(a).closest("tr");
    const platform = row.find(".pf_ps5,.pf_ps4,.pf_ps3,.pf_psv,.pf_pc")
      .map((_, e) => clean($(e).text())).get().join("/");
    const counts = row.find(".text-platinum,.text-gold,.text-silver,.text-bronze")
      .map((_, e) => clean($(e).text())).get();
    const players = clean(row.find("td").last().text());
    const cover = row.find("img").attr("src") || "";
    games.push({ id: gid, name, platform, counts, players, cover, url: `${BASE}/psngame/${gid}` });
  });
  return { total: totalTxt ? +totalTxt[1] : games.length, games, url };
}

// --- 2) trophy list for a game: GET /psngame/<gameId> ---
export async function getTrophies(gameId) {
  const url = `${BASE}/psngame/${gameId}`;
  const html = await getHtml(url);
  const $ = cheerio.load(html);
  const gameName = $("title").text().replace(/中文奖杯列表.*/, "").replace(/[《》]/g, "").trim();
  const totalM = html.match(/总(\d+)/), ptM = html.match(/点数(\d+)/), plM = html.match(/(\d+)人玩过/);
  const perfectM = html.match(/([\d.]+%完美)/);
  const summary = [totalM && `总${totalM[1]}`, ptM && `点数${ptM[1]}`,
    plM && `${plM[1]}人玩过`, perfectM && perfectM[1]].filter(Boolean).join(" · ");
  const trophies = [];
  $("tr.trophy").each((_, tr) => {
    const $tr = $(tr);
    const order = $tr.attr("id") || "";
    const a = $tr.find('a[href*="/trophy/"]').first();
    const trophyId = idFromUrl(a.attr("href"));
    const cellClass = $tr.find("td.t1,td.t2,td.t3,td.t4").first().attr("class") || "";
    const type = /t1/.test(cellClass) ? "platinum"
      : /t2/.test(cellClass) ? "gold"
      : /t3/.test(cellClass) ? "silver" : "bronze";
    const nameEn = $tr.find("p a").filter((_, e) =>
      /text-(platinum|gold|silver|bronze)/.test($(e).attr("class") || "")).first().text().trim();
    const nameCn = ($tr.find("p em").text().match(/[（(](.+?)[）)]/) || [])[1] || "";
    const tipsCount = parseInt($tr.find(".alert-success b").first().text() || "0", 10) || 0;
    const descEn = $tr.find("td > em").first().text().trim();
    const descCn = $tr.find(".text-strong.mt10").first().text().trim();
    const rarity = $tr.find("td.twoge").last().text().replace(/\s+/g, " ").trim();
    trophies.push({
      order, trophyId, type, nameEn, nameCn, descEn, descCn, tipsCount, rarity,
      tipsUrl: trophyId ? `${BASE}/trophy/${trophyId}` : null,
    });
  });
  return { gameId, gameName, summary, total: trophies.length, trophies, url };
}

// --- 3) trophy tips detail: GET /trophy/<trophyId> ---
export async function getTrophyTips(trophyId) {
  const url = `${BASE}/trophy/${trophyId}`;
  const html = await getHtml(url);
  const $ = cheerio.load(html);
  const title = $("title").text().trim(); // page title format: "<trophy> trophy - <game>"
  const tips = [];
  $("ul.list > li").each((_, li) => {
    const $li = $(li);
    const author = $li.find("a.psnnode").first().text().trim();
    const content = $li.find("div.content").first().text().replace(/\s*\n\s*/g, "\n").trim();
    const meta = $li.find("div.meta").last().text().replace(/\s+/g, " ").trim();
    if (content) tips.push({ author, content, meta });
  });
  return { trophyId, title, count: tips.length, tips, url };
}

// --- 4) patrol: live DOM drift check (read-only probes + semantic assertions) ---
const FIX_DIR = join(dirname(fileURLToPath(import.meta.url)), "fixtures");
const PATROL_QUERY = "战神"; // search keyword stays Chinese: the site catalog is Chinese
const PATROL_GAME = "57900";       // fixed probes: pages confirmed to carry real data
const PATROL_TROPHY = "57900001";
const PATROL_TIPS_PAGES = 3;

async function saveFixture(name, url) {
  try {
    const html = await getHtml(url);
    mkdirSync(FIX_DIR, { recursive: true });
    writeFileSync(join(FIX_DIR, name), html);
    return html;
  } catch (e) {
    return null;
  }
}

/* Same semantics as parse.c ps_parse_tips: div.post (with body) first, else ul.list>li */
function tipCensus(html) {
  const $ = cheerio.load(html);
  let post = 0, list = 0;
  $("div.post").each((_, e) => {
    if ($(e).find("div.content").first().text().trim()) post++;
  });
  $("ul.list > li").each((_, e) => {
    if ($(e).find("div.content").first().text().trim()) list++;
  });
  return { post, list };
}

/* The app validates psnine against one bundled anchor (vita/.../certs/usertrust_rsa.pem), because the Vita's
   own root store predates this chain and libhttp never refreshes anything. So the interesting drift is not
   "is the site https" but "does its chain still end at the root we shipped" -- a CA swap breaks every device
   build with nothing but an SSL error on screen. Plain http used to be the requirement; the site 302s it now. */
const ANCHOR_CN = "USERTrust RSA Certification Authority";

function chainRoot(host) {
  return new Promise((resolve, reject) => {
    const s = connect({ host, port: 443, servername: host, rejectUnauthorized: false });
    s.on("secureConnect", () => {
      let c = s.getPeerCertificate(true);
      let last = "";
      for (let i = 0; c && i < 8; i++) {
        if (c.issuer?.CN) last = c.issuer.CN;
        if (c.issuerCertificate === c) break;
        c = c.issuerCertificate;
      }
      s.destroy();
      resolve(last);
    });
    s.on("error", reject);
  });
}

async function transportCanary() {
  const url = "https://www.psnine.com/psngame?page=1";
  let status = 0, html = "", root = "", err = "";
  try {
    const res = await fetch(url, {
      headers: { "User-Agent": UA, "Accept-Language": "zh-CN,zh;q=0.9" },
      redirect: "manual",
    });
    status = res.status;
    html = status === 200 ? await res.text() : "";
    root = await chainRoot("www.psnine.com");
  } catch (e) {
    err = e.message;
  }
  const served = status === 200 && /<td class="[^"]*\btitle\b/.test(html);
  const anchored = root.includes(ANCHOR_CN);
  return {
    ok: served && anchored,
    detail: err || `status=${status} bytes=${html.length} root=${root || "?"}${anchored ? "" : " != bundled anchor"}`,
  };
}

export async function patrol() {
  const fails = [];
  const note = (good, label, detail = "") => {
    console.log(`${good ? "PASS" : "FAIL"}  ${label}${detail ? `  (${detail})` : ""}`);
    if (!good) fails.push(label);
  };

  const tc = await transportCanary();
  note(tc.ok, "https html served + chain ends at the bundled anchor", tc.detail);

  try {
    const r = await searchGames(PATROL_QUERY);
    note(r.total > 0, "search result count > 0", `total=${r.total}`);
    note(r.games.length > 0, "search game rows > 0", `rows=${r.games.length}`);
    const g0 = r.games[0];
    note(!!g0 && !!g0.id && !!g0.name, "search first row id+name",
         g0 ? `${g0.id} ${g0.name}` : "none");
    await saveFixture("search.html", r.url);
  } catch (e) {
    note(false, "search request/parse", e.message);
  }

  const tipProbes = [{ trophyId: PATROL_TROPHY, tipsUrl: `${BASE}/trophy/${PATROL_TROPHY}` }];
  try {
    const r = await getTrophies(PATROL_GAME);
    note(!!r.gameName, "trophies game name", r.gameName);
    note(!!r.summary, "trophies summary line", r.summary);
    note(r.trophies.length > 0, "trophies rows > 0", `rows=${r.trophies.length}`);
    note(r.trophies.every((t) => !!t.trophyId), "trophies every row has trophyId");
    const t0 = r.trophies[0];
    note(!!t0 && !!t0.nameEn && ["platinum", "gold", "silver", "bronze"].includes(t0.type),
         "trophies first row name+type", t0 ? `${t0.type} ${t0.nameEn}` : "none");
    for (const t of r.trophies) {
      if (t.tipsCount > 0 && t.tipsUrl && t.trophyId !== PATROL_TROPHY)
        tipProbes.push({ trophyId: t.trophyId, tipsUrl: t.tipsUrl });
    }
    await saveFixture(`trophies-${r.gameId}.html`, r.url);
  } catch (e) {
    note(false, "trophies request/parse", e.message);
  }
  tipProbes.length = Math.min(tipProbes.length, PATROL_TIPS_PAGES);

  const census = { post: 0, list: 0 };
  for (const t of tipProbes) {
    const label = `tips ${t.trophyId}`;
    try {
      const html = await saveFixture(`tips-${t.trophyId}.html`, t.tipsUrl);
      if (!html) { note(false, `${label} fixture fetch`); continue; }
      const c = tipCensus(html);
      census.post += c.post; census.list += c.list;
      const tmpl = c.post ? "div.post" : c.list ? "ul.list" : "none";
      note(c.post + c.list > 0, `${label} at least one tips template matches`,
           `template=${tmpl}`);
      const r = await getTrophyTips(t.trophyId);
      note(!!r.title, `${label} title`, r.title);
      if (c.list) note(r.count > 0 && !!r.tips[0].content, `${label} ul.list export body`,
                       `count=${r.count}`);
    } catch (e) {
      note(false, `${label} request/parse`, e.message);
    }
  }
  if (tipProbes.length)
    console.log(`census  div.post=${census.post}  ul.list=${census.list}`);
  console.log(`fixtures -> ${FIX_DIR}`);
  return fails.length;
}

const isMain = process.argv[1] &&
  import.meta.url === pathToFileURL(process.argv[1]).href;
if (isMain && process.argv.includes("--patrol")) {
  patrol().then((n) => {
    console.log(n ? `patrol FAILED ${n} check(s)` : "patrol: all checks passed");
    process.exit(n ? 1 : 0);
  });
}
