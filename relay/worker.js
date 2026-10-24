// Claude Cube relay: fetches what the cube can't (HTTPS-only sources) and answers in plain
// text over plain HTTP, so the cube works without the Mac being on.
//   /level             "yellow|red|none|unknown <unix>"   Kyiv alert level from KMDA's Telegram
//   /pair?s=WBTUSDT    "<price> <chg%> <spark csv>"       WBT/USDT (WhiteBIT) or USDUAH (NBU)
//   /outage?r=lvivska-oblast&g=1.1  "<start> <end> <group>"  next power outage window, any region
//                      (schedules aggregated by svitlo.live; without r: Yasno, Kyiv)
//   /outday?r=..&g=..  "<today> <96 chars>"                today and tomorrow in half hours: 0 power, 1 outage, ? unknown
//   /regions           JSON [{id, name, groups}]          regions and groups for the control panel

const UA = { "User-Agent": "Mozilla/5.0 (claude-cube relay)" };

const text = (s, ttl = 0) =>
  new Response(s, {
    headers: {
      "content-type": "text/plain; charset=utf-8",
      "access-control-allow-origin": "*",
      "cache-control": ttl ? `public, max-age=${ttl}` : "no-store",
    },
  });

async function getText(url, ttl) {
  const r = await fetch(url, { headers: UA, cf: { cacheTtl: ttl, cacheEverything: true } });
  if (!r.ok) throw new Error(`${url} ${r.status}`);
  return r.text();
}

// since 2026-09 Kyiv has two levels: yellow = drones only, red = missiles or a massive attack
async function level() {
  const page = await getText("https://t.me/s/VA_Kyiv", 30);
  const re = /tgme_widget_message_text[^>]*>([\s\S]*?)<\/div>[\s\S]*?<time datetime="([^"]+)"/g;
  const posts = [...page.matchAll(re)];
  for (let i = posts.length - 1; i >= 0; i--) {
    const t = posts[i][1].replace(/<[^>]+>/g, " ").toLowerCase();
    const lv = t.includes("жовтий рівень") ? "yellow" : t.includes("червоний рівень") ? "red" : t.includes("відбій") ? "none" : null;
    if (lv) return `${lv} ${Math.floor(Date.parse(posts[i][2]) / 1000)}`;
  }
  return "unknown 0";
}

async function pair(sym) {
  if (sym === "WBTUSDT") {
    const t = JSON.parse(await getText("https://whitebit.com/api/v1/public/ticker?market=WBT_USDT", 50)).result;
    const k = JSON.parse(await getText("https://whitebit.com/api/v1/public/kline?interval=1h&limit=24&market=WBT_USDT", 50)).result;
    return `${t.last} ${t.change} ${k.map((c) => c[2]).join(",")}`;  // kline: [time, open, close, ...]
  }
  if (sym === "USDUAH") {
    const day = (d) => d.toISOString().slice(0, 10).replace(/-/g, "");
    const end = new Date(Date.now() + 86400e3), start = new Date(Date.now() - 16 * 86400e3);
    const rows = JSON.parse(await getText(
      `https://bank.gov.ua/NBU_Exchange/exchange_site?start=${day(start)}&end=${day(end)}&valcode=usd&sort=exchangedate&order=asc&json`, 600));
    const r = rows.map((x) => +x.rate);
    const chg = r.length > 1 ? ((r.at(-1) - r.at(-2)) / r.at(-2)) * 100 : 0;
    return `${r.at(-1)} ${chg.toFixed(2)} ${r.join(",")}`;
  }
  throw new Error("unknown pair");
}

// Schedules for all of Ukraine, aggregated by svitlo.live (github.com/chaichuk/svitlo_live): per region and
// group, half-hour slots for today and tomorrow, 1 = power, 2 = outage, in Kyiv time. Two feeds: the newer
// covers most regions, the older one the rest.
const SVITLO = ["https://dtek-api.svitlo-proxy.workers.dev/", "https://svitlo-proxy.svitlo-proxy.workers.dev"];
const SVITLO_ALIAS = { "harkivska-oblast": "kharkivska-oblast", "hmelnitska-oblast": "khmelnytska-oblast",
  "chernigivska-oblast": "chernihivska-oblast", "jitomirska-oblast": "zhytomyrska-oblast" };

async function svitloRegions() {
  const out = new Map();
  for (const url of SVITLO) {
    let d = JSON.parse(await getText(url, 600));
    if (typeof d.body === "string") d = JSON.parse(d.body);
    for (const r of d.regions || []) {
      const id = SVITLO_ALIAS[r.cpu] || r.cpu;
      if (!r.schedule || out.has(id)) continue;
      out.set(id, { id, name: r.name_ua || r.name_en || id, name_en: r.name_en || id, schedule: r.schedule });
    }
  }
  return out;
}

