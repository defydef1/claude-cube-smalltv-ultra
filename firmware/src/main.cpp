// Claude Cube — custom firmware for GeekMagic SmallTV-Ultra (ESP8266 + ST7789 240x240).
// Screens: animated weather, Kyiv air-raid alert, photo/GIF album, Claude face, aerender
// progress, notes from the phone, night clock. HTTP API on port 80, OTA at /update.

#include <Arduino.h>
#include <new>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <U8g2_for_TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include <AnimatedGIF.h>
#include <time.h>
#include "webpage.h"
#include "aafonts.h"
#include "lang.h"

#define BL_PIN 5  // backlight, active low
#define W 240
#define H 240

TFT_eSPI tft;
// One block taken at boot, while the heap is still in one piece, and never freed: the
// weather sprite, the face sprite and the GIF decoder take turns in it. Allocating and
// freeing them separately left the heap in pieces too small for any of them.
#define ARENA_SIZE 14882  // 240x124 at 4 bits + 2, the largest tenant
uint8_t *arena = nullptr;

// A 4-bit sprite that draws into the arena instead of allocating its own buffer.
class ArenaSprite : public TFT_eSprite {
 public:
  ArenaSprite(TFT_eSPI *t) : TFT_eSprite(t) {}
  bool adopt(uint8_t *buf, size_t cap, int16_t w, int16_t h) {
    if (_created) return true;
    if (!buf || (size_t)((w * h) >> 1) + 1 > cap) return false;
    _iwidth = _dwidth = _bitwidth = w;
    _iheight = _dheight = h;
    cursor_x = cursor_y = 0;
    _sx = _sy = 0; _sw = w; _sh = h; _scolor = TFT_BLACK;
    _img8 = _img8_1 = _img8_2 = _img4 = buf;
    _img = (uint16_t *)buf;
    _created = true;
    rotation = 0;
    setViewport(0, 0, _dwidth, _dheight);
    setPivot(_iwidth / 2, _iheight / 2);
    return true;
  }
  void release() {  // give the arena back without freeing it
    if (_colorMap) { free(_colorMap); _colorMap = nullptr; }
    _created = false;
    _img8 = nullptr;
    _vpOoB = true;
  }
};
ArenaSprite spr(&tft);
extern AnimatedGIF *gif;  // defined with the album; shares the arena
bool lifeOwnsArena();     // a Life battle keeps its grids in the arena
void sendOk(const String &msg = "ok");
U8g2_for_TFT_eSPI u8t;  // draws on the panel
U8g2_for_TFT_eSPI u8s;  // draws into the sprite
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer updater;

// ---------- config ----------
struct Cfg {
  int brt = 60;        // day brightness, %
  int nbrt = 4;        // night brightness, %
  int non = 23;        // night starts, hour
  int noff = 7;        // night ends, hour
  int slide = 8;       // seconds per photo
  int wxsec = 20;      // seconds on the weather screen per round
  bool bgr = false;    // panel colour order
  int rot = 0;         // panel rotation
  float lat = 50.45, lon = 30.52;
  int wxstyle = 0;     // 0 animated sky, 1 "orb"
  String order = "weather,crypto,photos,pet,kyiv,outage";
  String relay;        // your relay/worker.js host: HTTPS sources as plain HTTP (empty = off)
  String outg;         // power outage group (Yasno, Kyiv), e.g. "1.1"; empty = off
  int cxsec = 12;      // seconds on the crypto screen, 0 = off
  String pairs = "USDUAH,BTCUSDT,WBTUSDT";  // Binance-style symbols; missing ones are pushed to /cx
  int lang = 0;        // index into LANG_CODES (lang.h)
  int areg = 0;        // alert region: index into REGION_KEYS (lang.h), 0 = Kyiv city
  String outr;         // outage region on the relay (svitlo.live ids, e.g. "lvivska-oblast"); empty = Yasno Kyiv
} cfg;

// the language's code ("uk"), and the index of a code (or -1)
String langCode(int i) {
  String all = LANG_CODES;
  for (int k = 0; k < i; k++) all = all.substring(all.indexOf(',') + 1);
  int c = all.indexOf(',');
  return c < 0 ? all : all.substring(0, c);
}
int langIndex(const String &code) {
  for (int i = 0; i < LANG_N; i++) if (langCode(i) == code) return i;
  return -1;
}

// a string in the cube's language. The tables live in flash; each call copies into the next of
// eight rotating buffers, so a few can be used together in one expression
const char *L(Str k) {
  static char buf[8][100];
  static uint8_t n = 0;
  n = (n + 1) & 7;
  int lang = cfg.lang >= 0 && cfg.lang < LANG_N ? cfg.lang : 0;
  strncpy_P(buf[n], (PGM_P)pgm_read_ptr(&LANG_TAB[lang][k]), sizeof buf[n] - 1);
  buf[n][sizeof buf[n] - 1] = 0;
  return buf[n];
}

void saveCfg() {
  JsonDocument d;
  d["brt"] = cfg.brt; d["nbrt"] = cfg.nbrt; d["non"] = cfg.non; d["noff"] = cfg.noff;
  d["slide"] = cfg.slide; d["wxsec"] = cfg.wxsec; d["bgr"] = cfg.bgr; d["rot"] = cfg.rot;
  d["lat"] = cfg.lat; d["lon"] = cfg.lon; d["cxsec"] = cfg.cxsec; d["wxstyle"] = cfg.wxstyle; d["order"] = cfg.order; d["relay"] = cfg.relay; d["outg"] = cfg.outg; d["areg"] = cfg.areg; d["outr"] = cfg.outr;
  d["pairs"] = cfg.pairs; d["lang"] = langCode(cfg.lang);
  File f = LittleFS.open("/cfg.json", "w");
  if (f) { serializeJson(d, f); f.close(); }
}

void loadCfg() {
  File f = LittleFS.open("/cfg.json", "r");
  if (!f) return;
  JsonDocument d;
  if (!deserializeJson(d, f)) {
    cfg.brt = d["brt"] | cfg.brt; cfg.nbrt = d["nbrt"] | cfg.nbrt;
    cfg.non = d["non"] | cfg.non; cfg.noff = d["noff"] | cfg.noff;
    cfg.slide = d["slide"] | cfg.slide; cfg.wxsec = d["wxsec"] | cfg.wxsec;
    cfg.bgr = d["bgr"] | cfg.bgr; cfg.rot = d["rot"] | cfg.rot;
    cfg.lat = d["lat"] | cfg.lat; cfg.lon = d["lon"] | cfg.lon;
    cfg.cxsec = d["cxsec"] | cfg.cxsec;
    cfg.wxstyle = d["wxstyle"] | cfg.wxstyle;
    if (d["order"].is<const char *>()) cfg.order = d["order"].as<String>();
    if (d["relay"].is<const char *>()) cfg.relay = d["relay"].as<String>();
    if (d["outg"].is<const char *>()) cfg.outg = d["outg"].as<String>();
    if (d["outr"].is<const char *>()) cfg.outr = d["outr"].as<String>();
    cfg.areg = constrain((int)(d["areg"] | cfg.areg), 0, REGION_N - 1);
    if (d["pairs"].is<const char *>()) cfg.pairs = d["pairs"].as<String>();
    if (d["lang"].is<const char *>()) cfg.lang = max(0, langIndex(d["lang"].as<String>()));
  }
  f.close();
}

// ---------- palette for 4-bit sprites ----------
enum { P_BG, P_WHITE, P_CORAL, P_CORALD, P_PINK, P_SKYDAY, P_SKYNIGHT, P_RAIN, P_CLOUD, P_CLOUDD,
       P_SUN, P_SUNO, P_SNOW, P_DIM, P_FLASH, P_RED };
uint16_t pal[16];
uint16_t C_CORAL, C_DIM, C_BG;

void initPalette() {
  pal[P_BG] = tft.color565(12, 10, 10);
  pal[P_WHITE] = tft.color565(245, 240, 232);
  pal[P_CORAL] = tft.color565(217, 119, 87);   // Claude orange
  pal[P_CORALD] = tft.color565(110, 52, 38);
  pal[P_PINK] = tft.color565(240, 120, 140);
  pal[P_SKYDAY] = tft.color565(40, 92, 160);
  pal[P_SKYNIGHT] = tft.color565(10, 14, 34);
  pal[P_RAIN] = tft.color565(140, 180, 240);
  pal[P_CLOUD] = tft.color565(170, 176, 188);
  pal[P_CLOUDD] = tft.color565(70, 76, 90);
  pal[P_SUN] = tft.color565(255, 210, 60);
  pal[P_SUNO] = tft.color565(255, 140, 30);
  pal[P_SNOW] = tft.color565(230, 240, 255);
  pal[P_DIM] = tft.color565(150, 150, 160);
  pal[P_FLASH] = tft.color565(200, 205, 230);
  pal[P_RED] = tft.color565(230, 40, 40);
  C_CORAL = pal[P_CORAL]; C_DIM = pal[P_DIM]; C_BG = pal[P_BG];
}

bool sprOpen(int w, int h) {
  if (gif || lifeOwnsArena()) return false;  // the decoder or a battle is using the arena
  spr.setColorDepth(4);
  if (!spr.adopt(arena, ARENA_SIZE, w, h)) return false;
  spr.createPalette(pal, 16);
  u8s.begin(spr);
  u8s.setFontMode(1);
  return true;
}
void sprClose() { if (spr.created()) spr.release(); }

// ---------- helpers ----------
void setBacklight(int pct) {
  pct = constrain(pct, 0, 100);
  analogWrite(BL_PIN, 1023 - pct * 1023 / 100);
}

void applyPanel() {
  tft.setRotation(cfg.rot);
  static const uint8_t mad[4] = {0x00, 0x60, 0xC0, 0xA0};  // MX/MY/MV per rotation
  tft.writecommand(0x36);
  tft.writedata(mad[cfg.rot & 3] | (cfg.bgr ? 0x08 : 0x00));
}

void textC(U8g2_for_TFT_eSPI &u, const char *s, int cx, int y) {
  u.drawUTF8(cx - u.getUTF8Width(s) / 2, y, s);
}

// cut a UTF-8 string so it fits maxW pixels
String fitText(U8g2_for_TFT_eSPI &u, String s, int maxW) {
  if (u.getUTF8Width(s.c_str()) <= maxW) return s;
  while (s.length() > 0) {
    int i = s.length() - 1;
    while (i > 0 && (s[i] & 0xC0) == 0x80) i--;
    s.remove(i);
    String t = s + "..";
    if (u.getUTF8Width(t.c_str()) <= maxW) return t;
  }
  return s;
}

bool timeValid() { return time(nullptr) > 1700000000; }
struct tm nowTm() { time_t t = time(nullptr); struct tm r; localtime_r(&t, &r); return r; }

bool isNight() {
  if (!timeValid()) return false;
  int h = nowTm().tm_hour;
  return cfg.non > cfg.noff ? (h >= cfg.non || h < cfg.noff) : (h >= cfg.non && h < cfg.noff);
}

// ---------- anti-aliased Montserrat (tools/make_fonts.py) ----------
uint16_t utf8Next(const char *&s) {
  uint8_t c = *s++;
  if (c < 0x80) return c;
  if ((c & 0xE0) == 0xC0) { uint16_t r = (c & 0x1F) << 6; if (*s) r |= *s++ & 0x3F; return r; }
  if ((c & 0xF0) == 0xE0) {
    uint16_t r = (c & 0x0F) << 12;
    if (*s) r |= (*s++ & 0x3F) << 6;
    if (*s) r |= *s++ & 0x3F;
    return r;
  }
  while (*s && (*s & 0xC0) == 0x80) s++;
  return '?';
}

bool aaFind(const AAFont &f, uint16_t cp, AAGlyph &g) {
  int lo = 0, hi = f.n - 1;  // glyphs are sorted by codepoint
  while (lo <= hi) {
    int m = (lo + hi) / 2;
    memcpy_P(&g, &f.g[m], sizeof g);
    if (g.cp == cp) return true;
    if (g.cp < cp) lo = m + 1; else hi = m - 1;
  }
  return false;
}

static inline uint8_t aaNib(const AAFont &f, uint32_t i) {
  uint8_t b = pgm_read_byte(f.bmp + (i >> 1));
  return (i & 1) ? (b & 15) : (b >> 4);
}

int aaWidth(const AAFont &f, const char *s, int track = 0) {
  int w = 0;
  AAGlyph g;
  while (*s) {
    uint16_t cp = utf8Next(s);
    w += aaFind(f, cp, g) ? g.adv + track : f.ascent / 4;
  }
  return w - (w > 0 ? track : 0);
}

// on the panel, blended against a known solid background; returns the pen x after the text
int aaDraw(const AAFont &f, int x, int base, const char *s, uint16_t fg, uint16_t bg, int track = 0) {
  uint16_t lut[16];
  for (int i = 0; i < 16; i++) lut[i] = tft.alphaBlend(i * 17, fg, bg);
  static uint16_t row[96];
  AAGlyph g;
  tft.setSwapBytes(true);
  tft.startWrite();
  while (*s) {
    uint16_t cp = utf8Next(s);
    if (!aaFind(f, cp, g)) { x += f.ascent / 4; continue; }
    if (g.w && g.h && g.w <= 96) {
      uint32_t idx = g.off;
      tft.setAddrWindow(x + g.xo, base + g.yo, g.w, g.h);
      for (int r = 0; r < g.h; r++) {
        for (int c = 0; c < g.w; c++) row[c] = lut[aaNib(f, idx++)];
        tft.pushPixels(row, g.w);
      }
    }
    x += g.adv + track;
  }
  tft.endWrite();
  tft.setSwapBytes(false);
  return x - track;
}

// into the 4-bit sprite: alpha 0..15 goes through ramp[] to a palette index, 255 leaves the pixel
int bandY0 = 0, bandY1 = 240;  // rows the weather sprite holds in the current pass
int aaSpr(const AAFont &f, int x, int base, const char *s, const uint8_t *ramp, int track = 0) {
  AAGlyph g;
  while (*s) {
    uint16_t cp = utf8Next(s);
    if (!aaFind(f, cp, g)) { x += f.ascent / 4; continue; }
    if (base + g.yo >= bandY1 || base + g.yo + g.h <= bandY0) { x += g.adv + track; continue; }
    uint32_t idx = g.off;
    for (int r = 0; r < g.h; r++)
      for (int c = 0; c < g.w; c++) {
        uint8_t p = ramp[aaNib(f, idx++)];
        if (p != 255) spr.drawPixel(x + g.xo + c, base + g.yo + r, p);
      }
    x += g.adv + track;
  }
  return x - track;
}

// ---------- shared drawing helpers (screens follow mock/screens.py) ----------
#define GROUND 0x040406UL
#define C_GREY 0x82828CUL
#define C_RULE 0x1E1E24UL
#define C_CORALX 0xD97757UL
#define C_ROSE 0xFF607CUL
#define C_RED 0xFF3B30UL
#define C_GREEN 0x30D158UL
#define C_AE 0x9999FFUL
#define C_CYAN 0x4CC9FFUL
#define C_PLAY 0xFFB020UL  // the console screen

uint16_t rgb(uint32_t c) { return tft.color565(c >> 16, (c >> 8) & 255, c & 255); }

uint32_t lerpRGB(uint32_t a, uint32_t b, int num, int den) {
  uint32_t r = 0;
  for (int s = 0; s <= 16; s += 8) {
    int ca = (a >> s) & 255, cb = (b >> s) & 255;
    r |= (uint32_t)(ca + (cb - ca) * num / den) << s;
  }
  return r;
}
uint32_t mixF(uint32_t a, uint32_t b, float t) { return lerpRGB(a, b, (int)(t * 1000), 1000); }

// AA text over a background that changes by row (gradients); bgAt(y) gives the colour
int aaDrawBg(const AAFont &f, int x, int base, const char *s, uint16_t fg, uint16_t (*bgAt)(int)) {
  static uint16_t row[96];
  uint16_t lut[16];
  AAGlyph g;
  tft.setSwapBytes(true);
  tft.startWrite();
  while (*s) {
    uint16_t cp = utf8Next(s);
    if (!aaFind(f, cp, g)) { x += f.ascent / 4; continue; }
    if (g.w && g.h && g.w <= 96) {
      uint32_t idx = g.off;
      tft.setAddrWindow(x + g.xo, base + g.yo, g.w, g.h);
      int lastBg = -1;
      for (int r = 0; r < g.h; r++) {
        uint16_t bg = bgAt(base + g.yo + r);
        if (bg != lastBg) { for (int i = 0; i < 16; i++) lut[i] = tft.alphaBlend(i * 17, fg, bg); lastBg = bg; }
        for (int c = 0; c < g.w; c++) row[c] = lut[aaNib(f, idx++)];
        tft.pushPixels(row, g.w);
      }
    }
    x += g.adv;
  }
  tft.endWrite();
  tft.setSwapBytes(false);
  return x;
}

void aaRight(const AAFont &f, int xr, int base, const char *s, uint16_t fg, uint16_t bg) {
  aaDraw(f, xr - aaWidth(f, s), base, s, fg, bg);
}
void aaCenter(const AAFont &f, int cx, int base, const char *s, uint16_t fg, uint16_t bg) {
  aaDraw(f, cx - aaWidth(f, s) / 2, base, s, fg, bg);
}

// cut a UTF-8 string so it fits maxW pixels in font f
String aaFit(const AAFont &f, String s, int maxW) {
  if (aaWidth(f, s.c_str()) <= maxW) return s;
  while (s.length()) {
    int i = s.length() - 1;
    while (i > 0 && (s[i] & 0xC0) == 0x80) i--;
    s.remove(i);
    String t = s + "..";
    if (aaWidth(f, t.c_str()) <= maxW) return t;
  }
  return s;
}

// soft glow: concentric filled circles from dim to bright, straight on the panel
void tftGlow(int cx, int cy, int r, uint32_t col, uint32_t base, int steps, float k = 1.0) {
  for (int i = 0; i < steps; i++) {
    int rr = r * (steps - i) / steps;
    float t = pow((float)(i + 1) / steps, 2.2) * k;
    tft.fillCircle(cx, cy, rr, rgb(mixF(base, col, t)));
  }
}
// colour of that glow at distance d from its centre
uint32_t glowAt(float d, int r, uint32_t col, uint32_t base, int steps, float k = 1.0) {
  uint32_t c = base;
  for (int i = 0; i < steps; i++) {
    if (r * (steps - i) / (float)steps >= d) c = mixF(base, col, pow((float)(i + 1) / steps, 2.2) * k);
  }
  return c;
}

// dark -> lit -> dark vertical band (the "sky" of alert screens)
uint32_t bumpGradA, bumpGradB;
int bumpGradH = 150;
uint32_t bumpRow(int y) {
  if (y < 0 || y >= bumpGradH) return GROUND;
  float t = (float)y / (bumpGradH - 1);
  return mixF(bumpGradA, bumpGradB, pow(sin(t * PI), 1.3));
}
void drawBump(int y0, int y1) {
  for (int y = y0; y < y1; y++) tft.drawFastHLine(0, y, W, rgb(bumpRow(y)));
}

void ruleLine(int y = 194, uint32_t c = C_RULE) { tft.drawFastHLine(12, y, 216, rgb(c)); }

void thickPoly(const int *xy, int n, float w, uint16_t col, uint16_t bg) {
  for (int i = 0; i + 1 < n; i++) tft.drawWideLine(xy[2 * i], xy[2 * i + 1], xy[2 * i + 2], xy[2 * i + 3], w, col, bg);
}

void tftHeart(int cx, int cy, int r, uint16_t col, uint16_t bg) {
  tft.fillSmoothCircle(cx - r, cy, r, col, bg);
  tft.fillSmoothCircle(cx + r, cy, r, col, bg);
  tft.fillTriangle(cx - r * 193 / 100, cy + r * 38 / 100, cx + r * 193 / 100, cy + r * 38 / 100, cx, cy + r * 23 / 10, col);
}

// ---------- state ----------
uint32_t minHeap = 0xFFFFFFFF;  // lowest free heap seen, for /status
enum Screen { S_NONE, S_WEATHER, S_PHOTO, S_FACE, S_ALERT, S_ALLCLEAR, S_RENDER, S_NOTE, S_NIGHT, S_SETUP, S_CRYPTO, S_OUTAGE, S_VIZ, S_K3026, S_LIFE, S_GAME, S_LISTEN };
Screen cur = S_NONE;
uint32_t screenSince = 0;

// weather
struct Wx { bool ok = false; float t = 0, tmin = 0, tmax = 0, wind = 0; int hum = 0, code = 0; bool day = true; } wx;
int wxForce = -1;  // preview a weather code from the web page
int wxForceDay = 1;  // and whether that preview is by day
uint32_t wxNext = 0;

// air raid alert, Kyiv city
bool alertOn = false, alertKnown = false;
time_t alertStart = 0, alertEnd = 0;
uint32_t alertNext = 0;
int alertForce = -1;

// notes
String noteText;
uint32_t noteUntil = 0;

// render
struct Rnd { bool on = false; int p = 0; long eta = -1; String name; uint32_t last = 0; int frame = 0, total = 0; long elapsed = 0; uint32_t start = 0, doneUntil = 0; int q = 0, qn = 0; String next; } rnd;

// album
#define MAXPH 48
String photos[MAXPH];
int nPhotos = 0, phIdx = -1;
String forcePhoto;
uint32_t forceUntil = 0;

// rotation
int roundIdx = 0;  // 0 weather, 1 crypto, 2 photos, 3 pet, 4 Kyiv 3026, 5 outage
const char *SLOT_NAMES[6] = {"weather", "crypto", "photos", "pet", "kyiv", "outage"};
int orderIds[6], nOrder = 0, roundPos = -1;
void parseOrder() {
  nOrder = 0;
  String list = cfg.order + ",";
  int from = 0;
  for (int i = 0; i < (int)list.length() && nOrder < 6; i++) {
    if (list[i] != ',') continue;
    String n = list.substring(from, i);
    n.trim();
    from = i + 1;
    for (int k = 0; k < 6; k++) if (n == SLOT_NAMES[k]) orderIds[nOrder++] = k;
  }
  roundPos = -1;
}

// crypto (Binance public market-data mirror, plain HTTP)
#define MAXC 4
struct Coin { String sym; float price = 0, chg = 0; float spark[24]; uint8_t nSpark = 0; bool ok = false; uint32_t pushed = 0; };
struct Cx { Coin c[MAXC]; int n = 0; uint32_t stamp = 0; } cx;
uint32_t cxNext = 0;
uint32_t roundUntil = 0;

// ---------- Claude sessions ----------
enum Emo { E_IDLE, E_THINK, E_WORK, E_ATTN, E_HAPPY, E_SAD, E_SLEEP, E_LOVE,
           E_MISS, E_YAWN, E_YAWN2, E_SNORE, E_HUNGRY, E_STARVE, E_AWAY, E_YUM, E_JUDGE, E_HELLO, E_LISTEN, E_N };
const char *emoNames[E_N] = {"idle", "thinking", "working", "attention", "happy", "sad", "sleepy", "love",
                             "miss", "yawn", "yawn2", "snore", "hungry", "starve", "away", "yum", "judge", "hello", "listen"};
const uint8_t emoPrio[E_N] = {0, 2, 2, 5, 3, 4, 0, 1, 4, 0, 0, 0, 0, 0, 0, 3, 3, 3, 3};
// his voice while he dictates to Claude (CubeSenses -> cube_viz.py, UDP 'L', level)
float listenLevel = 0, listenSmooth = 0;
uint32_t listenAt = 0;
bool listening() { return listenAt && millis() - listenAt < 800; }
// where he sits, from the camera tracker on the Mac (tools/cube_eyes.py): -1..1, fresh for 1.5 s
float gazeX = 0, gazeY = 0;
uint32_t gazeAt = 0;
bool gazeFresh() { return gazeAt && millis() - gazeAt < 1500; }
// pet state: when Claude last got a task, when anything last happened, tasks today
uint32_t lastFeed = 0, lastActivity = 0;
int tasksToday = 0, tasksDay = -1;
int forcedEmo = -1;  // night watcher / pet, chosen by decide()
#define TEAMS_UNREAD 0
uint32_t macSeen = 0;  // last ping from the Mac
long macIdle = 0;      // seconds since his last keyboard or mouse input
// offline: the Mac went quiet (asleep or off) or nobody touched it for 15 minutes
bool offline() {
  if (!macSeen) return millis() - lastActivity > 30UL * 60000;
  return millis() - macSeen > 180000 || macIdle > 900;
}
String forcedSub;

