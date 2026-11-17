/*
  ESP8266 LED Controller
  - LED 3 chan (WS2811/WS2812...) qua Adafruit NeoPixel
  - Lan dau: ESP phat AP "LED-Setup" -> quet WiFi, nhap mat khau -> hien IP tren trang AP -> dong AP
  - Web dieu khien: mau, so LED, che do, do sang, toc do, bat/tat (ngu), phat sang vung
  - 2 doan day khac thu tu mau: moi doan co "Thu tu mau" rieng + do tu dong

  Thu vien can cai (Library Manager): "Adafruit NeoPixel"
  Board: ESP8266 core 3.x -> NodeMCU 1.0 hoac LOLIN(WEMOS) D1 mini
  Dau day: chan data LED -> D2 (GPIO4) qua dien tro ~330 ohm, GND LED noi chung GND ESP,
           nguon 5V LED cap rieng (du dong), tot nhat co IC doi muc 3.3V->5V (74AHCT125).
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
// ======================================================

#define MAGIC 0x4C454433UL

enum { MODE_SOLID, MODE_BREATHE, MODE_RAINBOW, MODE_SPREAD, MODE_THEATER,
       MODE_COMET, MODE_TWINKLE, MODE_SCANNER, MODE_GRADIENT, MODE_COUNT };

// Thu tu byte gui ra day: PERM[order][k] = kenh logic (0=R,1=G,2=B) nam o byte thu k
// 0 RGB, 1 RBG, 2 GRB, 3 GBR, 4 BRG, 5 BGR
const uint8_t PERM[6][3] = {{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};

struct Settings {
  uint32_t magic;
  char     ssid[33];
  char     pass[65];
  uint8_t  on, bright, mode, speed;
  uint8_t  c1[3], c2[3];
  uint8_t  o1, o2, zoneOn, pad;
  uint16_t count, split, zs, ze;
};
Settings S;

Adafruit_NeoPixel strip(MAX_LEDS, LED_PIN, NEO_RGB + NEO_KHZ800);
ESP8266WebServer  server(80);
DNSServer         dns;

static uint8_t buf[MAX_LEDS][3];   // bo dem mau logic (R,G,B)

// trang thai chay
bool     portalActive = false, bootConn = false, closeApReq = false;
bool     dirty = true, countChanged = false, powerChanged = false;
enum { C_IDLE, C_CONNECTING, C_OK, C_FAIL } connState = C_IDLE;
bool     connectReq = false;
String   pendSsid, pendPass;
uint32_t connStart = 0, okAt = 0, bootStart = 0, saveAt = 0, lostAt = 0;
uint32_t lastFrame = 0, phaseAcc = 0;
uint16_t ph = 0;
uint8_t  testSeg = 0, testC = 0; bool testRaw = false; uint32_t testUntil = 0;

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
  S.magic = MAGIC; S.on = 1; S.bright = 128; S.speed = 128; S.mode = MODE_SOLID;
  setC(S.c1, 255, 0, 0); setC(S.c2, 0, 0, 255);
  S.count = 60 < MAX_LEDS ? 60 : MAX_LEDS;
  S.split = S.count / 2;
  S.o1 = 0; S.o2 = 2;                  // RGB (WS2811) + GRB (WS2812) - chinh lai tren web
  S.zs = 1; S.ze = 10;
}

void sanitize() {
  S.count = clampL(S.count, 1, MAX_LEDS);
  S.split = clampL(S.split, 1, S.count);
  if (S.mode >= MODE_COUNT) S.mode = 0;
  if (S.o1 > 5) S.o1 = 0;
  if (S.o2 > 5) S.o2 = 0;
  S.bright = clampL(S.bright, 1, 255);
  S.speed  = clampL(S.speed, 1, 255);
  S.zs = clampL(S.zs, 1, S.count);
  S.ze = clampL(S.ze, S.zs, S.count);
  S.on = S.on ? 1 : 0; S.zoneOn = S.zoneOn ? 1 : 0;
  S.ssid[32] = 0; S.pass[64] = 0;
}

void loadSettings() {
  EEPROM.get(0, S);
  if (S.magic != MAGIC) defaults();
  sanitize();
}
void saveSettings() { EEPROM.put(0, S); EEPROM.commit(); }

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

void renderFrame() {
  const uint16_t n = S.count;
  uint16_t lo = 0, hi = n;
  if (S.zoneOn) { lo = S.zs - 1; hi = S.ze; }          // vung [lo, hi) tinh tu 0
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
    case MODE_TWINKLE: {
      fadeToBg(lo, hi, bg, 3);
      uint8_t chance = 15 + S.speed / 4;
      for (uint16_t s = 0; s < 1 + len / 50; s++)
        if (random(100) < chance) setPx(lo + (uint16_t)random(len), S.c1);
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
  }
}

// Gui buf ra day, ap dung do sang + thu tu mau rieng tung doan
void pushFrame() {
  const uint16_t n = S.count, split = S.split;
  const uint16_t br = (uint16_t)S.bright + 1;
  const uint8_t *p1 = PERM[S.o1], *p2 = PERM[S.o2];
  for (uint16_t i = 0; i < n; i++) {
    const uint8_t* P = (i < split) ? p1 : p2;
    const uint8_t* L = buf[i];
    strip.setPixelColor(i, (L[P[0]] * br) >> 8, (L[P[1]] * br) >> 8, (L[P[2]] * br) >> 8);
  }
  strip.show();
}

// Che do thu mau: chi sang 1 doan. raw=1: sang dung byte thu testC (dung de do thu tu)
void pushTest() {
  for (uint16_t i = 0; i < S.count; i++) {
    uint8_t seg = i < S.split ? 1 : 2;
    uint8_t b[3] = {0, 0, 0};
    if (seg == testSeg) {
      if (testRaw) { if (testC < 3) b[testC] = 160; }
      else {
        uint8_t L[3] = {0, 0, 0};
        if (testC == 3) { L[0] = L[1] = L[2] = 160; } else if (testC < 3) L[testC] = 160;
        const uint8_t* P = PERM[seg == 1 ? S.o1 : S.o2];
        for (uint8_t k = 0; k < 3; k++) b[k] = L[P[k]];
      }
    }
    strip.setPixelColor(i, b[0], b[1], b[2]);
  }
  strip.show();
}

void applyCount() {
  strip.clear(); strip.show();               // tat het o do dai cu
  strip.updateLength(S.count);
  memset(buf, 0, sizeof(buf));
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
    if (!portalActive && WiFi.status() == WL_CONNECTED) WiFi.setSleepMode(WIFI_MODEM_SLEEP, 3);
  }
}

void ledTask() {
  uint32_t now = millis();
  if (countChanged) { countChanged = false; applyCount(); }
  if (powerChanged) { powerChanged = false; applyPower(); }

  bool testing = testSeg && (int32_t)(testUntil - now) > 0;
  if (testSeg && !testing) { testSeg = 0; dirty = true; }

  if (!S.on) {                                    // dang ngu: giu LED toi
    if (dirty) { strip.clear(); strip.show(); dirty = false; }
    return;
  }
  if (!dirty && (testing || S.mode == MODE_SOLID)) return;   // tinh: khong can gui lai
  if (now - lastFrame < 25) return;                          // ~40 khung/giay
  uint32_t dt = now - lastFrame; lastFrame = now; if (dt > 100) dt = 100;

  if (testing) { pushTest(); dirty = false; return; }
  phaseAcc += dt * (20 + (uint32_t)S.speed * 8);
  ph = (phaseAcc >> 4) & 0xFFFF;
  renderFrame();
  pushFrame();
  dirty = false;
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
  portalActive = false; closeApReq = false;
  applyPower();
  Serial.println(F("Da dong AP"));
}

void onConnected() {
  Serial.print(F("IP: ")); Serial.println(WiFi.localIP());
  MDNS.begin(HOSTNAME);
  MDNS.addService("http", "tcp", 80);
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
      strlcpy(S.ssid, pendSsid.c_str(), sizeof(S.ssid));
      strlcpy(S.pass, pendPass.c_str(), sizeof(S.pass));
      saveSettings();
    } else if ((el > 1500 && (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL)) || el > 20000) {
      connState = C_FAIL; WiFi.disconnect();
    }
  }

  // ESP tu noi lai router khi portal dang mo -> coi nhu xong, AP se tu dong
  if (portalActive && connState == C_IDLE && conn) { connState = C_OK; okAt = millis(); }

  if (portalActive && connState == C_OK && (closeApReq || millis() - okAt > AP_CLOSE_MS)) closePortal();

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

// ====================== HTTP ======================
void sendJson(const String& s) { server.send(200, "application/json", s); }

void handleRoot() {
  if (portalActive && server.client().localIP() == WiFi.softAPIP())
    server.send_P(200, "text/html; charset=utf-8", PAGE_PORTAL);
  else
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
  String s; s.reserve(360);
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
  s += F(",\"zon\":");    s += S.zoneOn;
  s += F(",\"zs\":");     s += S.zs;
  s += F(",\"ze\":");     s += S.ze;
  s += F(",\"rssi\":");   s += WiFi.RSSI();
  s += F(",\"ip\":\"");   s += (WiFi.status() == WL_CONNECTED ? WiFi.localIP() : WiFi.softAPIP()).toString();
  s += F("\"}");
  return s;
}

static bool argL(const char* k, long mn, long mx, long& out) {
  if (!server.hasArg(k)) return false;
  out = clampL(server.arg(k).toInt(), mn, mx);
  return true;
}
static void argColor(const char* k, uint8_t* c) {
  if (!server.hasArg(k)) return;
  String h = server.arg(k);
  if (h.length() != 6) return;
  uint32_t v = strtoul(h.c_str(), nullptr, 16);
  c[0] = v >> 16; c[1] = v >> 8; c[2] = v;
}

void handleState() { sendJson(stateJson()); }

void handleSet() {
  long v;
  if (argL("on", 0, 1, v))          { if (S.on != v) { S.on = v; powerChanged = true; } }
  if (argL("n", 1, MAX_LEDS, v))    { if (S.count != v) { S.count = v; countChanged = true; } }
  if (argL("bright", 1, 255, v))    S.bright = v;
  if (argL("mode", 0, MODE_COUNT - 1, v)) { if (S.mode != v) { S.mode = v; memset(buf, 0, sizeof(buf)); } }
  if (argL("speed", 1, 255, v))     S.speed = v;
  argColor("c1", S.c1); argColor("c2", S.c2);
  if (argL("split", 1, MAX_LEDS, v)) S.split = v;
  if (argL("o1", 0, 5, v))          S.o1 = v;
  if (argL("o2", 0, 5, v))          S.o2 = v;
  if (argL("zon", 0, 1, v))         S.zoneOn = v;
  if (argL("zs", 1, MAX_LEDS, v))   S.zs = v;
  if (argL("ze", 1, MAX_LEDS, v))   S.ze = v;
  sanitize();
  dirty = true; saveAt = millis() + 1500;
  sendJson(stateJson());
}

void handleTest() {
  long seg = 0, c = 255, raw = 0;
  argL("seg", 0, 2, seg); argL("c", 0, 255, c); argL("raw", 0, 1, raw);
  if (seg == 0 || c == 255) testSeg = 0;
  else {
    testSeg = seg; testC = c; testRaw = raw; testUntil = millis() + (raw ? 90000UL : 10000UL);
    if (!S.on) { S.on = 1; powerChanged = true; }
  }
  dirty = true;
  sendJson(F("{\"ok\":1}"));
}

void handleDeepSleep() {
  sendJson(F("{\"ok\":1}"));
  delay(150);
  strip.clear(); strip.show();
  if (LED_POWER_PIN >= 0) digitalWrite(LED_POWER_PIN, !LED_POWER_ON);
  S.on = 1; saveSettings();                  // thuc day (bam RST) la den chay lai
  ESP.deepSleep(0);
}

void handleForget() {
  S.ssid[0] = 0; S.pass[0] = 0; saveSettings();
  sendJson(F("{\"ok\":1}")); delay(300); ESP.restart();
}
void handleRestart() { sendJson(F("{\"ok\":1}")); delay(300); ESP.restart(); }

void handleNotFound() {
  if (portalActive) {                        // captive portal: moi URL la -> trang cai dat
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  } else server.send(404, "text/plain", "404");
}

// ====================== SETUP / LOOP ======================
void setup() {
  Serial.begin(115200);
  randomSeed(analogRead(A0) ^ micros());
  WiFi.persistent(false);
  EEPROM.begin(sizeof(Settings));
  loadSettings();

  if (LED_POWER_PIN >= 0) {
    pinMode(LED_POWER_PIN, OUTPUT);
    digitalWrite(LED_POWER_PIN, S.on ? LED_POWER_ON : !LED_POWER_ON);
  }
  strip.begin(); strip.clear(); strip.show();
  strip.updateLength(S.count);
  dirty = true;

  WiFi.mode(WIFI_STA);
  WiFi.hostname(HOSTNAME);
  if (S.ssid[0]) { WiFi.begin(S.ssid, S.pass); bootConn = true; bootStart = millis(); }
  else startPortal();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/app", HTTP_GET, []() { server.send_P(200, "text/html; charset=utf-8", PAGE_APP); });
  server.on("/glass.css", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "max-age=86400");
    server.send_P(200, "text/css; charset=utf-8", PAGE_CSS);
  });
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/connect", HTTP_POST, handleConnect);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/closeap", HTTP_POST, handleCloseAp);
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/set", HTTP_POST, handleSet);
  server.on("/api/test", HTTP_POST, handleTest);
  server.on("/api/deepsleep", HTTP_POST, handleDeepSleep);
  server.on("/api/forget", HTTP_POST, handleForget);
  server.on("/api/restart", HTTP_POST, handleRestart);
  server.onNotFound(handleNotFound);
  server.begin();
}

void loop() {
  server.handleClient();
  if (portalActive) dns.processNextRequest();
  MDNS.update();
  wifiTask();
  ledTask();
  if (saveAt && (int32_t)(millis() - saveAt) >= 0) { saveAt = 0; saveSettings(); }
  delay(S.on ? 1 : 4);
}