// unix time of a Kyiv wall-clock moment
function kyivUnix(date, hhmm) {
  const guess = Date.parse(`${date}T${hhmm}:00Z`);
  const p = Object.fromEntries(new Intl.DateTimeFormat("en-GB", { timeZone: "Europe/Kyiv", hourCycle: "h23",
    year: "numeric", month: "2-digit", day: "2-digit", hour: "2-digit", minute: "2-digit" })
    .formatToParts(new Date(guess)).map((x) => [x.type, x.value]));
  const shown = Date.parse(`${p.year}-${p.month}-${p.day}T${p.hour}:${p.minute}:00Z`);
  return (guess - (shown - guess)) / 1000;
}

async function svitloOutage(region, group) {
  const r = (await svitloRegions()).get(region);
  const days = (r && r.schedule[group]) || {};
  const win = [];
  for (const date of Object.keys(days).sort()) {
    for (const hhmm of Object.keys(days[date]).sort()) {
      if (days[date][hhmm] !== 2) continue;
      const s = kyivUnix(date, hhmm), e = s + 1800;
      if (win.length && Math.abs(win.at(-1)[1] - s) < 1) win.at(-1)[1] = e;
      else win.push([s, e]);
    }
  }
  const now = Date.now() / 1000;
  const next = win.find(([, e]) => e > now) || [0, 0];
  return `${next[0]} ${next[1]} ${group}`;
}

// today + tomorrow as 96 half-hour marks, Kyiv dates
function kyivToday() {
  return new Intl.DateTimeFormat("en-CA", { timeZone: "Europe/Kyiv" }).format(new Date());
}
function nextDay(date) {
  return new Date(Date.parse(date + "T12:00:00Z") + 86400e3).toISOString().slice(0, 10);
}
async function svitloDay(region, group) {
  const r = (await svitloRegions()).get(region);
  const days = (r && r.schedule[group]) || {};
  const today = kyivToday();
  let marks = "";
  for (const date of [today, nextDay(today)]) {
    const d = days[date];
    for (let i = 0; i < 48; i++) {
      const hhmm = `${String(i >> 1).padStart(2, "0")}:${i & 1 ? "30" : "00"}`;
      const v = d ? d[hhmm] : undefined;
      marks += v === 2 ? "1" : v === 1 ? "0" : "?";
    }
  }
  return `${today} ${marks}`;
}
async function yasnoDay(group) {
  const all = JSON.parse(await getText(
    "https://app.yasno.ua/api/blackout-service/public/shutdowns/regions/25/dsos/902/planned-outages", 300));
  const g = all[group] || {};
  let marks = "";
  for (const day of ["today", "tomorrow"]) {
    const d = g[day];
    const m = new Array(48).fill(d && d.date ? "0" : "?");
    for (const sl of (d && d.slots) || []) {
      if (sl.type !== "Definite") continue;
      for (let i = Math.floor(sl.start / 30); i < Math.ceil(sl.end / 30) && i < 48; i++) m[i] = "1";
    }
    marks += m.join("");
  }
  return `${kyivToday()} ${marks}`;
}

async function regions() {
  const list = [...(await svitloRegions()).values()].map((r) => ({ id: r.id, name: r.name, name_en: r.name_en,
    groups: Object.keys(r.schedule) }));
  return JSON.stringify(list.sort((a, b) => a.name.localeCompare(b.name, "uk")));
}

async function outage(group) {
  const all = JSON.parse(await getText(
    "https://app.yasno.ua/api/blackout-service/public/shutdowns/regions/25/dsos/902/planned-outages", 300));
  const g = all[group] || {};
  const win = [];
  for (const day of ["today", "tomorrow"]) {
    const d = g[day];
    if (!d || !d.date) continue;
    const midnight = Date.parse(d.date) / 1000;
    for (const sl of d.slots || []) {
      if (sl.type !== "Definite") continue;
      const s = midnight + sl.start * 60, e = midnight + sl.end * 60;
      if (win.length && Math.abs(win.at(-1)[1] - s) < 1) win.at(-1)[1] = e;  // touching slots are one outage
      else win.push([s, e]);
    }
  }
  const now = Date.now() / 1000;
  const next = win.find(([, e]) => e > now) || [0, 0];
  return `${next[0]} ${next[1]} ${group}`;
}


export default {
  async fetch(req, env) {
    const u = new URL(req.url);
    try {
      switch (u.pathname) {
        case "/level": return text(await level());
        case "/pair": return text(await pair(u.searchParams.get("s")), 60);
        case "/outage": {
          const r = u.searchParams.get("r"), g = u.searchParams.get("g");
          return text(await (r ? svitloOutage(r, g) : outage(g)), 120);
        }
        case "/outday": {
          const r = u.searchParams.get("r"), g = u.searchParams.get("g");
          return text(await (r ? svitloDay(r, g) : yasnoDay(g)), 120);
        }
        case "/regions": return text(await regions(), 600);
        default: return text("claude cube relay");
      }
    } catch (e) {
      return new Response(`error ${e.message}`, { status: 502 });
    }
  },
};