struct Sess { bool used; uint32_t id; uint8_t emo; String label; uint32_t t; };
#define MAXS 8
Sess sess[MAXS];
uint8_t manualEmo = E_IDLE;
String manualLabel;
uint32_t manualUntil = 0;
uint32_t faceUntil = 0;
uint8_t shownEmo = 255;

int emoFromName(const String &n) {
  for (int i = 0; i < E_N; i++) if (n == emoNames[i]) return i;
  return -1;
}

int topSession() {
  int best = -1;
  for (int i = 0; i < MAXS; i++) {
    if (!sess[i].used) continue;
    if (best < 0 || emoPrio[sess[i].emo] > emoPrio[sess[best].emo] ||
        (emoPrio[sess[i].emo] == emoPrio[sess[best].emo] && sess[i].t > sess[best].t)) best = i;
  }
  return best;
}
int countBusy() {
  int n = 0;
  for (int i = 0; i < MAXS; i++) if (sess[i].used && (sess[i].emo == E_THINK || sess[i].emo == E_WORK)) n++;
  return n;
}
bool anyAttn() {
  for (int i = 0; i < MAXS; i++) if (sess[i].used && sess[i].emo == E_ATTN) return true;
  return false;
}

void sessionEvent(uint32_t id, int emo, const String &label) {
  int slot = -1, freeSlot = -1, oldest = 0;
  for (int i = 0; i < MAXS; i++) {
    if (sess[i].used && sess[i].id == id) slot = i;
    if (!sess[i].used && freeSlot < 0) freeSlot = i;
    if (sess[i].t < sess[oldest].t) oldest = i;
  }
  if (emo < 0) { if (slot >= 0) sess[slot].used = false; return; }  // session ended
  if (slot < 0) slot = freeSlot >= 0 ? freeSlot : oldest;
  bool changed = !sess[slot].used || sess[slot].emo != emo;
  lastActivity = millis();
  if (emo == E_THINK && changed) {
    struct tm tm = nowTm();
    if (tm.tm_yday != tasksDay) { tasksDay = tm.tm_yday; tasksToday = 0; }
    tasksToday++;
    if (millis() - lastFeed > 2UL * 3600000) {  // fed after a hungry spell
      manualEmo = E_YUM; manualLabel = L(T_TASKS_TODAY) + String(tasksToday); manualUntil = millis() + 8000;
    }
    lastFeed = millis();
  }
  sess[slot].used = true; sess[slot].id = id; sess[slot].emo = emo; sess[slot].t = millis();
  if (label.length()) sess[slot].label = label;
  if (changed) {
    uint32_t hold = (emo == E_HAPPY || emo == E_SAD) ? 10000 : 20000;
    faceUntil = max(faceUntil, (uint32_t)(millis() + hold));
  }
}

void expireSessions() {
  uint32_t now = millis();
  for (int i = 0; i < MAXS; i++) {
    if (!sess[i].used) continue;
    uint32_t age = now - sess[i].t;
    uint8_t e = sess[i].emo;
    if ((e == E_HAPPY || e == E_SAD) && age > 12000) sess[i].used = false;
    else if ((e == E_THINK || e == E_WORK) && age > 15UL * 60000) sess[i].used = false;
    else if (e == E_ATTN && age > 30UL * 60000) sess[i].used = false;
  }
}

// ---------- face ----------
// Eyes and their glow animate in a 4-bit sprite (top 140 px); word, sub-line, live
// indicator and the sessions strip sit on the panel below, on the main screen's grid.
#define FACE_H 124  // eyes and glints fit; labels sit below on the panel
#define EYE_Y 84
// face sprite palette: ground, 8 glow steps, coral, white, cheek, rose, lid, dark, grey
enum { F_G, F_GL1, F_GL8 = 8, F_CORAL, F_WHITE, F_CHEEK, F_ROSE, F_LID, F_DARK, F_GREYI };
uint16_t facePal[16];
int facePalMode = -1;  // 0 normal, 1 sleepy, 2 attention

struct EyeT { float w, h, gx, gy; };
EyeT eyeCur = {46, 66, 0, 0};
uint32_t nextBlink = 0, blinkStart = 0, nextGaze = 0;
float idleGx = 0, idleGy = 0;
String faceL1, faceL2;
int faceCount = -1, faceDrawnEmo = -1;
uint32_t faceIndT = 0;

const char *emoWord(uint8_t e) {
  switch (e) {
    case E_THINK: return L(T_EMO_THINK);
    case E_WORK: return L(T_EMO_WORK);
    case E_ATTN: return L(T_EMO_ATTN);
    case E_HAPPY: return L(T_EMO_HAPPY);
    case E_SAD: return L(T_EMO_SAD);
    case E_SLEEP: return L(T_EMO_SLEEP);
    case E_LOVE: return L(T_EMO_LOVE);
    case E_MISS: return L(T_EMO_MISS);
    case E_YAWN: return L(T_EMO_YAWN);
    case E_YAWN2: return L(T_EMO_YAWN2);
    case E_SNORE: return L(T_EMO_SNORE);
    case E_HUNGRY: return L(T_EMO_HUNGRY);
    case E_STARVE: return L(T_EMO_STARVE);
    case E_AWAY: return L(T_EMO_AWAY);
    case E_YUM: return L(T_EMO_YUM);
    case E_JUDGE: return L(T_EMO_JUDGE);
    case E_HELLO: return L(T_EMO_HELLO);
    case E_LISTEN: return L(T_EMO_LISTEN);
    default: return L(T_EMO_IDLE);
  }
}
const char *emoSub(uint8_t e) {
  switch (e) {
    case E_SLEEP: return L(T_SUB_SLEEP);
    case E_LOVE: return L(T_SUB_LOVE);
    case E_ATTN: return L(T_SUB_ATTN);
    case E_MISS: return L(T_SUB_MISS);
    case E_AWAY: return L(T_SUB_AWAY);
    case E_JUDGE: return L(T_SUB_JUDGE);
    case E_HELLO: return L(T_SUB_HELLO);
    default: return L(T_SUB_IDLE);
  }
}

void setFacePal(int mode) {
  facePalMode = mode;
  uint32_t glowCol = mode == 1 ? 0x0E0806 : 0x220F0A;
  facePal[F_G] = rgb(mode == 2 ? C_CORALX : GROUND);
  for (int i = 0; i < 8; i++) facePal[F_GL1 + i] = rgb(mode == 2 ? C_CORALX : mixF(GROUND, glowCol, pow((i + 1) / 8.0, 2.2)));
  facePal[F_CORAL] = rgb(C_CORALX);
  facePal[F_WHITE] = rgb(0xFFFFFF);
  facePal[F_CHEEK] = rgb(0x602820);
  facePal[F_ROSE] = rgb(C_ROSE);
  facePal[F_LID] = rgb(0x7C4230);
  facePal[F_DARK] = rgb(0x160804);
  facePal[F_GREYI] = rgb(C_GREY);
  spr.createPalette(facePal, 16);
}

// palette slot of the glow under a point, so cut-outs match what's behind them
uint8_t faceGlowAt(int x, int y) {
  if (facePalMode == 2) return F_G;
  float d = sqrt((x - 120) * (x - 120) + (y - EYE_Y) * (y - EYE_Y));
  uint8_t c = F_G;
  for (int i = 0; i < 8; i++) if (116.0 * (8 - i) / 8 >= d) c = F_GL1 + i;
  return c;
}

void brush(int x0, int y0, int x1, int y1, int r, uint8_t c) {
  int n = max(1, (int)(sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0)) / 2));
  for (int i = 0; i <= n; i++) spr.fillCircle(x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, r, c);
}

void sprHeart(int cx, int cy, int r, uint8_t c) {
  spr.fillCircle(cx - r, cy, r, c);
  spr.fillCircle(cx + r, cy, r, c);
  spr.fillTriangle(cx - r * 193 / 100, cy + r * 38 / 100, cx + r * 193 / 100, cy + r * 38 / 100, cx, cy + r * 23 / 10, c);
}

void pillEye(int cx, int cy, int w, int h, uint8_t col, bool glint) {
  spr.fillRoundRect(cx - w / 2, cy - h / 2, w, h, min(w, h) / 2, col);
  if (glint && h > 24) {
    int gx = cx - w / 2 + 8, gy = cy - h / 2 + 8;
    spr.fillRoundRect(gx, gy, 10, 13, 5, F_WHITE);
    spr.fillCircle(gx + 15, gy + 17, 2, F_WHITE);
  }
}

void faceFrame(uint8_t emo, uint32_t t) {
  int mode = emo == E_ATTN ? 2 : (emo == E_SLEEP || emo == E_SNORE || emo == E_AWAY) ? 1 : 0;
  if (mode != facePalMode) setFacePal(mode);
  spr.fillSprite(F_G);
  if (mode != 2)
    for (int i = 0; i < 8; i++) spr.fillCircle(120, EYE_Y, 116 * (8 - i) / 8, F_GL1 + i);

  // tween pill eyes towards the state's pose, with gaze and blinks
  float s = t / 1000.0;
  // with the camera on, the eyes look at him: his position replaces the idle wander
  float lookX = gazeFresh() ? gazeX * 14 : idleGx, lookY = gazeFresh() ? gazeY * 9 : idleGy;
  EyeT tg = {46, 66, lookX, lookY};
  if (emo == E_THINK) tg = {46, 66, 7 + (gazeFresh() ? lookX * 0.5f : 0), (float)(-6 - fabs(sin(s * 1.5)) * 3)};
  if (emo == E_WORK) tg = {46, 52, (float)(sin(s * 2.2) * 8) + (gazeFresh() ? lookX * 0.6f : 0), 4};
  if (emo == E_JUDGE) tg = {52, 30, 0, 4};  // a hard squint straight at him
  if (emo == E_LISTEN) tg = {48, 62 + listenSmooth * 16, 0, -5};  // all ears: eyes open wider as he speaks
  if (t > nextGaze) { idleGx = random(-6, 7); idleGy = random(-4, 5); nextGaze = t + random(1500, 4000); }
  const float k = 0.22;
  eyeCur.w += (tg.w - eyeCur.w) * k; eyeCur.h += (tg.h - eyeCur.h) * k;
  eyeCur.gx += (tg.gx - eyeCur.gx) * k; eyeCur.gy += (tg.gy - eyeCur.gy) * k;
  float blink = 1;
  if (t > nextBlink) { blinkStart = t; nextBlink = t + random(2500, 6000); }
  if (t - blinkStart < 160) blink = 1 - sin((t - blinkStart) / 160.0 * PI) * 0.9;

  switch (emo) {
    case E_ATTN: {
      int bob = (int)(sin(s * 5) * 2);
      for (int sx = -1; sx <= 1; sx += 2) {
        int x = 120 + sx * 42, h = (int)(80 * blink);
        spr.fillRoundRect(x - 27, 72 - h / 2 + bob, 54, h, min(22, h / 2), F_DARK);
        if (h > 30) spr.fillRoundRect(x - 18, 72 - 31 + bob, 12, 16, 6, F_WHITE);
      }
      break;
    }
    case E_HAPPY:
    case E_HELLO: {
      int bob = (int)(-fabs(sin(s * 4)) * 4);
      for (int sx = -1; sx <= 1; sx += 2) {
        int x = 120 + sx * 40, y = EYE_Y + bob;
        brush(x - 20, y + 8, x, y - 14, 6, F_CORAL);
        brush(x, y - 14, x + 20, y + 8, 6, F_CORAL);
        spr.fillEllipse(x, EYE_Y + 31, 16, 7, F_CHEEK);
      }
      break;
    }
    case E_SAD:
      for (int sx = -1; sx <= 1; sx += 2) {
        int x = 120 + sx * 40, y = EYE_Y;
        spr.fillRoundRect(x - 23, y - 14, 46, 48, 22, F_CORAL);
        // the outer top corner droops away
        int ox = x - sx * 30, ix = x + sx * 30;
        spr.fillTriangle(ox, y - 20, ix, y - 20, ox, y + 8, faceGlowAt(ox, y - 8));
        spr.fillRoundRect(x - 13, y + 18, 8, 9, 4, F_WHITE);
      }
      break;
    case E_SLEEP: {
      for (int sx = -1; sx <= 1; sx += 2) {
        int x = 120 + sx * 40, cy = EYE_Y - 18;
        for (int a = 35; a <= 145; a += 5) {
          float r = a * PI / 180;
          spr.fillCircle(x + cos(r) * 20.5, cy + sin(r) * 20.5, 3, F_LID);
        }
      }
      static const uint8_t ramp[16] = {255, 255, 255, 255, 255, F_GREYI, F_GREYI, F_GREYI, F_GREYI, F_GREYI, F_GREYI, F_GREYI, F_GREYI, F_GREYI, F_GREYI, F_GREYI};
      int shown = (t / 600) % 4;  // the z's climb one by one
      for (int i = 0; i < shown && i < 3; i++) aaSpr(F_NUM, 180 + i * 15, 52 - i * 17, "z", ramp);
      break;
    }
    case E_LOVE: {
      int r = 13 + (int)(sin(s * 6) * 1.5);
      for (int sx = -1; sx <= 1; sx += 2) {
        int x = 120 + sx * 40;
        sprHeart(x, EYE_Y - 8, r, F_ROSE);
        spr.fillRoundRect(x - 18, EYE_Y - 15, 7, 9, 3, F_WHITE);
      }
      break;
    }
    case E_MISS: {
      // a heartbeat: two quick thumps, then a pause
      int ph = t % 1100;
      float bump = (ph < 140 ? sin(ph / 140.0 * PI) : 0) + (ph > 260 && ph < 400 ? 0.8 * sin((ph - 260) / 140.0 * PI) : 0);
      int r = 13 + (int)(bump * 5);
      for (int sx = -1; sx <= 1; sx += 2) {
        int x = 120 + sx * 40;
        sprHeart(x, EYE_Y - 8, r, F_ROSE);
        spr.fillRoundRect(x - r - 4, EYE_Y - 15, 7, 9, 3, F_WHITE);
      }
      break;
    }
    case E_YAWN:
    case E_YAWN2: {
      // heavy lids; every few seconds a big yawn (more often when it insists)
      uint32_t cyc = emo == E_YAWN ? 4200 : 2400, ph = t % cyc;
      float y = ph < 1300 ? sin(ph / 1300.0 * PI) : 0;
      int eh = max(6, (int)((26 - y * 18) * blink));
      for (int sx = -1; sx <= 1; sx += 2) pillEye(120 + sx * 40, EYE_Y + 6, 46, eh, F_CORAL, eh > 24);
      if (y > 0.05) { spr.fillEllipse(120, EYE_Y + 44, 6 + (int)(y * 8), 4 + (int)(y * 12), F_LID); spr.fillEllipse(120, EYE_Y + 45, 3 + (int)(y * 6), 2 + (int)(y * 9), F_DARK); }
      break;
    }
    case E_SNORE: {
      for (int sx = -1; sx <= 1; sx += 2) {
        int x = 120 + sx * 40, cy = EYE_Y - 18;
        for (int a = 35; a <= 145; a += 5) { float r = a * PI / 180; spr.fillCircle(x + cos(r) * 20.5, cy + sin(r) * 20.5, 3, F_LID); }
      }
      // z's drifting up all over the face
      static const uint8_t ramp[16] = {255, 255, 255, 255, 255, F_GREYI, F_GREYI, F_GREYI, F_GREYI, F_GREYI, F_GREYI, F_GREYI, F_GREYI, F_GREYI, F_GREYI, F_GREYI};
      for (int i = 0; i < 7; i++) {
        int y = 150 - (int)((t / 25 + i * 37) % 170);
        int x = 18 + i * 33 + (int)(sin(t / 600.0 + i) * 8);
        aaSpr(F_NUM, x, y, "z", ramp);
      }
      break;
    }
    case E_HUNGRY:
    case E_STARVE: {
      int shake = (int)(sin(t / 25.0) * (emo == E_STARVE ? 1.5 : 0.8));
      for (int sx = -1; sx <= 1; sx += 2) {
        int x = 120 + sx * 40 + shake, y = EYE_Y;
        if (emo == E_HUNGRY) {
          spr.fillRoundRect(x - 23, y - 14, 46, 48, 22, F_CORAL);
          int ox = x - sx * 30, ix = x + sx * 30;
          spr.fillTriangle(ox, y - 20, ix, y - 20, ox, y + 8, faceGlowAt(ox, y - 8));
          spr.fillRoundRect(x - 13, y + 18, 8, 9, 4, F_WHITE);
        } else {  // sunken, thin
          spr.fillRoundRect(x - 15, y - 22, 30, 44, 15, F_CORAL);
          spr.fillRoundRect(x - 10, y - 17, 20, 34, 10, F_LID);
          spr.fillCircle(x - 4, y - 8, 2, F_WHITE);
        }
      }
      break;
    }
    case E_AWAY: {
      // nobody home: a little dacha with smoke from the chimney
      int hx = 120, hy = EYE_Y + 6;
      spr.fillRect(hx - 24, hy - 10, 48, 34, F_CORAL);
      spr.fillTriangle(hx - 32, hy - 8, hx + 32, hy - 8, hx, hy - 36, F_LID);
      spr.fillRect(hx + 12, hy - 34, 8, 16, F_LID);
      spr.fillRect(hx - 6, hy + 6, 12, 18, F_DARK);
      spr.fillRect(hx - 18, hy - 2, 9, 8, F_DARK);
      for (int i = 0; i < 3; i++) {
        int sy = hy - 40 - (int)((t / 40 + i * 25) % 75);
        spr.fillCircle(hx + 16 + (int)(sin(sy / 9.0) * 4), sy, 3 + i, F_GREYI);
      }
      break;
    }
    case E_YUM: {
      int bob = (int)(-fabs(sin(s * 6)) * 4);
      for (int sx = -1; sx <= 1; sx += 2) {
        int x = 120 + sx * 40, y = EYE_Y + bob;
        brush(x - 20, y + 8, x, y - 14, 6, F_CORAL);
        brush(x, y - 14, x + 20, y + 8, 6, F_CORAL);
        spr.fillEllipse(x, EYE_Y + 31, 16, 7, F_CHEEK);
      }
      break;
    }
    default:
      for (int sx = -1; sx <= 1; sx += 2)
        pillEye(120 + sx * 40 + (int)eyeCur.gx, EYE_Y + (int)eyeCur.gy, (int)eyeCur.w, max(6, (int)(eyeCur.h * blink)), F_CORAL, blink > 0.6 && emo != E_JUDGE);
      if (emo == E_JUDGE) {  // angry: brows slope down towards the middle
        for (int sx = -1; sx <= 1; sx += 2) {
          int cx = 120 + sx * 40 + (int)eyeCur.gx, top = EYE_Y + (int)eyeCur.gy - (int)eyeCur.h / 2;
          int inner = cx - sx * 30, outer = cx + sx * 30;
          spr.fillTriangle(inner, top - 4, outer, top - 4, inner, top + 16, faceGlowAt(cx, top));
        }
      }
  }
  spr.pushSprite(0, 0);
}

// live indicator right of the word: dots, spinner, tick or heart
void faceIndicator(uint8_t emo, uint32_t t) {
  // only states with a live indicator touch this corner: the "needs you" word runs under it
  if (emo != E_THINK && emo != E_WORK && emo != E_HAPPY && emo != E_LOVE && emo != E_MISS && emo != E_YUM && emo != E_HELLO) return;
  const int ix = 222, iy = 156;
  uint16_t bg = rgb(GROUND);
  tft.fillRect(ix - 46, iy - 15, 48, 30, bg);
  if (emo == E_THINK) {
    int lit = (t / 300) % 3;
    for (int i = 0; i < 3; i++)
      tft.fillSmoothCircle(ix - 34 + i * 13, iy, 3, rgb(mixF(GROUND, C_CORALX, i == lit ? 1.0 : 0.4)), bg);
  } else if (emo == E_WORK) {
    int a = (t / 4) % 360;
    tft.drawSmoothArc(ix - 10, iy, 11, 7, 0, 360, rgb(0x341C16), bg, false);
    tft.drawSmoothArc(ix - 10, iy, 11, 7, a, (a + 120) % 360, rgb(C_CORALX), bg, true);
  } else if (emo == E_HAPPY || emo == E_YUM || emo == E_HELLO) {
    const int xy[] = {ix - 22, iy, ix - 14, iy + 7, ix, iy - 8};
    thickPoly(xy, 3, 4, rgb(C_CORALX), bg);
  } else if (emo == E_LOVE || emo == E_MISS) {
    tftHeart(ix - 12, iy - 3, 6, rgb(C_ROSE), bg);
  }
}

void faceLabels(uint8_t emo, const String &sub, int count) {
  bool att = emo == E_ATTN;
  uint32_t bgc = att ? C_CORALX : GROUND;
  uint16_t bg = rgb(bgc);
  tft.fillRect(0, FACE_H, W, H - FACE_H, bg);
  // the lower part of the glow, so it continues past the sprite's edge without a seam
  bool dim = emo == E_SLEEP || emo == E_SNORE || emo == E_AWAY;
  if (!att) tftGlow(120, EYE_Y, 116, dim ? 0x0E0806 : 0x220F0A, GROUND, 8);
  uint16_t ink = rgb(att ? 0x180905 : (dim ? C_GREY : 0xFFFFFF));
  uint16_t subInk = rgb(att ? 0x602616 : C_GREY);
  if (att) aaDraw(F_ATTN, 14, 170, emoWord(emo), ink, bg, -1);  // tight: ТИ ПОТРІБЕН just fits
  else aaDraw(F_WORD, 14, 164, emoWord(emo), ink, bg);
  aaDraw(F_SMALL, 14, att ? 187 : 184, aaFit(F_SMALL, sub, att ? 212 : 160).c_str(), subInk, bg);
  ruleLine(att ? 196 : 194, att ? 0xB45E44 : C_RULE);
  aaDraw(F_SMALL, 14, 222, L(T_SESSIONS), subInk, bg);
  uint16_t on = rgb(att ? 0x180905 : C_CORALX), off = rgb(att ? 0xB46046 : 0x281A16);
  for (int i = 0; i < 4; i++) tft.fillSmoothCircle(196 - (3 - i) * 13, 214, 4, i < count ? on : off, bg);
  char b[8];
  snprintf(b, sizeof b, "%d", count);
  aaRight(F_NUM, 228, 222, b, att ? rgb(0x180905) : TFT_WHITE, bg);
}

void faceScreen(bool enter) {
  if (enter) { facePalMode = -1; faceDrawnEmo = -1; }
  if (!spr.created()) {  // no memory right now: try again next frame rather than freeze
    if (!sprOpen(W, FACE_H)) return;
    facePalMode = -1;
  }
  uint8_t emo;
  String sub;
  int ts = topSession();
  if (millis() < manualUntil) { emo = manualEmo; sub = manualLabel; }
  else if (forcedEmo >= 0 && !anyAttn()) { emo = forcedEmo; sub = forcedSub; }
  else if (ts >= 0) { emo = sess[ts].emo; sub = sess[ts].label; }
  else emo = E_IDLE;
  if (!sub.length()) sub = emoSub(emo);
  int count = countBusy();
  if (emo != faceDrawnEmo || sub != faceL1 || count != faceCount) {
    faceLabels(emo, sub, count);
    faceDrawnEmo = emo; faceL1 = sub; faceCount = count;
  }
  uint32_t t = millis();
  faceFrame(emo, t);
  if (t - faceIndT > 90) { faceIndT = t; faceIndicator(emo, t); }
}

// ---------- weather ----------
enum Sky { K_CLEAR, K_CLOUDS, K_FOG, K_RAIN, K_SNOW, K_STORM };
struct Part { int16_t x, y; int8_t v, s; };
#define NP 56
Part parts[NP];
#define SKY_H 132  // animated sky; it fades into the black ground
#define WX_H 184   // weather sprite: sky plus temperature, clock and date
int skyKind = -1;
uint32_t flashUntil = 0;

