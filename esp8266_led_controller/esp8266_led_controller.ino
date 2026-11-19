/*
  ESP8266 LED Controller
  - LED 3 chan (WS2811/WS2812...) qua Adafruit NeoPixel
  - Lan dau: ESP phat AP "LED-Setup" -> quet WiFi, nhap mat khau -> hien IP tren trang AP -> dong AP
  - Web dieu khien: mau, so LED, che do, do sang, toc do, bat/tat den (ESP luon online), phat sang vung
  - 2 doan day khac thu tu mau: moi doan co "Thu tu mau" rieng + do tu dong

  Thu vien can cai (Library Manager): "Adafruit NeoPixel"
  Board: ESP8266 core 3.x -> NodeMCU 1.0 hoac LOLIN(WEMOS) D1 mini
  Dau day: chan data LED -> D2 (GPIO4) qua dien tro ~330 ohm, GND LED noi chung GND ESP,
           nguon 5V LED cap rieng (du dong), tot nhat co IC doi muc 3.3V->5V (74AHCT125).
           CHI 1 CHAN DATA (D2) cho ca 2 doan: data di ESP -> doan 1 -> doan 2.
  Cap nguon 2 dau (chong sut ap): 5V+GND cap o dau doan 1 VA o dau cuoi doan 2.
           GND cua 2 nguon + GND ESP phai noi chung. Cat day +5V o cho noi giua 2 doan
           (giu nguyen GND va DATA) de 2 nguon khong day nguoc vao nhau.

  Can chinh mau (can bang trang) rieng tung doan: tab "Day LED", moi doan co 3 thanh R/G/B (10%..100%).
    Bam "Trang" o doan do roi ha kenh mau bi lo cho toi khi trang deu giua 2 doan. 100% = khong chinh.
    Luu o vung EEPROM rieng (CAL_EEPROM_OFF), khong lam mat WiFi va cai dat da luu.
  Nut FLASH (GPIO0) tren NodeMCU: giu 2 giay = mo AP "LED-Setup" va GIU MO (khong tu dong dong).
    Noi dien thoai vao "LED-Setup" roi mo http://192.168.4.1 de dieu khien truc tiep, khong
    can qua router. Giu 2 giay lan nua = dong AP.
  Serial 115200: in MAC, IP, gateway, RSSI luc vao WiFi va moi 15 giay.
  TCP (de lam app): cong 7777, moi lenh 1 dong chu, tra loi 1 dong JSON. Xem PROTOCOL.md.
    Web server van chay binh thuong song song; TCP va web dung chung logic lenh.
    App tu tim thiet bi qua mDNS dich vu _ledctl._tcp.
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <DNSServer.h>
#include <EEPROM.h>
#include <Adafruit_NeoPixel.h>

// ====================== CAU HINH ======================
#define LED_PIN        4          // D2 = GPIO4
#define MAX_LEDS       500        // so LED toi da cho phep chon tren web
#define LED_POWER_PIN  -1         // chan dieu khien MOSFET/relay cat nguon LED (-1 = khong dung)
#define LED_POWER_ON   HIGH       // muc logic de cap nguon LED
#define AP_SSID        "LED-Setup"
#define AP_PASS        "12345678" // >= 8 ky tu; de "" neu muon AP mo
#define HOSTNAME       "ledctl"   // truy cap http://ledctl.local
#define AP_CLOSE_MS    60000UL    // AP tu dong sau khi ket noi thanh cong
#define BTN_PIN        0          // nut FLASH tren NodeMCU (GPIO0); -1 = khong dung
#define BTN_HOLD_MS    2000UL     // giu nut lau hon the nay thi mo/dong AP "LED-Setup"
#define AUTO_REFRESH_MS 4000UL    // gui lai khung tinh moi 4 giay: LED bi reset do sut nguon se tu sang lai
#define TCP_PORT       7777       // cong TCP cho app; 0 = tat TCP
#define TCP_TOKEN      ""         // mat khau TCP; de "" = khong can. Neu dat, app phai gui AUTH <mat khau> truoc
#define TCP_MAX        3          // so ket noi TCP dong thoi
#define TCP_IDLE_MS    120000UL   // ngat ket noi TCP neu 2 phut khong nhan duoc gi (app gui PING moi 30 giay)
#define TCP_PROTO      1          // phien ban giao thuc
// ======================================================

#define WMAGIC 0x4C454433UL       // vung WiFi (giu nguyen ma cu de khong mat WiFi da luu)
#define LMAGIC 0x4C454435UL       // vung cai dat LED
#define LED_EEPROM_OFF 128        // cai dat LED luu tu byte 128, WiFi o byte 0
#define CAL_EEPROM_OFF 192        // can chinh mau luu rieng tu byte 192 (khong dung chung vung cai dat LED)
#define CMAGIC 0x43414C31UL       // "CAL1"

// Thu tu phai khop MODES[] trong web_pages.h. Chi them o cuoi de so mode da luu khong bi lech.
enum { MODE_SOLID, MODE_BREATHE, MODE_RAINBOW, MODE_SPREAD, MODE_THEATER,
       MODE_COMET, MODE_TWINKLE, MODE_SCANNER, MODE_GRADIENT,
       MODE_SHIMMER, MODE_FIRE, MODE_CANDLE, MODE_WAVE, MODE_RAIN, MODE_FADE,
       MODE_STORM, MODE_POLICE, MODE_FIREWORK, MODE_AURORA, MODE_RGLITTER,
       MODE_RCHASE, MODE_HEART, MODE_WIPE, MODE_PLASMA, MODE_DISCO, MODE_DUAL,
       MODE_FIREFLY, MODE_BOUNCE, MODE_OCEAN, MODE_RIPPLE, MODE_TRAIN, MODE_SUNSET,
       MODE_SEGS, MODE_CONFETTI, MODE_JUGGLE, MODE_XMAS, MODE_FIREM, MODE_FIREC,
       MODE_BLOCKS, MODE_INTERF, MODE_TURN, MODE_EVENODD, MODE_STARS, MODE_COUNT };

// Thu tu byte gui ra day: PERM[order][k] = kenh logic (0=R,1=G,2=B) nam o byte thu k
// 0 RGB, 1 RBG, 2 GRB, 3 GBR, 4 BRG, 5 BGR
const uint8_t PERM[6][3] = {{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};

struct WifiCfg {                      // EEPROM byte 0
  uint32_t magic;
  char     ssid[33];
  char     pass[65];
};
struct Settings {                     // EEPROM byte LED_EEPROM_OFF
  uint32_t magic;
  uint8_t  on, bright, mode, speed;
  uint8_t  c1[3], c2[3];
  uint8_t  o1, o2, zoneOn, pad;
  uint16_t count, split, zs, ze;
};
struct Calib {                        // EEPROM byte CAL_EEPROM_OFF
  uint32_t magic;
  uint8_t  g[2][3];                   // he so mau [doan 1/2][R,G,B], 255 = 100% (khong chinh)
  uint8_t  pad[2];
};
static_assert(LED_EEPROM_OFF + sizeof(Settings) <= CAL_EEPROM_OFF, "Settings de len vung can chinh mau");
WifiCfg  W;
Settings S;
Calib    CL;

Adafruit_NeoPixel strip(MAX_LEDS, LED_PIN, NEO_RGB + NEO_KHZ800);
ESP8266WebServer  server(80);
DNSServer         dns;

static uint8_t buf[MAX_LEDS][3];   // bo dem mau logic (R,G,B)

// trang thai chay
bool     portalActive = false, bootConn = false, closeApReq = false;
bool     manualAp = false;                 // AP mo bang nut: khong tu dong dong
bool     dirty = true, countChanged = false, powerChanged = false;
enum { C_IDLE, C_CONNECTING, C_OK, C_FAIL } connState = C_IDLE;
bool     connectReq = false;
String   pendSsid, pendPass;
uint32_t connStart = 0, okAt = 0, bootStart = 0, saveAt = 0, lostAt = 0;
uint32_t lastFrame = 0, phaseAcc = 0;
uint16_t ph = 0;
uint8_t  testSeg = 0, testC = 0; bool testRaw = false; uint32_t testUntil = 0;
bool     refreshReq = false;               // yeu cau lam moi toan bo LED
uint8_t  resendFrames = 0;                 // gui lai khung hinh vai lan sau khi doi cau hinh
uint32_t lastPush = 0;                     // lan gui cuoi ra day LED

// ====================== TRANG WEB (PROGMEM) ======================
#include "web_pages.h"

// ====================== TIEN ICH ======================
static long clampL(long v, long lo, long hi) { return v < lo ? lo : (v > hi ? hi : v); }

static String jsonEsc(const String& s) {
  String o; o.reserve(s.length() + 4);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c >= 0x20) o += c;
  }
  return o;
}

static String hex6(const uint8_t* c) {
  char b[7]; snprintf(b, sizeof(b), "%02x%02x%02x", c[0], c[1], c[2]);
  return String(b);
}

static void setC(uint8_t* c, uint8_t r, uint8_t g, uint8_t b) { c[0] = r; c[1] = g; c[2] = b; }

// ====================== CAI DAT (EEPROM) ======================
void defaults() {
  memset(&S, 0, sizeof(S));
  S.magic = LMAGIC; S.on = 1; S.bright = 128; S.speed = 128; S.mode = MODE_SOLID;
  setC(S.c1, 255, 0, 0); setC(S.c2, 0, 0, 255);
  S.count = 146 < MAX_LEDS ? 146 : MAX_LEDS;   // tong 146 LED
  S.split = 49 < S.count ? 49 : S.count;       // doan 1 = LED 1..49, doan 2 = LED 50..146
  S.o1 = 0; S.o2 = 2;                  // RGB (WS2811) + GRB (WS2812) - chinh lai tren web
  S.zs = 1; S.ze = 10;
}

void defaultsCal() {
  memset(&CL, 0, sizeof(CL));
  CL.magic = CMAGIC;
  memset(CL.g, 255, sizeof(CL.g));             // 100% moi kenh = giu nguyen nhu cu
}

void sanitize() {
  S.count = clampL(S.count, 1, MAX_LEDS);
  S.split = clampL(S.split, 1, MAX_LEDS);      // giu gia tri nguoi dung chon; luc dung se cat theo count
  if (S.mode >= MODE_COUNT) S.mode = 0;
  if (S.o1 > 5) S.o1 = 0;
  if (S.o2 > 5) S.o2 = 0;
  S.bright = clampL(S.bright, 1, 255);
  S.speed  = clampL(S.speed, 1, 255);
  S.zs = clampL(S.zs, 1, MAX_LEDS);
  S.ze = clampL(S.ze, S.zs, MAX_LEDS);
  S.on = S.on ? 1 : 0; S.zoneOn = S.zoneOn ? 1 : 0;
}

// diem chia 2 doan dang dung that su (cat theo so LED hien tai, khong lam mat gia tri da luu)
static inline uint16_t effSplit() { return S.split < S.count ? S.split : S.count; }

void loadSettings() {
  EEPROM.get(0, W);
  if (W.magic != WMAGIC) { memset(&W, 0, sizeof(W)); W.magic = WMAGIC; }
  W.ssid[32] = 0; W.pass[64] = 0;
  EEPROM.get(LED_EEPROM_OFF, S);
  if (S.magic != LMAGIC) defaults();
  EEPROM.get(CAL_EEPROM_OFF, CL);
  if (CL.magic != CMAGIC) defaultsCal();
  sanitize();
}
void saveSettings() { EEPROM.put(LED_EEPROM_OFF, S); EEPROM.put(CAL_EEPROM_OFF, CL); EEPROM.commit(); }
void saveWifi()     { EEPROM.put(0, W); EEPROM.commit(); }

// ====================== HIEU UNG ======================
static inline void setPx(uint16_t i, const uint8_t* c) { buf[i][0] = c[0]; buf[i][1] = c[1]; buf[i][2] = c[2]; }
static void scaleC(const uint8_t* c, uint8_t v, uint8_t* o) {
  o[0] = ((uint16_t)c[0] * (v + 1)) >> 8;
  o[1] = ((uint16_t)c[1] * (v + 1)) >> 8;
  o[2] = ((uint16_t)c[2] * (v + 1)) >> 8;
}
static void wheel(uint8_t p, uint8_t* c) {
  if (p < 85)       { c[0] = 255 - p * 3; c[1] = p * 3;       c[2] = 0; }
  else if (p < 170) { p -= 85;  c[0] = 0; c[1] = 255 - p * 3; c[2] = p * 3; }
  else              { p -= 170; c[0] = p * 3; c[1] = 0;       c[2] = 255 - p * 3; }
}
static inline uint8_t tri8(uint8_t t) { return t < 128 ? (t << 1) : ((255 - t) << 1); }

static void fadeToBg(uint16_t lo, uint16_t hi, const uint8_t* bg, uint8_t shift) {
  for (uint16_t i = lo; i < hi; i++)
    for (uint8_t k = 0; k < 3; k++) {
      int v = buf[i][k], b = bg[k];
      if (v > b) { v -= ((v - b) >> shift) + 1; if (v < b) v = b; } else v = b;
      buf[i][k] = v;
    }
}

// ---- bang sin 8 bit (0..255 tuong ung 0..2*PI), tranh dung sinf() cho nhe CPU ----
static const uint8_t SIN8[256] = {
  128,131,134,137,140,143,146,149,152,155,158,162,165,167,170,173,
  176,179,182,185,188,190,193,196,198,201,203,206,208,211,213,215,
  218,220,222,224,226,228,230,232,234,235,237,238,240,241,243,244,
  245,246,248,249,250,250,251,252,253,253,254,254,254,255,255,255,
  255,255,255,255,254,254,254,253,253,252,251,250,250,249,248,246,
  245,244,243,241,240,238,237,235,234,232,230,228,226,224,222,220,
  218,215,213,211,208,206,203,201,198,196,193,190,188,185,182,179,
  176,173,170,167,165,162,158,155,152,149,146,143,140,137,134,131,
  128,124,121,118,115,112,109,106,103,100, 97, 93, 90, 88, 85, 82,
   79, 76, 73, 70, 67, 65, 62, 59, 57, 54, 52, 49, 47, 44, 42, 40,
   37, 35, 33, 31, 29, 27, 25, 23, 21, 20, 18, 17, 15, 14, 12, 11,
   10,  9,  7,  6,  5,  5,  4,  3,  2,  2,  1,  1,  1,  0,  0,  0,
    0,  0,  0,  0,  1,  1,  1,  2,  2,  3,  4,  5,  5,  6,  7,  9,
   10, 11, 12, 14, 15, 17, 18, 20, 21, 23, 25, 27, 29, 31, 33, 35,
   37, 40, 42, 44, 47, 49, 52, 54, 57, 59, 62, 65, 67, 70, 73, 76,
   79, 82, 85, 88, 90, 93, 97,100,103,106,109,112,115,118,121,124
};
static inline uint8_t sin8(uint8_t x) { return SIN8[x]; }
static inline uint8_t smooth8(uint8_t t) { return ((uint32_t)t * t * (765 - 2 * (uint32_t)t)) / 65025UL; }   // smoothstep 0..255
static inline uint8_t hash8(uint16_t i, uint16_t s) {
  uint32_t h = ((uint32_t)i * 2654435761UL) ^ ((uint32_t)s * 40503UL);
  h ^= h >> 15; h *= 2246822519UL; h ^= h >> 13;
  return h >> 24;
}
// nhiet do 0..255 -> mau lua (den, do, cam, vang, trang)
static void heatColor(uint8_t h, uint8_t* c) {
  uint8_t t192 = ((uint16_t)h * 191) >> 8;
  uint8_t ramp = (t192 & 0x3F) << 2;
  if (t192 & 0x80)      { c[0] = 255;  c[1] = 255;  c[2] = ramp; }
  else if (t192 & 0x40) { c[0] = 255;  c[1] = ramp; c[2] = 0; }
  else                  { c[0] = ramp; c[1] = 0;    c[2] = 0; }
}
static inline uint8_t qsub8(uint8_t a, uint8_t b) { return a > b ? a - b : 0; }
static inline uint8_t qadd8(uint8_t a, uint8_t b) { uint16_t s = a + b; return s > 255 ? 255 : s; }
// tron a -> b theo t (0..255)
static void mixC(const uint8_t* a, const uint8_t* b, uint8_t t, uint8_t* o) {
  for (uint8_t k = 0; k < 3; k++) o[k] = a[k] + (((int)b[k] - (int)a[k]) * t) / 255;
}
// lay gia tri lon hon theo tung kenh (de cac diem sang chong len nen ma khong lam toi nen)
static void maxPx(uint16_t i, const uint8_t* c) {
  for (uint8_t k = 0; k < 3; k++) if (c[k] > buf[i][k]) buf[i][k] = c[k];
}

// ---- trang thai cua cac hieu ung co "bo nho" (xoa khi doi mode) ----
#define SPARKS_MAX 24
#define DROPS_MAX  12
#define FW_MAX     3
struct Spark { uint16_t pos; uint8_t age, life, hue; };            // life = 0: o trong; hue: mau ngau nhien (Sao mau)
struct Drop  { uint16_t pos16; uint8_t spd; bool on; };            // pos16: vi tri x16, spd: do dai buoc moi khung (x16)
struct Burst { uint16_t pos; uint8_t r, rmax, hue; bool on; };
static Spark sparks[SPARKS_MAX];
static Drop  drops[DROPS_MAX];
static Burst bursts[FW_MAX];
static uint8_t heat[MAX_LEDS];                                      // lua / nen
#define BALLS 3
struct Ball  { int32_t pos, vel; bool init; };                     // pos, vel: x256 don vi LED
static Ball  balls[BALLS];
static int16_t jprev[4];                                            // vi tri truoc cua 4 cham Tung hung
static const uint8_t SUN[5][3] = {{70,0,130},{255,0,90},{255,90,0},{255,190,30},{70,0,130}};   // bang mau hoang hon (khep vong)
static uint8_t stormFlashes = 0, stormGap = 0;
static uint16_t stormPos = 0, stormLen = 0;

static void resetFx() {
  memset(buf, 0, sizeof(buf));
  memset(sparks, 0, sizeof(sparks));
  memset(drops, 0, sizeof(drops));
  memset(bursts, 0, sizeof(bursts));
  memset(heat, 0, sizeof(heat));
  memset(balls, 0, sizeof(balls));
  for (uint8_t d = 0; d < 4; d++) jprev[d] = -1;
  stormFlashes = 0; stormGap = 0;
}

void renderFrame() {
  const uint16_t n = S.count;
  uint16_t lo = 0, hi = n;
  if (S.zoneOn) {                                      // vung [lo, hi) tinh tu 0, cat theo so LED hien tai
    uint16_t zs = S.zs < n ? S.zs : n, ze = S.ze < n ? S.ze : n;
    if (ze < zs) ze = zs;
    lo = zs - 1; hi = ze;
  }
  for (uint16_t i = 0; i < n; i++)
    if (i < lo || i >= hi) { buf[i][0] = buf[i][1] = buf[i][2] = 0; }
  const uint16_t len = hi - lo;
  const uint8_t ph8 = ph >> 8;
  uint8_t c[3], bg[3];
  scaleC(S.c2, 40, bg);

  switch (S.mode) {
    case MODE_SOLID:
      for (uint16_t i = lo; i < hi; i++) setPx(i, S.c1);
      break;
    case MODE_BREATHE: {
      uint8_t t = tri8(ph8);
      uint8_t v = ((uint16_t)t * t) >> 8;
      scaleC(S.c1, v, c);
      for (uint16_t i = lo; i < hi; i++) setPx(i, c);
    } break;
    case MODE_RAINBOW:
      wheel(ph8, c);
      for (uint16_t i = lo; i < hi; i++) setPx(i, c);
      break;
    case MODE_SPREAD:
      for (uint16_t i = lo; i < hi; i++) {
        wheel((uint8_t)(((uint32_t)(i - lo) * 256 / len) + ph8), c);
        setPx(i, c);
      }
      break;
    case MODE_THEATER: {
      uint8_t sh = ph >> 12;
      for (uint16_t i = lo; i < hi; i++) setPx(i, (((i - lo) + sh) & 3) < 2 ? S.c1 : S.c2);
    } break;
    case MODE_COMET: {
      uint16_t tail = len / 4; if (tail < 4) tail = 4; if (tail > len) tail = len;
      uint16_t head = ((uint32_t)ph * len) >> 16;
      for (uint16_t k = 0; k < len; k++) {
        uint16_t d = (head + len - k) % len;
        if (d < tail) { scaleC(S.c1, 255 - (uint32_t)d * 255 / tail, c); setPx(lo + k, c); }
        else setPx(lo + k, bg);
      }
    } break;
    case MODE_TWINKLE:
    case MODE_STARS:
    case MODE_FIREFLY: {
      // Nen tat. Moi diem sang la dung 1 LED (mau 1), nhieu LED lech nhau.
      // Lap lanh: bat nhanh roi tat dan, day dac. Dom dom: it hon, len/tat cham va mem.
      const bool fly = (S.mode == MODE_FIREFLY), stars = (S.mode == MODE_STARS);
      static const uint8_t zero3[3] = {0, 0, 0};
      for (uint16_t i = lo; i < hi; i++) setPx(i, zero3);
      uint8_t maxActive = fly ? 1 + len / 14 : 2 + len / 6;
      if (maxActive > SPARKS_MAX) maxActive = SPARKS_MAX;
      uint8_t active = 0;
      for (uint8_t s = 0; s < SPARKS_MAX; s++) if (sparks[s].life) active++;
      if (active < maxActive && random(100) < (fly ? 6 + S.speed / 16 : 25 + S.speed / 3)) {
        for (uint8_t s = 0; s < SPARKS_MAX; s++) if (!sparks[s].life) {
          sparks[s].pos  = lo + random(len);
          sparks[s].age  = 0;
          sparks[s].hue  = random(256);
          sparks[s].life = fly ? 50 + random(60) + (255 - S.speed) / 3       // 50..194 khung (~2..8 giay)
                               : 12 + random(20) + (255 - S.speed) / 10;     // 12..57 khung (~0.5..2.3 giay)
          break;
        }
      }
      for (uint8_t s = 0; s < SPARKS_MAX; s++) {
        Spark& sp = sparks[s];
        if (!sp.life) continue;
        if (++sp.age >= sp.life) { sp.life = 0; continue; }
        uint8_t atk = sp.life / 4;                                     // lap lanh: 1/4 dau bat len, 3/4 sau tat dan
        uint8_t env;
        if (fly) env = smooth8(tri8((uint16_t)sp.age * 255 / sp.life));   // dom dom: len va tat doi xung, mem
        else     env = sp.age < atk ? (uint16_t)sp.age * 255 / atk
                                    : (uint16_t)(sp.life - sp.age) * 255 / (sp.life - atk);
        if (sp.pos >= lo && sp.pos < hi) {
          if (stars) { wheel(sp.hue, c); scaleC(c, env, c); }       // Sao mau: moi diem mot mau ngau nhien
          else       scaleC(S.c1, env, c);
          maxPx(sp.pos, c);
        }
      }
    } break;
    case MODE_SCANNER: {
      static int32_t lastPos = -1;
      fadeToBg(lo, hi, bg, 1);
      int32_t pos = len > 1 ? ((uint32_t)tri8(ph8) * (len - 1)) / 254 : 0;
      int32_t from = (lastPos >= 0 && lastPos < (int32_t)len) ? lastPos : pos;
      int32_t a = from < pos ? from : pos, b = from < pos ? pos : from;
      for (int32_t k = a; k <= b; k++) setPx(lo + k, S.c1);
      lastPos = pos;
    } break;
    case MODE_GRADIENT:
      for (uint16_t i = lo; i < hi; i++) {
        uint8_t t = tri8((uint8_t)(((uint32_t)(i - lo) * 256 / len) + ph8));
        for (uint8_t k = 0; k < 3; k++) c[k] = S.c1[k] + ((int)S.c2[k] - (int)S.c1[k]) * t / 255;
        setPx(i, c);
      }
      break;

    case MODE_SHIMMER: {                               // ca day sang nhe, tung LED long lanh lech pha
      uint8_t sb[3], sh[3];
      scaleC(S.c2, 56, sb); scaleC(S.c1, 210, sh);
      for (uint16_t i = lo; i < hi; i++) {
        uint8_t v = ((uint16_t)sin8(ph8 + hash8(i, 1)) + sin8((uint8_t)(ph8 * 2) + hash8(i, 2))) >> 1;
        mixC(sb, sh, v, c);
        setPx(i, c);
      }
    } break;

    case MODE_FIRE:
    case MODE_FIREM:
    case MODE_FIREC: {                                 // lua: nong o dau day (LED 1) bay len; Lua hai dau: nong o ca hai dau; Lua mau: mau 1 -> mau 2
      const bool mirror = (S.mode == MODE_FIREM), colored = (S.mode == MODE_FIREC);
      const uint16_t L = mirror ? (len + 1) / 2 : len;
      uint16_t coolMax = 550 / L + 2; if (coolMax > 60) coolMax = 60;
      for (uint16_t i = 0; i < L; i++) heat[i] = qsub8(heat[i], random(0, coolMax));
      for (int32_t k = (int32_t)L - 1; k >= 2; k--) heat[k] = ((uint16_t)heat[k - 1] + heat[k - 2] + heat[k - 2]) / 3;
      if (random(255) < 70 + S.speed / 2) {
        uint16_t y = random(L < 7 ? L : 7);
        heat[y] = qadd8(heat[y], 160 + random(95));
      }
      for (uint16_t i = 0; i < len; i++) {
        uint8_t h = heat[(mirror && i >= L) ? len - 1 - i : i];
        if (colored) { if (h < 128) scaleC(S.c1, h * 2, c); else mixC(S.c1, S.c2, (h - 128) * 2, c); }
        else heatColor(h, c);
        setPx(lo + i, c);
      }
    } break;

    case MODE_CANDLE: {                                // nen: chap chon chung, mau 1
      uint8_t g = 110 + random(146);                   // chap chon chung ca day
      for (uint16_t i = 0; i < len; i++) {
        uint8_t own = 80 + random(176);
        uint8_t tgt = ((uint16_t)g * 3 + own) >> 2;
        heat[i] = heat[i] + ((int)tgt - (int)heat[i]) / 3;
        scaleC(S.c1, heat[i], c);
        setPx(lo + i, c);
      }
    } break;

    case MODE_WAVE: {                                  // song sang chay doc day
      uint8_t wb[3]; scaleC(S.c2, 40, wb);
      for (uint16_t i = lo; i < hi; i++) {
        uint8_t v = sin8((uint8_t)((i - lo) * 6 + ph8));
        v = ((uint16_t)v * v) >> 8;                    // nhon dinh song
        mixC(wb, S.c1, v, c);
        setPx(i, c);
      }
    } break;

    case MODE_RAIN: {                                  // nhieu giot sang chay tu LED dau den LED cuoi
      fadeToBg(lo, hi, bg, 2);
      if (random(100) < 4 + S.speed / 16) {
        for (uint8_t s = 0; s < DROPS_MAX; s++) if (!drops[s].on) {
          drops[s].on = true; drops[s].pos16 = 0; drops[s].spd = 10 + random(14) + S.speed / 12;
          break;
        }
      }
      for (uint8_t s = 0; s < DROPS_MAX; s++) {
        Drop& d = drops[s];
        if (!d.on) continue;
        uint16_t oldp = d.pos16 >> 4;
        d.pos16 += d.spd;
        uint16_t newp = d.pos16 >> 4;
        for (uint16_t k = oldp; k <= newp && k < len; k++) setPx(lo + k, S.c1);
        if (newp >= len) d.on = false;
      }
    } break;

    case MODE_FADE:                                    // doi mau muot giua mau 1 va mau 2
      mixC(S.c1, S.c2, smooth8(tri8(ph8)), c);
      for (uint16_t i = lo; i < hi; i++) setPx(i, c);
      break;

    case MODE_STORM: {                                 // bao: nen toi (mau 2), thinh thoang chop sang mot doan (mau 1)
      uint8_t sb[3]; scaleC(S.c2, 30, sb);
      fadeToBg(lo, hi, sb, 2);
      if (stormFlashes) {
        if (stormGap) stormGap--;
        else {
          for (uint16_t k = stormPos; k < stormPos + stormLen && k < len; k++) setPx(lo + k, S.c1);
          stormFlashes--; stormGap = 2 + random(5);
        }
      } else if (random(1000) < 8 + S.speed / 8) {
        stormFlashes = 1 + random(3);
        stormGap = 0;
        stormLen = len / 3 + random(len / 2 + 1);
        if (stormLen > len) stormLen = len;
        if (stormLen < 1) stormLen = 1;
        stormPos = random(len - stormLen + 1);
      }
    } break;

    case MODE_POLICE: {                                // nua dau nhay mau 1, nua sau nhay mau 2, luan phien
      const uint8_t blk[3] = {0, 0, 0};
      uint8_t step = ph >> 12;                         // 0..15
      bool a = (step == 0 || step == 2), b = (step == 8 || step == 10);
      uint16_t mid = lo + len / 2;
      for (uint16_t i = lo; i < hi; i++) {
        if (i < mid) setPx(i, a ? S.c1 : blk);
        else         setPx(i, b ? S.c2 : blk);
      }
    } break;

    case MODE_FIREWORK: {                              // phao hoa: no vong sang mau ngau nhien roi tat dan
      const uint8_t dark[3] = {0, 0, 0}, wht[3] = {255, 255, 255};
      fadeToBg(lo, hi, dark, 2);
      if (random(100) < 3 + S.speed / 20) {
        for (uint8_t s = 0; s < FW_MAX; s++) if (!bursts[s].on) {
          Burst& b = bursts[s];
          b.on = true; b.r = 0; b.hue = random(256);
          b.pos = lo + random(len);
          b.rmax = 5 + random(10); if (b.rmax > len / 2) b.rmax = len / 2;
          break;
        }
      }
      for (uint8_t s = 0; s < FW_MAX; s++) {
        Burst& b = bursts[s];
        if (!b.on) continue;
        if (b.r == 0) { if (b.pos >= lo && b.pos < hi) setPx(b.pos, wht); }
        else {
          wheel(b.hue, c);
          int l = (int)b.pos - b.r, r = (int)b.pos + b.r;
          if (l >= (int)lo && l < (int)hi) setPx(l, c);
          if (r >= (int)lo && r < (int)hi) setPx(r, c);
        }
        if (++b.r > b.rmax) b.on = false;
      }
    } break;

    case MODE_AURORA:                                  // cuc quang: hai lop song cham nguoc chieu, tron mau 1 - mau 2
      for (uint16_t i = lo; i < hi; i++) {
        uint8_t t1 = sin8((uint8_t)((i - lo) * 6 + ph8));
        uint8_t t2 = sin8((uint8_t)((i - lo) * 4 - ph8));
        mixC(S.c1, S.c2, t1, c);
        scaleC(c, 70 + (((uint16_t)t2 * 185) >> 8), c);
        setPx(i, c);
      }
      break;

    case MODE_RGLITTER: {                              // cau vong dim + vai diem sao trang nhe
      for (uint16_t i = lo; i < hi; i++) {
        wheel((uint8_t)(((uint32_t)(i - lo) * 256 / len) + ph8), c);
        scaleC(c, 150, c);
        setPx(i, c);
      }
      const uint8_t w1[3] = {230, 230, 230}, w2[3] = {110, 110, 110};
      for (uint16_t s = 0; s < 1 + len / 60u; s++) {
        if (random(100) >= 30 + S.speed / 3) continue;
        int p = lo + random(len);
        maxPx(p, w1);
        if (p - 1 >= (int)lo) maxPx(p - 1, w2);
        if (p + 1 < (int)hi)  maxPx(p + 1, w2);
      }
    } break;

    case MODE_RCHASE: {                                // cau vong chay theo kieu den rap hat (cu 3 LED sang 1)
      const uint8_t blk[3] = {0, 0, 0};
      uint8_t sh = ((uint32_t)ph * 12) >> 16;          // 0..11
      for (uint16_t i = lo; i < hi; i++) {
        if (((i - lo) + sh) % 3 == 0) {
          wheel((uint8_t)(((uint32_t)(i - lo) * 256 / len) + ph8), c);
          setPx(i, c);
        } else setPx(i, blk);
      }
    } break;

    case MODE_WIPE: {                                  // phu mau: mau 1 phu dan len mau 2, roi mau 2 phu lai
      uint16_t p = ((uint32_t)ph * 2 * len) >> 16;     // 0 .. 2*len-1
      bool second = p >= len;
      uint16_t cut = second ? p - len : p;
      const uint8_t* a = second ? S.c2 : S.c1;         // mau dang phu len
      const uint8_t* b = second ? S.c1 : S.c2;         // mau cu
      for (uint16_t i = lo; i < hi; i++) setPx(i, (i - lo) < cut ? a : b);
    } break;

    case MODE_PLASMA:                                  // plasma: ba lop song cong lai thanh mau chay muot
      for (uint16_t i = lo; i < hi; i++) {
        int x = i - lo;
        uint16_t sum = sin8((uint8_t)(x * 5 + ph8)) + sin8((uint8_t)(x * 3 - ph8 * 2)) + sin8((uint8_t)(x * 2 + ph8 * 3));
        wheel((uint8_t)(sum / 3 * 2 + ph8), c);
        setPx(i, c);
      }
      break;

    case MODE_DISCO: {                                 // moi buoc moi LED doi mau ngau nhien (hoac tat)
      const uint8_t blk[3] = {0, 0, 0};
      uint8_t step = ph >> 12;                         // 0..15
      for (uint16_t i = lo; i < hi; i++) {
        if (hash8(i, step + 99) > 90) { wheel(hash8(i, step), c); setPx(i, c); }
        else setPx(i, blk);
      }
    } break;

    case MODE_DUAL: {                                  // hai sao bang chay nguoc chieu: mau 1 xuoi, mau 2 nguoc
      const uint8_t blk[3] = {0, 0, 0};
      uint16_t tail = len / 5; if (tail < 4) tail = 4; if (tail > len) tail = len;
      uint16_t head1 = ((uint32_t)ph * len) >> 16;
      uint16_t head2 = len - 1 - head1;
      for (uint16_t k = 0; k < len; k++) {
        setPx(lo + k, blk);
        uint16_t d1 = (head1 + len - k) % len;         // dang sau sao bang 1 (chay len)
        uint16_t d2 = (k + len - head2) % len;         // dang sau sao bang 2 (chay xuong)
        if (d1 < tail) { scaleC(S.c1, 255 - (uint32_t)d1 * 255 / tail, c); maxPx(lo + k, c); }
        if (d2 < tail) { scaleC(S.c2, 255 - (uint32_t)d2 * 255 / tail, c); maxPx(lo + k, c); }
      }
    } break;

    case MODE_BOUNCE: {                                // 3 qua bong nay tren day: mau 1, mau 2, va mau tron
      const uint8_t blk[3] = {0, 0, 0};
      uint8_t mid[3]; mixC(S.c1, S.c2, 128, mid);
      const uint8_t* bc[BALLS] = {S.c1, S.c2, mid};
      fadeToBg(lo, hi, blk, 1);
      int32_t H = ((int32_t)len - 1) * 256; if (H < 256) H = 256;
      for (uint8_t b = 0; b < BALLS; b++) {
        Ball& bl = balls[b];
        int32_t T = (90 - S.speed / 4) * (100 - 14 * b) / 100;     // so khung bay len toi dinh: nhanh khi toc do cao
        if (T < 12) T = 12;
        int32_t g = 2 * H / (T * T); if (g < 1) g = 1;              // trong luc, de bong cham dung dinh day
        if (!bl.init) { bl.init = true; bl.pos = 0; bl.vel = g * T; }
        bl.vel -= g; bl.pos += bl.vel;
        if (bl.pos <= 0) {
          bl.pos = 0;
          bl.vel = -bl.vel * 7 / 8;                                 // nay lai, mat bot nang luong
          if (bl.vel < g * 4) bl.vel = g * T;                       // nay het da -> tha lai tu dau
        }
        if (bl.pos > H) { bl.pos = H; if (bl.vel > 0) bl.vel = 0; }
        uint16_t idx = lo + (uint16_t)((bl.pos + 128) >> 8);
        if (idx >= hi) idx = hi - 1;
        maxPx(idx, bc[b]);
      }
    } break;

    case MODE_OCEAN:                                   // dai duong: xanh la - xanh bien, song sang toi chay cham
      for (uint16_t i = lo; i < hi; i++) {
        int x = i - lo;
        uint8_t a  = sin8((uint8_t)(x * 4 + ph8));
        uint8_t b2 = sin8((uint8_t)(x * 7 - ph8 * 2));
        wheel(105 + (((uint16_t)a * 65) >> 8), c);
        scaleC(c, 80 + (((uint16_t)b2 * 175) >> 8), c);
        setPx(i, c);
      }
      break;

    case MODE_RIPPLE: {                                // gon song lan toa tu giua day ra hai dau
      uint8_t rb[3]; scaleC(S.c2, 30, rb);
      int center = lo + len / 2;
      for (uint16_t i = lo; i < hi; i++) {
        int d = (int)i - center; if (d < 0) d = -d;
        uint8_t v = sin8((uint8_t)(d * 8 - ph8));
        v = ((uint16_t)v * v) >> 8; v = ((uint16_t)v * v) >> 8;   // mong dinh song
        mixC(rb, S.c1, v, c);
        setPx(i, c);
      }
    } break;

    case MODE_TRAIN: {                                 // doan tau: moi 24 LED co 1 toa 6 LED (than mau 1, dau mau 2)
      const uint8_t blk[3] = {0, 0, 0};
      uint16_t sh = ((uint32_t)ph * 24) >> 16;         // 0..23
      for (uint16_t i = lo; i < hi; i++) {
        uint16_t r = ((i - lo) + 24 - sh) % 24;
        if (r == 5)      setPx(i, S.c2);
        else if (r < 5)  setPx(i, S.c1);
        else             setPx(i, blk);
      }
    } break;

    case MODE_SUNSET:                                  // hoang hon: tim - hong - cam - vang truot nhe theo day
      for (uint16_t i = lo; i < hi; i++) {
        uint8_t t = (uint8_t)(((uint32_t)(i - lo) * 256 / len) + ph8);
        mixC(SUN[t >> 6], SUN[(t >> 6) + 1], (t & 63) * 4, c);
        setPx(i, c);
      }
      break;

    case MODE_SEGS: {                                  // doan 1 mau 1, doan 2 mau 2, tho luan phien nhau
      uint16_t sp = effSplit();
      uint8_t v1 = smooth8(tri8(ph8)), v2 = 255 - v1;
      uint8_t a[3], b[3];
      scaleC(S.c1, 40 + (((uint16_t)v1 * 215) >> 8), a);
      scaleC(S.c2, 40 + (((uint16_t)v2 * 215) >> 8), b);
      for (uint16_t i = lo; i < hi; i++) setPx(i, i < sp ? a : b);
    } break;

    case MODE_CONFETTI: {                              // hoa giay: cham mau ngau nhien bat len roi tat dan
      const uint8_t blk[3] = {0, 0, 0};
      fadeToBg(lo, hi, blk, 3);
      for (uint8_t k = 0; k < 1 + len / 70u; k++)
        if (random(100) < 20 + S.speed / 5) { wheel(random(256), c); setPx(lo + random(len), c); }
    } break;

    case MODE_JUGGLE: {                                // tung hung: 4 cham mau luon qua lai voi nhip khac nhau, de vet mo
      static const uint8_t JM[4] = {1, 2, 3, 1};
      const uint8_t blk[3] = {0, 0, 0};
      fadeToBg(lo, hi, blk, 2);
      for (uint8_t d = 0; d < 4; d++) {
        int16_t pos = ((uint32_t)sin8((uint8_t)(ph8 * JM[d] + d * 64)) * (len - 1)) / 255;
        int16_t from = (jprev[d] >= 0 && jprev[d] < (int16_t)len) ? jprev[d] : pos;
        int16_t a = from < pos ? from : pos, b = from < pos ? pos : from;
        wheel(d * 64, c);
        for (int16_t k = a; k <= b; k++) maxPx(lo + k, c);
        jprev[d] = pos;
      }
    } break;

    case MODE_XMAS: {                                  // den Noel: cu 3 LED mot bo mau 1 / mau 2 / trang am, tho lech pha
      const uint8_t warm[3] = {255, 170, 60};
      const uint8_t* xc[3] = {S.c1, S.c2, warm};
      for (uint16_t i = lo; i < hi; i++) {
        uint8_t g = (i - lo) % 3;
        scaleC(xc[g], 70 + (((uint16_t)sin8((uint8_t)(ph8 + g * 85)) * 185) >> 8), c);
        setPx(i, c);
      }
    } break;

    case MODE_BLOCKS: {                                // khoi mau: cu 7 LED mot khoi, moi buoc moi khoi troi sang mau moi
      uint8_t step = ph >> 13;                         // 0..7
      uint8_t t = smooth8((ph & 0x1FFF) >> 5);
      uint8_t ca[3], cb[3];
      for (uint16_t i = lo; i < hi; i++) {
        uint16_t bl = (i - lo) / 7;
        wheel(hash8(bl, step), ca);
        wheel(hash8(bl, (step + 1) & 7), cb);
        mixC(ca, cb, t, c);
        setPx(i, c);
      }
    } break;

    case MODE_INTERF: {                                // giao thoa: song mau 1 chay xuoi, song mau 2 chay nguoc, gap nhau o giua
      uint8_t a[3], b[3];
      for (uint16_t i = lo; i < hi; i++) {
        int x = i - lo;
        uint8_t va = sin8((uint8_t)(x * 5 + ph8));
        uint8_t vb = sin8((uint8_t)(((int)len - 1 - x) * 5 + ph8));
        va = ((uint16_t)va * va) >> 8; vb = ((uint16_t)vb * vb) >> 8;
        scaleC(S.c1, va, a); scaleC(S.c2, vb, b);
        setPx(i, a); maxPx(i, b);
      }
    } break;

    case MODE_TURN: {                                  // xi-nhan: sang dan tung nac tu LED dau den cuoi roi tat het
      const uint8_t blk[3] = {0, 0, 0};
      uint8_t st = ((uint32_t)ph * 11) >> 16;          // 0..10
      uint16_t cnt = st <= 8 ? (uint32_t)(st < 7 ? st + 1 : 8) * len / 8 : 0;
      for (uint16_t i = lo; i < hi; i++) setPx(i, (i - lo) < cnt ? S.c1 : blk);
    } break;

    case MODE_EVENODD: {                               // chan le: LED chan va le doi cho mau 1 <-> mau 2 muot ma
      uint8_t v = smooth8(tri8(ph8)), a[3], b[3];
      mixC(S.c1, S.c2, v, a); mixC(S.c2, S.c1, v, b);
      for (uint16_t i = lo; i < hi; i++) setPx(i, ((i - lo) & 1) ? b : a);
    } break;

    case MODE_HEART: {                                 // nhip tim: hai nhip thinh thich roi nghi
      uint8_t hb[3]; scaleC(S.c2, 25, hb);
      uint8_t u = ph8, lvl = 0;
      if (u < 48)                  lvl = tri8((uint16_t)u * 255 / 47);
      else if (u >= 64 && u < 128) lvl = ((uint16_t)tri8((uint16_t)(u - 64) * 255 / 63) * 180) >> 8;
      mixC(hb, S.c1, lvl, c);
      for (uint16_t i = lo; i < hi; i++) setPx(i, c);
    } break;
  }
}

// Gui buf ra day, ap dung do sang + can chinh mau + thu tu mau rieng tung doan
void pushFrame() {
  const uint16_t n = S.count, split = effSplit();
  const uint16_t br = (uint16_t)S.bright + 1;
  const uint8_t *p1 = PERM[S.o1], *p2 = PERM[S.o2];
  uint16_t m1[3], m2[3];                       // do sang x he so can chinh, theo kenh logic R,G,B (toi da 256)
  for (uint8_t k = 0; k < 3; k++) {
    m1[k] = ((uint32_t)br * (CL.g[0][k] + 1)) >> 8;
    m2[k] = ((uint32_t)br * (CL.g[1][k] + 1)) >> 8;
  }
  for (uint16_t i = 0; i < n; i++) {
    const bool a = i < split;
    const uint8_t* P = a ? p1 : p2;
    const uint16_t* M = a ? m1 : m2;
    const uint8_t* L = buf[i];
    strip.setPixelColor(i, (L[P[0]] * M[P[0]]) >> 8, (L[P[1]] * M[P[1]]) >> 8, (L[P[2]] * M[P[2]]) >> 8);
  }
  strip.show();
  lastPush = millis();
}

// Che do thu mau: chi sang 1 doan. raw=1: sang dung byte thu testC (dung de do thu tu)
void pushTest() {
  const uint16_t split = effSplit();
  for (uint16_t i = 0; i < S.count; i++) {
    uint8_t seg = i < split ? 1 : 2;
    uint8_t b[3] = {0, 0, 0};
    if (seg == testSeg) {
      if (testRaw) { if (testC < 3) b[testC] = 160; }
      else {
        uint8_t L[3] = {0, 0, 0};
        if (testC == 3) { L[0] = L[1] = L[2] = 160; } else if (testC < 3) L[testC] = 160;
        const uint8_t* P = PERM[seg == 1 ? S.o1 : S.o2];
        const uint8_t* G = CL.g[seg - 1];                      // thu mau co ap dung can chinh
        for (uint8_t k = 0; k < 3; k++) b[k] = (L[P[k]] * (G[P[k]] + 1)) >> 8;
      }
    }
    strip.setPixelColor(i, b[0], b[1], b[2]);
  }
  strip.show();
  lastPush = millis();
}

void applyCount() {
  strip.clear(); strip.show();               // tat het o do dai cu
  delay(2);
  strip.updateLength(S.count);
  if (strip.numPixels() != S.count) strip.updateLength(S.count);   // thu lai neu cap phat bo nho that bai
  memset(buf, 0, sizeof(buf));
  resendFrames = 3;
  dirty = true;
}

// Lam moi toan bo LED: dat lai chan data, xoa het, roi gui lai khung hinh vai lan
void doRefresh() {
  strip.begin();
  strip.updateLength(S.count);
  strip.clear(); strip.show();
  delay(2);
  memset(buf, 0, sizeof(buf));
  resendFrames = 3;
  dirty = true;
}

void applyPower() {
  if (S.on) {
    if (LED_POWER_PIN >= 0) digitalWrite(LED_POWER_PIN, LED_POWER_ON);
    if (!portalActive) WiFi.setSleepMode(WIFI_NONE_SLEEP);
    dirty = true;
  } else {
    strip.clear(); strip.show();
    if (LED_POWER_PIN >= 0) digitalWrite(LED_POWER_PIN, !LED_POWER_ON);
    // Tat den KHONG co nghia la ngu: WiFi luon bat het cong suat de luon online 100%
    if (!portalActive) WiFi.setSleepMode(WIFI_NONE_SLEEP);
  }
}

void ledTask() {
  uint32_t now = millis();
  if (countChanged) { countChanged = false; applyCount(); }
  if (powerChanged) { powerChanged = false; applyPower(); }
  if (refreshReq)   { refreshReq = false; doRefresh(); }

  bool testing = testSeg && (int32_t)(testUntil - now) > 0;
  if (testSeg && !testing) { testSeg = 0; dirty = true; }

  if (!S.on) {                                    // dang ngu: giu LED toi
    if (dirty) { strip.clear(); strip.show(); dirty = false; }
    return;
  }
  if (!dirty && !testing && S.mode == MODE_SOLID && now - lastPush > AUTO_REFRESH_MS) dirty = true;   // gui lai dinh ky
  if (!dirty && (testing || S.mode == MODE_SOLID)) return;   // tinh: khong can gui lai
  if (now - lastFrame < 25) return;                          // ~40 khung/giay
  uint32_t dt = now - lastFrame; lastFrame = now; if (dt > 100) dt = 100;

  if (testing) { pushTest(); dirty = false; return; }
  phaseAcc += dt * (20 + (uint32_t)S.speed * 8);
  ph = (phaseAcc >> 4) & 0xFFFF;
  renderFrame();
  pushFrame();
  dirty = false;
  if (resendFrames) { resendFrames--; dirty = true; }       // gui them vai khung de chac chan LED nhan du
}

// ====================== WIFI / PORTAL ======================
void startPortal() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  if (strlen(AP_PASS) >= 8) WiFi.softAP(AP_SSID, AP_PASS); else WiFi.softAP(AP_SSID);
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", IPAddress(192, 168, 4, 1));
  portalActive = true; connState = C_IDLE; closeApReq = false;
  WiFi.scanDelete(); WiFi.scanNetworks(true);
  Serial.println(F("Portal: noi WiFi " AP_SSID " roi mo 192.168.4.1"));
}

void closePortal() {
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  portalActive = false; closeApReq = false; manualAp = false;
  applyPower();
  Serial.println(F("Da dong AP"));
}

void onConnected() {
  Serial.printf("WiFi OK  MAC=%s  IP=%s  GW=%s  RSSI=%d\n", WiFi.macAddress().c_str(),
                WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(), (int)WiFi.RSSI());
  MDNS.begin(HOSTNAME);
  MDNS.addService("http", "tcp", 80);
#if TCP_PORT
  MDNS.addService("ledctl", "tcp", TCP_PORT);          // app tim thiet bi: _ledctl._tcp
  MDNS.addServiceTxt("ledctl", "tcp", "proto", "1");           // = TCP_PROTO
#endif
  applyPower();
}

void wifiTask() {
  static bool was = false;

  if (connectReq) {
    connectReq = false;
    WiFi.disconnect();
    WiFi.begin(pendSsid.c_str(), pendPass.c_str());
    connStart = millis();
  }

  wl_status_t st = WiFi.status();
  bool conn = (st == WL_CONNECTED);
  if (conn && !was) onConnected();
  was = conn;

  if (connState == C_CONNECTING) {
    uint32_t el = millis() - connStart;
    if (conn && el > 500 && WiFi.SSID() == pendSsid) {
      connState = C_OK; okAt = millis();
      strlcpy(W.ssid, pendSsid.c_str(), sizeof(W.ssid));
      strlcpy(W.pass, pendPass.c_str(), sizeof(W.pass));
      saveWifi();
    } else if ((el > 1500 && (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL)) || el > 20000) {
      connState = C_FAIL; WiFi.disconnect();
    }
  }

  // ESP tu noi lai router khi portal dang mo -> coi nhu xong, AP se tu dong
  if (portalActive && connState == C_IDLE && conn) { connState = C_OK; okAt = millis(); }

  if (portalActive && connState == C_OK && (closeApReq || (!manualAp && millis() - okAt > AP_CLOSE_MS))) closePortal();

  if (bootConn) {
    if (conn) bootConn = false;
    else if (millis() - bootStart > 15000) { bootConn = false; startPortal(); }
  }

  // mat WiFi qua lau -> mo lai AP de cau hinh
  if (!portalActive && !bootConn) {
    if (conn) lostAt = 0;
    else if (!lostAt) lostAt = millis();
    else if (millis() - lostAt > 90000) { startPortal(); lostAt = 0; }
  }
}

// ====================== NUT / CHAN DOAN ======================
// Giu nut FLASH >= BTN_HOLD_MS: mo AP va giu mo; giu lan nua: dong AP (chi khi da co WiFi nha)
void buttonTask() {
  if (BTN_PIN < 0) return;
  static uint32_t downAt = 0;
  static bool fired = false;
  if (digitalRead(BTN_PIN) != LOW) { downAt = 0; fired = false; return; }
  if (!downAt) { downAt = millis() | 1; return; }
  if (fired || millis() - downAt < BTN_HOLD_MS) return;
  fired = true;
  if (!portalActive)  { manualAp = true; startPortal(); }
  else if (!manualAp) { manualAp = true; }                           // dang dem nguoc dong AP -> giu mo
  else if (WiFi.status() == WL_CONNECTED) closePortal();
}

void netReport() {
  static uint32_t last = 0;
  if (millis() - last < 15000) return;
  last = millis();
  Serial.printf("[net] wifi=%d ip=%s gw=%s rssi=%d ap=%d heap=%u\n", (int)WiFi.status(),
                WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(),
                (int)WiFi.RSSI(), portalActive ? 1 : 0, (unsigned)ESP.getFreeHeap());
}

// ====================== HTTP ======================
void sendJson(const String& s) { server.send(200, "application/json", s); }

void handleRoot() {
  bool fromAp = portalActive && server.client().localIP() == WiFi.softAPIP();
  if (fromAp && !manualAp)                                   // AP cai dat WiFi
    server.send_P(200, "text/html; charset=utf-8", PAGE_PORTAL);
  else                                                       // WiFi nha, hoac AP mo bang nut -> trang dieu khien
    server.send_P(200, "text/html; charset=utf-8", PAGE_APP);
}

void handleScan() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) { sendJson(F("{\"scanning\":true}")); return; }
  if (n < 0) { WiFi.scanNetworks(true); sendJson(F("{\"scanning\":true}")); return; }
  String s = F("{\"scanning\":false,\"list\":[");
  bool first = true;
  for (int i = 0; i < n; i++) {
    String ss = WiFi.SSID(i);
    if (!ss.length()) continue;
    bool dup = false;                                  // gop SSID trung, giu song manh nhat
    for (int j = 0; j < n; j++) {
      if (j == i || WiFi.SSID(j) != ss) continue;
      if (WiFi.RSSI(j) > WiFi.RSSI(i) || (WiFi.RSSI(j) == WiFi.RSSI(i) && j < i)) { dup = true; break; }
    }
    if (dup) continue;
    if (!first) s += ','; first = false;
    s += F("{\"s\":\""); s += jsonEsc(ss);
    s += F("\",\"r\":");  s += WiFi.RSSI(i);
    s += F(",\"e\":");    s += (WiFi.encryptionType(i) != ENC_TYPE_NONE) ? 1 : 0;
    s += '}';
  }
  s += F("]}");
  WiFi.scanDelete();
  sendJson(s);
}

void handleConnect() {
  if (!server.hasArg("ssid") || !server.arg("ssid").length()) { server.send(400, "text/plain", "ssid"); return; }
  pendSsid = server.arg("ssid"); pendPass = server.arg("pass");
  connectReq = true; connState = C_CONNECTING;
  sendJson(F("{\"ok\":1}"));
}

void handleStatus() {
  const char* st = connState == C_IDLE ? "idle" : connState == C_CONNECTING ? "connecting" : connState == C_OK ? "ok" : "fail";
  long left = 0;
  if (connState == C_OK && portalActive) {
    uint32_t el = millis() - okAt;
    left = el < AP_CLOSE_MS ? (AP_CLOSE_MS - el) / 1000 : 0;
  }
  String s; s.reserve(160);
  s += F("{\"state\":\""); s += st;
  s += F("\",\"ip\":\"");  s += WiFi.localIP().toString();
  s += F("\",\"host\":\"" HOSTNAME ".local\",\"left\":"); s += left;
  s += '}';
  sendJson(s);
}

void handleCloseAp() { closeApReq = true; sendJson(F("{\"ok\":1}")); }

String stateJson() {
  String s; s.reserve(520);
  s += F("{\"on\":");     s += S.on;
  s += F(",\"n\":");      s += S.count;
  s += F(",\"max\":");    s += MAX_LEDS;
  s += F(",\"bright\":"); s += S.bright;
  s += F(",\"mode\":");   s += S.mode;
  s += F(",\"speed\":");  s += S.speed;
  s += F(",\"c1\":\"");   s += hex6(S.c1);
  s += F("\",\"c2\":\""); s += hex6(S.c2);
  s += F("\",\"split\":"); s += S.split;
  s += F(",\"o1\":");     s += S.o1;
  s += F(",\"o2\":");     s += S.o2;
  s += F(",\"g1\":\"");   s += hex6(CL.g[0]);
  s += F("\",\"g2\":\""); s += hex6(CL.g[1]);
  s += '"';
  s += F(",\"zon\":");    s += S.zoneOn;
  s += F(",\"zs\":");     s += S.zs;
  s += F(",\"ze\":");     s += S.ze;
  s += F(",\"rssi\":");   s += WiFi.RSSI();
  s += F(",\"ip\":\"");   s += (WiFi.status() == WL_CONNECTED ? WiFi.localIP() : WiFi.softAPIP()).toString();
  s += F("\",\"up\":");  s += millis() / 1000;
  s += F(",\"heap\":");  s += ESP.getFreeHeap();
  s += F(",\"rst\":\""); s += jsonEsc(ESP.getResetReason());
  s += F("\"}");
  return s;
}

// Nguon tham so dung chung: HTTP (server.arg) hoac dong lenh TCP (k=v). Cung mot ham xu ly cho ca hai.
#define TCP_ARGS_MAX 12
static String  tcpK[TCP_ARGS_MAX], tcpV[TCP_ARGS_MAX];
static uint8_t tcpN = 0;
static bool    fromTcp = false;
static uint32_t stateVer = 1;                 // tang moi khi cai dat doi: de day su kien cho app dang SUB
static bool hasArg(const char* k) {
  if (!fromTcp) return server.hasArg(k);
  for (uint8_t i = 0; i < tcpN; i++) if (tcpK[i] == k) return true;
  return false;
}
static String argStr(const char* k) {
  if (!fromTcp) return server.arg(k);
  for (uint8_t i = 0; i < tcpN; i++) if (tcpK[i] == k) return tcpV[i];
  return String();
}
static bool argL(const char* k, long mn, long mx, long& out) {
  if (!hasArg(k)) return false;
  out = clampL(argStr(k).toInt(), mn, mx);
  return true;
}
static void argColor(const char* k, uint8_t* c) {
  if (!hasArg(k)) return;
  String h = argStr(k);
  if (h.length() != 6) return;
  uint32_t v = strtoul(h.c_str(), nullptr, 16);
  c[0] = v >> 16; c[1] = v >> 8; c[2] = v;
}

void handleState() { sendJson(stateJson()); }

String doSet() {
  long v;
  if (argL("on", 0, 1, v))          { if (S.on != v) { S.on = v; powerChanged = true; } }
  if (argL("bright", 1, 255, v))    S.bright = v;
  if (argL("mode", 0, MODE_COUNT - 1, v)) { if (S.mode != v) { S.mode = v; resetFx(); } }
  if (argL("speed", 1, 255, v))     S.speed = v;
  argColor("c1", S.c1); argColor("c2", S.c2);
  bool calSet = hasArg("g1") || hasArg("g2");     // can chinh mau: hex RRGGBB, ff = 100%
  argColor("g1", CL.g[0]); argColor("g2", CL.g[1]);
  if (calSet && testSeg && !testRaw) testUntil = millis() + 10000UL;   // dang thu mau: keo thanh khong bi tat giua chung
  if (argL("o1", 0, 5, v))          S.o1 = v;
  if (argL("o2", 0, 5, v))          S.o2 = v;
  if (argL("zon", 0, 1, v))         S.zoneOn = v;
  if (argL("zs", 1, MAX_LEDS, v))   S.zs = v;
  if (argL("ze", 1, MAX_LEDS, v))   S.ze = v;
  sanitize();
  dirty = true; saveAt = millis() + 1500;
  stateVer++;
  return stateJson();
}
void handleSet() { sendJson(doSet()); }

// Ap dung so LED + diem chia 2 doan cung luc (nut "Ap dung" tren web)
String doApply() {
  long n = S.count, sp = S.split;
  argL("n", 1, MAX_LEDS, n);
  argL("split", 1, MAX_LEDS, sp);
  if (sp > n) sp = n;
  if (S.count != n) { S.count = n; countChanged = true; }
  S.split = sp;
  sanitize();
  resendFrames = 3; dirty = true; saveAt = millis() + 500;
  stateVer++;
  return stateJson();
}
void handleApply() { sendJson(doApply()); }

void handleRefresh() { refreshReq = true; sendJson(F("{\"ok\":1}")); }

String doTest() {
  long seg = 0, c = 255, raw = 0;
  argL("seg", 0, 2, seg); argL("c", 0, 255, c); argL("raw", 0, 1, raw);
  if (seg == 0 || c == 255) testSeg = 0;
  else {
    testSeg = seg; testC = c; testRaw = raw; testUntil = millis() + (raw ? 90000UL : 10000UL);
    if (!S.on) { S.on = 1; powerChanged = true; }
  }
  dirty = true;
  return F("{\"ok\":1}");
}
void handleTest() { sendJson(doTest()); }

void handleDeepSleep() {
  sendJson(F("{\"ok\":1}"));
  delay(150);
  strip.clear(); strip.show();
  if (LED_POWER_PIN >= 0) digitalWrite(LED_POWER_PIN, !LED_POWER_ON);
  S.on = 1; saveSettings();                  // thuc day (bam RST) la den chay lai
  ESP.deepSleep(0);
}

void handleForget() {
  W.ssid[0] = 0; W.pass[0] = 0; saveWifi();
  sendJson(F("{\"ok\":1}")); delay(300); ESP.restart();
}
void handleRestart() { sendJson(F("{\"ok\":1}")); delay(300); ESP.restart(); }

void handleNotFound() {
  if (portalActive) {                        // captive portal: moi URL la -> trang cai dat
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  } else server.send(404, "text/plain", "404");
}

// ====================== TCP (cho app) ======================
// Giao thuc: moi lenh la 1 dong chu ket thuc bang \n (chap nhan \r\n):  LENH [khoa=gia_tri ...]
// Moi tra loi la 1 dong JSON. Loi: {"ok":0,"err":"..."}. Su kien (sau SUB 1): {"ev":"state","s":{...}}
#if TCP_PORT
#define TCP_LINE 160
static WiFiServer tcpServer(TCP_PORT);
static WiFiClient tcpC[TCP_MAX];
static char       tcpLine[TCP_MAX][TCP_LINE];
static uint8_t    tcpLen[TCP_MAX];
static bool       tcpOver[TCP_MAX], tcpSub[TCP_MAX], tcpAuth[TCP_MAX];
static uint32_t   tcpRx[TCP_MAX], tcpSeen[TCP_MAX], tcpPushAt[TCP_MAX];

static void tcpDrop(uint8_t i) {
  tcpC[i].stop();
  tcpLen[i] = 0; tcpOver[i] = tcpSub[i] = tcpAuth[i] = false;
}

static void tcpReply(uint8_t i, const String& s) {
  WiFiClient& c = tcpC[i];
  if (!c.connected()) return;
  String t = s; t += '\n';
  if ((size_t)c.availableForWrite() < t.length()) { tcpDrop(i); return; }   // client khong doc: ngat, khong de treo vong lap
  c.write((const uint8_t*)t.c_str(), t.length());
}

static String tcpErr(const char* m) { String r = F("{\"ok\":0,\"err\":\""); r += m; r += F("\"}"); return r; }

static String tcpHello() {
  String r; r.reserve(140);
  r += F("{\"ok\":1,\"proto\":");  r += TCP_PROTO;
  r += F(",\"name\":\"LED Controller\",\"host\":\""); r += HOSTNAME;
  r += F("\",\"modes\":");         r += MODE_COUNT;
  r += F(",\"max\":");             r += MAX_LEDS;
  r += F(",\"auth\":");            r += (TCP_TOKEN[0] ? 1 : 0);
  r += '}';
  return r;
}

static char* tcpTok(char*& p) {                 // lay tu tiep theo, cat bang khoang trang
  while (*p == ' ' || *p == '\t') p++;
  if (!*p) return nullptr;
  char* t = p;
  while (*p && *p != ' ' && *p != '\t') p++;
  if (*p) *p++ = 0;
  return t;
}

static void tcpLineDone(uint8_t i, char* line) {
  char* p = line;
  char* cmd = tcpTok(p);
  if (!cmd) return;                              // dong trong: bo qua
  char* pos = nullptr;                           // tham so dau tien khong co '='
  tcpN = 0;
  while (char* t = tcpTok(p)) {
    char* eq = strchr(t, '=');
    if (!eq) { if (!pos) pos = t; continue; }
    if (tcpN >= TCP_ARGS_MAX) continue;
    *eq = 0;
    for (char* q = t; *q; q++) *q = tolower((unsigned char)*q);
    tcpK[tcpN] = t; tcpV[tcpN] = eq + 1; tcpN++;
  }
  const uint32_t before = stateVer;
  String r;
  if      (!strcasecmp(cmd, "PING"))  r = F("{\"ok\":1,\"pong\":1}");
  else if (!strcasecmp(cmd, "HELLO")) r = tcpHello();
  else if (!strcasecmp(cmd, "AUTH")) {
    if (!TCP_TOKEN[0] || (pos && !strcmp(pos, TCP_TOKEN))) { tcpAuth[i] = true; r = F("{\"ok\":1}"); }
    else r = tcpErr("auth");
  }
  else if (TCP_TOKEN[0] && !tcpAuth[i]) r = tcpErr("auth");
  else if (!strcasecmp(cmd, "STATE"))   r = stateJson();
  else if (!strcasecmp(cmd, "SET"))     { fromTcp = true; r = doSet();   fromTcp = false; }
  else if (!strcasecmp(cmd, "APPLY"))   { fromTcp = true; r = doApply(); fromTcp = false; }
  else if (!strcasecmp(cmd, "TEST"))    { fromTcp = true; r = doTest();  fromTcp = false; }
  else if (!strcasecmp(cmd, "REFRESH")) { refreshReq = true; r = F("{\"ok\":1}"); }
  else if (!strcasecmp(cmd, "SUB"))     { tcpSub[i] = !(pos && !strcmp(pos, "0")); tcpSeen[i] = stateVer; r = F("{\"ok\":1}"); }
  else r = tcpErr("unknown");
  if (stateVer != before) tcpSeen[i] = stateVer;  // nguoi gui khong nhan lai su kien do chinh minh gay ra
  tcpReply(i, r);
}

static void tcpTask() {
  const uint32_t now = millis();
  // nhan ket noi moi; het cho thi thay ket noi im lau lau nhat (app doi mang de lai ket noi chet)
  WiFiClient nc = tcpServer.accept();
  if (nc) {
    int slot = -1;
    for (uint8_t i = 0; i < TCP_MAX && slot < 0; i++) if (!tcpC[i].connected()) slot = i;
    if (slot < 0) {
      slot = 0;
      for (uint8_t i = 1; i < TCP_MAX; i++) if ((int32_t)(tcpRx[slot] - tcpRx[i]) > 0) slot = i;
    }
    tcpDrop(slot);
    tcpC[slot] = nc;
    tcpC[slot].setNoDelay(true);
    tcpRx[slot] = now; tcpSeen[slot] = stateVer; tcpPushAt[slot] = now;
    tcpAuth[slot] = !TCP_TOKEN[0];
  }
  for (uint8_t i = 0; i < TCP_MAX; i++) {
    WiFiClient& c = tcpC[i];
    if (!c.connected()) { if (tcpLen[i] || tcpSub[i]) tcpDrop(i); continue; }
    uint8_t budget = 96;                          // doc toi da 96 byte/vong de khong lam cham LED
    while (budget-- && c.available()) {
      int ch = c.read();
      if (ch < 0) break;
      tcpRx[i] = now;
      if (ch == '\r') continue;
      if (ch == '\n') {
        tcpLine[i][tcpLen[i]] = 0;
        if (tcpOver[i]) { tcpOver[i] = false; tcpReply(i, tcpErr("too long")); }
        else tcpLineDone(i, tcpLine[i]);
        tcpLen[i] = 0;
        if (!c.connected()) break;
        continue;
      }
      if (tcpLen[i] < TCP_LINE - 1) tcpLine[i][tcpLen[i]++] = (char)ch;
      else tcpOver[i] = true;                     // dong qua dai: bo het cho den het dong
    }
    if (!c.connected()) continue;
    if ((int32_t)(now - tcpRx[i]) > (int32_t)TCP_IDLE_MS) { tcpDrop(i); continue; }
    if (tcpSub[i] && tcpSeen[i] != stateVer && (int32_t)(now - tcpPushAt[i]) >= 150) {
      tcpSeen[i] = stateVer; tcpPushAt[i] = now;
      String ev = F("{\"ev\":\"state\",\"s\":"); ev += stateJson(); ev += '}';
      tcpReply(i, ev);
    }
  }
}
#endif

// ====================== SETUP / LOOP ======================
void setup() {
  Serial.begin(115200);
  randomSeed(analogRead(A0) ^ micros());
  WiFi.persistent(false);
  EEPROM.begin(CAL_EEPROM_OFF + sizeof(Calib));
  loadSettings();

  if (LED_POWER_PIN >= 0) {
    pinMode(LED_POWER_PIN, OUTPUT);
    digitalWrite(LED_POWER_PIN, S.on ? LED_POWER_ON : !LED_POWER_ON);
  }
  if (BTN_PIN >= 0) pinMode(BTN_PIN, INPUT_PULLUP);
  strip.begin(); strip.clear(); strip.show();
  strip.updateLength(S.count);
  dirty = true;

  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);          // tat ngu modem: tranh bo lo ARP/HTTP tu dien thoai
  WiFi.hostname(HOSTNAME);
  Serial.printf("\nLED Controller | MAC=%s | hostname=%s | LED=%u | on=%u\n",
                WiFi.macAddress().c_str(), HOSTNAME, (unsigned)S.count, (unsigned)S.on);
  if (W.ssid[0]) { WiFi.begin(W.ssid, W.pass); bootConn = true; bootStart = millis(); }
  else startPortal();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/app", HTTP_GET, []() { server.send_P(200, "text/html; charset=utf-8", PAGE_APP); });
  server.on("/ui.css", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "max-age=86400");
    server.send_P(200, "text/css; charset=utf-8", PAGE_CSS);
  });
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/connect", HTTP_POST, handleConnect);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/closeap", HTTP_POST, handleCloseAp);
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/set", HTTP_POST, handleSet);
  server.on("/api/apply", HTTP_POST, handleApply);
  server.on("/api/refresh", HTTP_POST, handleRefresh);
  server.on("/api/test", HTTP_POST, handleTest);
  server.on("/api/deepsleep", HTTP_POST, handleDeepSleep);
  server.on("/api/forget", HTTP_POST, handleForget);
  server.on("/api/restart", HTTP_POST, handleRestart);
  server.onNotFound(handleNotFound);
  server.begin();
#if TCP_PORT
  tcpServer.begin();
  tcpServer.setNoDelay(true);
#endif
}

void loop() {
  server.handleClient();
#if TCP_PORT
  tcpTask();
#endif
  if (portalActive) dns.processNextRequest();
  MDNS.update();
  wifiTask();
  buttonTask();
  netReport();
  ledTask();
  if (saveAt && (int32_t)(millis() - saveAt) >= 0) { saveAt = 0; saveSettings(); }
  delay(S.on ? 1 : 4);
}