int skyOf(int c) {
  if (c <= 1) return K_CLEAR;
  if (c <= 3) return K_CLOUDS;
  if (c == 45 || c == 48) return K_FOG;
  if ((c >= 71 && c <= 77) || c == 85 || c == 86) return K_SNOW;
  if (c >= 95) return K_STORM;
  return K_RAIN;
}

const char *wxWord(int c) {
  switch (c) {
    case 0: return L(T_WX_CLEAR);
    case 1: return L(T_WX_FEW);
    case 2: return L(T_WX_CLOUDY);
    case 3: return L(T_WX_OVERCAST);
    case 45: case 48: return L(T_WX_FOG);
    case 51: case 53: case 55: return L(T_WX_DRIZZLE);
    case 56: case 57: return L(T_WX_FDRIZZLE);
    case 61: return L(T_WX_LRAIN);
    case 63: return L(T_WX_RAIN);
    case 65: return L(T_WX_HRAIN);
    case 66: case 67: return L(T_WX_FRAIN);
    case 71: return L(T_WX_LSNOW);
    case 73: return L(T_WX_SNOW);
    case 75: return L(T_WX_HSNOW);
    case 77: return L(T_WX_GRAINS);
    case 80: case 81: return L(T_WX_SHOWERS);
    case 82: return L(T_WX_HSHOWERS);
    case 85: case 86: return L(T_WX_SSHOWERS);
    case 95: return L(T_WX_STORM);
    case 96: case 99: return L(T_WX_HAIL);
    default: return "";
  }
}

// weather sprite palette: ground, three sky bands, cloud x3, sun/moon x3, precipitation x2, text ramp x3, accent
enum { W_G, W_S1, W_S2, W_S3, W_CSH, W_CMID, W_CHI, W_CORE, W_GLOW, W_HALO, W_FAR, W_NEAR, W_T1, W_T2, W_T3, W_ACC };
// per sky kind: sky peak, cloud shadow/body/lit, core, glow, halo, far, near, accent
static const uint32_t WX_PAL[8][10] PROGMEM = {
  {0x102E62, 0x0C1830, 0x1C2C4C, 0x34486E, 0xFFECAA, 0xFFB000, 0x4A3C22, 0x3A5070, 0xFFFFFF, 0xFFB000},  // clear day
  {0x0C1A3C, 0x080C18, 0x141C30, 0x283450, 0xF4F1E0, 0xB9B6A0, 0x1C2440, 0x56607E, 0xFFFFFF, 0xA8B8FF},  // clear night
  {0x1A3256, 0x0E121A, 0x2A3346, 0x5A6680, 0xFFECAA, 0xFFB000, 0x3A3428, 0x3A5070, 0xFFFFFF, 0xC8D4E8},  // clouds day
  {0x101A30, 0x080A10, 0x1C2232, 0x3A4458, 0xF4F1E0, 0xB9B6A0, 0x1A2036, 0x56607E, 0xFFFFFF, 0xA8B8FF},  // clouds night
  {0x2A2E36, 0x1A1C22, 0x464A54, 0x6E727C, 0xD0D0C8, 0xA0A098, 0x3A3C40, 0x50545C, 0x8A8E96, 0xB8BCC6},  // fog
  {0x142234, 0x0A0C12, 0x1E2636, 0x2E3A50, 0xD0D8E8, 0x90A0B8, 0x283040, 0x285A78, 0x4CC9FF, 0x4CC9FF},  // rain
  {0x222C3E, 0x0E1018, 0x2A3244, 0x46526A, 0xFFFFFF, 0xDDE6F4, 0x3A4458, 0x5A6E8C, 0xBEE1FF, 0xBEE1FF},  // snow
  {0x1C1430, 0x08060E, 0x1E1830, 0x342A4C, 0xFFFFFF, 0xD8D0FF, 0x6050A0, 0x34406A, 0x9AA8E0, 0xB49CFF},  // storm
};
static const uint8_t RAMP_W[16] = {255, 255, 255, W_T1, W_T1, W_T1, W_T1, W_T2, W_T2, W_T2, W_T2, W_T3, W_T3, W_T3, W_T3, W_T3};
static const uint8_t RAMP_G[16] = {255, 255, 255, 255, W_T1, W_T1, W_T1, W_T1, W_T2, W_T2, W_T2, W_T2, W_T2, W_T2, W_T2, W_T2};
static const uint8_t RAMP_A[16] = {255, 255, 255, 255, 255, 255, 255, W_ACC, W_ACC, W_ACC, W_ACC, W_ACC, W_ACC, W_ACC, W_ACC, W_ACC};
uint16_t wxPal[16];
int wxPalIdx = -1;
bool wxPalStale = false;  // sprite was rebuilt: palette only, keep the particles
bool wxFlashed = false;
uint8_t skyRow[SKY_H];  // palette index per sky row, with the dither baked in
uint32_t wxStamp = 0;

// The weather screen is a full 240x240 scene drawn in two 120-row passes through one
// 240x120 sprite (a full-screen 4-bit sprite doesn't fit next to Wi-Fi). Particles move
// once per frame, then each pass draws everything and the sprite clips to its band.
#define BAND_H 120
inline bool inBand(int y0, int y1) { return y1 > bandY0 && y0 < bandY1; }

// where text is, in 2x2 cells, so rain can splash on the digits and snow can settle on them
uint8_t *textMask = nullptr;  // 1.8 KB, held only while the weather screen is up
inline bool maskAt(int x, int y) {
  if (!textMask || x < 0 || y < 0 || x >= W || y >= H) return false;
  int c = (y / 2) * 120 + x / 2;
  return textMask[c >> 3] & (1 << (c & 7));
}
void maskText(const AAFont &f, int x, int base, const char *s, int track = 0) {
  AAGlyph g;
  while (*s) {
    uint16_t cp = utf8Next(s);
    if (!aaFind(f, cp, g)) { x += f.ascent / 4; continue; }
    uint32_t idx = g.off;
    for (int r = 0; r < g.h; r++)
      for (int c = 0; c < g.w; c++) {
        if (aaNib(f, idx++) < 8) continue;
        int px = x + g.xo + c, py = base + g.yo + r;
        if (!textMask || px < 0 || py < 0 || px >= W || py >= H) continue;
        int cell = (py / 2) * 120 + px / 2;
        textMask[cell >> 3] |= 1 << (cell & 7);
      }
    x += g.adv + track;
  }
}

void skyRows() {
  static const uint8_t pat[4] = {0x0, 0x1, 0x5, 0x7};
  for (int y = 0; y < SKY_H; y++) {
    float g = pow(sin(y * PI / (SKY_H - 1)), 1.3);
    int q = (int)(g * 12);
    int a = q / 4, lvl = q % 4;
    int c = a + (((pat[lvl] >> (y & 3)) & 1) ? 1 : 0);
    skyRow[y] = W_G + min(c, 3);
  }
}

void setWxPalette(int k, bool day) {
  int i = k == K_CLEAR ? (day ? 0 : 1) : k == K_CLOUDS ? (day ? 2 : 3) : k == K_FOG ? 4 : k == K_RAIN ? 5 : k == K_SNOW ? 6 : 7;
  wxPalIdx = i;
  uint32_t v[10];
  for (int j = 0; j < 10; j++) v[j] = pgm_read_dword(&WX_PAL[i][j]);
  wxPal[W_G] = rgb(GROUND);
  wxPal[W_S1] = rgb(lerpRGB(GROUND, v[0], 1, 3));
  wxPal[W_S2] = rgb(lerpRGB(GROUND, v[0], 2, 3));
  wxPal[W_S3] = rgb(v[0]);
  for (int j = 1; j <= 8; j++) wxPal[W_CSH + j - 1] = rgb(v[j]);
  wxPal[W_T1] = rgb(0x46464E);
  wxPal[W_T2] = rgb(0x82828C);
  wxPal[W_T3] = rgb(0xFFFFFF);
  wxPal[W_ACC] = rgb(v[9]);
  spr.createPalette(wxPal, 16);
  wxFlashed = false;
}

void skyFlash(bool on) {
  if (on == wxFlashed) return;
  wxFlashed = on;
  uint32_t peak = pgm_read_dword(&WX_PAL[wxPalIdx][0]);
  spr.setPaletteColor(W_G, rgb(on ? 0x1A1630 : GROUND));
  spr.setPaletteColor(W_S1, rgb(on ? 0x3C3460 : lerpRGB(GROUND, peak, 1, 3)));
  spr.setPaletteColor(W_S2, rgb(on ? 0x6A5EA0 : lerpRGB(GROUND, peak, 2, 3)));
  spr.setPaletteColor(W_S3, rgb(on ? 0x9C90D8 : peak));
}

// ---- effect state ----
struct Drop { float x, y, vy; uint8_t r; uint16_t life; bool on; };  // water on the glass
#define ND 9
Drop drops[ND];
struct Spl { int16_t x, y; uint8_t age; };                              // splash on a digit or the floor
#define NSPL 14
Spl spl[NSPL];
struct Stuck { int16_t x, y; uint16_t age; };                           // snow settled on the text
#define NSTUCK 70
Stuck stuck[NSTUCK];
uint8_t pile[61];                                                       // snow drift along the bottom, per 4 px
struct Mote { float x, y, vx, vy; };                                    // pollen in the sun
#define NMOTE 16
Mote motes[NMOTE];
float starX, starY, starVX, starVY;                                     // shooting star
uint32_t starAt = 0, starNext = 0, shakeUntil = 0, pileDecay = 0;

void skyInit(int k) {
  skyKind = k;
  for (int i = 0; i < NP; i++) {
    parts[i].x = random(W + 40);
    parts[i].y = random(-40, H);
    parts[i].s = (i % 3 == 0) ? 2 : 1;  // a third of the particles are near, over everything
    parts[i].v = parts[i].s == 2 ? random(9, 13) : random(4, 7);
  }
  for (auto &d : drops) d.on = false;
  for (auto &s : spl) s.age = 255;
  for (auto &s : stuck) s.age = 0xFFFF;
  memset(pile, 0, sizeof pile);
  for (auto &m : motes) { m.x = random(W); m.y = random(H); m.vx = random(-30, 31) / 100.0; m.vy = -random(5, 25) / 100.0; }
  starAt = 0; starNext = millis() + random(3000, 7000);
}

void addSplash(int x, int y) {
  for (auto &s : spl) if (s.age >= 6) { s.x = x; s.y = y; s.age = 0; return; }
}

void updateEffects(int k, uint32_t t) {
  bool wet = k == K_RAIN || k == K_STORM;
  if (wet) {
    for (int i = 0; i < NP; i++) {
      Part &p = parts[i];
      bool nearP = p.s == 2;
      p.y += p.v; p.x -= nearP ? 2 : 1;
      if (nearP) {
        int hx = p.x - 3, hy = p.y + 13;
        if (hy > 0 && maskAt(hx, hy) && random(3)) { addSplash(hx, hy); p.y = random(-40, -10); p.x = random(W + 40); }
        else if (hy >= H - 1) { addSplash(hx, H - 2); p.y = random(-40, -10); p.x = random(W + 40); }
      } else if (p.y > SKY_H - 6) { p.y = random(-24, 20); p.x = random(W + 40); }
    }
    for (auto &s : spl) if (s.age < 6) s.age++;
    // water on the glass: drops appear, some start to slide and leave a wet trail
    for (auto &d : drops) {
      if (!d.on) {
        if (random(1000) < 18) { d.on = true; d.x = random(10, 230); d.y = random(10, 200); d.r = random(2, 7); d.vy = 0; d.life = 0; }
        continue;
      }
      d.life++;
      if (d.vy == 0 && d.r >= 4 && random(1000) < 12) d.vy = 0.3;
      if (d.vy > 0) { d.vy = min(2.2f, d.vy * 1.04f); d.y += d.vy; d.x += random(-1, 2) * 0.3; }
      if (d.y > H + 10 || (d.vy == 0 && d.life > 500)) d.on = false;
    }
  } else if (k == K_SNOW) {
    for (int i = 0; i < NP; i++) {
      Part &p = parts[i];
      bool nearP = p.s == 2;
      p.y += nearP ? 2 : 1;
      int x = p.x + (int)(sin((p.y + i * 20) / (nearP ? 14.0 : 22.0)) * (nearP ? 8 : 4));
      if (!nearP) { if (p.y > SKY_H - 6) { p.y = random(-10, 20); p.x = random(W); } continue; }
      int col = constrain(x / 4, 0, 60);
      bool settle = p.y > 0 && maskAt(x, p.y + 2) && !maskAt(x, p.y) && random(4);
      if (settle) {
        for (auto &s : stuck) if (s.age == 0xFFFF) { s.x = x; s.y = p.y; s.age = 0; break; }
        p.y = random(-30, 0); p.x = random(W);
      } else if (p.y >= H - 1 - pile[col]) {
        if (pile[col] < 12) pile[col]++;
        p.y = random(-30, 0); p.x = random(W);
      }
    }
    for (auto &s : stuck) if (s.age != 0xFFFF && ++s.age > 900) s.age = 0xFFFF;  // melts in ~30 s
    if (t > pileDecay) { pileDecay = t + 1500; int c = random(61); if (pile[c]) pile[c]--; }
  } else if (k == K_CLEAR && skyKind == K_CLEAR) {
    for (auto &m : motes) {
      m.x += m.vx + sin((t / 900.0) + m.y / 30) * 0.15; m.y += m.vy;
      if (m.y < -4) { m.y = H + 2; m.x = random(W); }
      if (m.x < -4) m.x = W + 2;
      if (m.x > W + 4) m.x = -2;
    }
  }
  if (k == K_STORM && t > flashUntil && random(1000) < 12) { flashUntil = t + 110; shakeUntil = t + 320; }
}

// puffy cloud: shadow underside, body, lit top-left; back clouds skip the highlight
void cloud(int x, int y, int s, bool front) {
  if (!inBand(y - s * 2, y + s + 6)) return;
  const int n = 4;
  int cx[n] = {x, x + s, x + (s * 22) / 10, x + (s * 11) / 10};
  int cy[n] = {y, y - s / 2, y, y + s / 3};
  int r[n] = {s, (s * 5) / 4, (s * 9) / 10, s};
  for (int i = 0; i < n; i++) spr.fillCircle(cx[i], cy[i] + 4, r[i], W_CSH);
  uint8_t body = front ? W_CMID : W_CSH;
  uint8_t lit = front ? W_CHI : W_CMID;
  for (int i = 0; i < n; i++) spr.fillCircle(cx[i], cy[i], r[i], body);
  for (int i = 0; i < n; i++) spr.fillCircle(cx[i] - r[i] / 5, cy[i] - r[i] / 4, (r[i] * 6) / 10, lit);
}

void sun(int cx, int cy, uint32_t t) {
  int p = (int)(sin(t / 700.0) * 2);
  for (int r = 25; r <= 44 + p; r += 3) spr.drawCircle(cx, cy, r, W_HALO);
  float a = t / 3000.0;
  for (int i = 0; i < 12; i++) {
    float b = a + i * PI / 6;
    int r0 = 27, r1 = (i % 2) ? 34 + p : 39 + p;
    spr.drawLine(cx + cos(b) * r0, cy + sin(b) * r0, cx + cos(b) * r1, cy + sin(b) * r1, W_HALO);
  }
  spr.fillCircle(cx, cy, 21, W_GLOW);
  spr.fillCircle(cx - 3, cy - 4, 14, W_CORE);
}

void moon(int cx, int cy) {
  for (int r = 22; r <= 36; r += 3) spr.drawCircle(cx, cy, r, W_HALO);
  spr.fillCircle(cx, cy, 18, W_GLOW);
  spr.fillCircle(cx - 3, cy - 3, 14, W_CORE);
  spr.fillCircle(cx + 6, cy + 5, 3, W_GLOW);
  spr.fillCircle(cx - 6, cy + 7, 2, W_GLOW);
  spr.fillCircle(cx + 2, cy - 8, 2, W_GLOW);
}

// the texts of the screen, shared by drawing and the collision mask
// Cyrillic takes 2 bytes a letter: the longest weather words need ~40
struct WxText { char word[48], temp[12], clock[8], date[40], lo[8], hi[8], hum[8]; int tv; bool hasTemp, hasTime; } wxt;
void wxTextBuild(int code) {
  strncpy(wxt.word, wxWord(code), sizeof wxt.word - 1);
  wxt.hasTemp = wx.ok || wxForce >= 0;
  wxt.tv = (int)lround(wx.t);
  snprintf(wxt.temp, sizeof wxt.temp, "%d\xC2\xB0", abs(wxt.tv));
  wxt.hasTime = timeValid();
  if (wxt.hasTime) {
    struct tm tm = nowTm();
    snprintf(wxt.clock, sizeof wxt.clock, "%02d:%02d", tm.tm_hour, tm.tm_min);
    snprintf(wxt.date, sizeof wxt.date, "%s, %d %s", L(Str(T_WD0 + tm.tm_wday)), tm.tm_mday, L(Str(T_MO0 + tm.tm_mon)));
  }
  snprintf(wxt.lo, sizeof wxt.lo, "%d\xC2\xB0", (int)lround(wx.tmin));
  snprintf(wxt.hi, sizeof wxt.hi, "%d\xC2\xB0", (int)lround(wx.tmax));
  snprintf(wxt.hum, sizeof wxt.hum, "%d%%", wx.hum);
  if (!textMask) textMask = (uint8_t *)malloc(120 * 120 / 8);
  if (textMask) memset(textMask, 0, 120 * 120 / 8);
  if (wxt.hasTemp) maskText(F_BIG, wxt.tv < 0 ? 30 : 10, 158, wxt.temp, -2);
  if (wxt.hasTime) {
    maskText(F_CLOCK, 228 - aaWidth(F_CLOCK, wxt.clock), 158, wxt.clock);
    maskText(F_LABEL, 228 - aaWidth(F_LABEL, wxt.date), 178, wxt.date);
  }
  maskText(F_LABEL, 14, 30, wxt.word);
  if (wx.ok) { maskText(F_NUM, 28, 222, wxt.lo); maskText(F_NUM, 90, 222, wxt.hi); maskText(F_NUM, 152, 222, wxt.hum); }
}

void drawTexts() {
  if (inBand(10, 34)) aaSpr(F_LABEL, 14, 30, wxt.word, RAMP_A);
  if (wxt.hasTemp) {
    int x = 10;
    if (wxt.tv < 0) { spr.fillRect(x, 158 - 30, 16, 8, W_T3); x += 20; }  // a short bar keeps "-14°" clear of the clock
    aaSpr(F_BIG, x, 158, wxt.temp, RAMP_W, -2);
  } else aaSpr(F_LABEL, 14, 150, L(T_LOADING), RAMP_G);
  if (wxt.hasTime) {
    aaSpr(F_CLOCK, 228 - aaWidth(F_CLOCK, wxt.clock), 158, wxt.clock, RAMP_W);
    aaSpr(F_LABEL, 228 - aaWidth(F_LABEL, wxt.date), 178, wxt.date, RAMP_G);
  }
  // bottom strip lives in the scene too, so weather can fall over it
  if (inBand(190, 240)) {
    spr.drawFastHLine(12, 194, 216, W_T1);
    if (wx.ok) {
      int y = 222;
      spr.fillTriangle(14, y - 10, 24, y - 10, 19, y - 3, W_T2);
      aaSpr(F_NUM, 28, y, wxt.lo, RAMP_W);
      spr.fillTriangle(76, y - 3, 86, y - 3, 81, y - 10, W_T2);
      aaSpr(F_NUM, 90, y, wxt.hi, RAMP_W);
      spr.fillCircle(144, y - 5, 4, W_ACC);
      spr.fillTriangle(140, y - 6, 148, y - 6, 144, y - 14, W_ACC);
      aaSpr(F_NUM, 152, y, wxt.hum, RAMP_W);
    }
  }
}

// a warm glint that sweeps across the white digits every few seconds
void sunShine(uint32_t t) {
  int ph = t % 7000;
  if (ph > 1600) return;
  int s = -80 + ph * 400 / 1600;
  for (int y = max(bandY0, 96); y < min(bandY1, 184); y++)
    for (int x = max(0, s + y / 3); x < min(W, s + y / 3 + 16); x++)
      if (spr.readPixelValue(x, y) == W_T3) spr.drawPixel(x, y, W_CORE);
}

void fogOver(uint32_t t) {
  // dithered bands drift across everything, the text included
  for (int i = 0; i < 7; i++) {
    int y0 = 20 + i * 31 + (int)(sin(t / 2600.0 + i) * 6);
    if (!inBand(y0, y0 + 7)) continue;
    bool nearL = i % 2;
    int span = W + 200;
    long off = (long)i * 97 + (nearL ? 1 : -1) * (long)(t / (nearL ? 22 : 40));
    int x0 = (int)(((off % span) + span) % span) - 170;
    uint8_t c = nearL ? W_CHI : W_CMID;
    for (int y = y0; y < y0 + 7; y++)
      for (int x = max(0, x0 + ((y + i) & 1)); x < min(W, x0 + 170); x += 2) spr.drawPixel(x, y, c);
  }
}

void drawScene(int k, bool day, bool flash, uint32_t t) {
  spr.fillRect(0, bandY0, W, BAND_H, W_G);  // not fillSprite: its fast path overruns a banded viewport
  for (int y = max(0, bandY0); y < min(SKY_H, bandY1); y++) if (skyRow[y] != W_G) spr.drawFastHLine(0, y, W, skyRow[y]);

  // behind the text
  switch (k) {
    case K_CLEAR:
      if (day) { if (inBand(0, 100)) sun(184, 50, t); }
      else if (inBand(0, 130)) {
        for (int i = 0; i < NP; i++) {
          bool tw = ((t / 220) + i * 7) % 13 == 0;
          int x = parts[i].x % W, y = abs(parts[i].y) % 110;
          spr.drawPixel(x, y, tw ? W_FAR : W_NEAR);
          if (parts[i].s == 2 && !tw) { spr.drawPixel(x + 1, y, W_FAR); spr.drawPixel(x, y + 1, W_FAR); }
        }
        moon(184, 48);
      }
      break;
    case K_CLOUDS:
      if (inBand(0, 100)) { if (day) sun(180, 46, t); else moon(182, 44); }
      for (int i = 0; i < 2; i++) cloud((int)((i * 150 + t / 70) % (W + 140)) - 90, 30 + i * 30, 11, false);
      cloud((int)((60 + t / 35) % (W + 180)) - 110, 50, 16, true);
      break;
    case K_FOG:
      for (int i = 0; i < 9; i++) {
        bool nearL = i % 2;
        int span = W + 180;
        long off = (long)i * 97 + (nearL ? 1 : -1) * (long)(t / (nearL ? 18 : 34));
        int x = (int)(((off % span) + span) % span) - 150;
        if (inBand(12 + i * 12, 20 + i * 12)) spr.fillRoundRect(x, 12 + i * 12, 150, nearL ? 6 : 4, 3, nearL ? W_CHI : W_CMID);
      }
      break;
    case K_RAIN:
    case K_STORM:
      for (int i = 0; i < NP; i++) if (parts[i].s == 1) spr.drawLine(parts[i].x, parts[i].y, parts[i].x - 2, parts[i].y + 6, W_FAR);
      cloud(-10, 10, 22, false); cloud(170, 12, 22, false); cloud(80, 4, 26, true);
      break;
    case K_SNOW:
      for (int i = 0; i < NP; i++) {
        Part &p = parts[i];
        if (p.s == 2) continue;
        spr.drawPixel(p.x + (int)(sin((p.y + i * 20) / 22.0) * 4), p.y, W_FAR);
      }
      cloud(-10, 10, 22, false); cloud(170, 12, 22, false); cloud(80, 4, 26, true);
      break;
  }

  drawTexts();
  if (k == K_CLEAR && day) sunShine(t);

  // in front of the text
  switch (k) {
    case K_CLEAR:
      if (day) for (auto &m : motes) spr.fillCircle(m.x, m.y, ((int)m.x & 3) == 0 ? 1 : 0, ((int)m.y & 1) ? W_GLOW : W_HALO);
      else if (starAt && t - starAt < 700) {
        float f = (t - starAt) / 700.0;
        int hx = starX + starVX * f, hy = starY + starVY * f;
        spr.drawLine(hx, hy, hx - starVX * 0.18, hy - starVY * 0.18, W_FAR);
        spr.drawLine(hx, hy, hx - starVX * 0.08, hy - starVY * 0.08, W_NEAR);
        spr.fillCircle(hx, hy, 1, W_T3);
      }
      break;
    case K_CLOUDS:
      // a low cloud drifts across in front of the digits
      cloud((int)((t / 45) % (W + 260)) - 140, 112, 18, true);
      break;
    case K_FOG:
      fogOver(t);
      break;
    case K_RAIN:
    case K_STORM:
      for (int i = 0; i < NP; i++) {
        Part &p = parts[i];
        if (p.s != 2 || !inBand(p.y, p.y + 14)) continue;
        spr.drawLine(p.x, p.y, p.x - 3, p.y + 13, W_NEAR);
        spr.drawLine(p.x + 1, p.y, p.x - 2, p.y + 13, W_NEAR);
      }
      for (auto &s : spl) {
        if (s.age >= 6 || !inBand(s.y - 8, s.y + 2)) continue;
        int a = s.age;
        spr.drawPixel(s.x - 2 - a, s.y - 1 - a + a * a / 6, W_NEAR);
        spr.drawPixel(s.x + 2 + a, s.y - 1 - a + a * a / 6, W_NEAR);
        if (a < 3) spr.drawFastHLine(s.x - 2, s.y, 5, W_NEAR);
      }
      for (auto &d : drops) {
        if (!d.on || !inBand(d.y - 40, d.y + d.r + 2)) continue;
        if (d.vy > 0) spr.drawFastVLine(d.x, d.y - min(36, (int)d.vy * 18), min(36, (int)d.vy * 18), W_CSH);  // wet trail
        spr.fillCircle(d.x, d.y, d.r, W_CSH);
        spr.fillCircle(d.x, d.y, d.r - 1, W_CMID);
        if (d.r > 2) spr.fillCircle(d.x - d.r / 3, d.y - d.r / 3, d.r / 2, W_CHI);
        spr.drawPixel(d.x - d.r / 2, d.y - d.r / 2, W_T3);
      }
      if (k == K_STORM && flash) {
        int bx = 60 + (flashUntil % 120);
        int px[5] = {bx, bx - 14, bx + 4, bx - 12, bx - 2};
        int py[5] = {30, 64, 66, 108, 100};
        for (int i = 0; i < 4; i++) {
          spr.drawLine(px[i] - 1, py[i], px[i + 1] - 1, py[i + 1], W_HALO);
          spr.drawLine(px[i] + 1, py[i], px[i + 1] + 1, py[i + 1], W_HALO);
          spr.drawLine(px[i], py[i], px[i + 1], py[i + 1], W_CORE);
        }
      }
      break;
    case K_SNOW:
      for (auto &s : stuck) if (s.age != 0xFFFF && inBand(s.y - 2, s.y + 2)) {
        spr.fillCircle(s.x, s.y, s.age > 700 ? 1 : 2, s.age > 800 ? W_FAR : W_NEAR);
      }
      if (inBand(H - 14, H))
        for (int x = 0; x < W; x++) {
          int c = x / 4, h = pile[c] + (pile[min(60, c + 1)] - pile[c]) * (x % 4) / 4;
          if (h > 0) { spr.drawFastVLine(x, H - h, h, W_NEAR); spr.drawPixel(x, H - h, W_T3); }
        }
      for (int i = 0; i < NP; i++) {
        Part &p = parts[i];
        if (p.s != 2) continue;
        int x = p.x + (int)(sin((p.y + i * 20) / 14.0) * 8);
        if (!inBand(p.y - 3, p.y + 3)) continue;
        spr.fillCircle(x, p.y, 2, W_NEAR);
        spr.drawPixel(x - 1, p.y - 1, W_T3);
      }
      break;
  }
  if (countBusy() > 0 && inBand(0, 20)) spr.fillCircle(228, 12, 3 + (t / 300) % 2, W_ACC);
}

int wxMin = -1;
uint32_t wxTextStamp = 0;
void weatherFrame(uint32_t t) {
  int code = wxForce >= 0 ? wxForce : wx.code;
  bool day = wxForce >= 0 ? wxForceDay != 0 : wx.day;
  int k = skyOf(code);
  bool repal = k != skyKind || wxPalIdx < 0 || ((k == K_CLEAR || k == K_CLOUDS) && ((wxPalIdx % 2 == 0) != day));
  if (repal) { skyInit(k); setWxPalette(k, day); }
  else if (wxPalStale) setWxPalette(k, day);
  wxPalStale = false;
  struct tm tm = nowTm();
  if (repal || tm.tm_min != wxMin || wxTextStamp != wxStamp) { wxMin = tm.tm_min; wxTextStamp = wxStamp; wxTextBuild(code); }
  if (k == K_CLEAR && !day && t > starNext) {
    starAt = t; starNext = t + random(5000, 12000);
    starX = random(40, 260); starY = random(-10, 40); starVX = -random(160, 260); starVY = random(90, 150);
  }
  bool flash = false;
  if (k == K_STORM) flash = t < flashUntil;
  skyFlash(flash);
  updateEffects(k, t);

  int dx = 0, dy = 0;
  if (t < shakeUntil) { dx = random(-3, 4); dy = random(-3, 4); }
  for (int band = 0; band < 2; band++) {
    bandY0 = band * BAND_H; bandY1 = bandY0 + BAND_H;
    spr.setViewport(0, -bandY0, W, bandY0 + BAND_H, true);
    drawScene(k, day, flash, t);
    spr.resetViewport();
    spr.pushSprite(dx, bandY0 + dy);
  }
  bandY0 = 0; bandY1 = H;
  if (dx || dy) {  // the shaken frame leaves a thin margin; keep it black
    uint16_t g = rgb(GROUND);
    if (dx > 0) tft.fillRect(0, 0, dx, H, g); else if (dx < 0) tft.fillRect(W + dx, 0, -dx, H, g);
    if (dy > 0) tft.fillRect(0, 0, W, dy, g); else if (dy < 0) tft.fillRect(0, H + dy, W, -dy, g);
  }
}

// ---------- weather, "orb" style (mock/screens.py weather()) ----------
// No illustration: one soft orb of light top-right in the condition's colour. Static,
// drawn straight on the panel; the clock corner is redrawn through a viewport each minute.
enum { O_SUN, O_NIGHT, O_CLOUD, O_RAIN, O_STORM, O_SNOW, O_FOG };
// accent (word, bolt), light (glow), core (disc where it differs)
static const uint32_t ORB[7][3] PROGMEM = {
  {0xFFB000, 0xFFB000, 0},         {0xC4CEFF, 0xC4CEFF, 0},         {0xB0B8C8, 0xB0B8C8, 0x5C6270},
  {0x4CC9FF, 0x4CC9FF, 0},         {0xFFD650, 0x9678FF, 0x463878},  {0xC8E1FF, 0xC8E1FF, 0},
  {0xAAB0BC, 0xAAB0BC, 0x686C76},
};
const int OCX = 176, OCY = 58, ORR = 26;
int orbK = -1;
uint32_t orbAcc, orbLight, orbCore, orbL1, orbL2, orbB2;
float orbTX = 0;
uint32_t orbSeed;

int orbKindOf(int code, bool day) {
  switch (skyOf(code)) {
    case K_CLEAR: return day ? O_SUN : O_NIGHT;
    case K_CLOUDS: return O_CLOUD;
    case K_FOG: return O_FOG;
    case K_SNOW: return O_SNOW;
    case K_STORM: return O_STORM;
    default: return O_RAIN;
  }
}

// colour of the two glows under a point
uint32_t orbAt(float x, float y) {
  float d = sqrt((x - OCX) * (x - OCX) + (y - OCY) * (y - OCY));
  if (d <= 64) return glowAt(d, 64, orbL2, orbB2, 20);
  return glowAt(d, 150, orbL1, GROUND, 28);
}
uint16_t orbRowBg(int y) { return rgb(orbAt(orbTX, y)); }

float orbRnd() { orbSeed = orbSeed * 1103515245 + 12345; return ((orbSeed >> 8) & 0xFFFF) / 65535.0; }

// disc minus a cut (night: crescent, fog: slits), 3x3 supersampled against the glow
void orbMasked(uint32_t col, int kind) {
  for (int y = OCY - ORR - 1; y <= OCY + ORR + 1; y++)
    for (int x = OCX - ORR - 1; x <= OCX + ORR + 1; x++) {
      int hit = 0;
      for (int sy = 0; sy < 3; sy++)
        for (int sx = 0; sx < 3; sx++) {
          float px = x + (sx - 1) / 3.0, py = y + (sy - 1) / 3.0;
          if ((px - OCX) * (px - OCX) + (py - OCY) * (py - OCY) > ORR * ORR) continue;
          bool cut = false;
          if (kind == O_NIGHT) cut = (px - (OCX + 11)) * (px - (OCX + 11)) + (py - (OCY - 9)) * (py - (OCY - 9)) <= 25 * 25;
          else for (int j = 0; j < 3; j++) if (py >= OCY - 13 + j * 12 && py <= OCY - 9 + j * 12) cut = true;
          if (!cut) hit++;
        }
      if (hit) tft.drawPixel(x, y, rgb(mixF(orbAt(x, y), col, hit / 9.0)));
    }
}

// even-odd scanline fill, for the lightning bolt
void fillPoly(const int *xy, int n, uint16_t col) {
  int y0 = 999, y1 = -999;
  for (int i = 0; i < n; i++) { y0 = min(y0, xy[2 * i + 1]); y1 = max(y1, xy[2 * i + 1]); }
  for (int y = y0; y <= y1; y++) {
    float xs[16]; int m = 0;
    for (int i = 0; i < n; i++) {
      int ax = xy[2 * i], ay = xy[2 * i + 1], bx = xy[2 * ((i + 1) % n)], by = xy[2 * ((i + 1) % n) + 1];
      if ((ay <= y && by > y) || (by <= y && ay > y)) xs[m++] = ax + (float)(y - ay) * (bx - ax) / (by - ay);
    }
    for (int i = 0; i < m; i++) for (int j = i + 1; j < m; j++) if (xs[j] < xs[i]) { float t = xs[i]; xs[i] = xs[j]; xs[j] = t; }
    for (int i = 0; i + 1 < m; i += 2) tft.drawFastHLine((int)(xs[i] + 0.5), y, (int)(xs[i + 1] + 0.5) - (int)(xs[i] + 0.5), col);
  }
}

// rain streaks or snow dots scattered over a box, same every redraw
void orbMarks(bool rain, uint32_t col, int x0, int y0, int x1, int y1, int n) {
  orbSeed = 5;
  for (int i = 0; i < n; i++) {
    float x = x0 + orbRnd() * (x1 - x0), y = y0 + orbRnd() * (y1 - y0);
    bool nearM = i % 3 == 0;
    uint32_t c = nearM ? col : mixF(GROUND, col, 0.5);
    uint16_t bg = rgb(orbAt(x, y));
    if (rain) tft.drawWideLine(x, y, x - 2, y + (nearM ? 11 : 7), nearM ? 2 : 1.5, rgb(c), bg);
    else tft.fillSmoothCircle(x, y, nearM ? 2 : 1, rgb(c), bg);
  }
}

void orbArt(int k) {
  tftGlow(OCX, OCY, 150, orbL1, GROUND, 28);
  tftGlow(OCX, OCY, 64, orbL2, orbB2, 20);
  uint16_t edge = rgb(orbAt(OCX + ORR + 1, OCY));
  switch (k) {
    case O_SUN:
      tft.fillSmoothCircle(OCX, OCY, ORR, rgb(orbAcc), edge);
      tft.fillSmoothCircle(OCX - 3, OCY - 4, 17, rgb(0xFFE296), rgb(orbAcc));
      break;
    case O_NIGHT:
      orbMasked(0xE8ECFF, O_NIGHT);
      orbSeed = 11;
      for (int i = 0; i < 16; i++) {
        float x = 8 + orbRnd() * 224, y = 44 + orbRnd() * 74, b = orbRnd();
        if ((x - OCX) * (x - OCX) + (y - OCY) * (y - OCY) < 40 * 40) continue;
        uint16_t c = rgb(mixF(GROUND, 0xFFFFFF, 0.35 + 0.5 * b));
        if (b > 0.75) tft.fillSmoothCircle(x, y, 1, c, rgb(orbAt(x, y)));
        else tft.drawPixel(x, y, c);
      }
      break;
    case O_CLOUD:
      tft.fillSmoothCircle(OCX, OCY, ORR, rgb(mixF(GROUND, orbAcc, 0.85)), edge);
      tft.fillSmoothCircle(OCX - 19, OCY + 19, 25, rgb(orbCore), rgb(orbAt(OCX - 44, OCY + 19)));
      tft.fillEllipse(OCX - 22, OCY + 14, 18, 16, rgb(mixF(orbCore, 0xFFFFFF, 0.12)));
      break;
    case O_STORM: {
      tft.fillSmoothCircle(OCX, OCY, ORR, rgb(orbCore), edge);
      const int bolt[] = {OCX + 2, OCY - 30, OCX - 13, OCY + 3, OCX - 2, OCY + 3, OCX - 9, OCY + 32, OCX + 13, OCY - 6, OCX + 2, OCY - 6, OCX + 11, OCY - 30};
      fillPoly(bolt, 7, rgb(orbAcc));
      orbMarks(true, 0xAAA0FF, OCX - 50, OCY + 20, OCX + 46, OCY + 60, 10);
      break;
    }
    case O_FOG:
      orbMasked(orbCore, O_FOG);
      {
        const int xs[3][2] = {{118, 222}, {130, 236}, {112, 214}};
        for (int j = 0; j < 3; j++) {
          int y = OCY + 30 + j * 10;
          tft.fillSmoothRoundRect(xs[j][0], y, xs[j][1] - xs[j][0], 3, 1, rgb(mixF(GROUND, orbAcc, 0.32 - j * 0.07)), rgb(orbAt(xs[j][0], y)));
        }
      }
      break;
    default:  // rain, snow
      tft.fillSmoothCircle(OCX, OCY, ORR, rgb(mixF(GROUND, orbAcc, 0.62)), edge);
      orbMarks(k == O_RAIN, k == O_RAIN ? 0xD2F0FF : 0xFFFFFF, OCX - 40, OCY - 30, OCX + 40, OCY + 50, 14);
  }
}

// label, temperature, clock and date; also used inside the minute viewport
void orbText(int code) {
  orbTX = 40;
  aaDrawBg(F_LABEL, 14, 31, wxWord(code), rgb(orbAcc), orbRowBg);
  if (wx.ok || wxForce >= 0) {
    int tv = (int)lround(wx.t), x = 10;
    if (tv < 0) { tft.fillRect(x, 158 - 30, 16, 8, TFT_WHITE); x += 20; }
    char buf[12];
    snprintf(buf, sizeof buf, "%d\xC2\xB0", abs(tv));
    orbTX = 60;
    aaDrawBg(F_BIG, x, 158, buf, TFT_WHITE, orbRowBg);
  }
  if (!timeValid()) return;
  struct tm tm = nowTm();
  char b[24];
  snprintf(b, sizeof b, "%02d:%02d", tm.tm_hour, tm.tm_min);
  orbTX = 185;
  aaDrawBg(F_CLOCK, 228 - aaWidth(F_CLOCK, b), 158, b, TFT_WHITE, orbRowBg);
  snprintf(b, sizeof b, "%s, %d %s", L(Str(T_WD0 + tm.tm_wday)), tm.tm_mday, L(Str(T_MO0 + tm.tm_mon)));
  orbTX = 200;
  aaDrawBg(F_SMALL, 228 - aaWidth(F_SMALL, b), 178, b, rgb(C_GREY), orbRowBg);
}

void orbStrip() {
  tft.drawFastHLine(12, 194, 216, rgb(C_RULE));
  if (!wx.ok) return;
  uint16_t txt = rgb(0xEBEBF0), cyan = rgb(C_CYAN);
  int y = 222;
  char b[12];
  orbTX = 50;
  tft.fillTriangle(14, y - 10, 24, y - 10, 19, y - 3, rgb(0x78AAFF));
  snprintf(b, sizeof b, "%d\xC2\xB0", (int)lround(wx.tmin));
  aaDrawBg(F_NUM, 28, y, b, txt, orbRowBg);
  tft.fillTriangle(80, y - 3, 90, y - 3, 85, y - 10, rgb(0xFF8C3C));
  snprintf(b, sizeof b, "%d\xC2\xB0", (int)lround(wx.tmax));
  orbTX = 110;
  aaDrawBg(F_NUM, 94, y, b, txt, orbRowBg);
  tft.fillCircle(152, y - 5, 4, cyan);
  tft.fillTriangle(148, y - 6, 156, y - 6, 152, y - 14, cyan);
  snprintf(b, sizeof b, "%d%%", wx.hum);
  orbTX = 180;
  aaDrawBg(F_NUM, 160, y, b, txt, orbRowBg);
}

int orbMin = -1;
uint32_t orbStamp = 0;
void orbScreen(bool enter) {
  int code = wxForce >= 0 ? wxForce : wx.code;
  bool day = wxForce >= 0 ? wxForceDay != 0 : wx.day;
  int k = orbKindOf(code, day);
  struct tm tm = nowTm();
  if (enter || k != orbK || orbStamp != wxStamp) {
    orbK = k;
    orbStamp = wxStamp;
    orbAcc = pgm_read_dword(&ORB[k][0]);
    orbLight = pgm_read_dword(&ORB[k][1]);
    orbCore = pgm_read_dword(&ORB[k][2]);
    orbL1 = mixF(GROUND, orbLight, k == O_NIGHT ? 0.15 : 0.2);
    orbL2 = mixF(GROUND, orbLight, 0.46);
    orbB2 = mixF(GROUND, orbLight, 0.19);
    tft.fillScreen(rgb(GROUND));
    orbArt(k);
    orbText(code);
    orbStrip();
    orbMin = tm.tm_min;
    return;
  }
  if (tm.tm_min != orbMin) {  // repaint just the clock corner, glow and all
    orbMin = tm.tm_min;
    tft.setViewport(120, 110, 120, 74, false);
    orbArt(k);
    orbText(code);
    tft.resetViewport();
  }
}

void weatherScreen(bool enter) {
  if (cfg.wxstyle == 1) { orbScreen(enter); return; }
  if (enter) { skyKind = -1; wxPalIdx = -1; }
  if (!spr.created()) {  // no memory right now: try again next frame rather than freeze
    if (!sprOpen(W, BAND_H)) return;
    wxPalStale = true;
  }
  weatherFrame(millis());
}

void fetchWeather() {
  WiFiClient c;
  HTTPClient http;
  http.useHTTP10(true);
  String url = "http://api.open-meteo.com/v1/forecast?latitude=" + String(cfg.lat, 2) + "&longitude=" + String(cfg.lon, 2) +
               "&current=temperature_2m,weather_code,wind_speed_10m,relative_humidity_2m,is_day"
               "&daily=temperature_2m_max,temperature_2m_min&timezone=Europe%2FKyiv&forecast_days=1";
  if (!http.begin(c, url)) return;
  http.setTimeout(6000);
  if (http.GET() == 200) {
    JsonDocument d;
    if (!deserializeJson(d, http.getStream())) {
      wx.t = d["current"]["temperature_2m"];
      wx.code = d["current"]["weather_code"];
      wx.wind = d["current"]["wind_speed_10m"];
      wx.hum = d["current"]["relative_humidity_2m"];
      wx.day = (int)d["current"]["is_day"] == 1;
      wx.tmin = d["daily"]["temperature_2m_min"][0];
      wx.tmax = d["daily"]["temperature_2m_max"][0];
      wx.ok = true;
      wxStamp++;
    }
  }
  http.end();
}

// ---------- air raid alert (ubilling feed: every oblast and Kyiv city) ----------
bool alertFromBoot = false;  // already on when the cube started and the feed gave no start: real start unknown

// the feed escapes non-ASCII as \u0441: build the region's key the same way
String feedKey(int reg) {
  char name[64];
  strncpy_P(name, (PGM_P)pgm_read_ptr(&REGION_KEYS[reg]), sizeof name - 1);
  name[sizeof name - 1] = 0;
  String k = "\"";
  for (const char *p = name; *p;) {
    uint16_t cp = utf8Next(p);
    if (cp < 0x80) k += (char)cp;
    else { char u[8]; snprintf(u, sizeof u, "\\u%04x", cp); k += u; }
  }
  return k + "\":{\"alertnow\":";
}

void fetchAlert() {
  if (!timeValid()) return;  // no clock yet: a start time now would be 1970
  WiFiClient c;
  HTTPClient http;
  http.useHTTP10(true);
  if (!http.begin(c, "http://ubilling.net.ua/aerialalerts/")) return;
  http.setTimeout(6000);
  if (http.GET() == 200) {
    Stream &s = http.getStream();
    s.setTimeout(4000);
    if (s.find(feedKey(cfg.areg).c_str())) {
      bool on = s.read() == 't';
      // "changed":"2026-10-24 17:05:12" (Kyiv time): when the alert really began
      time_t changed = 0;
      if (s.find("\"changed\":\"")) {
        char b[20] = {0};
        s.readBytes(b, 19);
        struct tm tm = {};
        if (sscanf(b, "%d-%d-%d %d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
          tm.tm_year -= 1900; tm.tm_mon -= 1; tm.tm_isdst = -1;
          changed = mktime(&tm);
          if (changed < 1600000000) changed = 0;  // "1970-01-01 03:00:00" = never
        }
      }
      if (on && !alertOn) {
        alertStart = changed > 0 ? changed : time(nullptr);
        alertFromBoot = !alertKnown && changed <= 0;
      }
      if (!on && alertOn) alertEnd = time(nullptr);
      alertOn = on;
      alertKnown = true;
    }
  }
  http.end();
}

// Kyiv's level (since 2026-09): yellow = drones only, red = missiles or a massive attack.
// The feed above only says "alert"; the Mac reads the level from KMDA's Telegram (/alertlevel).
int kyivLevel = 0;  // 0 unknown or none, 1 yellow, 2 red
time_t levelSince = 0;
uint32_t levelAt = 0;
bool levelFresh() { return cfg.areg == 0 && levelAt && millis() - levelAt < 10UL * 60000; }
bool alertActive() { return alertForce >= 0 ? alertForce >= 1 : alertOn; }  // city alert, any level
bool alertIsYellow() { return alertForce >= 0 ? alertForce == 2 : (levelFresh() && kyivLevel == 1); }
// when the alert started: the level post's time is exact, the feed only knows when the cube saw it
time_t alertStartReal() { return (levelFresh() && kyivLevel && levelSince) ? levelSince : alertStart; }
bool alertStartExact() { return (levelFresh() && kyivLevel && levelSince) || !alertFromBoot; }

String hms(long s) {
  if (s < 0) s = 0;
  char b[12];
  snprintf(b, sizeof b, "%02ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
  return b;
}

// alert: red "sky" over black, beacon top-right, the alert word, running timer, shelter hint.
// For the first minute every other second flashes the whole screen solid red.
uint16_t alertBg(int y) { return rgb(bumpRow(y)); }
uint16_t flashBg(int) { return rgb(0x960E0A); }
int alertPhase = -1;
uint32_t alertTick = 0;

void shelterIcon(int x, int y, uint16_t col, uint16_t bg) {
  tft.drawWideLine(x + 9, y - 16, x + 9, y - 6, 3, col, bg);
  tft.fillTriangle(x + 3, y - 7, x + 15, y - 7, x + 9, y - 1, col);
  tft.drawWideLine(x + 1, y + 2, x + 17, y + 2, 3, col, bg);
}

bool alertYel = false;  // the alert screen is showing the yellow (oblast) alert
void alertTimer(bool flash) {
  time_t st = alertStartReal();
  long el = (st > 0 && timeValid()) ? (long)(time(nullptr) - st) : 0;
  tft.setViewport(0, 136, W, 46, false);
  if (flash) tft.fillRect(0, 136, W, 46, rgb(0x960E0A));
  else { drawBump(136, 150); tft.fillRect(0, 150, W, 32, rgb(GROUND)); }
  aaDrawBg(F_TIMER, 12, 172, hms(el).c_str(), rgb(flash ? 0xFFFFFF : alertYel ? 0xFFD666 : 0xFF786E), flash ? flashBg : alertBg);
  tft.resetViewport();
}

String hhmmOf(time_t t);  // with the outage screen
void alertScreen(bool enter) {
  uint32_t t = millis();
  bool yel = alertIsYellow();
  if (yel != alertYel) { alertYel = yel; alertPhase = -1; }
  long el = (alertStartReal() > 0 && timeValid()) ? (long)(time(nullptr) - alertStartReal()) : 999;
  int phase = (!yel && el < 60 && (t / 1000) % 2) ? 1 : 0;  // only red flashes
  // her messages don't wait for the all-clear: they ride the bottom line
  String foot = yel ? L(T_AL_NEAR) : L(T_AL_GO);
  if (anyAttn()) foot += L(T_AL_WAITS);
  static String alertFoot;
  if (foot != alertFoot) { alertFoot = foot; alertPhase = -1; }
  if (!enter && phase == alertPhase) {
    if (t - alertTick >= 1000) { alertTick = t; alertTimer(phase); }
    return;
  }
  alertPhase = phase;
  alertTick = t;
  bool flash = phase;
  bumpGradA = yel ? 0x060502 : 0x060202; bumpGradB = yel ? 0x3A2C00 : 0x460606; bumpGradH = 150;
  uint32_t acc = yel ? 0xFFC400 : C_RED;
  uint16_t (*bg)(int) = flash ? flashBg : alertBg;
  if (flash) {
    tft.fillScreen(rgb(0x960E0A));
    tft.fillSmoothCircle(206, 30, 9, TFT_WHITE, rgb(0x960E0A));
  } else {
    tft.fillScreen(rgb(GROUND));
    drawBump(0, 150);
    tftGlow(206, 30, 34, acc, yel ? 0x262000 : 0x280404, 6, 0.55);
    tft.fillSmoothCircle(206, 30, 9, rgb(acc), rgb(yel ? 0x605010 : 0x701010));
    tft.fillSmoothCircle(204, 27, 3, rgb(yel ? 0xFFE89A : 0xFF968C), rgb(acc));
  }
  const char *lab = yel ? L(T_AL_YELLOW) : (levelFresh() && kyivLevel == 2) ? L(T_AL_RED) : L(Str(T_RG0 + cfg.areg));
  aaDrawBg(F_LABEL, 14, 31, lab, rgb(flash ? 0xFFFFFF : acc), bg);
  aaDrawBg(F_ALERT, 12, 106, L(T_AL_WORD), TFT_WHITE, bg);
  String sub = yel ? L(T_AL_DRONES) : (levelFresh() && kyivLevel == 2) ? L(T_AL_MISSILES) : L(T_AL_ON);
  if (!alertStartExact()) sub = L(T_AL_SEEN) + hhmmOf(alertStart);
  aaDrawBg(F_SMALL, 14, 128, sub.c_str(), rgb(flash ? 0xFFC8C3 : C_GREY), bg);
  alertTimer(flash);
  uint16_t lower = flash ? rgb(0x960E0A) : rgb(GROUND);
  ruleLine(194, flash ? 0xBE322C : C_RULE);
  shelterIcon(14, 222, TFT_WHITE, lower);
  aaDraw(F_NUM, 40, 222, aaFit(F_NUM, alertFoot, 190).c_str(), TFT_WHITE, lower);
}

uint16_t clearBg(int y) { return rgb(bumpRow(y)); }
void allClearScreen(bool enter) {
  if (!enter) return;
  bumpGradA = 0x020604; bumpGradB = 0x082E16; bumpGradH = 150;
  tft.fillScreen(rgb(GROUND));
  drawBump(0, 150);
  tftGlow(206, 30, 34, C_GREEN, 0x061E0E, 6, 0.5);
  tft.fillSmoothCircle(206, 30, 9, rgb(C_GREEN), rgb(0x0C3A1C));
  aaDrawBg(F_LABEL, 14, 31, L(Str(T_RG0 + cfg.areg)), rgb(C_GREEN), clearBg);
  aaDrawBg(F_CLEAR, 12, 106, L(T_CLR_WORD), TFT_WHITE, clearBg);
  aaDrawBg(F_SMALL, 14, 128, L(T_CLR_LASTED), rgb(C_GREY), clearBg);
  long d = (alertStart > 0 && alertEnd > alertStart) ? (long)(alertEnd - alertStart) : 0;
  char b[24];
  if (d >= 3600) snprintf(b, sizeof b, "%ld %s %ld %s", d / 3600, L(T_U_H), (d / 60) % 60, L(T_U_MIN));
  else snprintf(b, sizeof b, "%ld %s", d / 60, L(T_U_MIN));
  aaDrawBg(F_TIMER, 12, 172, b, rgb(0x96EBAA), clearBg);
  ruleLine();
  uint16_t g = rgb(GROUND);
  const int tick[] = {15, 214, 21, 220, 31, 208};
  thickPoly(tick, 3, 3.5, rgb(C_GREEN), g);
  aaDraw(F_NUM, 40, 222, L(T_CLR_OUT), TFT_WHITE, g);
}

// ---------- album ----------
bool isGif(const String &p) { String l = p; l.toLowerCase(); return l.endsWith(".gif"); }

void scanDir(const char *dir) {
  Dir d = LittleFS.openDir(dir);
  while (d.next() && nPhotos < MAXPH) {
    if (d.isDirectory() || d.fileSize() == 0) continue;
    String n = d.fileName();
    String l = n; l.toLowerCase();
    if (l.startsWith("k3026_")) continue;  // the Kyiv 3026 window has its own screen
    if (l.endsWith(".jpg") || l.endsWith(".jpeg") || l.endsWith(".gif")) photos[nPhotos++] = String(dir) + "/" + n;
  }
}
void scanPhotos() { nPhotos = 0; scanDir("/image"); scanDir("/gif"); }

bool jpgOut(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bmp) {
  if (y >= H) return false;
  tft.pushImage(x, y, w, h, bmp);
  return true;
}

AnimatedGIF *gif = nullptr;
File gifFile;
int gifX = 0, gifY = 0;
uint32_t gifNextFrame = 0;

void *gifOpen(const char *fn, int32_t *size) {
  gifFile = LittleFS.open(fn, "r");
  if (!gifFile) return nullptr;
  *size = gifFile.size();
  return &gifFile;
}
void gifClose(void *h) { if (h) ((File *)h)->close(); }
int32_t gifRead(GIFFILE *pf, uint8_t *buf, int32_t len) {
  File *f = (File *)pf->fHandle;
  int32_t n = len;
  if (pf->iSize - pf->iPos < len) n = pf->iSize - pf->iPos - 1;
  if (n <= 0) return 0;
  n = f->read(buf, n);
  pf->iPos = f->position();
  return n;
}
int32_t gifSeek(GIFFILE *pf, int32_t pos) {
  File *f = (File *)pf->fHandle;
  f->seek(pos);
  pf->iPos = f->position();
  return pf->iPos;
}

void gifDraw(GIFDRAW *pd) {
  static uint16_t line[W];
  int w = pd->iWidth;
  if (w + pd->iX > W) w = W - pd->iX;
  int y = pd->iY + pd->y + gifY;
  if (y < 0 || y >= H || w < 1) return;
  uint8_t *s = pd->pPixels;
  uint16_t *pal16 = pd->pPalette;
  if (pd->ucDisposalMethod == 2) {
    for (int x = 0; x < w; x++) if (s[x] == pd->ucTransparent) s[x] = pd->ucBackground;
    pd->ucHasTransparency = 0;
  }
  if (pd->ucHasTransparency) {
    uint8_t tr = pd->ucTransparent;
    int x = 0;
    while (x < w) {
      int n = 0;
      while (x + n < w && s[x + n] != tr) { line[n] = pal16[s[x + n]]; n++; }
      if (n) { tft.setAddrWindow(pd->iX + x + gifX, y, n, 1); tft.pushPixels(line, n); x += n; }
      while (x < w && s[x] == tr) x++;
    }
  } else {
    for (int x = 0; x < w; x++) line[x] = pal16[s[x]];
    tft.setAddrWindow(pd->iX + gifX, y, w, 1);
    tft.pushPixels(line, w);
  }
}

void gifStop() {
  if (gif) { gif->close(); gif->~AnimatedGIF(); gif = nullptr; }  // lives in the arena
}

// The decoder is built with a 10-bit LZW dictionary (~8 KB instead of ~23 KB), so only GIFs
// re-encoded by tools/cube_gif.py (or the panel) can play; they carry a "CUBE10" comment.
bool isCubeGif(const String &p) {
  File f = LittleFS.open(p, "r");
  if (!f) return false;
  char buf[1100];
  int n = f.read((uint8_t *)buf, sizeof buf);
  f.close();
  for (int i = 0; i + 6 <= n; i++) if (memcmp(buf + i, "CUBE10", 6) == 0) return true;
  return false;
}

bool showFile(const String &p) {
  gifStop();
  tft.fillScreen(TFT_BLACK);
  if (isGif(p)) {
    if (!isCubeGif(p) || lifeOwnsArena()) return false;  // 12-bit GIFs come out as garbage
    static_assert(sizeof(AnimatedGIF) <= ARENA_SIZE, "GIF decoder must fit the arena");
    sprClose();
    gif = new (arena) AnimatedGIF;
    gif->begin(BIG_ENDIAN_PIXELS);
    if (!gif->open(p.c_str(), gifOpen, gifClose, gifRead, gifSeek, gifDraw)) { gifStop(); return false; }
    gifX = max(0, (W - gif->getCanvasWidth()) / 2);
    gifY = max(0, (H - gif->getCanvasHeight()) / 2);
    gifNextFrame = 0;
    return true;
  }
  uint16_t w = 0, h = 0;
  TJpgDec.getFsJpgSize(&w, &h, p.c_str(), LittleFS);
  if (!w) return false;
  int sc = 1;
  while (sc < 8 && (w / sc > W || h / sc > H)) sc *= 2;
  TJpgDec.setJpgScale(sc);
  tft.setSwapBytes(true);
  TJpgDec.drawFsJpg((W - w / sc) / 2, (H - h / sc) / 2, p.c_str(), LittleFS);
  tft.setSwapBytes(false);
  return true;
}

void gifTick() {
  if (!gif || millis() < gifNextFrame) return;
  int d = 0;
  tft.startWrite();
  int rc = gif->playFrame(false, &d);
  tft.endWrite();
  if (rc == 0) gif->reset();
  if (rc < 0) { gifStop(); return; }
  gifNextFrame = millis() + max(d, 20);
}

uint32_t slideUntil = 0;
void photoScreen(bool enter) {
  uint32_t t = millis();
  if (enter || t > slideUntil) {
    if (forcePhoto.length() && t < forceUntil) {
      showFile(forcePhoto);
      slideUntil = forceUntil;
    } else {
      forcePhoto = "";
      if (!nPhotos) { slideUntil = t + 3000; return; }
      phIdx = (phIdx + 1) % nPhotos;
      if (!showFile(photos[phIdx])) { slideUntil = t + 200; return; }
      slideUntil = t + (gif ? cfg.slide * 2000UL : cfg.slide * 1000UL);
    }
  }
  gifTick();
}

// ---------- note ----------
// pink glow from the bottom-right corner, NOTE + heart, up to four lines of Bold text
String noteTime;
void noteScreen(bool enter) {
  if (!enter) return;
  uint16_t g = rgb(GROUND);
  tft.fillScreen(g);
  tftGlow(222, 238, 160, 0x280B14, GROUND, 12);
  aaDraw(F_LABEL, 14, 31, L(T_NOTE), rgb(C_ROSE), g);
  tftHeart(213, 24, 7, rgb(C_ROSE), g);
  // word wrap by pixel width
  String text = noteText + " ", line, word;
  int y = 80, maxW = 216;
  for (unsigned i = 0; i < text.length() && y <= 170; i++) {
    char ch = text[i];
    if (ch == ' ' || ch == '\n') {
      String probe = line.length() ? line + " " + word : word;
      if (aaWidth(F_NOTE, probe.c_str()) > maxW && line.length()) {
        aaDraw(F_NOTE, 12, y, line.c_str(), TFT_WHITE, g);
        y += 30;
        line = word;
      } else line = probe;
      word = "";
      if (ch == '\n' && y <= 170) { aaDraw(F_NOTE, 12, y, line.c_str(), TFT_WHITE, g); y += 30; line = ""; }
    } else word += ch;
  }
  if (line.length() && y <= 170) aaDraw(F_NOTE, 12, y, line.c_str(), TFT_WHITE, g);
  ruleLine();
  aaDraw(F_SMALL, 14, 222, L(T_NOTE_FROM), rgb(C_GREY), g);
  aaRight(F_NUM, 228, 222, noteTime.c_str(), TFT_WHITE, g);
}

// ---------- render ----------
// ring in After Effects violet with a lit head, percent inside; done = green ring + tick
int rndDrawnP = -1;
uint32_t rndTick = 0;
const int RCX = 120, RCY = 108;

String mmss(long s) {
  if (s < 0) s = 0;
  char b[12];
  if (s >= 3600) snprintf(b, sizeof b, "%ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
  else snprintf(b, sizeof b, "%ld:%02ld", s / 60, s % 60);
  return b;
}

uint32_t rndGlowCol(bool done) { return mixF(GROUND, done ? C_GREEN : C_AE, 0.16); }

void renderEta(bool done) {
  uint32_t gc = rndGlowCol(done);
  tft.setViewport(60, 128, 120, 20, false);
  tftGlow(RCX, RCY, 100, gc, GROUND, 6);
  String s;
  if (done) s = L(T_RND_TOOK) + mmss(rnd.elapsed);
  else if (rnd.eta >= 0) s = L(T_RND_LEFT) + mmss(rnd.eta - (long)((millis() - rnd.last) / 1000));
  if (!done && rnd.qn > 1 && rnd.total > 0) s += " · " + String(rnd.frame) + "/" + String(rnd.total);  // frames move here in a queue
  aaCenter(F_SMALL, RCX, RCY + 34, s.c_str(), rgb(C_GREY), rgb(glowAt(34, 100, gc, GROUND, 6)));
  tft.resetViewport();
}

void renderScreen(bool enter) {
  bool done = !rnd.on && millis() < rnd.doneUntil;
  uint32_t t = millis();
  if (!enter && rnd.p == rndDrawnP) {
    if (!done && t - rndTick >= 1000) { rndTick = t; renderEta(false); }
    return;
  }
  rndDrawnP = rnd.p;
  rndTick = t;
  uint32_t acc = done ? C_GREEN : C_AE, gc = rndGlowCol(done);
  uint16_t g = rgb(GROUND);
  tft.fillScreen(g);
  tftGlow(RCX, RCY, 100, gc, GROUND, 6);
  aaDraw(F_LABEL, 14, 31, done ? L(T_EMO_HAPPY) : L(T_RND), rgb(acc), g);
  if (rnd.qn > 1) {  // a queue: which job of how many
    char qb[16];
    snprintf(qb, sizeof qb, "%d / %d", done ? rnd.qn : rnd.q, rnd.qn);
    aaRight(F_LABEL, 228, 31, qb, rgb(acc), g);
  }
  uint16_t ringBg = rgb(glowAt(66, 100, gc, GROUND, 6));
  tft.drawSmoothArc(RCX, RCY, 70, 62, 0, 360, rgb(done ? 0x0E2816 : 0x1A1A28), ringBg, false);
  if (done) {
    tft.drawSmoothArc(RCX, RCY, 70, 62, 0, 360, rgb(acc), ringBg, false);
    const int tick[] = {RCX - 22, RCY - 2, RCX - 7, RCY + 13, RCX + 22, RCY - 16};
    thickPoly(tick, 3, 9, TFT_WHITE, rgb(glowAt(10, 100, gc, GROUND, 6)));
  } else {
    int p = constrain(rnd.p, 0, 100);
    int a0 = 180, a1 = (180 + p * 360 / 100) % 360;  // 0 is six o'clock, clockwise
    if (p > 0) tft.drawSmoothArc(RCX, RCY, 70, 62, a0, p >= 100 ? 180 : a1, rgb(acc), ringBg, p < 100);
    float a = (180 + p * 3.6) * PI / 180;
    int hx = RCX - (int)(sin(a) * 66), hy = RCY + (int)(cos(a) * 66);
    tftGlow(hx, hy, 14, acc, glowAt(66, 100, gc, GROUND, 6), 5, 0.5);
    tft.fillSmoothCircle(hx, hy, 7, rgb(acc), ringBg);
    tft.fillSmoothCircle(hx, hy, 3, TFT_WHITE, rgb(acc));
    char b[8];
    snprintf(b, sizeof b, "%d", p);
    int nw = aaWidth(F_PCT, b);
    uint16_t inner = rgb(glowAt(10, 100, gc, GROUND, 6));
    aaDraw(F_PCT, RCX - 7 - nw / 2, RCY + 12, b, TFT_WHITE, inner);
    aaDraw(F_LABEL, RCX - 7 + nw / 2 + 2, RCY - 8, "%", rgb(acc), inner);
  }
  renderEta(done);
  ruleLine();
  bool queued = !done && rnd.qn > 1 && rnd.next.length();
  aaDraw(F_NUM, 14, 222, aaFit(F_NUM, rnd.name, queued ? 110 : 140).c_str(), TFT_WHITE, g);
  if (queued) {
    aaRight(F_SMALL, 228, 222, aaFit(F_SMALL, L(T_RND_NEXT) + rnd.next, 100).c_str(), rgb(C_GREY), g);
    return;
  }
  char fr[24] = "";
  if (rnd.total > 0) {
    if (done) snprintf(fr, sizeof fr, "%d", rnd.total);
    else snprintf(fr, sizeof fr, "%d/%d", rnd.frame, rnd.total);
  }
  aaRight(F_SMALL, 228, 222, fr, rgb(C_GREY), g);
}

// ---------- crypto ----------
// Pairs come from Binance's plain-HTTP mirror. Anything it lacks (WBT) the Mac pushes to /cx.
String pairLabel(const String &s) {
  static const char *quotes[] = {"USDT", "UAH", "BTC", "EUR"};
  for (const char *q : quotes) {
    int n = strlen(q);
    if (s.endsWith(q) && (int)s.length() > n) return s.substring(0, s.length() - n) + "/" + q;
  }
  return s;
}

String fmtPrice(float v, const String &sym) {
  const char *pre = sym.endsWith("UAH") ? "\xE2\x82\xB4" : "$";
  char b[24];
  if (v >= 1000) {
    char t[16];
    snprintf(t, sizeof t, "%ld", lround(v));
    int len = strlen(t), j = 0;
    for (int i = 0; i < len; i++) { if (i && (len - i) % 3 == 0) b[j++] = ','; b[j++] = t[i]; }
    b[j] = 0;
  } else if (v >= 1) snprintf(b, sizeof b, "%.2f", v);
  else snprintf(b, sizeof b, "%.4f", v);
  return String(pre) + b;
}

void parsePairs() {
  String list = cfg.pairs + ",";
  int n = 0, from = 0;
  for (int i = 0; i < (int)list.length() && n < MAXC; i++) {
    if (list[i] != ',') continue;
    String s = list.substring(from, i);
    s.trim(); s.toUpperCase();
    from = i + 1;
    if (!s.length()) continue;
    if (cx.c[n].sym != s) { cx.c[n] = Coin(); cx.c[n].sym = s; }
    n++;
  }
  cx.n = n;
}

bool cxAny() {
  for (int i = 0; i < cx.n; i++) if (cx.c[i].ok) return true;
  return false;
}

void fetchPair(Coin &k) {
  WiFiClient c;
  HTTPClient http;
  http.useHTTP10(true);
  bool ok = false;
  if (http.begin(c, "http://data-api.binance.vision/api/v3/ticker/24hr?type=MINI&symbol=" + k.sym)) {
    http.setTimeout(5000);
    if (http.GET() == 200) {
      JsonDocument f;
      f["lastPrice"] = true; f["openPrice"] = true;
      JsonDocument d;
      if (!deserializeJson(d, http.getStream(), DeserializationOption::Filter(f))) {
        float last = atof(d["lastPrice"] | "0"), open = atof(d["openPrice"] | "0");
        if (last > 0) { k.price = last; k.chg = open > 0 ? (last - open) / open * 100 : 0; ok = true; }
      }
    }
    http.end();
  }
  if (!ok) return;
  k.ok = true;
  // 24 hourly closes for the sparkline
  if (http.begin(c, "http://data-api.binance.vision/api/v3/klines?interval=1h&limit=24&symbol=" + k.sym)) {
    http.setTimeout(5000);
    if (http.GET() == 200) {
      Stream &s = http.getStream();
      s.setTimeout(3000);
      int n = 0;
      s.find("[");
      while (n < 24 && s.find("[")) {
        for (int j = 0; j < 4; j++) s.find(",");  // open time, open, high, low
        s.find("\"");
        k.spark[n++] = s.readStringUntil('"').toFloat();
        s.find("]");
      }
      k.nSpark = n;
    }
    http.end();
  }
}

void fetchCrypto() {
  parsePairs();
  for (int i = 0; i < cx.n; i++) {
    Coin &k = cx.c[i];
    if (k.pushed && millis() - k.pushed < 10UL * 60000) continue;  // fed by the Mac
    fetchPair(k);
  }
  cx.stamp = millis();
}

uint32_t cxDrawn = 0;

// sparkline: area fading from a dim tint into the ground, a 2 px line, and a "now" dot
void sparkline(const Coin &k, int x0, int y0, int x1, int y1, uint32_t col) {
  if (k.nSpark < 2) return;
  float lo = k.spark[0], hi = k.spark[0];
  for (int i = 1; i < k.nSpark; i++) { lo = min(lo, k.spark[i]); hi = max(hi, k.spark[i]); }
  if (hi - lo < 1e-9) hi = lo + 1;
  auto px = [&](int i) { return x0 + (float)(x1 - x0) * i / (k.nSpark - 1); };
  auto py = [&](int i) { return y1 - (y1 - y0) * (k.spark[i] - lo) / (hi - lo); };
  for (int x = x0; x < x1; x++) {
    float f = (float)(x - x0) / (x1 - x0) * (k.nSpark - 1);
    int i = min(k.nSpark - 2, (int)f);
    float yt = py(i) + (py(i + 1) - py(i)) * (f - i);
    for (int y = (int)yt; y < y1 + 6; y++) {
      float t = (float)(y - y0) / (y1 + 6 - y0);
      tft.drawPixel(x, y, rgb(mixF(col, GROUND, 0.78 + 0.22 * t)));
    }
  }
  uint16_t c = rgb(col);
  for (int i = 0; i + 1 < k.nSpark; i++) tft.drawWideLine(px(i), py(i), px(i + 1), py(i + 1), 2, c, rgb(mixF(col, GROUND, 0.8)));
  float ex = px(k.nSpark - 1), ey = py(k.nSpark - 1);
  tft.fillSmoothCircle(ex, ey, 4, rgb(mixF(col, GROUND, 0.6)), rgb(GROUND));
  tft.fillSmoothCircle(ex, ey, 2, c, rgb(mixF(col, GROUND, 0.6)));
}

void cryptoScreen(bool enter) {
  if (!enter && cxDrawn == cx.stamp) return;
  cxDrawn = cx.stamp;
  uint16_t g = rgb(GROUND), grey = rgb(C_GREY);
  tft.fillScreen(g);
  aaDraw(F_LABEL, 14, 31, L(T_CX), TFT_WHITE, g);
  aaRight(F_SMALL, 228, 31, L(T_CX_24), grey, g);
  for (int i = 0; i < cx.n && i < 3; i++) {
    Coin &k = cx.c[i];
    int y = 44 + i * 66;
    if (i) ruleLine(y - 8);
    String lab = pairLabel(k.sym);
    lab.replace("/", " / ");
    aaDraw(F_SMALL, 14, y + 9, lab.c_str(), grey, g);
    if (!k.ok) { aaDraw(F_SMALL, 14, y + 36, L(T_NO_DATA), rgb(0x4E4E58), g); continue; }
    uint32_t col = k.chg >= 0 ? C_GREEN : C_RED;
    char b[16];
    snprintf(b, sizeof b, "%+.2f%%", k.chg);
    aaRight(F_SMALL, 228, y + 9, b, rgb(col), g);
    aaDraw(F_PRICE, 12, y + 40, fmtPrice(k.price, k.sym).c_str(), TFT_WHITE, g);
    bool upT = k.nSpark > 1 ? k.spark[k.nSpark - 1] >= k.spark[0] : k.chg >= 0;
    sparkline(k, 156, y + 18, 226, y + 44, upT ? C_GREEN : C_RED);
  }
}

// ---------- night ----------
// black sky with a few twinkling stars, my face asleep above the clock (it breathes, z's climb);
// while a session still works one eye peeks open; now and then a shooting star. All in dim ember.
int nightMin = -1;
uint32_t nightTick = 0, nightAnim = 0, nightShootAt = 0;
int nightZ = -1;
float nightShoot = -1;  // shooting star progress 0..1, <0 = none
uint8_t nStarX[14], nStarY[14];
const int NF_X0 = 76, NF_Y0 = 38, NF_X1 = 164, NF_Y1 = 92;  // face box, repainted every frame

void nightFace(uint32_t t) {
  tft.fillRect(NF_X0, NF_Y0, NF_X1 - NF_X0, NF_Y1 - NF_Y0, TFT_BLACK);
  float s = t / 1000.0;
  int bob = (int)(sin(s * 1.4) * 2.5);  // slow breathing
  uint16_t lid = rgb(0x6A2A1A), cheek = rgb(0x2C0E08);
  bool peek = countBusy() > 0 || anyAttn();
  for (int sx = -1; sx <= 1; sx += 2) {
    int x = 120 + sx * 26, cy = 58 + bob;
    tft.fillEllipse(x + sx * 4, cy + 20, 9, 4, cheek);
    if (peek && sx > 0) {  // one eye open: is it still running?
      int gx = (int)(sin(s * 0.9) * 5);
      tft.fillSmoothRoundRect(x - 8 + gx, cy - 4, 16, 22, 8, rgb(C_CORALX), TFT_BLACK);
      continue;
    }
    for (int deg = 30; deg <= 150; deg += 6) {
      float r = deg * PI / 180;
      tft.fillCircle(x + cos(r) * 14, cy + sin(r) * 9, 2, lid);
    }
  }
}

void nightScreen(bool enter) {
  struct tm tm = nowTm();
  uint32_t t = millis();
  if (enter) {
    for (int i = 0; i < 14; i++) {  // stars anywhere above the clock, not on the face
      int x, y;
      do { x = random(8, 232); y = random(8, 86); } while (x > NF_X0 - 6 && x < 206 && y > NF_Y0 - 30);
      nStarX[i] = x; nStarY[i] = y;
    }
    nightShootAt = t + random(40, 120) * 1000UL;
    nightZ = -1;
  }
  if (enter || tm.tm_min != nightMin) {
    nightMin = tm.tm_min;
    tft.fillScreen(TFT_BLACK);
    char b[24];
    snprintf(b, sizeof b, "%02d:%02d", tm.tm_hour, tm.tm_min);
    aaDraw(F_BIG, 120 - aaWidth(F_BIG, b, -2) / 2, 166, b, rgb(0x5C1E12), TFT_BLACK, -2);
    snprintf(b, sizeof b, "%s, %d %s", L(Str(T_WD0 + tm.tm_wday)), tm.tm_mday, L(Str(T_MO0 + tm.tm_mon)));
    aaCenter(F_SMALL, 120, 192, b, rgb(0x481C12), TFT_BLACK);
    nightZ = -1;
  }
  if (t - nightAnim >= 66 || enter) {
    nightAnim = t;
    nightFace(t);
    // z's climb one by one off the right eye, unless it's peeking
    int z = (countBusy() > 0 || anyAttn()) ? 0 : (t / 900) % 5;
    if (z != nightZ) {
      if (z < nightZ || nightZ < 0) tft.fillRect(166, 8, 50, 52, TFT_BLACK);
      for (int i = 0; i < z && i < 3; i++)
        aaDraw(F_SMALL, 170 + i * 13, 54 - i * 15, "z", rgb(mixF(0x000000, 0x6A4A40, 1 - i * 0.25)), TFT_BLACK);
      nightZ = z;
    }
    // twinkle: each star breathes on its own phase
    for (int i = 0; i < 14; i++) {
      float w = 0.5 + 0.5 * sin(t / 1000.0 * (0.6 + (i % 5) * 0.25) + i * 1.7);
      tft.drawPixel(nStarX[i], nStarY[i], rgb(mixF(0x000000, i % 3 ? 0x5A4840 : 0x6A3020, 0.15 + w * 0.85)));
      if (i % 4 == 0) {
        uint16_t c = rgb(mixF(0x000000, 0x3A2A24, w));
        tft.drawPixel(nStarX[i] - 1, nStarY[i], c); tft.drawPixel(nStarX[i] + 1, nStarY[i], c);
        tft.drawPixel(nStarX[i], nStarY[i] - 1, c); tft.drawPixel(nStarX[i], nStarY[i] + 1, c);
      }
    }
    // a shooting star across the top every few minutes
    if (nightShoot < 0 && t > nightShootAt) nightShoot = 0;
    if (nightShoot >= 0) {
      auto at = [](float p, int &x, int &y) { x = 20 + p * 190; y = 10 + p * 26; };
      int hx, hy, mx, my, ex, ey;
      at(nightShoot, hx, hy); at(max(0.0f, nightShoot - 0.06f), mx, my); at(max(0.0f, nightShoot - 0.14f), ex, ey);
      tft.drawLine(ex, ey, mx, my, TFT_BLACK);
      tft.drawLine(mx, my, hx, hy, rgb(0x8A6A5A));
      nightShoot += 0.035;
      if (nightShoot > 1.15) {
        tft.fillRect(10, 4, 220, 40, TFT_BLACK);  // sweep the trail; stars redraw next frame
        for (int i = 0; i < 14; i++) if (nStarY[i] < 44) tft.drawPixel(nStarX[i], nStarY[i], TFT_BLACK);
        nightShoot = -1; nightShootAt = t + random(120, 300) * 1000UL; nightZ = -1;
      }
    }
  }
  if (t - nightTick >= 1000 || enter) {
    nightTick = t;
    bool on = (t / 1000) % 2;
    uint32_t dot = anyAttn() ? 0x8C1C14 : countBusy() ? 0x6C3426 : 0x28100A;
    tft.fillSmoothCircle(120, 214, 3, on ? rgb(dot) : TFT_BLACK, TFT_BLACK);
  }
}

// ---------- setup screen (no Wi-Fi) ----------
void setupScreen(bool enter) {
  if (!enter) return;
  uint16_t g = rgb(GROUND), grey = rgb(C_GREY);
  tft.fillScreen(g);
  tftGlow(196, 46, 70, 0x081E2C, GROUND, 6);
  aaDraw(F_LABEL, 14, 31, L(T_SETUP), rgb(C_CYAN), g);
  uint16_t arcBg = rgb(glowAt(30, 70, 0x081E2C, GROUND, 6));
  const int rr[3] = {40, 27, 14};
  for (int i = 0; i < 3; i++)
    tft.drawSmoothArc(196, 64, rr[i] + 3, rr[i] - 3, 135, 225, rgb(mixF(GROUND, C_CYAN, 0.4 + i * 0.3)), arcBg, true);
  tft.fillSmoothCircle(196, 64, 5, rgb(C_CYAN), arcBg);
  const char *hint[2] = {L(T_SETUP_JOIN), L(T_SETUP_OPEN)};
  const char *val[2] = {"ClaudeCube", "192.168.4.1"};
  const char *num[2] = {"1", "2"};
  const int ys[2] = {106, 164};
  for (int i = 0; i < 2; i++) {
    int y = ys[i];
    tft.fillSmoothCircle(21, y - 21, 9, rgb(0x0E2838), g);
    aaCenter(F_SMALL, 21, y - 16, num[i], rgb(C_CYAN), rgb(0x0E2838));
    aaDraw(F_SMALL, 38, y - 16, hint[i], grey, g);
    aaDraw(F_PRICE, 12, y + 14, val[i], TFT_WHITE, g);
  }
  ruleLine();
  aaDraw(F_SMALL, 14, 222, L(T_SETUP_OFF), grey, g);
}

// ---------- power outages (Yasno schedule, pushed by the Mac to /outage) ----------
// One window at a time: the next or current outage for the user's group.
time_t outStart = 0, outEnd = 0;
String outGroup;
int outDrawnKey = -1;
// today and tomorrow in half hours from the relay's /outday: '0' power, '1' outage, '?' unknown
char outDay[97] = "";
uint32_t outDayAt = 0;
bool outDayOk() { return outDayAt && millis() - outDayAt < 6UL * 3600000 && strpbrk(outDay, "01"); }
int outPhase = -1;

String hhmmOf(time_t t) {
  struct tm r;
  localtime_r(&t, &r);
  char b[8];
  snprintf(b, sizeof b, "%02d:%02d", r.tm_hour, r.tm_min);
  return b;
}
long outIn() { return timeValid() && outStart ? (long)(outStart - time(nullptr)) : -1; }
bool outNow() { return timeValid() && outStart && time(nullptr) >= outStart && time(nullptr) < outEnd; }
bool outPanic() { long s = outIn(); return s > 0 && s <= 300; }
bool outSoon() { long s = outIn(); return (s > 0 && s <= 1800) || outNow(); }  // the last half hour

// the day plan: next outage big, then today and tomorrow as 24-hour bars of half hours
void outagePlan(bool enter) {
  static int drawnMin = -1;
  struct tm tm = nowTm();
  if (!enter && tm.tm_min == drawnMin) return;
  drawnMin = tm.tm_min;
  uint16_t g = rgb(GROUND), amber = rgb(0xFFA000);
  tft.fillScreen(g);
  tftGlow(206, 30, 34, 0xFFA000, 0x2A1A00, 6, 0.5);
  tft.fillSmoothCircle(206, 30, 9, amber, rgb(0x5A3A08));
  aaDraw(F_LABEL, 14, 31, L(T_OUT), amber, g);
  if (outStart && outEnd > time(nullptr)) {
    struct tm st; localtime_r(&outStart, &st);
    bool tmrw = st.tm_yday != tm.tm_yday;
    aaDraw(F_SMALL, 14, 62, tmrw ? L(T_OUT_NEXT_T) : L(T_OUT_NEXT), rgb(C_GREY), g);
    String a = hhmmOf(outStart), b = hhmmOf(outEnd);
    aaDraw(F_CLOCK, 12, 100, a.c_str(), TFT_WHITE, g);
    int x = 14 + aaWidth(F_CLOCK, a.c_str()) + 6;
    tft.fillRect(x, 86, 12, 4, amber);
    aaDraw(F_CLOCK, x + 20, 100, b.c_str(), TFT_WHITE, g);
  } else {
    aaDraw(F_NUM, 14, 92, L(T_OUT_CLEAN), TFT_WHITE, g);
  }
  // two bars, 48 half hours each; past half hours of today are dimmer
  const int X0 = 14, BW = 212;
  int nowSlot = tm.tm_hour * 2 + (tm.tm_min >= 30);
  for (int day = 0; day < 2; day++) {
    int y = day ? 178 : 136;
    aaDraw(F_SMALL, X0, y - 6, day ? L(T_OUT_TMRW) : L(T_OUT_TODAY), rgb(C_GREY), g);
    for (int i = 0; i < 48; i++) {
      char m = outDay[day * 48 + i];
      bool past = day == 0 && i < nowSlot;
      uint32_t c = m == '1' ? (past ? 0x6A4A10 : 0xFFA000) : m == '0' ? (past ? 0x1C1C22 : 0x34343C) : 0x141418;
      int x0 = X0 + i * BW / 48, x1 = X0 + (i + 1) * BW / 48;
      tft.fillRect(x0, y, x1 - x0 - (i % 2 ? 1 : 0), 14, rgb(c));
    }
    if (day == 0) {  // now
      int x = X0 + (tm.tm_hour * 60 + tm.tm_min) * BW / 1440;
      tft.fillRect(x - 1, y - 3, 2, 20, TFT_WHITE);
    }
  }
  const char *hrs[5] = {"0", "6", "12", "18", "24"};
  for (int i = 0; i < 5; i++) {
    int x = X0 + i * BW / 4;
    int w = aaWidth(F_SMALL, hrs[i]);
    aaDraw(F_SMALL, i == 0 ? x : i == 4 ? x - w : x - w / 2, 214, hrs[i], rgb(0x4E4E58), g);
  }
  if (outGroup.length()) aaRight(F_SMALL, 226, 62, (L(T_OUT_GROUP) + outGroup).c_str(), rgb(C_GREY), g);
}

extern Screen forceScr;
extern uint32_t forceScrUntil;
void outageScreen(bool enter) {
  static bool wasPlan = false;
  // the plan, unless an outage is close or on; asked for from the panel, always the plan
  bool plan = (forceScr == S_OUTAGE && millis() < forceScrUntil) || (!outSoon() && !outPanic());
  if (plan) { outagePlan(enter || !wasPlan); wasPlan = true; return; }
  if (wasPlan) { wasPlan = false; enter = true; outDrawnKey = -1; }
  bool panic = outPanic(), now = outNow();
  long left = now ? (long)(outEnd - time(nullptr)) : outIn();
  uint32_t t = millis();
  int phase = panic ? (int)((t / 600) % 2) : 0;
  // redraw when the shown number changes (seconds in the last hour, minutes before)
  int key = left < 3600 ? (int)left : (int)(left / 60);
  if (!enter && key == outDrawnKey && phase == outPhase) return;
  bool full = enter || phase != outPhase || outDrawnKey < 0;
  outDrawnKey = key;
  outPhase = phase;
  char num[16];
  if (left >= 3600) snprintf(num, sizeof num, "%ld:%02ld", left / 3600, (left / 60) % 60);
  else snprintf(num, sizeof num, "%ld:%02ld", max(0L, left) / 60, max(0L, left) % 60);

  if (panic) {
    uint32_t bgc = phase ? 0xFFA000 : GROUND;
    uint16_t bg = rgb(bgc), ink = phase ? rgb(0x1A0E00) : TFT_WHITE, accent = phase ? rgb(0x1A0E00) : rgb(0xFFA000);
    tft.fillScreen(bg);
    aaDraw(F_LABEL, 14, 31, L(T_OUT), accent, bg);
    aaDraw(F_WORD, 14, 76, L(T_OUT_SAVE), ink, bg);
    aaDraw(F_BIG, 10, 158, num, ink, bg, -2);
    tft.drawFastHLine(12, 194, 216, phase ? rgb(0x8A5600) : rgb(C_RULE));
    aaDraw(F_NUM, 14, 222, (L(T_OUT_OFF_AT) + hhmmOf(outStart)).c_str(), ink, bg);
    return;
  }
  uint16_t g = rgb(GROUND), amber = rgb(0xFFA000);
  if (full) {
    tft.fillScreen(g);
    tftGlow(206, 30, 34, 0xFFA000, 0x2A1A00, 6, 0.5);
    tft.fillSmoothCircle(206, 30, 9, now ? rgb(0x3A2A10) : amber, rgb(0x5A3A08));
    aaDraw(F_LABEL, 14, 31, now ? L(T_OUT_NONE) : L(T_OUT), amber, g);
    aaDraw(F_SMALL, 14, 128, now ? L(T_OUT_BACK_IN) : L(T_OUT_OFF_IN), rgb(C_GREY), g);
    tft.drawFastHLine(12, 194, 216, rgb(C_RULE));
    // bolt
    const int bolt[] = {18, 204, 12, 216, 17, 216, 14, 226, 22, 211, 17, 211, 20, 204};
    fillPoly(bolt, 7, amber);
    String s = now ? L(T_OUT_BACK_AT) + hhmmOf(outEnd) : hhmmOf(outStart) + " - " + hhmmOf(outEnd);
    aaDraw(F_NUM, 32, 222, s.c_str(), TFT_WHITE, g);
    if (outGroup.length()) aaRight(F_SMALL, 228, 222, (L(T_OUT_GROUP) + outGroup).c_str(), rgb(C_GREY), g);
  }
  tft.fillRect(0, 64, W, 112, g);
  aaDraw(F_BIG, 10, 112, num, TFT_WHITE, g, -2);
  aaDraw(F_SMALL, 14, 172, left >= 3600 ? L(T_OUT_HM) : L(T_OUT_MS), rgb(0x4E4E58), g);
}

// ---------- visualizer (Mac mic FFT over UDP, port 4210) ----------
// packet: 'V', 24 levels 0..255, flags (bit 0 = beat)
WiFiUDP vizUdp;
#define NBARS 24
uint8_t vizLvl[NBARS];
float vizShown[NBARS], vizPeak[NBARS];
int vizDrawn[NBARS], vizPeakDrawn[NBARS];
uint32_t vizLast = 0, vizBeat = 0;

void vizPoll() {
  int n = vizUdp.parsePacket();
  while (n > 0) {
    uint8_t buf[32];
    int len = vizUdp.read(buf, sizeof buf);
    if (len >= 2 && buf[0] == 'L') {  // his voice level while he dictates
      listenLevel = buf[1] / 255.0f;
      listenAt = millis();
    } else if (len >= 3 && buf[0] == 'G') {  // gaze from the camera tracker: 'G', x, y as signed -100..100
      gazeX = (int8_t)buf[1] / 100.0f;
      gazeY = (int8_t)buf[2] / 100.0f;
      gazeAt = millis();
    } else if (len >= 1 + NBARS && buf[0] == 'V') {
      memcpy(vizLvl, buf + 1, NBARS);
      if (len > 1 + NBARS && (buf[1 + NBARS] & 1)) vizBeat = millis();
      vizLast = millis();
    }
    n = vizUdp.parsePacket();
  }
}
bool vizActive() { return vizLast && millis() - vizLast < 1500; }

const int VZ_BASE = 186, VZ_MAX = 140, VZ_X0 = 12, VZ_W = 7, VZ_STEP = 9;
uint16_t vizRow[VZ_MAX + 4];  // bar colour by height: dark ember -> coral -> warm white
void vizColors() {
  for (int h = 0; h < VZ_MAX + 4; h++) {
    uint32_t c = mixF(0x6A2E1E, C_CORALX, min(1.0f, h / 60.0f));
    if (h > 70) c = mixF(c, 0xFFE0C8, min(1.0f, (h - 70) / 70.0f));
    vizRow[h] = rgb(c);
  }
}
uint16_t vizRowColor(int h) { return vizRow[constrain(h, 0, VZ_MAX + 3)]; }

void vizScreen(bool enter) {
  uint16_t g = rgb(GROUND);
  if (enter) {
    vizColors();
    tft.fillScreen(g);
    aaDraw(F_LABEL, 14, 31, L(T_VIZ), rgb(C_CORALX), g);
    tft.drawFastHLine(12, 194, 216, rgb(C_RULE));
    aaDraw(F_SMALL, 14, 222, L(T_VIZ_MIC), rgb(C_GREY), g);
    for (int i = 0; i < NBARS; i++) { vizShown[i] = 0; vizPeak[i] = 0; vizDrawn[i] = 0; vizPeakDrawn[i] = -1; }
  }
  // a little face top-right that nods on the beat
  static int lastNod = -1;
  int nod = millis() - vizBeat < 140 ? 4 : 0;
  if (nod != lastNod || enter) {
    lastNod = nod;
    tft.fillRect(176, 8, 56, 34, g);
    for (int sx = -1; sx <= 1; sx += 2) tft.fillSmoothRoundRect(204 + sx * 12 - 6, 14 + nod, 12, nod ? 12 : 18, 6, rgb(C_CORALX), g);
  }
  for (int i = 0; i < NBARS; i++) {
    float target = vizActive() ? vizLvl[i] * VZ_MAX / 255.0f : 0;
    vizShown[i] = target > vizShown[i] ? target : vizShown[i] * 0.86f;  // fast attack, slow fall
    vizPeak[i] = max(vizPeak[i] - 1.2f, vizShown[i]);
    int h = (int)vizShown[i], x = VZ_X0 + i * VZ_STEP;
    if (h > vizDrawn[i]) for (int y = vizDrawn[i]; y < h; y++) tft.drawFastHLine(x, VZ_BASE - y, VZ_W, vizRowColor(y));
    else if (h < vizDrawn[i]) tft.fillRect(x, VZ_BASE - vizDrawn[i] + 1, VZ_W, vizDrawn[i] - h, g);
    vizDrawn[i] = h;
    int p = (int)vizPeak[i] + 3;
    if (p != vizPeakDrawn[i]) {
      if (vizPeakDrawn[i] >= 0 && vizPeakDrawn[i] > h) tft.drawFastHLine(x, VZ_BASE - vizPeakDrawn[i], VZ_W, g);
      if (p > h + 1) tft.drawFastHLine(x, VZ_BASE - p, VZ_W, TFT_WHITE);
      vizPeakDrawn[i] = p;
    }
  }
}

// ---------- window into Kyiv 3026 (GIF loops from the user's game, picked by reality) ----------
String k3026File() {
  const char *st;
  if (alertActive()) st = "alert";
  else {
    int k = skyOf(wx.code);
    st = (k == K_RAIN || k == K_STORM) ? "rain" : k == K_SNOW ? "snow" : (wx.day ? "day" : "night");
  }
  String p = String("/gif/k3026_") + st + ".gif";
  if (LittleFS.exists(p)) return p;
  p = "/gif/k3026_day.gif";
  return LittleFS.exists(p) ? p : "";
}

void k3026Screen(bool enter) {
  if (enter) {
    String f = k3026File();
    if (!f.length() || !showFile(f)) { tft.fillScreen(rgb(GROUND)); return; }
  }
  gifTick();
}

// ---------- the console: now playing + battery (anbernic-cfw/apps/connect/cube-now.sh) ----------
// No sprite: drawn straight on the panel, only the bottom strip and the battery repaint.
struct Game { String name, sys; int bat = -1; bool chg = false; uint32_t start = 0, last = 0; } game;
uint32_t gameUntil = 0;   // a takeover after a new game or low battery
bool gameLowShown = false;
bool gameOn() { return game.last && millis() - game.last < 180000; }  // the console beats every minute

void gameBattery(uint16_t g) {
  tft.fillRect(150, 12, 90, 24, g);
  if (game.bat < 0) return;
  bool low = game.bat <= 15 && !game.chg;
  uint16_t ink = rgb(game.chg ? C_GREEN : low ? C_RED : 0xFFFFFFUL);
  int x = 198, y = 18;
  tft.drawRoundRect(x, y, 26, 14, 3, ink);
  tft.fillRect(x + 26, y + 4, 3, 6, ink);
  int w = constrain(game.bat, 0, 100) * 20 / 100;
  if (w > 0) tft.fillRect(x + 3, y + 3, w, 8, ink);
  if (game.chg) {  // a bolt cut out of the cell
    const int bolt[] = {x + 15, y + 1, x + 9, y + 8, x + 14, y + 8, x + 11, y + 13};
    thickPoly(bolt, 4, 2, g, ink);
  }
  char b[8];
  snprintf(b, sizeof b, "%d%%", game.bat);
  aaRight(F_LABEL, x - 6, 31, b, ink, g);
}

void gameStrip(uint16_t g) {
  tft.fillRect(0, 204, W, 36, g);
  uint32_t m = (millis() - game.start) / 60000;
  String el = m >= 60 ? String(m / 60) + " " + L(T_U_H) + " " + String(m % 60) + " " + L(T_U_MIN) : String(m) + " " + L(T_U_MIN);
  int ew = aaWidth(F_SMALL, el.c_str());
  aaDraw(F_NUM, 14, 222, aaFit(F_NUM, game.sys, 200 - ew).c_str(), TFT_WHITE, g);
  aaRight(F_SMALL, 228, 222, el.c_str(), rgb(C_GREY), g);
}

int gameDrawnBat = -2, gameDrawnMin = -1;
bool gameDrawnChg = false;
void gameScreen(bool enter) {
  uint16_t g = rgb(GROUND);
  int mins = (millis() - game.start) / 60000;
  if (!enter) {
    if (game.bat != gameDrawnBat || game.chg != gameDrawnChg) { gameDrawnBat = game.bat; gameDrawnChg = game.chg; gameBattery(g); }
    if (mins != gameDrawnMin) { gameDrawnMin = mins; gameStrip(g); }
    return;
  }
  gameDrawnBat = game.bat; gameDrawnChg = game.chg; gameDrawnMin = mins;
  tft.fillScreen(g);
  // the handheld, lit from behind
  const int cx = 120, cy = 88;
  uint32_t gc = mixF(GROUND, C_PLAY, 0.16);
  tftGlow(cx, cy, 58, gc, GROUND, 8);
  uint16_t body = rgb(0x2A2A30), around = rgb(glowAt(40, 58, gc, GROUND, 8));
  tft.fillSmoothRoundRect(cx - 52, cy - 30, 104, 60, 12, body, around);
  tft.fillSmoothRoundRect(cx - 24, cy - 21, 48, 36, 3, rgb(mixF(0x101014, C_PLAY, 0.55)), body);
  tft.fillRect(cx - 21, cy - 18, 42, 4, rgb(mixF(0x101014, C_PLAY, 0.85)));  // a lit line on the screen
  tft.fillRect(cx - 42, cy - 2, 12, 4, rgb(0x0C0C10));   // d-pad
  tft.fillRect(cx - 38, cy - 6, 4, 12, rgb(0x0C0C10));
  const int bx = cx + 37, by = cy;
  tft.fillSmoothCircle(bx, by - 6, 3, rgb(C_PLAY), body);
  tft.fillSmoothCircle(bx + 6, by, 3, rgb(C_PLAY), body);
  tft.fillSmoothCircle(bx - 6, by, 3, rgb(0x0C0C10), body);
  tft.fillSmoothCircle(bx, by + 6, 3, rgb(0x0C0C10), body);
  aaDraw(F_LABEL, 14, 31, L(T_GAME), rgb(C_PLAY), g);
  gameBattery(g);
  // the title: two lines at most, centred
  String text = game.name + " ", line, word, lines[2];
  int n = 0;
  unsigned i = 0;
  for (; i < text.length() && n < 1; i++) {  // fill the first line, the rest goes to the second
    char ch = text[i];
    if (ch != ' ') { word += ch; continue; }
    String probe = line.length() ? line + " " + word : word;
    if (aaWidth(F_NOTE, probe.c_str()) > 216 && line.length()) { lines[n++] = line; line = word; }
    else line = probe;
    word = "";
  }
  if (n == 0) lines[n++] = line;
  else { String rest = line + " " + text.substring(i); rest.trim(); if (rest.length()) lines[n++] = rest; }
  for (int i = 0; i < n; i++) {
    String l = aaFit(F_NOTE, lines[i], 216);
    aaCenter(F_NOTE, 120, (n == 1 ? 170 : 155 + i * 27), l.c_str(), TFT_WHITE, g);
  }
  ruleLine();
  gameStrip(g);
}

// /play?g=<game>&s=<system>&b=<battery %>&c=<1 charging>   — a launch or a heartbeat
// /play?b=..&c=..                                            — battery only
// /play?end=1                                                — the game closed
void handlePlay() {
  uint32_t t = millis();
  if (server.hasArg("b")) game.bat = constrain(server.arg("b").toInt(), 0, 100);
  if (server.hasArg("c")) game.chg = server.arg("c") == "1";
  if (game.chg || game.bat > 20) gameLowShown = false;
  if (server.hasArg("end")) {
    game.last = 0;
    gameUntil = 0;
    return sendOk();
  }
  if (server.hasArg("g")) {
    String g = server.arg("g"), s = server.arg("s");
    g.trim(); s.trim();
    if (g.length() > 80) g = g.substring(0, 80);
    if (s.length() > 40) s = s.substring(0, 40);
    if (g != game.name || !gameOn()) { game.start = t; gameUntil = t + 20000; if (cur == S_GAME) cur = S_NONE; }
    game.name = g; game.sys = s;
    game.last = t;
  }
  if (gameOn() && game.bat >= 0 && game.bat <= 10 && !game.chg && !gameLowShown) { gameLowShown = true; gameUntil = t + 15000; }
  sendOk();
}

// ---------- Claude as a pet: hunger, the dacha, the night watcher ----------
String hoursAgo(uint32_t ms) {
  uint32_t m = ms / 60000;
  return m >= 60 ? String(m / 60) + " " + L(T_U_H) : String(m) + " " + L(T_U_MIN);
}

// the pet only comes out while he's away: he works with Claude all day, so long breaks mean offline
int petState(String &sub) {
  static uint32_t offSince = 0;
  if (!offline()) { offSince = 0; return -1; }
  if (!offSince) offSince = millis() - (macSeen && macIdle > 900 ? macIdle * 1000UL : 0);
  uint32_t away = millis() - offSince;
  struct tm tm = nowTm();
  bool weekend = timeValid() && (tm.tm_wday == 0 || tm.tm_wday == 6);
  if (weekend && away > 3600000UL) { sub = L(T_PET_AWAY); return E_AWAY; }
  if (away > 3600000UL) { sub = hoursAgo(away) + L(T_PET_WITHOUT); return E_STARVE; }
  sub = L(T_PET_MISS) + hoursAgo(away); return E_HUNGRY;
}

int nagState(String &sub) {
  if (!timeValid() || millis() - lastActivity > 10UL * 60000 || !lastActivity) return -1;
  struct tm tm = nowTm();
  int m = tm.tm_hour * 60 + tm.tm_min;
  if (m < 120 || m >= 360) return -1;  // 02:00 - 06:00
  char b[8];
  snprintf(b, sizeof b, "%02d:%02d", tm.tm_hour, tm.tm_min);
  if (m < 165) { sub = String(b) + L(T_NAG_EARLY); return E_YAWN; }
  if (m < 210) { sub = String(b) + L(T_NAG_SERIOUS); return E_YAWN2; }
  sub = String(b) + L(T_NAG_SIT); return E_SNORE;
}

// ---------- Cell battle: two-colour Game of Life (Immigration rules) ----------
// He draws a pattern on the phone (side 1, cyan, starts left), Claude sends one (side 2,
// coral, starts right). Survive on 2-3 neighbours; a birth takes the majority colour of
// its 3 parents. Torus 60x52, 4 px cells under a score bar. Grids live in the arena.
#define LW 60
#define LH 52
#define LPAD 20                      // each side's pattern is at most 20x20
#define LMAX 60                      // and at most 60 live cells
#define LGEN_MAX 1000
#define LY0 26                       // grid starts below the score bar
enum LifeState { L_IDLE, L_WAIT, L_COUNT, L_RUN, L_OVER };
int lifeState = L_IDLE;
uint32_t lifeWaitAt = 0;
uint8_t lifePat[2][LPAD * LPAD / 8];  // submitted patterns, one bit a cell
inline bool patAt(int s, int i) { return lifePat[s][i >> 3] & (1 << (i & 7)); }
bool lifeHas[2] = {false, false};
uint8_t *lifeA = nullptr, *lifeB = nullptr;  // current / next generation, in the arena
int lifeGen = 0, lifeCnt[2] = {0, 0}, lifeWinner = -1;
uint32_t lifeT = 0, lifeOverAt = 0, lifeStill = 0;
bool lifeDrawn = false;
const uint16_t LIFE_C1 = 0x4E5F, LIFE_C2 = 0xDBAA;  // cyan (his), coral (Claude) in RGB565

bool lifeOwnsArena() {  // declared early: sprites and GIFs check it
  return lifeState == L_COUNT || lifeState == L_RUN || lifeState == L_OVER; }
bool lifeActive() { return lifeState != L_IDLE; }

void lifeCell(int x, int y, uint8_t v) {
  uint16_t c = v == 1 ? rgb(0x4CC9FF) : v == 2 ? rgb(C_CORALX) : rgb(GROUND);
  tft.fillRect(x * 4, LY0 + y * 4, 3, 3, c);
}

void lifeScore(bool full) {
  uint16_t g = rgb(GROUND);
  tft.fillRect(0, 0, W, LY0 - 2, g);
  char b[24];
  snprintf(b, sizeof b, L(T_LF_YOU_N), lifeCnt[0]);
  aaDraw(F_LABEL, 6, 18, b, rgb(0x4CC9FF), g);
  snprintf(b, sizeof b, "%d CLAUDE", lifeCnt[1]);
  aaRight(F_LABEL, 234, 18, b, rgb(C_CORALX), g);
  snprintf(b, sizeof b, "%d", lifeGen);
  aaCenter(F_SMALL, 120, 17, b, rgb(C_GREY), g);
}

void lifeStart() {
  // place both patterns (needs the arena: drop whatever screen held it)
  sprClose();
  gifStop();
  lifeA = arena;
  lifeB = arena + LW * LH;
  memset(lifeA, 0, LW * LH);
  for (int s = 0; s < 2; s++) {
    int ox = (s == 0 ? LW / 4 : 3 * LW / 4) - LPAD / 2, oy = LH / 2 - LPAD / 2;
    for (int y = 0; y < LPAD; y++)
      for (int x = 0; x < LPAD; x++)
        if (patAt(s, y * LPAD + x)) lifeA[(oy + y) * LW + ox + x] = s + 1;
  }
  lifeGen = 0; lifeWinner = -1; lifeStill = 0;
  lifeState = L_COUNT;
  lifeT = millis();
  lifeDrawn = false;
}

void lifeStep() {
  int changed = 0;
  lifeCnt[0] = lifeCnt[1] = 0;
  for (int y = 0; y < LH; y++) {
    int ym = (y + LH - 1) % LH, yp = (y + 1) % LH;
    for (int x = 0; x < LW; x++) {
      int xm = (x + LW - 1) % LW, xp = (x + 1) % LW;
      uint8_t nb[8] = {lifeA[ym * LW + xm], lifeA[ym * LW + x], lifeA[ym * LW + xp], lifeA[y * LW + xm],
                       lifeA[y * LW + xp], lifeA[yp * LW + xm], lifeA[yp * LW + x], lifeA[yp * LW + xp]};
      int c1 = 0, c2 = 0;
      for (int i = 0; i < 8; i++) { if (nb[i] == 1) c1++; else if (nb[i] == 2) c2++; }
      int n = c1 + c2;
      uint8_t cur = lifeA[y * LW + x], nx = 0;
      if (cur) nx = (n == 2 || n == 3) ? cur : 0;
      else if (n == 3) nx = c1 > c2 ? 1 : 2;  // born in the majority colour of its parents
      lifeB[y * LW + x] = nx;
      if (nx) lifeCnt[nx - 1]++;
      if (nx != cur) { lifeCell(x, y, nx); changed++; }
    }
  }
  uint8_t *t = lifeA; lifeA = lifeB; lifeB = t;
  lifeGen++;
  lifeStill = changed ? 0 : lifeStill + 1;
  if (!lifeCnt[0] || !lifeCnt[1] || lifeGen >= LGEN_MAX || lifeStill > 3) {
    lifeWinner = lifeCnt[0] == lifeCnt[1] ? 0 : (lifeCnt[0] > lifeCnt[1] ? 1 : 2);
    lifeState = L_OVER;
    lifeOverAt = millis();
  }
}

void lifeScreen(bool enter) {
  uint32_t t = millis();
  uint16_t g = rgb(GROUND);
  if (lifeState == L_WAIT) {  // one pattern in, waiting for the other
    if (!enter) return;
    tft.fillScreen(g);
    aaDraw(F_LABEL, 14, 31, L(T_LF_TITLE), rgb(C_CORALX), g);
    aaDraw(F_WORD, 14, 110, lifeHas[0] ? L(T_LF_WAIT) : L(T_LF_YOUR), TFT_WHITE, g);
    aaDraw(F_SMALL, 14, 132, lifeHas[0] ? L(T_LF_SET) : L(T_LF_READY), rgb(C_GREY), g);
    aaDraw(F_SMALL, 14, 150, lifeHas[0] ? L(T_LF_TELL) : L(T_LF_DRAW), rgb(C_GREY), g);
    ruleLine();
    aaDraw(F_SMALL, 14, 222, L(T_LF_COLORS), rgb(C_GREY), g);
    return;
  }
  if (lifeState == L_COUNT) {
    int left = 3 - (int)((t - lifeT) / 1000);
    static int shown = -1;
    if (enter) shown = -1;
    if (left != shown) {
      shown = left;
      if (!lifeDrawn) {  // draw the starting field once
        tft.fillScreen(g);
        for (int y = 0; y < LH; y++) for (int x = 0; x < LW; x++) if (lifeA[y * LW + x]) lifeCell(x, y, lifeA[y * LW + x]);
        lifeCnt[0] = lifeCnt[1] = 0;
        for (int i = 0; i < LW * LH; i++) if (lifeA[i]) lifeCnt[lifeA[i] - 1]++;
        lifeScore(true);
        lifeDrawn = true;
      }
      tft.fillRect(90, 0, 60, LY0 - 2, g);
      char b[4];
      snprintf(b, sizeof b, "%d", max(left, 1));
      aaCenter(F_LABEL, 120, 18, b, TFT_WHITE, g);
    }
    if (left <= 0) { lifeState = L_RUN; lifeT = t; }
    return;
  }
  if (lifeState == L_RUN) {
    if (t - lifeT < 120) return;  // ~8 generations a second
    lifeT = t;
    lifeStep();
    lifeScore(false);
    if (lifeState == L_OVER) lifeDrawn = false;
    return;
  }
  if (lifeState == L_OVER) {
    if (lifeDrawn) return;
    lifeDrawn = true;
    uint32_t col = lifeWinner == 1 ? 0x4CC9FF : lifeWinner == 2 ? C_CORALX : 0xFFFFFF;
    tft.fillRect(0, 70, W, 100, g);
    tft.drawFastHLine(0, 70, W, rgb(col));
    tft.drawFastHLine(0, 169, W, rgb(col));
    aaCenter(F_LABEL, 120, 100, L(T_LF_WIN), rgb(C_GREY), g);
    aaCenter(F_WORD, 120, 132, lifeWinner == 1 ? L(T_LF_YOU) : lifeWinner == 2 ? "CLAUDE" : L(T_LF_DRAWN), rgb(col), g);
    char b[40];
    snprintf(b, sizeof b, L(T_LF_SCORE), lifeCnt[0], lifeCnt[1], lifeGen);
    aaCenter(F_SMALL, 120, 156, b, rgb(C_GREY), g);
  }
}

void lifeTick() {  // the battle ends its result screen after 40 s
  if (lifeState == L_OVER && millis() - lifeOverAt > 40000) { lifeState = L_IDLE; lifeHas[0] = lifeHas[1] = false; }
}

// /life?side=1|2&cells=<LPAD*LPAD chars of 0/1>   submit a pattern
// /life?reset=1                                     cancel
void handleLife() {
  if (server.hasArg("reset")) { lifeState = L_IDLE; lifeHas[0] = lifeHas[1] = false; return server.send(200, "text/plain", "ok"); }
  int side = server.arg("side").toInt();
  String c = server.arg("cells");
  if ((side != 1 && side != 2) || (int)c.length() != LPAD * LPAD) return server.send(400, "text/plain", "side=1|2 and 400 cells");
  if (lifeOwnsArena()) return server.send(409, "text/plain", "battle in progress");
  int live = 0;
  for (int i = 0; i < LPAD * LPAD; i++) live += c[i] == '1';
  if (live == 0 || live > LMAX) return server.send(400, "text/plain", "1..60 live cells");
  memset(lifePat[side - 1], 0, sizeof lifePat[0]);
  for (int i = 0; i < LPAD * LPAD; i++) if (c[i] == '1') lifePat[side - 1][i >> 3] |= 1 << (i & 7);
  lifeHas[side - 1] = true;
  if (lifeHas[0] && lifeHas[1]) lifeStart();
  else { lifeState = L_WAIT; lifeWaitAt = millis(); }
  cur = S_NONE;  // redraw
  server.send(200, "text/plain", "ok");
}

// ---------- relay (Cloudflare Worker, relay/worker.js): works with the Mac off ----------
uint32_t rlLevelNext = 0, rlPairNext = 0, rlOutNext = 0;
bool rlOk = false;   // last relay call worked

bool relayGet(const String &path, String &out) {
  if (!cfg.relay.length()) return false;
  WiFiClient c;
  HTTPClient http;
  http.useHTTP10(true);
  if (!http.begin(c, "http://" + cfg.relay + path)) return false;
  http.setTimeout(6000);
  rlOk = http.GET() == 200;
  if (rlOk) out = http.getString();
  http.end();
  return rlOk;
}

void relayLevel() {  // KMDA's yellow/red: Kyiv city only
  String r;
  if (cfg.areg != 0 || !relayGet("/level", r)) return;
  int sp = r.indexOf(' ');
  String lv = r.substring(0, sp);
  if (lv == "unknown") return;
  kyivLevel = lv == "yellow" ? 1 : lv == "red" ? 2 : 0;
  levelSince = (time_t)r.substring(sp + 1).toInt();
  levelAt = millis();
}

void relayPairs() {  // the pairs Binance doesn't list
  for (int i = 0; i < cx.n; i++) {
    Coin &k = cx.c[i];
    if (k.sym != "USDUAH" && k.sym != "WBTUSDT") continue;
    String r;
    if (!relayGet("/pair?s=" + k.sym, r)) continue;
    int a = r.indexOf(' '), b = r.indexOf(' ', a + 1);
    k.price = r.substring(0, a).toFloat();
    k.chg = r.substring(a + 1, b).toFloat();
    String sp = r.substring(b + 1);
    k.nSpark = 0;
    int from = 0;
    for (int j = 0; j <= (int)sp.length() && k.nSpark < 24; j++) {
      if (j < (int)sp.length() && sp[j] != ',') continue;
      if (j > from) k.spark[k.nSpark++] = sp.substring(from, j).toFloat();
      from = j + 1;
    }
    k.ok = k.price > 0;
    k.pushed = millis();
    cx.stamp = millis();
  }
}

void relayOutage() {
  String r;
  if (!cfg.outg.length() || !relayGet("/outage?g=" + cfg.outg + (cfg.outr.length() ? "&r=" + cfg.outr : String("")), r)) return;
  int a = r.indexOf(' '), b = r.indexOf(' ', a + 1);
  outStart = (time_t)r.substring(0, a).toInt();
  outEnd = (time_t)r.substring(a + 1, b).toInt();
  outGroup = r.substring(b + 1);
  outGroup.trim();
  outDrawnKey = -1;
  if (!relayGet("/outday?g=" + cfg.outg + (cfg.outr.length() ? "&r=" + cfg.outr : String("")), r)) return;
  int sp = r.indexOf(' ');
  if (sp < 0 || (int)r.length() < sp + 97) return;
  strncpy(outDay, r.c_str() + sp + 1, 96);
  outDay[96] = 0;
  outDayAt = millis();
}


// ---------- listening: he's dictating a voice message to Claude ----------
// Eyes on top (all ears), bars that follow his voice, LISTENING below.
// the voice as Claude's own dictation box draws it: thin rounded bars, newest on the right,
// scrolling left, older ones fading out
#define BARS_N 40
uint8_t barH[BARS_N], barDrawn[BARS_N];
uint32_t listenFrames = 0;  // frames drawn, for /status lfps
uint32_t barStepAt = 0;
float barPeak = 0;

void listenScreen(bool enter) {
  uint16_t g = rgb(GROUND);
  const int BY = 158, BX = 21, PITCH = 5, BW = 3, BMAX = 30;
  if (enter) {
    tft.fillScreen(g);
    tftGlow(120, EYE_Y, 116, 0x220F0A, GROUND, 8);
    tft.fillRect(0, FACE_H, W, H - FACE_H, g);
    ruleLine(196);
    aaDraw(F_WORD, 14, 226, L(T_EMO_LISTEN), TFT_WHITE, g);
    facePalMode = -1;
    faceDrawnEmo = -1;
    memset(barH, 1, sizeof barH);
    memset(barDrawn, 0, sizeof barDrawn);
    barPeak = 0;
  }
  if (!spr.created()) {
    if (!sprOpen(W, FACE_H)) return;
    facePalMode = -1;
  }
  float lv = listening() ? listenLevel : 0;
  listenSmooth += (lv - listenSmooth) * 0.35f;
  barPeak = max(barPeak, lv);
  uint32_t t = millis();
  // the eyes are the heavy part (a full sprite push): ~12 fps is plenty for them
  static uint32_t eyesAt = 0;
  if (enter || t - eyesAt >= 80) { eyesAt = t; faceFrame(E_LISTEN, t); }
  listenFrames++;
  // a new bar every 70 ms from the loudest moment since the last one; the row shifts left
  if (t - barStepAt >= 70) {
    barStepAt = t;
    memmove(barH, barH + 1, BARS_N - 1);
    barH[BARS_N - 1] = 1 + (uint8_t)(pow(barPeak, 0.7f) * (BMAX - 1));
    barPeak = 0;
    for (int i = 0; i < BARS_N; i++) {
      int x = BX + i * PITCH, h = barH[i], d = barDrawn[i];
      if (h == d) continue;
      if (h < d) {  // shrink: clear the ends that are gone
        tft.fillRect(x, BY - d, BW, d - h, g);
        tft.fillRect(x, BY + h, BW, d - h, g);
      }
      float fade = 0.25f + 0.75f * i / (BARS_N - 1);  // older bars sink into the ground
      tft.fillRoundRect(x, BY - h, BW, h * 2, 1, rgb(mixF(GROUND, C_CORALX, fade)));
      barDrawn[i] = h;
    }
  }
  // a pulsing "recording" dot by the word
  static int dotR = -1;
  int r = 4 + (int)(listenSmooth * 6);
  if (r != dotR || enter) {
    tft.fillRect(204, 204, 28, 28, g);
    tft.fillSmoothCircle(218, 218, r, rgb(C_CORALX), g);
    dotR = r;
  }
}

// every 30 min while he's at the desk, by day: SIT UP, angrily. Busy then? Try again in a minute.
void postureNag() {
  static uint32_t next = 0;
  if (!next) next = millis() + 30 * 60000UL;
  if (millis() < next) return;
  if (offline() || isNight() || alertActive() || lifeOwnsArena() || anyAttn()) { next = millis() + 60000UL; return; }
  next = millis() + 30 * 60000UL;
  manualEmo = E_JUDGE; manualLabel = L(T_SUB_JUDGE); manualUntil = millis() + 7000;
}

// ---------- screen choice ----------
bool apMode = false;
uint32_t allClearUntil = 0;
Screen forceScr = S_NONE;
uint32_t forceScrUntil = 0;
bool prevAlert = false;

Screen decide() {
  uint32_t t = millis();
  if (apMode) return S_SETUP;
  bool al = alertActive();
  // all clear for one minute; a feed that flickers on/off right after doesn't restart it
  if (prevAlert && !al && (!allClearUntil || (int32_t)(t - allClearUntil) > 5 * 60000L)) allClearUntil = t + 60000;
  prevAlert = al;
  // a long alert stops hogging the cube: after 20 min it shows 15 s of every 3 minutes
  auto alertTurn = [&](time_t st) {
    long el = (st > 0 && timeValid()) ? (long)(time(nullptr) - st) : 0;
    return el < 20 * 60 || (t / 1000) % 180 < 15;
  };
  if (listening() || (cur == S_LISTEN && millis() - listenAt < 1500)) return S_LISTEN;  // he's talking to Claude: seconds, even over an alert
  if (al && alertTurn(alertStartReal())) return S_ALERT;
  if (t < allClearUntil) return S_ALLCLEAR;
  if (t < noteUntil) return S_NOTE;
  forcedEmo = -1;
  if (outPanic()) return S_OUTAGE;
  if (lifeOwnsArena()) return S_LIFE;  // a battle runs to the end, the result stays 40 s
  if (lifeState == L_WAIT && t - lifeWaitAt < 30000) return S_LIFE;
  if (t < manualUntil || anyAttn()) return S_FACE;
  // night watcher: Claude still busy after 2 a.m.
  { int n = nagState(forcedSub); if (n >= 0) { forcedEmo = n; return S_FACE; } }
  if (rnd.on && millis() - rnd.last > 90000) rnd.on = false;
  if (rnd.on || millis() < rnd.doneUntil) return S_RENDER;
  if (vizActive()) return S_VIZ;
  if (forcePhoto.length() && t < forceUntil) return S_PHOTO;
  if (t < forceScrUntil) return forceScr;  // /set?screen=outage from the panel
  if (t < faceUntil && topSession() >= 0) return S_FACE;
  if (gameOn() && t < gameUntil) return S_GAME;  // a new game or low battery
  if (isNight()) return S_NIGHT;
  // photos and GIFs only while Claude and aerender are idle
  bool busy = countBusy() > 0 || anyAttn() || rnd.on || TEAMS_UNREAD > 0;
  if (gameOn() && !busy && (t / 1000) % 120 < 12) return S_GAME;  // while he plays: 12 s of every 2 min
  if (roundIdx == 2 && busy) roundUntil = 0;
  if (t > roundUntil) {
    // weather -> crypto -> photos -> pet -> Kyiv 3026 -> outage, skipping whatever has nothing to show
    String petSub;
    bool found = false;
    for (int i = 0; i < nOrder && !found; i++) {
      roundPos = (roundPos + 1) % nOrder;
      roundIdx = orderIds[roundPos];
      found = true;
      if (roundIdx == 0) break;
      if (roundIdx == 1 && cfg.cxsec > 0 && cxAny() && !TEAMS_UNREAD) break;
      if (roundIdx == 2 && nPhotos > 0 && !busy) break;
      if (roundIdx == 3 && !busy && petState(petSub) >= 0) break;
      if (roundIdx == 4 && !busy && k3026File().length()) break;
      if (roundIdx == 5 && (outSoon() || outDayOk())) break;  // the countdown, or the day's plan
      found = false;
    }
    if (!found) roundIdx = 0;  // nothing in the order can show: weather
    uint32_t len = roundIdx == 0 ? cfg.wxsec * 1000UL
                 : roundIdx == 1 ? cfg.cxsec * 1000UL
                 : roundIdx == 2 ? max(1, min(nPhotos, 4)) * (cfg.slide * 1000UL + 1000)
                 : roundIdx == 4 ? 14000UL : 9000UL;
    roundUntil = t + len;
  }
  if (roundIdx == 3) {
    int p = petState(forcedSub);
    if (p >= 0 && !busy) { forcedEmo = p; return S_FACE; }
    roundUntil = 0;
  }
  return roundIdx == 0 ? S_WEATHER : roundIdx == 1 ? S_CRYPTO : roundIdx == 2 ? S_PHOTO
       : roundIdx == 4 ? S_K3026 : roundIdx == 5 ? S_OUTAGE : S_WEATHER;
}

void runScreen(Screen s, bool enter) {
  switch (s) {
    case S_WEATHER: weatherScreen(enter); break;
    case S_PHOTO: photoScreen(enter); break;
    case S_FACE: faceScreen(enter); break;
    case S_ALERT: alertScreen(enter); break;
    case S_ALLCLEAR: allClearScreen(enter); break;
    case S_RENDER: renderScreen(enter); break;
    case S_NOTE: noteScreen(enter); break;
    case S_NIGHT: nightScreen(enter); break;
    case S_CRYPTO: cryptoScreen(enter); break;
    case S_OUTAGE: outageScreen(enter); break;
    case S_VIZ: vizScreen(enter); break;
    case S_K3026: k3026Screen(enter); break;
    case S_LIFE: lifeScreen(enter); break;
    case S_LISTEN: listenScreen(enter); break;
    case S_GAME: gameScreen(enter); break;
    case S_SETUP: setupScreen(enter); break;
    default: break;
  }
}

void leaveScreen() {
  sprClose();
  if (textMask) { free(textMask); textMask = nullptr; }
  gifStop();
  faceDrawnEmo = -1;
}

int curBrt = -1;
void updateBacklight() {
  int b = isNight() ? cfg.nbrt : cfg.brt;
  if (cur == S_ALERT || cur == S_FACE) b = max(b, isNight() ? 15 : b);
  if (alertActive()) b = 100;  // an alert is on: full brightness, day or night
  if (b != curBrt) { setBacklight(b); curBrt = b; }
}

// ---------- web ----------
uint32_t hashStr(const String &s) {
  uint32_t h = 2166136261u;
  for (unsigned i = 0; i < s.length(); i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
  return h;
}

void sendOk(const String &msg) { server.send(200, "text/plain; charset=utf-8", msg); }

void handleFace() {
  String e = server.arg("e");
  String sid = server.arg("sid");
  String label = server.arg("t");
  if (sid.length()) {
    if (e == "end") { sessionEvent(hashStr(sid), -1, ""); return sendOk(); }
    int emo = emoFromName(e);
    if (emo < 0) return server.send(400, "text/plain", "bad emotion");
    sessionEvent(hashStr(sid), emo, label);
  } else {
    if (e == "clear") { manualUntil = 0; faceUntil = 0; return sendOk(); }
    int emo = emoFromName(e);
    if (emo < 0) return server.send(400, "text/plain", "bad emotion");
    manualEmo = emo; manualLabel = label;
    if (emo == E_MISS) noteUntil = 0;  // her heartbeat beats a note that is still up
    int hold = server.hasArg("hold") ? server.arg("hold").toInt() : 8;
    manualUntil = millis() + constrain(hold, 1, 600) * 1000UL;
  }
  sendOk();
}

void handleRender() {
  String st = server.arg("state");
  if (server.hasArg("name")) rnd.name = server.arg("name");
  if (server.hasArg("n")) rnd.total = server.arg("n").toInt();
  if (server.hasArg("q")) rnd.q = server.arg("q").toInt();
  if (server.hasArg("qn")) rnd.qn = server.arg("qn").toInt();
  if (server.hasArg("next")) rnd.next = server.arg("next");
  if (st == "done" || st == "fail") {
    rnd.on = false;
    rnd.elapsed = server.hasArg("el") ? server.arg("el").toInt() : (millis() - rnd.start) / 1000;
    if (st == "done") rnd.doneUntil = millis() + 30000;
    else sessionEvent(hashStr("aerender"), E_SAD, L(T_RND_FAIL) + rnd.name);
    return sendOk();
  }
  if (!rnd.on) rnd.start = millis();
  rnd.on = true;
  rnd.doneUntil = 0;
  if (server.hasArg("f")) rnd.frame = server.arg("f").toInt();
  rnd.p = constrain(server.arg("p").toInt(), 0, 100);
  rnd.eta = server.hasArg("eta") ? server.arg("eta").toInt() : -1;
  rnd.last = millis();
  sendOk();
}

void handleNote() {
  noteText = server.arg("t");
  noteText.trim();
  if (!noteText.length()) return server.send(400, "text/plain", "empty");
  int sec = server.hasArg("sec") ? server.arg("sec").toInt() : 60;
  noteUntil = millis() + constrain(sec, 5, 3600) * 1000UL;
  if (cur == S_NOTE) cur = S_NONE;  // a new note while one is showing: draw it now
  struct tm tm = nowTm();
  char b[8];
  snprintf(b, sizeof b, "%02d:%02d", tm.tm_hour, tm.tm_min);
  noteTime = timeValid() ? b : "";
  sendOk();
}

void handleSet() {
  if (server.hasArg("brt")) cfg.brt = constrain(server.arg("brt").toInt(), 1, 100);
  if (server.hasArg("nbrt")) cfg.nbrt = constrain(server.arg("nbrt").toInt(), 0, 100);
  if (server.hasArg("non")) cfg.non = constrain(server.arg("non").toInt(), 0, 23);
  if (server.hasArg("noff")) cfg.noff = constrain(server.arg("noff").toInt(), 0, 23);
  if (server.hasArg("slide")) cfg.slide = constrain(server.arg("slide").toInt(), 2, 120);
  if (server.hasArg("wxsec")) cfg.wxsec = constrain(server.arg("wxsec").toInt(), 5, 600);
  if (server.hasArg("cxsec")) cfg.cxsec = constrain(server.arg("cxsec").toInt(), 0, 600);
  if (server.hasArg("pairs")) { cfg.pairs = server.arg("pairs"); cfg.pairs.toUpperCase(); cfg.pairs.replace(" ", ""); parsePairs(); cxNext = 0; }
  if (server.hasArg("bgr")) { cfg.bgr = server.arg("bgr") == "1"; applyPanel(); cur = S_NONE; }
  if (server.hasArg("rot")) { cfg.rot = server.arg("rot").toInt() & 3; applyPanel(); cur = S_NONE; }
  if (server.hasArg("relay")) cfg.relay = server.arg("relay");
  if (server.hasArg("lat") && server.hasArg("lon")) { cfg.lat = server.arg("lat").toFloat(); cfg.lon = server.arg("lon").toFloat(); wxNext = 0; }
  if (server.hasArg("outg")) { cfg.outg = server.arg("outg"); rlOutNext = 0; }
  if (server.hasArg("outr")) { cfg.outr = server.arg("outr"); rlOutNext = 0; }
  if (server.arg("screen") == "outage") { forceScr = S_OUTAGE; forceScrUntil = millis() + 20000; cur = S_NONE; }
  if (server.hasArg("areg")) {  // a new alert region also moves the weather there
    cfg.areg = constrain((int)server.arg("areg").toInt(), 0, REGION_N - 1);
    cfg.lat = REGION_LAT[cfg.areg]; cfg.lon = REGION_LON[cfg.areg];
    alertKnown = false; alertOn = false; alertNext = 0; wxNext = 0; levelAt = 0; cur = S_NONE;
  }
  if (server.hasArg("order")) { cfg.order = server.arg("order"); parseOrder(); roundUntil = 0; }
  if (server.hasArg("wxday")) wxForceDay = server.arg("wxday").toInt();
  if (server.hasArg("wxstyle")) { cfg.wxstyle = server.arg("wxstyle").toInt() ? 1 : 0; cur = S_NONE; }
  if (server.hasArg("lang")) { int i = langIndex(server.arg("lang")); if (i >= 0) { cfg.lang = i; cur = S_NONE; } }
  if (server.hasArg("wx")) { wxForce = server.arg("wx").toInt(); roundIdx = 0; roundUntil = millis() + 30000; cur = S_NONE; }
  if (server.hasArg("alert")) {
    int a = server.arg("alert").toInt();
    if (a == 1 && !alertActive()) { alertStart = time(nullptr); alertFromBoot = false; }  // a test alert runs its own timer
    if (a == -1 && alertForce == 1) alertEnd = time(nullptr);
    alertForce = a;
  }
  saveCfg();
  curBrt = -1;
  sendOk();
}

void handleStatus() {
  // the JSON plus its serialized copy need ~2-3 KB in one piece: refuse rather than run out (OOM = reboot)
  if (ESP.getFreeHeap() < 5000 || ESP.getMaxFreeBlockSize() < 3000) return server.send(503, "text/plain", "busy");
  JsonDocument d;
  d["fw"] = "claude-cube 4.3.3";
  { static uint32_t f0 = 0, t0 = 0; uint32_t now = millis();
    if (now - t0 > 0) d["lfps"] = (listenFrames - f0) * 1000.0 / (now - t0); f0 = listenFrames; t0 = now; }
  d["screen"] = (int)cur;
  d["heap"] = ESP.getFreeHeap();
  d["minHeap"] = minHeap;
  d["maxBlock"] = ESP.getMaxFreeBlockSize();
  d["gifNeed"] = sizeof(AnimatedGIF);
  d["up"] = millis() / 1000;
  d["reset"] = ESP.getResetReason();
  d["alert"] = alertOn; d["yellow"] = alertIsYellow(); d["level"] = kyivLevel; d["levelFresh"] = levelFresh(); d["alertKnown"] = alertKnown;
  d["wx"]["ok"] = wx.ok; d["wx"]["t"] = wx.t; d["wx"]["code"] = wx.code;
  d["photos"] = nPhotos;
  d["night"] = isNight();
  d["outage"]["start"] = (long)outStart; d["outage"]["end"] = (long)outEnd; d["outage"]["group"] = outGroup;
  d["relayOk"] = rlOk;
  if (gameOn()) {
    JsonObject p = d["play"].to<JsonObject>();
    p["on"] = gameOn(); p["game"] = game.name; p["sys"] = game.sys; p["bat"] = game.bat; p["chg"] = game.chg;
  }
  {
    JsonObject l = d["life"].to<JsonObject>();
    static const char *st[] = {"idle", "wait", "count", "run", "over"};
    l["state"] = st[lifeState]; l["you"] = lifeHas[0]; l["claude"] = lifeHas[1];
    l["gen"] = lifeGen; l["cnt1"] = lifeCnt[0]; l["cnt2"] = lifeCnt[1]; l["winner"] = lifeWinner;
  }
  d["tasksToday"] = tasksToday; d["offline"] = offline(); d["macIdle"] = macIdle;
  FSInfo fi; LittleFS.info(fi);
  d["fsTotal"] = fi.totalBytes; d["fsUsed"] = fi.usedBytes;
  JsonObject c = d["cfg"].to<JsonObject>();
  c["lang"] = langCode(cfg.lang); c["langs"] = LANG_CODES;
  c["brt"] = cfg.brt; c["nbrt"] = cfg.nbrt; c["non"] = cfg.non; c["noff"] = cfg.noff;
  c["cxsec"] = cfg.cxsec; c["pairs"] = cfg.pairs; c["wxstyle"] = cfg.wxstyle; c["order"] = cfg.order;
  c["lat"] = cfg.lat; c["lon"] = cfg.lon; c["relay"] = cfg.relay; c["outg"] = cfg.outg; c["outr"] = cfg.outr; c["areg"] = cfg.areg;
  c["slide"] = cfg.slide; c["wxsec"] = cfg.wxsec; c["bgr"] = cfg.bgr; c["rot"] = cfg.rot;
  JsonArray ss = d["sessions"].to<JsonArray>();
  for (int i = 0; i < MAXS; i++) if (sess[i].used) {
    JsonObject o = ss.add<JsonObject>();
    o["emo"] = emoNames[sess[i].emo]; o["label"] = sess[i].label; o["age"] = (millis() - sess[i].t) / 1000;
  }
  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

void handleList() {
  if (ESP.getFreeHeap() < 4000) return server.send(503, "text/plain", "busy");
  String out = "[";
  for (int i = 0; i < nPhotos; i++) {
    File f = LittleFS.open(photos[i], "r");
    out += (i ? ",{\"f\":\"" : "{\"f\":\"") + photos[i] + "\",\"s\":" + String(f ? f.size() : 0) + "}";
    if (f) f.close();
  }
  out += "]";
  server.send(200, "application/json", out);
}

void handleDel() {
  String f = server.arg("f");
  if (!(f.startsWith("/image/") || f.startsWith("/gif/")) || f.indexOf("..") >= 0) return server.send(400, "text/plain", "bad path");
  LittleFS.remove(f);
  scanPhotos();
  sendOk();
}

void handleShow() {
  String f = server.arg("f");
  if (!LittleFS.exists(f)) return server.send(404, "text/plain", "no file");
  forcePhoto = f;
  forceUntil = millis() + (server.hasArg("sec") ? server.arg("sec").toInt() : 30) * 1000UL;
  cur = S_NONE;
  sendOk();
}

File upFile;
String upPath;
bool upFail = false;
void handleUploadData() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    gifStop();
    String n = up.filename;
    int sl = n.lastIndexOf('/');
    if (sl >= 0) n = n.substring(sl + 1);
    n.replace(" ", "_");
    if (!n.length()) n = "p" + String(millis()) + ".jpg";
    upPath = (isGif(n) ? "/gif/" : "/image/") + n;
    upFile = LittleFS.open(upPath, "w");
    upFail = !upFile;
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (upFile && upFile.write(up.buf, up.currentSize) != up.currentSize) upFail = true;
  } else if (up.status == UPLOAD_FILE_END || up.status == UPLOAD_FILE_ABORTED) {
    if (upFile) upFile.close();
    if (up.status == UPLOAD_FILE_ABORTED) upFail = true;
  }
}
void handleUploadDone() {
  if (upFail) { LittleFS.remove(upPath); scanPhotos(); return server.send(507, "text/plain", "upload failed (no space?)"); }
  scanPhotos();
  if (server.arg("show") == "1") { forcePhoto = upPath; forceUntil = millis() + 30000; cur = S_NONE; }
  sendOk(upPath);
}

String apSsid, apPass;
void handleWifi() {
  if (server.method() == HTTP_POST) {
    WiFi.persistent(true);
    WiFi.begin(server.arg("s").c_str(), server.arg("p").c_str());
    sendOk(L(T_SETUP_SAVED));
    delay(10000);
    ESP.restart();
    return;
  }
  server.send(200, "text/html; charset=utf-8",
              String("<meta name=viewport content='width=device-width'><form method=post style='font:18px sans-serif'>") +
              L(T_SETUP_NET) + "<br><input name=s><br>" + L(T_SETUP_PASS) + "<br><input name=p type=password><br><button>" + L(T_SETUP_SAVE) + "</button></form>");
}

// the Mac pushes pairs Binance doesn't list (WBT): /cx?sym=WBTUSDT&p=80.7&chg=0.67&spark=80.1,80.3,...
void handleCx() {
  String sym = server.arg("sym");
  sym.toUpperCase();
  for (int i = 0; i < cx.n; i++) {
    if (cx.c[i].sym != sym) continue;
    Coin &k = cx.c[i];
    k.price = server.arg("p").toFloat();
    k.chg = server.arg("chg").toFloat();
    String sp = server.arg("spark");
    k.nSpark = 0;
    int from = 0;
    for (int j = 0; j <= (int)sp.length() && k.nSpark < 24; j++) {
      if (j < (int)sp.length() && sp[j] != ',') continue;
      if (j > from) k.spark[k.nSpark++] = sp.substring(from, j).toFloat();
      from = j + 1;
    }
    k.ok = k.price > 0;
    k.pushed = millis();
    cx.stamp = millis();
    return sendOk();
  }
  server.send(404, "text/plain", "pair is not in cfg.pairs");
}

// next outage window for the user's group: /outage?start=<unix>&end=<unix>&group=20.1 (start=0 clears)
void handleOutage() {
  outStart = (time_t)server.arg("start").toInt();
  outEnd = (time_t)server.arg("end").toInt();
  if (server.hasArg("group")) outGroup = server.arg("group");
  outDrawnKey = -1;
  sendOk();
}

// /presence?idle=<s>: the Mac is on and how long it has been idle (tools/cube_agent.py, every 30 s)
void handleTeams() {
  macSeen = millis();
  if (server.hasArg("idle")) macIdle = server.arg("idle").toInt();
  sendOk();
}

// /alertlevel?level=yellow|red|none&since=<unix>   from tools/cube_prices.py (KMDA Telegram)
void handleAlertLevel() {
  String lv = server.arg("level");
  kyivLevel = lv == "yellow" ? 1 : lv == "red" ? 2 : 0;
  levelSince = (time_t)server.arg("since").toInt();
  levelAt = millis();
  server.send(200, "text/plain", "ok");
}

// /gaze?x=-1..1&y=-1..1   where he is in the camera frame (tools/cube_eyes.py, ~8 a second)
void handleGaze() {
  gazeX = constrain(server.arg("x").toFloat(), -1.0f, 1.0f);
  gazeY = constrain(server.arg("y").toFloat(), -1.0f, 1.0f);
  gazeAt = millis();
  server.send(200, "text/plain", "ok");
}

void setupWeb() {
  server.on("/", []() { server.sendHeader("Content-Encoding", "gzip"); server.send_P(200, "text/html; charset=utf-8", (const char *)INDEX_HTML_GZ, INDEX_HTML_GZ_LEN); });
  server.on("/face", handleFace);
  server.on("/render", handleRender);
  server.on("/cx", handleCx);
  server.on("/outage", handleOutage);
  server.on("/presence", handleTeams);
  server.on("/alertlevel", handleAlertLevel);
  server.on("/gaze", handleGaze);
  server.on("/life", handleLife);
  server.on("/play", handlePlay);
  server.on("/note", handleNote);
  server.on("/set", handleSet);
  server.on("/status", handleStatus);
  server.on("/list", handleList);
  server.on("/del", handleDel);
  server.on("/show", handleShow);
  server.on("/wifi", handleWifi);
  server.on("/reboot", []() { sendOk(); delay(300); ESP.restart(); });
  server.on("/upload", HTTP_POST, handleUploadDone, handleUploadData);
  server.onNotFound([]() {
    String p = ESP8266WebServer::urlDecode(server.uri());  // Cyrillic file names arrive percent-encoded
    if ((p.startsWith("/image/") || p.startsWith("/gif/")) && LittleFS.exists(p)) {
      File f = LittleFS.open(p, "r");
      server.streamFile(f, isGif(p) ? "image/gif" : "image/jpeg");
      f.close();
      return;
    }
    if (apMode) { server.sendHeader("Location", "/wifi"); server.send(302); return; }
    server.send(404, "text/plain", "not found");
  });
  updater.setup(&server, "/update");
  server.begin();
}

// ---------- boot ----------
void bootMsg(const char *s) {
  tft.fillRect(0, 150, W, 40, C_BG);
  aaCenter(F_SMALL, 120, 172, s, C_DIM, C_BG);
}

void setup() {
  Serial.begin(115200);
  arena = (uint8_t *)malloc(ARENA_SIZE);  // first, while the heap is one piece
  analogWriteRange(1023);
  analogWriteFreq(1000);
  pinMode(BL_PIN, OUTPUT);
  setBacklight(0);

  LittleFS.begin();
  loadCfg();
  parsePairs();
  parseOrder();
  tft.init();
  applyPanel();
  initPalette();
  skyRows();
  u8t.begin(tft);
  u8t.setFontMode(1);
  tft.fillScreen(C_BG);
  setBacklight(cfg.brt);
  // boot face: two coral eyes
  tft.fillRoundRect(43, 58, 58, 78, 18, C_CORAL);
  tft.fillRoundRect(139, 58, 58, 78, 18, C_CORAL);
  bootMsg(L(T_SETUP_BOOT));

  TJpgDec.setCallback(jpgOut);
  scanPhotos();

  WiFi.mode(WIFI_STA);
  WiFi.hostname("claudecube");
  WiFi.begin();
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(200);
  if (WiFi.status() != WL_CONNECTED) {
    apMode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP("ClaudeCube");
  } else {
    bootMsg(WiFi.localIP().toString().c_str());
    MDNS.begin("claudecube");
    MDNS.addService("http", "tcp", 80);
    configTime("EET-2EEST,M3.5.0/3,M10.5.0/4", "pool.ntp.org", "time.google.com");
  }
  setupWeb();
  vizUdp.begin(4210);
  lastFeed = millis();
  randomSeed(ESP.getCycleCount());
  delay(800);
}

uint32_t lastFrame = 0, lastHouse = 0;
void loop() {
  server.handleClient();
  vizPoll();
  if (!apMode) MDNS.update();
  uint32_t t = millis();

  // Network fetches need heap that a GIF (24 KB) or the big sprite (14-17 KB) is holding.
  // During a GIF they wait for the slide to end; a sprite is handed back for the fetch.
  bool due = t > alertNext || t > wxNext || (cfg.cxsec > 0 && t > cxNext) ||
             t > rlLevelNext || t > rlPairNext || t > rlOutNext;
  if (due && !gif && !apMode && WiFi.status() == WL_CONNECTED) {
    if (t > alertNext) { fetchAlert(); alertNext = t + 20000; }
    if (t > wxNext) { fetchWeather(); wxNext = t + (wx.ok ? 15UL * 60000 : 30000); }
    if (cfg.cxsec > 0 && t > cxNext) { fetchCrypto(); cxNext = t + (cxAny() ? 120000 : 30000); }
    if (t > rlLevelNext) { relayLevel(); rlLevelNext = t + 60000; }
    if (t > rlPairNext) { relayPairs(); rlPairNext = t + 120000; }
    if (t > rlOutNext) { relayOutage(); rlOutNext = t + 300000; }
  }
  uint32_t fh = ESP.getFreeHeap();
  if (fh < minHeap) minHeap = fh;
  if (t - lastHouse > 1000) { lastHouse = t; expireSessions(); updateBacklight(); lifeTick(); postureNag(); }

  Screen s = decide();
  if (s != cur) { leaveScreen(); cur = s; screenSince = t; runScreen(s, true); lastFrame = t; return; }
  uint32_t frameMs = cur == S_LISTEN ? 20 : (cur == S_WEATHER || cur == S_FACE || cur == S_VIZ) ? 33 : 10;
  if (t - lastFrame >= frameMs) { lastFrame = t; runScreen(cur, false); }
}
