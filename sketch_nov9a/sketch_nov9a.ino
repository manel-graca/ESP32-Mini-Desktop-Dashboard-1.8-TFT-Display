#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiUdp.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <time.h>
#include <sys/time.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

// ================= CONFIG =================
const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* TZ_INFO   = "WET0WEST,M3.5.0/1,M10.5.0";   // Lisbon (auto DST)

const char* WEATHER_LAT  = "38.7223";   // your latitude  (default: Lisbon)
const char* WEATHER_LON  = "-9.1393";   // your longitude
#define WEATHER_REFRESH_MS (15UL * 60 * 1000)
#define WEATHER_STALE_MS   (35UL * 60 * 1000)        // older than this -> shown as outdated
#define WEATHER_EXPIRE_MS  (3UL * 60 * 60 * 1000)    // older than this -> not shown at all

// PC stats + Claude / Codex usage come from pc/painel_bridge.py, over USB serial
// and/or Wi-Fi (UDP). The board only listens, so the PC program can start any time.
#define PC_UDP_PORT  4210       // must match UDP_PORT in painel_bridge.py
#define PC_HOSTNAME  "painel"   // the PC finds the board as painel.local
#define USAGE_EXPIRE_MS 30000   // usage is re-sent every 5 s

// CoinGecko ids + ticker shown on screen (3 rows fit)
struct Coin { const char* id; const char* symbol; };
static const Coin COINS[] = {
  {"bitcoin",  "BTC"},
  {"ethereum", "ETH"},
  {"solana",   "SOL"}
};
#define COIN_COUNT (sizeof(COINS) / sizeof(COINS[0]))
#define CRYPTO_REFRESH_MS (5UL * 60 * 1000)
#define CRYPTO_STALE_MS   (12UL * 60 * 1000)
#define CRYPTO_EXPIRE_MS  (60UL * 60 * 1000)

#define PC_TIMEOUT_MS 4000   // no line from the PC for this long -> "no data"

#define LED_MAX 90   // 0-255, overall LED brightness cap

// ================= PINS =================
#define TFT_CS    5
#define TFT_DC    16
#define TFT_RST   17
#define TFT_SCK   18
#define TFT_MOSI  23

#define BUTTON_L  4   // left  -> click: page action, hold: prev page
#define BUTTON_R  0   // right -> click: page action, hold: next page

#define LED_GREEN 15
#define LED_BLUE  2
#define LED_RED   13

#define SCR_W    128
#define SCR_H    160
#define FRAME_MS 33   // ~30 fps

#define RGB(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

const uint16_t C_BG      = RGB(10, 12, 20);
const uint16_t C_TEXT    = RGB(235, 235, 240);
const uint16_t C_DIM     = RGB(90, 95, 110);
const uint16_t C_ACCENT  = RGB(255, 190, 40);
const uint16_t C_OK      = RGB(60, 220, 100);
const uint16_t C_BAD     = RGB(230, 60, 60);
const uint16_t C_HOLIDAY = RGB(70, 200, 230);
const uint16_t C_FAINT   = RGB(45, 48, 60);

Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_RST);
GFXcanvas16 fb(SCR_W, SCR_H);   // full framebuffer (40 KB) -> no flicker

// ================= TEXT (UTF-8, Portuguese accents) =================
// The built-in GFX font has no ã/õ/Á/..., so accented letters are drawn as
// the base glyph plus a few accent pixels. Strings are plain UTF-8.
#define AC_NONE    0
#define AC_GRAVE   1
#define AC_ACUTE   2
#define AC_CIRC    3
#define AC_TILDE   4
#define AC_CEDILLA 5

// two pixel rows per accent, bit n = column n
static const uint8_t ACCENT_ROWS[][2] = {
  {0x00, 0x00},
  {0x02, 0x04},   // grave
  {0x08, 0x04},   // acute
  {0x04, 0x0A},   // circumflex
  {0x12, 0x0D},   // tilde
  {0x04, 0x02},   // cedilla
};

int16_t textWidth(const char* s, uint8_t size) {
  int n = 0;
  for (; *s; s++) if ((*s & 0xC0) != 0x80) n++;   // count code points
  return n ? n * 6 * size - size : 0;
}

void drawText(GFXcanvas16& g, const char* s, int16_t x, int16_t y, uint8_t size, uint16_t color) {
  for (const uint8_t* p = (const uint8_t*)s; *p; p++, x += 6 * size) {
    uint8_t c = *p, accent = AC_NONE;
    bool euro = false;

    if (c == 0xC2 && p[1] == 0xB0) {     // ° (needs cp437(true) on the canvas)
      p++; c = 0xF8;
    } else if (c == 0xE2 && p[1] == 0x82 && p[2] == 0xAC) {   // € = 'C' + two bars
      p += 2; c = 'C'; euro = true;
    } else if (c == 0xC3 && p[1]) {      // Latin-1 supplement (À..ú)
      uint8_t b = *++p;
      bool lower = b & 0x20;
      switch (b & 0xDF) {
        case 0x80: c = 'A'; accent = AC_GRAVE;   break;
        case 0x81: c = 'A'; accent = AC_ACUTE;   break;
        case 0x82: c = 'A'; accent = AC_CIRC;    break;
        case 0x83: c = 'A'; accent = AC_TILDE;   break;
        case 0x87: c = 'C'; accent = AC_CEDILLA; break;
        case 0x89: c = 'E'; accent = AC_ACUTE;   break;
        case 0x8A: c = 'E'; accent = AC_CIRC;    break;
        case 0x8D: c = 'I'; accent = AC_ACUTE;   break;
        case 0x93: c = 'O'; accent = AC_ACUTE;   break;
        case 0x94: c = 'O'; accent = AC_CIRC;    break;
        case 0x95: c = 'O'; accent = AC_TILDE;   break;
        case 0x9A: c = 'U'; accent = AC_ACUTE;   break;
        default:   c = '?'; lower = false;       break;
      }
      if (lower) {
        if (c == 'I') { c = 0xA1; accent = AC_NONE; }   // font's own dotless í
        else c += 32;
      }
    }

    g.drawChar(x, y, c, color, color, size);
    if (euro) {
      g.fillRect(x - size, y + 2 * size, 4 * size, size, color);
      g.fillRect(x - size, y + 4 * size, 4 * size, size, color);
    }
    if (accent) {
      int16_t row = accent == AC_CEDILLA ? 7 : (c >= 'a' ? -1 : -3);
      for (int r = 0; r < 2; r++)
        for (int col = 0; col < 5; col++)
          if (ACCENT_ROWS[accent][r] & (1 << col))
            g.fillRect(x + col * size, y + (row + r) * size, size, size, color);
    }
  }
}

void drawCenteredAt(GFXcanvas16& g, const char* s, int16_t cx, int16_t y, uint8_t size, uint16_t color) {
  drawText(g, s, cx - textWidth(s, size) / 2, y, size, color);
}

void drawCentered(GFXcanvas16& g, const char* s, int16_t y, uint8_t size, uint16_t color) {
  drawCenteredAt(g, s, SCR_W / 2, y, size, color);
}

// centered around yMid, split in two lines if it doesn't fit the screen
void drawCenteredWrapped(GFXcanvas16& g, const char* s, int16_t yMid, uint16_t color) {
  const int MAX_CHARS = SCR_W / 6;
  int n = 0, split = -1;
  for (int i = 0; s[i]; i++) {
    if ((s[i] & 0xC0) == 0x80) continue;
    if (s[i] == ' ' && n <= MAX_CHARS) split = i;
    n++;
  }
  if (n <= MAX_CHARS || split < 0) { drawCentered(g, s, yMid - 4, 1, color); return; }

  char first[48];
  snprintf(first, sizeof first, "%.*s", split, s);
  drawCentered(g, first,         yMid - 9, 1, color);
  drawCentered(g, s + split + 1, yMid + 2, 1, color);
}

// ================= LOCALE =================
const char* const DAYS_SHORT[]   = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
const char* const MONTHS_SHORT[] = {"JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG","SEP","OCT","NOV","DEC"};
const char* const MONTHS_FULL[]  = {"JANUARY","FEBRUARY","MARCH","APRIL","MAY","JUNE",
                                    "JULY","AUGUST","SEPTEMBER","OCTOBER","NOVEMBER","DECEMBER"};

// ================= DATES / HOLIDAYS (Portugal) =================
bool isLeap(int year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

int daysInMonth(int year, int mon) {   // mon 0-11
  static const uint8_t DIM[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  return DIM[mon] + (mon == 1 && isLeap(year));
}

int dayOfYear(int year, int mon, int day) {
  int doy = day;
  for (int m = 0; m < mon; m++) doy += daysInMonth(year, m);
  return doy;
}

// Easter Sunday as day-of-year (anonymous Gregorian algorithm)
int easterDayOfYear(int year) {
  static int cachedYear = 0, cachedDoy = 0;
  if (year == cachedYear) return cachedDoy;
  int a = year % 19, b = year / 100, c = year % 100;
  int d = b / 4, e = b % 4, f = (b + 8) / 25, g = (b - f + 1) / 3;
  int h = (19 * a + b - d - g + 15) % 30;
  int i = c / 4, k = c % 4;
  int l = (32 + 2 * e + 2 * i - h - k) % 7;
  int m = (a + 11 * h + 22 * l) / 451;
  int mon = (h + l - 7 * m + 114) / 31;          // 3 = March, 4 = April
  int day = (h + l - 7 * m + 114) % 31 + 1;
  cachedYear = year;
  cachedDoy  = dayOfYear(year, mon - 1, day);
  return cachedDoy;
}

struct FixedHoliday   { uint8_t mon, day; const char* name; };   // mon 1-12
struct MovableHoliday { int8_t easterOffset; const char* name; };

static const FixedHoliday FIXED_HOLIDAYS[] = {
  { 1,  1, "New Year's Day"},
  { 4, 25, "Freedom Day"},
  { 5,  1, "Labour Day"},
  { 6, 10, "Portugal Day"},
  { 8, 15, "Assumption of Mary"},
  {10,  5, "Republic Day"},
  {11,  1, "All Saints' Day"},
  {12,  1, "Restoration of Independence"},
  {12,  8, "Immaculate Conception"},
  {12, 25, "Christmas Day"},
};

static const MovableHoliday MOVABLE_HOLIDAYS[] = {
  {-2, "Good Friday"},
  { 0, "Easter Sunday"},
  {60, "Corpus Christi"},
};

// mon 0-11; returns nullptr if the day is not a holiday
const char* holidayName(int year, int mon, int day) {
  for (const FixedHoliday& h : FIXED_HOLIDAYS)
    if (h.mon == mon + 1 && h.day == day) return h.name;
  int diff = dayOfYear(year, mon, day) - easterDayOfYear(year);
  for (const MovableHoliday& h : MOVABLE_HOLIDAYS)
    if (h.easterOffset == diff) return h.name;
  return nullptr;
}

// ================= BUTTONS =================
// short press -> fires on release, long press -> fires once after LONG_MS
#define EV_NONE   0
#define EV_SHORT  1
#define EV_LONG   2
#define LONG_MS   500

struct Button {
  uint8_t  pin;
  bool     raw = false, stable = false, longFired = false;
  uint32_t changedAt = 0, downAt = 0;

  explicit Button(uint8_t p) : pin(p) {}
  void begin() { pinMode(pin, INPUT_PULLUP); }

  uint8_t poll() {
    bool r = digitalRead(pin) == LOW;
    uint32_t now = millis();
    if (r != raw) { raw = r; changedAt = now; }
    if (raw != stable && now - changedAt > 20) {
      stable = raw;
      if (stable) { downAt = now; longFired = false; }
      else if (!longFired) return EV_SHORT;
    }
    if (stable && !longFired && now - downAt >= LONG_MS) { longFired = true; return EV_LONG; }
    return EV_NONE;
  }
};

Button btnL(BUTTON_L), btnR(BUTTON_R);

// ================= BACKGROUND DATA FEEDS (one task on core 0) =================
// http or https (certificate not verified), body parsed as JSON
bool httpGetJson(const char* url, JsonDocument& doc) {
  WiFiClientSecure tls;
  HTTPClient http;
  bool begun;
  if (strncmp(url, "https", 5) == 0) { tls.setInsecure(); begun = http.begin(tls, url); }
  else                               begun = http.begin(url);
  if (!begun) return false;
  http.setConnectTimeout(3000);
  http.setTimeout(8000);
  int status = http.GET();
  if (status != 200) { http.end(); return false; }
  String body = http.getString();
  http.end();
  return !deserializeJson(doc, body);
}

struct FeedData {
  bool     valid = false;
  uint32_t fetchedAt = 0;   // millis() of the successful fetch
};

// T must derive from FeedData and provide: static bool fetch(T& out)
template <typename T>
struct Feed {
  const char*   name;
  uint32_t      refreshMs, retryMs, expireMs;
  T             data;
  volatile bool lastFailed = false;
  uint32_t      nextAt = 0;
  portMUX_TYPE  mux = portMUX_INITIALIZER_UNLOCKED;

  Feed(const char* n, uint32_t refresh, uint32_t retry, uint32_t expire)
    : name(n), refreshMs(refresh), retryMs(retry), expireMs(expire) {}

  // data too old to be trusted comes back as invalid
  T get() {
    portENTER_CRITICAL(&mux);
    T copy = data;
    portEXIT_CRITICAL(&mux);
    if (copy.valid && millis() - copy.fetchedAt > expireMs) copy.valid = false;
    return copy;
  }

  // called from the network task; fetches when due
  void poll() {
    if ((int32_t)(millis() - nextAt) < 0) return;
    T fresh;
    bool ok = T::fetch(fresh);
    if (ok) {
      fresh.valid = true;
      fresh.fetchedAt = millis();
      portENTER_CRITICAL(&mux);
      data = fresh;
      portEXIT_CRITICAL(&mux);
    }
    lastFailed = !ok;
    nextAt = millis() + (ok ? refreshMs : retryMs);
    Serial.printf("[%s] %s\n", name, ok ? "updated" : "failed");
  }
};

// ---- weather (Open-Meteo) ----
struct WeatherData : FeedData {
  float    temp = 0, wind = 0;
  int      code = 0, humidity = 0;
  bool     isDay = true;
  int      dCode[4] = {}, dMax[4] = {}, dMin[4] = {};
  float    uv = 0, uvMax = 0;                   // now / today's maximum
  int      sunrise[2] = {-1, -1}, sunset[2] = {-1, -1};   // today, tomorrow: minutes since midnight

  // "2026-11-12T07:15" (local time) -> minutes since midnight, -1 if missing
  static int minuteOfDay(const char* iso) {
    if (!iso || strlen(iso) < 16) return -1;
    return atoi(iso + 11) * 60 + atoi(iso + 14);
  }

  static bool fetch(WeatherData& out) {
    char url[400];
    snprintf(url, sizeof url,
      "http://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
      "&current=temperature_2m,relative_humidity_2m,wind_speed_10m,weather_code,is_day,uv_index"
      "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset,uv_index_max"
      "&timezone=auto&forecast_days=4",
      WEATHER_LAT, WEATHER_LON);

    JsonDocument doc;
    if (!httpGetJson(url, doc)) return false;

    JsonObject cur = doc["current"], day = doc["daily"];
    out.temp     = cur["temperature_2m"] | 0.0f;
    out.humidity = cur["relative_humidity_2m"] | 0;
    out.wind     = cur["wind_speed_10m"] | 0.0f;
    out.code     = cur["weather_code"] | 0;
    out.isDay    = (cur["is_day"] | 1) == 1;
    for (int i = 0; i < 4; i++) {
      out.dCode[i] = day["weather_code"][i] | 0;
      out.dMax[i]  = (int)lroundf(day["temperature_2m_max"][i] | 0.0f);
      out.dMin[i]  = (int)lroundf(day["temperature_2m_min"][i] | 0.0f);
    }
    out.uv    = cur["uv_index"] | 0.0f;
    out.uvMax = day["uv_index_max"][0] | 0.0f;
    for (int i = 0; i < 2; i++) {
      out.sunrise[i] = minuteOfDay(day["sunrise"][i] | (const char*)nullptr);
      out.sunset[i]  = minuteOfDay(day["sunset"][i] | (const char*)nullptr);
    }
    return true;
  }
};

// ---- crypto prices in EUR (CoinGecko) ----
struct CryptoData : FeedData {
  float price[COIN_COUNT] = {}, change[COIN_COUNT] = {};   // change = 24 h, in %

  static bool fetch(CryptoData& out) {
    String url = "https://api.coingecko.com/api/v3/simple/price"
                 "?vs_currencies=eur&include_24hr_change=true&ids=";
    for (size_t i = 0; i < COIN_COUNT; i++) {
      if (i) url += ',';
      url += COINS[i].id;
    }

    JsonDocument doc;
    if (!httpGetJson(url.c_str(), doc)) return false;
    for (size_t i = 0; i < COIN_COUNT; i++) {
      JsonObject c = doc[COINS[i].id];
      if (c.isNull()) return false;
      out.price[i]  = c["eur"] | 0.0f;
      out.change[i] = c["eur_24h_change"] | 0.0f;
    }
    return true;
  }
};

Feed<WeatherData> weatherFeed("weather", WEATHER_REFRESH_MS, 30000, WEATHER_EXPIRE_MS);
Feed<CryptoData>  cryptoFeed ("crypto",  CRYPTO_REFRESH_MS,  60000, CRYPTO_EXPIRE_MS);

// feeds are fetched one at a time (a single TLS connection at once keeps RAM use low)
void netTask(void*) {
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
    weatherFeed.poll();
    cryptoFeed.poll();
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// ================= PC LINK (painel_bridge.py over USB serial and/or UDP) =================
// hardware, once per second:  cpu%,cpuTemp,gpu%,gpuTemp,ramGB,ram%
// AI usage, every 5 s:        C,<5h %>,<5h reset in s>,<7d %>,<7d reset in s>   (X,... = Codex)
#define PC_HIST 116   // samples kept for the graph (1 per second)

struct UsageData {
  bool     seen = false;
  uint32_t receivedAt = 0;
  float    sessionPct = 0, weeklyPct = 0;
  int32_t  sessionResetIn = -1, weeklyResetIn = -1;   // seconds left when received, -1 = unknown

  bool live() const { return seen && millis() - receivedAt < USAGE_EXPIRE_MS; }
};

UsageData claudeUsage, codexUsage;

WiFiUDP pcUdp;

struct PcMonitor {
  int      cpu = 0, cpuT = 0, gpu = 0, gpuT = 0, ramPct = 0;
  float    ramGb = 0;
  bool     seen = false;                       // hardware stats
  uint32_t lastAt = 0, lastHistAt = 0;
  bool     usbSeen = false, udpSeen = false;   // any valid line, per path
  uint32_t lastUsbAt = 0, lastUdpAt = 0;
  uint8_t  histCpu[PC_HIST] = {}, histGpu[PC_HIST] = {};
  int      histCount = 0, histHead = 0;
  char     line[48];
  uint8_t  len = 0;
  bool     udpUp = false;

  bool live()    const { return seen && millis() - lastAt < PC_TIMEOUT_MS; }
  bool usbLive() const { return usbSeen && millis() - lastUsbAt < PC_TIMEOUT_MS; }
  bool udpLive() const { return udpSeen && millis() - lastUdpAt < PC_TIMEOUT_MS; }
  bool linked()  const { return usbLive() || udpLive(); }

  // returns false if the line is not understood
  bool apply(const char* s) {
    if ((s[0] == 'C' || s[0] == 'X') && s[1] == ',') {   // AI usage line
      UsageData& u = s[0] == 'C' ? claudeUsage : codexUsage;
      float sp, wp; long sr, wr;
      if (sscanf(s + 2, "%f,%ld,%f,%ld", &sp, &sr, &wp, &wr) != 4) return false;
      u.sessionPct = sp; u.sessionResetIn = sr;
      u.weeklyPct  = wp; u.weeklyResetIn  = wr;
      u.seen = true;
      u.receivedAt = millis();
      return true;
    }

    int c, ct, gp, gt, rp; float rg;
    if (sscanf(s, "%d,%d,%d,%d,%f,%d", &c, &ct, &gp, &gt, &rg, &rp) != 6) return false;
    cpu = constrain(c, 0, 100);  cpuT = ct;
    gpu = constrain(gp, 0, 100); gpuT = gt;
    ramGb = rg; ramPct = constrain(rp, 0, 100);
    uint32_t now = millis();
    if (!seen || now - lastHistAt >= 700) {   // the same sample can arrive over USB and Wi-Fi
      lastHistAt = now;
      histCpu[histHead] = cpu;
      histGpu[histHead] = gpu;
      histHead = (histHead + 1) % PC_HIST;
      if (histCount < PC_HIST) histCount++;
    }
    seen = true;
    lastAt = now;
    return true;
  }

  void parse(const char* s, bool viaUdp) {
    if (!apply(s)) return;
    if (viaUdp) { udpSeen = true; lastUdpAt = millis(); }
    else        { usbSeen = true; lastUsbAt = millis(); }
  }

  // called every loop: the board just listens, nothing has to be "connected" first
  void poll() {
    while (Serial.available()) {
      char ch = Serial.read();
      if (ch == '\n') { line[len] = 0; parse(line, false); len = 0; }
      else if (ch != '\r') {
        if (len < sizeof(line) - 1) line[len++] = ch;
        else len = 0;   // line too long: drop it
      }
    }

    bool wifi = WiFi.status() == WL_CONNECTED;
    if (wifi && !udpUp) {
      udpUp = pcUdp.begin(PC_UDP_PORT);
      MDNS.begin(PC_HOSTNAME);
    } else if (!wifi && udpUp) {
      pcUdp.stop();
      MDNS.end();
      udpUp = false;
    }
    if (!udpUp) return;

    for (int n = 0; n < 8 && pcUdp.parsePacket() > 0; n++) {
      char pkt[200];
      int got = pcUdp.read(pkt, sizeof(pkt) - 1);
      if (got <= 0) continue;
      pkt[got] = 0;
      char* save = nullptr;
      for (char* s = strtok_r(pkt, "\r\n", &save); s; s = strtok_r(nullptr, "\r\n", &save)) parse(s, true);
    }
  }
};

PcMonitor pc;

// ================= WEATHER ICONS =================
#define WX_CLEAR   0
#define WX_PARTLY  1
#define WX_CLOUDY  2
#define WX_FOG     3
#define WX_RAIN    4
#define WX_SNOW    5
#define WX_STORM   6

#define PX(v) ((int16_t)lroundf(v))

const uint16_t C_CLOUD   = RGB(200, 204, 216);
const uint16_t C_CLOUD_D = RGB(120, 126, 142);
const uint16_t C_RAIN    = RGB(74, 168, 255);
const uint16_t C_MOON    = RGB(220, 225, 240);

uint8_t wxKind(int code) {
  if (code == 0) return WX_CLEAR;
  if (code <= 2) return WX_PARTLY;
  if (code == 3) return WX_CLOUDY;
  if (code == 45 || code == 48) return WX_FOG;
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return WX_SNOW;
  if (code >= 95) return WX_STORM;
  return WX_RAIN;
}

const char* wxLabel(int code) {
  switch (code) {
    case 0:  return "CLEAR SKY";
    case 1:  return "MAINLY CLEAR";
    case 2:  return "PARTLY CLOUDY";
    case 3:  return "OVERCAST";
    case 45: case 48: return "FOG";
    case 51: case 53: case 55: return "DRIZZLE";
    case 56: case 57: return "FREEZING DRIZZLE";
    case 61: return "LIGHT RAIN";
    case 63: return "RAIN";
    case 65: return "HEAVY RAIN";
    case 66: case 67: return "FREEZING RAIN";
    case 71: case 73: case 75: case 77: return "SNOW";
    case 80: case 81: case 82: return "SHOWERS";
    case 85: case 86: return "SNOW SHOWERS";
    case 95: return "THUNDERSTORM";
    case 96: case 99: return "STORM WITH HAIL";
  }
  return "--";
}

void drawCloud(GFXcanvas16& g, float cx, float cy, float s, uint16_t c) {
  g.fillCircle(PX(cx - 9 * s),  PX(cy + 3 * s), PX(7 * s),  c);
  g.fillCircle(PX(cx + 1 * s),  PX(cy - 3 * s), PX(10 * s), c);
  g.fillCircle(PX(cx + 10 * s), PX(cy + 3 * s), PX(7 * s),  c);
  g.fillRect(PX(cx - 9 * s), PX(cy + 3 * s), PX(19 * s) + 1, PX(7 * s) + 1, c);
}

void drawSun(GFXcanvas16& g, float cx, float cy, float s, float t) {
  float r = 11 * s;
  if (s > 0.8f) {
    for (int k = 0; k < 8; k++) {
      float a = t * 0.6f + k * PI / 4;
      int16_t x1 = PX(cx + cosf(a) * (r + 4 * s)), y1 = PX(cy + sinf(a) * (r + 4 * s));
      int16_t x2 = PX(cx + cosf(a) * (r + 9 * s)), y2 = PX(cy + sinf(a) * (r + 9 * s));
      g.drawLine(x1, y1, x2, y2, C_ACCENT);
      g.drawLine(x1 + 1, y1, x2 + 1, y2, C_ACCENT);
    }
  }
  g.fillCircle(PX(cx), PX(cy), PX(r), C_ACCENT);
}

void drawMoon(GFXcanvas16& g, float cx, float cy, float s) {
  float r = 11 * s;
  g.fillCircle(PX(cx), PX(cy), PX(r), C_MOON);
  g.fillCircle(PX(cx + r * 0.45f), PX(cy - r * 0.35f), PX(r * 0.85f), C_BG);
}

void drawWeatherIcon(GFXcanvas16& g, float cx, float cy, uint8_t kind, bool day, float s, float t) {
  float drift = sinf(t * 0.8f) * 2 * s;

  switch (kind) {
    case WX_CLEAR:
      if (day) drawSun(g, cx, cy, s, t); else drawMoon(g, cx, cy, s);
      break;

    case WX_PARTLY:
      if (day) drawSun(g, cx - 7 * s, cy - 6 * s, s * 0.8f, t);
      else     drawMoon(g, cx - 7 * s, cy - 6 * s, s * 0.8f);
      drawCloud(g, cx + 4 * s + drift, cy + 4 * s, s * 0.85f, C_CLOUD);
      break;

    case WX_CLOUDY:
      drawCloud(g, cx - 6 * s - drift, cy - 4 * s, s * 0.7f, C_CLOUD_D);
      drawCloud(g, cx + 3 * s + drift, cy + 3 * s, s, C_CLOUD);
      break;

    case WX_FOG:
      for (int i = 0; i < 4; i++) {
        float off = sinf(t + i * 1.3f) * 4 * s;
        int16_t y = PX(cy - 9 * s + i * 6 * s);
        g.drawFastHLine(PX(cx - 14 * s + off), y, PX(28 * s), C_CLOUD);
        if (s > 0.8f) g.drawFastHLine(PX(cx - 14 * s + off), y + 1, PX(28 * s), C_CLOUD);
      }
      break;

    default: {   // rain / snow / storm
      bool snow = kind == WX_SNOW;
      drawCloud(g, cx, cy - 5 * s, s, kind == WX_STORM ? C_CLOUD_D : C_CLOUD);
      for (int i = 0; i < 3; i++) {
        float phase = fmodf(t * (snow ? 0.6f : 1.6f) + i * 0.37f, 1.f);
        float x = cx + (i - 1) * 8 * s, y = cy + 6 * s + phase * 9 * s;
        if (snow) g.fillRect(PX(x + sinf(t * 2 + i) * 2 * s), PX(y), 2, 2, C_TEXT);
        else      g.drawFastVLine(PX(x), PX(y), PX(4 * s) < 2 ? 2 : PX(4 * s), C_RAIN);
      }
      if (kind == WX_STORM && fmodf(t, 2.f) < 1.6f) {
        g.fillTriangle(PX(cx + 2 * s), PX(cy + 3 * s), PX(cx - 4 * s), PX(cy + 12 * s), PX(cx + 1 * s), PX(cy + 12 * s), C_ACCENT);
        g.fillTriangle(PX(cx - 1 * s), PX(cy + 10 * s), PX(cx + 4 * s), PX(cy + 10 * s), PX(cx - 3 * s), PX(cy + 20 * s), C_ACCENT);
      }
    }
  }
}

// ================= RGB LED =================
// breathing color = current weather (dimmer at night)
// red pulse = no wifi, blue pulse = waiting for weather, white blip = page change
struct LedController {
  uint32_t flashUntil = 0;

  void begin() {
    pinMode(LED_RED, OUTPUT);
    pinMode(LED_GREEN, OUTPUT);
    pinMode(LED_BLUE, OUTPUT);
  }

  void flash() { flashUntil = millis() + 90; }

  void set(float r, float g, float b, float level) {
    level = constrain(level, 0.f, 1.f);
    level *= level;   // perceptual curve
    analogWrite(LED_RED,   (int)(r * level * LED_MAX / 255.f));
    analogWrite(LED_GREEN, (int)(g * level * LED_MAX / 255.f));
    analogWrite(LED_BLUE,  (int)(b * level * LED_MAX / 255.f));
  }

  void update(uint32_t now) {
    float t = now / 1000.f;
    if (now < flashUntil) { set(255, 255, 255, 1.f); return; }

    if (WiFi.status() != WL_CONNECTED) {               // fast red pulse
      set(255, 0, 0, 0.15f + 0.85f * (0.5f - 0.5f * cosf(t * 2 * PI / 1.2f)));
      return;
    }

    WeatherData w = weatherFeed.get();
    if (!w.valid) {                                     // blue pulse
      set(0, 60, 255, 0.15f + 0.85f * (0.5f - 0.5f * cosf(t * 2 * PI / 1.5f)));
      return;
    }

    float r, g, b;
    switch (wxKind(w.code)) {
      case WX_CLEAR:  if (w.isDay) { r = 255; g = 110; b = 0; } else { r = 40; g = 30; b = 200; } break;
      case WX_PARTLY: r = 255; g = 160; b = 60;  break;
      case WX_CLOUDY: r = 150; g = 160; b = 190; break;
      case WX_FOG:    r = 110; g = 120; b = 130; break;
      case WX_RAIN:   r = 0;   g = 90;  b = 255; break;
      case WX_SNOW:   r = 170; g = 210; b = 255; break;
      default:        r = 150; g = 0;   b = 255; break;  // storm
    }

    float level = 0.3f + 0.7f * (0.5f - 0.5f * cosf(t * 2 * PI / 5.f));   // slow breathe
    if (!w.isDay) level *= 0.4f;
    if (wxKind(w.code) == WX_STORM && fmodf(t, 7.f) < 0.08f) { set(255, 255, 255, 1.f); return; }  // lightning
    set(r, g, b, level);
  }
};

LedController led;

// ================= PAGES =================
struct Page {
  virtual void enter() {}
  virtual void action(bool right) {}   // short click on L / R
  virtual void draw(GFXcanvas16& g, uint32_t now) = 0;
  virtual ~Page() = default;
};

struct DashboardPage : Page {
  void draw(GFXcanvas16& g, uint32_t now) override {
    bool wifi = WiFi.status() == WL_CONNECTED;
    struct tm t;
    bool haveTime = getLocalTime(&t, 0);
    char buf[40];

    g.fillCircle(SCR_W - 5, 4, 2, wifi ? C_OK : C_BAD);

    // ---- clock ----
    if (haveTime) {
      snprintf(buf, sizeof buf, (t.tm_sec & 1) ? "%02d %02d" : "%02d:%02d", t.tm_hour, t.tm_min);
      drawCentered(g, buf, 8, 4, C_TEXT);

      struct timeval tv;
      gettimeofday(&tv, nullptr);
      float frac = (t.tm_sec + tv.tv_usec / 1e6f) / 60.f;
      g.drawFastHLine(14, 45, 100, C_DIM);
      g.drawFastHLine(14, 45, (int16_t)(100 * frac), C_ACCENT);

      snprintf(buf, sizeof buf, "%s %d %s %d", DAYS_SHORT[t.tm_wday], t.tm_mday, MONTHS_SHORT[t.tm_mon], t.tm_year + 1900);
      drawCentered(g, buf, 50, 1, C_DIM);
    } else {
      drawCentered(g, "--:--", 8, 4, C_DIM);
      drawCentered(g, wifi ? "syncing time..." : "connecting wi-fi...", 50, 1, C_DIM);
    }

    // ---- weather ----
    WeatherData w = weatherFeed.get();
    if (!w.valid) {
      drawCentered(g, !wifi ? "no wi-fi" : weatherFeed.lastFailed ? "weather unavailable" : "loading weather...", 96, 1, C_DIM);
      return;
    }
    uint32_t age = millis() - w.fetchedAt;
    float ts = now / 1000.f;

    drawWeatherIcon(g, 32, 82, wxKind(w.code), w.isDay, 0.9f, ts);

    snprintf(buf, sizeof buf, "%d", (int)lroundf(w.temp));
    int16_t x1, y1; uint16_t tw, th;
    g.setTextSize(3);
    g.setTextColor(C_TEXT);
    g.getTextBounds(buf, 0, 0, &x1, &y1, &tw, &th);
    int16_t tx = 90 - (tw + 4) / 2;
    g.setCursor(tx, 72);
    g.print(buf);
    g.drawCircle(tx + tw + 2, 75, 2, C_TEXT);
    g.drawCircle(tx + tw + 2, 75, 1, C_TEXT);

    drawCentered(g, wxLabel(w.code), 104, 1, C_ACCENT);

    if (age > WEATHER_STALE_MS) {          // refresh is failing: say how old the data is
      unsigned long mins = age / 60000UL;
      if (mins < 60) snprintf(buf, sizeof buf, "updated %lu min ago", mins);
      else           snprintf(buf, sizeof buf, "updated %lu h ago", mins / 60);
      drawCentered(g, buf, 115, 1, C_BAD);
    } else {                               // \x18 / \x19 = up / down arrows in the GFX font
      snprintf(buf, sizeof buf, "\x18%d \x19%d  %d%%  %dkm/h", w.dMax[0], w.dMin[0], w.humidity, (int)lroundf(w.wind));
      drawCentered(g, buf, 115, 1, C_DIM);
    }

    // 3-day forecast: [icon DAY] / hi/lo
    for (int i = 1; i <= 3; i++) {
      int16_t cx = 22 + (i - 1) * 42;
      drawWeatherIcon(g, cx - 11, 131, wxKind(w.dCode[i]), true, 0.35f, ts);
      if (haveTime) drawText(g, DAYS_SHORT[(t.tm_wday + i) % 7], cx - 1, 127, 1, C_DIM);
      snprintf(buf, sizeof buf, "%d/%d", w.dMax[i], w.dMin[i]);
      drawCenteredAt(g, buf, cx + 1, 140, 1, C_TEXT);
    }
  }
};

// click L/R = select previous/next day (rolls over into other months),
// re-entering the page resets the selection to today
struct CalendarPage : Page {
  int dayOffset = 0;   // selected day, relative to today

  void enter() override { dayOffset = 0; }
  void action(bool right) override { dayOffset += right ? 1 : -1; }

  void draw(GFXcanvas16& g, uint32_t now) override {
    static const char WDAYS[] = "MTWTFSS";   // Monday-first
    const uint16_t C_WEEKEND = RGB(150, 95, 95);
    const int16_t  X0 = 16, CELL = 16, Y0 = 36;

    struct tm today;
    if (!getLocalTime(&today, 0)) {
      drawCentered(g, "syncing time...", 74, 1, C_DIM);
      return;
    }

    // selected day (mktime normalizes the date and fills tm_wday)
    struct tm sel = {};
    sel.tm_year  = today.tm_year;
    sel.tm_mon   = today.tm_mon;
    sel.tm_mday  = today.tm_mday + dayOffset;
    sel.tm_hour  = 12;
    sel.tm_isdst = -1;
    mktime(&sel);

    // first day of the month being shown
    struct tm m = sel;
    m.tm_mday  = 1;
    m.tm_isdst = -1;
    mktime(&m);

    int year = sel.tm_year + 1900, mon = sel.tm_mon;
    int days     = daysInMonth(year, mon);
    int prevDays = mon == 0 ? 31 : daysInMonth(year, mon - 1);
    int lead     = (m.tm_wday + 6) % 7;               // Monday-first
    int rows     = (lead + days + 6) / 7;
    bool thisMonth = sel.tm_year == today.tm_year && sel.tm_mon == today.tm_mon;

    // header: "NOVEMBER 2026"
    char buf[24];
    snprintf(buf, sizeof buf, " %d", year);
    int16_t wm = textWidth(MONTHS_FULL[mon], 1);
    int16_t hx = (SCR_W - (wm + 1 + textWidth(buf, 1))) / 2;
    drawText(g, MONTHS_FULL[mon], hx, 6, 1, C_ACCENT);
    drawText(g, buf, hx + wm + 1, 6, 1, C_DIM);

    for (int c = 0; c < 7; c++) {
      char s[2] = { WDAYS[c], 0 };
      drawCenteredAt(g, s, X0 + c * CELL + 1, 20, 1, c > 4 ? C_BAD : C_DIM);
    }
    g.drawFastHLine(8, 30, SCR_W - 16, C_FAINT);

    // grid
    for (int i = 0; i < rows * 7; i++) {
      int col = i % 7, x = X0 + col * CELL, y = Y0 + (i / 7) * CELL;
      int d = i - lead + 1;

      if (d < 1 || d > days) {                        // neighbouring months
        snprintf(buf, sizeof buf, "%d", d < 1 ? d + prevDays : d - days);
        drawCenteredAt(g, buf, x + 1, y, 1, C_FAINT);
        continue;
      }

      bool isToday = thisMonth && d == today.tm_mday;
      uint16_t color = col > 4 ? C_WEEKEND : C_TEXT;

      if (isToday) {
        g.fillRoundRect(x - 7, y - 4, 16, 16, 3, C_ACCENT);
        color = C_BG;
      }
      if (d == sel.tm_mday) g.drawRoundRect(x - 7, y - 4, 16, 16, 3, C_TEXT);

      snprintf(buf, sizeof buf, "%d", d);
      drawCenteredAt(g, buf, x + 1, y, 1, color);

      if (holidayName(year, mon, d))                  // marker under the number
        g.fillRect(x - 1, y + 9, 5, 2, isToday ? C_BG : C_HOLIDAY);
    }

    // selected day: holiday name, otherwise just the date
    const char* holiday = holidayName(year, mon, sel.tm_mday);
    if (holiday) {
      drawCenteredWrapped(g, holiday, 142, C_HOLIDAY);
    } else {
      snprintf(buf, sizeof buf, "%s %d %s", DAYS_SHORT[sel.tm_wday], sel.tm_mday, MONTHS_SHORT[mon]);
      drawCentered(g, buf, 138, 1, C_DIM);
    }
  }
};

struct SystemPage : Page {
  void draw(GFXcanvas16& g, uint32_t now) override {
    drawCentered(g, "SYSTEM", 12, 2, C_ACCENT);

    bool wifi = WiFi.status() == WL_CONNECTED;
    char buf[32];
    int16_t y = 42;
    auto row = [&](const char* k, const char* v) {
      drawText(g, k, 4, y, 1, C_DIM);
      drawText(g, v, 36, y, 1, C_TEXT);
      y += 16;
    };

    row("Wi-Fi", wifi ? WiFi.SSID().c_str() : "offline");
    row("IP",    wifi ? WiFi.localIP().toString().c_str() : "-");
    snprintf(buf, sizeof buf, "%d dBm", WiFi.RSSI());
    row("RSSI", wifi ? buf : "-");
    uint32_t s = now / 1000;
    snprintf(buf, sizeof buf, "%luh %02lum %02lus",
             (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
    row("Up", buf);
    snprintf(buf, sizeof buf, "%lu KB", (unsigned long)(ESP.getFreeHeap() / 1024));
    row("Mem.", buf);
    // which paths the PC bridge is reaching the board through
    snprintf(buf, sizeof buf, "USB %s WiFi %s", pc.usbLive() ? "ok" : "--", pc.udpLive() ? "ok" : "--");
    row("PC", buf);
  }
};

// green -> yellow -> red as a percentage fills up
uint16_t levelColor(float pct) { return pct >= 85 ? C_BAD : pct >= 60 ? C_ACCENT : C_OK; }

void drawBar(GFXcanvas16& g, int16_t x, int16_t y, int16_t w, int16_t h, float pct) {
  pct = constrain(pct, 0.f, 100.f);
  g.fillRect(x, y, w, h, C_FAINT);
  g.fillRect(x, y, (int16_t)lroundf(w * pct / 100.f), h, levelColor(pct));
}

void drawRight(GFXcanvas16& g, const char* s, int16_t xRight, int16_t y, uint8_t size, uint16_t color) {
  drawText(g, s, xRight - textWidth(s, size), y, size, color);
}

// sun path for today (sunrise -> sunset) + UV index
struct SunPage : Page {
  static const char* uvLabel(float uv) {
    return uv < 3 ? "LOW" : uv < 6 ? "MODERATE" : uv < 8 ? "HIGH" : uv < 11 ? "VERY HIGH" : "EXTREME";
  }
  static uint16_t uvColor(float uv) {
    return uv < 3 ? C_OK : uv < 6 ? C_ACCENT : uv < 8 ? RGB(255, 130, 30) : uv < 11 ? C_BAD : RGB(190, 90, 255);
  }

  void draw(GFXcanvas16& g, uint32_t now) override {
    drawCentered(g, "SUN", 6, 1, C_ACCENT);

    WeatherData w = weatherFeed.get();
    struct tm t;
    bool haveTime = getLocalTime(&t, 0);
    if (!w.valid || !haveTime || w.sunrise[0] < 0 || w.sunset[0] <= w.sunrise[0]) {
      bool wifi = WiFi.status() == WL_CONNECTED;
      drawCentered(g, !wifi ? "no wi-fi" : !haveTime ? "syncing time..." :
                      weatherFeed.lastFailed ? "weather unavailable" : "loading weather...", 74, 1, C_DIM);
      return;
    }

    const int16_t CX = SCR_W / 2, CY = 78, R = 46;
    int   rise = w.sunrise[0], set = w.sunset[0];
    float nowMin = t.tm_hour * 60 + t.tm_min + t.tm_sec / 60.f;
    bool  day = nowMin >= rise && nowMin < set;
    float frac = day ? (nowMin - rise) / (set - rise) : 0;
    char  buf[32];

    // arc: travelled part solid, the rest dotted
    for (int i = 0; i <= 150; i++) {
      float f = i / 150.f, a = PI * (1 - f);
      int16_t x = PX(CX + cosf(a) * R), y = PX(CY - sinf(a) * R);
      if (day && f <= frac) g.drawPixel(x, y, C_ACCENT);
      else if (i % 5 == 0)  g.drawPixel(x, y, C_DIM);
    }
    g.drawFastHLine(8, CY, SCR_W - 16, C_FAINT);

    int left;   // minutes until the next sunset / sunrise
    if (day) {
      float a = PI * (1 - frac);
      drawSun(g, CX + cosf(a) * R, CY - sinf(a) * R, 0.5f, now / 1000.f);
      left = set - (int)nowMin;
    } else {
      drawMoon(g, CX, CY - 22, 0.8f);
      int next = nowMin < rise ? rise : (w.sunrise[1] >= 0 ? w.sunrise[1] : rise) + 1440;
      left = next - (int)nowMin;
    }

    // sunrise / day length / sunset (\x18 \x19 = up / down arrows)
    snprintf(buf, sizeof buf, "\x18%02d:%02d", rise / 60, rise % 60);
    drawText(g, buf, 6, CY + 6, 1, C_TEXT);
    snprintf(buf, sizeof buf, "\x19%02d:%02d", set / 60, set % 60);
    drawRight(g, buf, SCR_W - 6, CY + 6, 1, C_TEXT);
    snprintf(buf, sizeof buf, "%dh%02d", (set - rise) / 60, (set - rise) % 60);
    drawCentered(g, buf, CY + 6, 1, C_DIM);

    if (left >= 60) snprintf(buf, sizeof buf, "%s in %dh %02dm", day ? "sunset" : "sunrise", left / 60, left % 60);
    else            snprintf(buf, sizeof buf, "%s in %dm", day ? "sunset" : "sunrise", left < 1 ? 1 : left);
    drawCentered(g, buf, CY + 19, 1, C_ACCENT);

    // ---- UV ----
    g.drawFastHLine(6, 110, SCR_W - 12, C_FAINT);
    uint16_t color = uvColor(w.uv);
    drawText(g, "UV", 6, 116, 1, C_DIM);
    snprintf(buf, sizeof buf, "%d", (int)lroundf(w.uv));
    drawText(g, buf, 6, 126, 2, C_TEXT);
    drawRight(g, uvLabel(w.uv), SCR_W - 6, 118, 1, color);
    snprintf(buf, sizeof buf, "max today %d", (int)lroundf(w.uvMax));
    drawRight(g, buf, SCR_W - 6, 130, 1, C_DIM);

    // scale 0-11, tick marks today's maximum
    const int16_t BX = 6, BW = SCR_W - 12, BY = 144;
    g.fillRect(BX, BY, BW, 4, C_FAINT);
    g.fillRect(BX, BY, (int16_t)(BW * constrain(w.uv, 0.f, 11.f) / 11.f), 4, color);
    g.drawFastVLine(BX + (int16_t)((BW - 1) * constrain(w.uvMax, 0.f, 11.f) / 11.f), BY - 2, 8, C_TEXT);
  }
};

struct CryptoPage : Page {
  // 91 234 / 152.34 / 0.4521
  static void formatPrice(char* out, size_t n, float p) {
    if (p >= 1000) {
      long v = lroundf(p);
      if (v >= 1000000) snprintf(out, n, "%ld %03ld %03ld", v / 1000000, v / 1000 % 1000, v % 1000);
      else              snprintf(out, n, "%ld %03ld", v / 1000, v % 1000);
    }
    else if (p >= 1) snprintf(out, n, "%.2f", p);
    else             snprintf(out, n, "%.4f", p);
  }

  void draw(GFXcanvas16& g, uint32_t now) override {
    drawCentered(g, "CRYPTO", 6, 1, C_ACCENT);

    CryptoData c = cryptoFeed.get();
    if (!c.valid) {
      bool wifi = WiFi.status() == WL_CONNECTED;
      drawCentered(g, !wifi ? "no wi-fi" : cryptoFeed.lastFailed ? "no data" : "loading...", 74, 1, C_DIM);
      return;
    }
    bool stale = millis() - c.fetchedAt > CRYPTO_STALE_MS;   // refresh failing: grey out
    char buf[24], num[16];

    for (size_t i = 0; i < COIN_COUNT && i < 3; i++) {
      int16_t y = 22 + i * 42;
      drawText(g, COINS[i].symbol, 6, y, 2, C_ACCENT);

      // \x1E / \x1F = up / down triangles in the GFX font
      snprintf(buf, sizeof buf, "%c%.1f%%", c.change[i] >= 0 ? 0x1E : 0x1F, fabsf(c.change[i]));
      drawRight(g, buf, SCR_W - 6, y + 4, 1, stale ? C_DIM : c.change[i] >= 0 ? C_OK : C_BAD);

      formatPrice(num, sizeof num, c.price[i]);
      snprintf(buf, sizeof buf, "%s €", num);
      drawText(g, buf, 6, y + 19, 2, stale ? C_DIM : C_TEXT);

      if (i < 2 && i + 1 < COIN_COUNT) g.drawFastHLine(6, y + 38, SCR_W - 12, C_FAINT);
    }
  }
};

struct PcPage : Page {
  void draw(GFXcanvas16& g, uint32_t now) override {
    drawCentered(g, "PC", 6, 1, C_ACCENT);

    if (!pc.live()) {
      if (pc.linked()) {   // the bridge is talking but has no hardware stats
        drawCentered(g, "no hardware data", 68, 1, C_DIM);
        drawCentered(g, "(LHM running?)", 80, 1, C_FAINT);
      } else {
        drawCentered(g, "no PC connection", 68, 1, C_DIM);
        drawCentered(g, "(painel_bridge?)", 80, 1, C_FAINT);
      }
      return;
    }

    char right[16], val[8];
    auto row = [&](int16_t y, const char* label, uint16_t labelColor, int pct) {
      drawText(g, label, 6, y, 1, labelColor);
      drawRight(g, right, SCR_W - 6, y, 1, C_DIM);
      snprintf(val, sizeof val, "%d%%", pct);
      drawText(g, val, 6, y + 10, 2, C_TEXT);
      drawBar(g, 62, y + 13, 60, 8, pct);
    };

    // label colors double as the graph legend
    snprintf(right, sizeof right, "%d°C", pc.cpuT);
    row(20, "CPU", C_ACCENT, pc.cpu);
    snprintf(right, sizeof right, "%d°C", pc.gpuT);
    row(50, "GPU", C_HOLIDAY, pc.gpu);
    snprintf(right, sizeof right, "%.1f GB", pc.ramGb);
    row(80, "RAM", C_DIM, pc.ramPct);

    // CPU / GPU load history, newest on the right
    const int16_t GX = 6, GY = 110, GH = 36;
    g.drawFastHLine(GX, GY + GH, PC_HIST, C_FAINT);
    g.drawFastHLine(GX, GY + GH / 2, PC_HIST, C_FAINT);
    for (int pass = 0; pass < 2; pass++) {
      const uint8_t* hist = pass == 0 ? pc.histGpu : pc.histCpu;   // CPU drawn on top
      uint16_t color = pass == 0 ? C_HOLIDAY : C_ACCENT;
      int16_t px = 0, py = 0;
      for (int k = 0; k < pc.histCount; k++) {
        int idx = (pc.histHead - pc.histCount + k + PC_HIST) % PC_HIST;
        int16_t x = GX + (PC_HIST - pc.histCount) + k;
        int16_t y = GY + GH - 1 - hist[idx] * (GH - 1) / 100;
        if (k) g.drawLine(px, py, x, y, color);
        px = x; py = y;
      }
    }
  }
};

struct UsagePage : Page {
  // 2d 4h / 2h 13m / 13m
  static void formatLeft(char* out, size_t n, long s) {
    if (s >= 86400)     snprintf(out, n, "resets in%ldd %ldh", s / 86400, s % 86400 / 3600);
    else if (s >= 3600) snprintf(out, n, "resets in%ldh %02ldm", s / 3600, s % 3600 / 60);
    else if (s > 0)     snprintf(out, n, "resets in%ldm", (s + 59) / 60);
    else                snprintf(out, n, "resetting...");
  }

  void meter(GFXcanvas16& g, int16_t y, const char* label, float pct, int32_t resetIn, uint32_t ageS) {
    char buf[32];
    drawText(g, label, 6, y, 1, C_DIM);
    drawBar(g, 24, y, 66, 7, pct);
    snprintf(buf, sizeof buf, "%d%%", (int)lroundf(pct));
    drawRight(g, buf, SCR_W - 6, y, 1, C_TEXT);
    if (resetIn >= 0) {
      formatLeft(buf, sizeof buf, (long)resetIn - (long)ageS);
      drawText(g, buf, 24, y + 10, 1, C_DIM);
    }
  }

  void section(GFXcanvas16& g, int16_t y, const char* name, const UsageData& u) {
    drawText(g, name, 6, y, 1, C_ACCENT);
    if (!u.live()) {
      if (pc.linked()) {   // the bridge is talking, so it could not get this one's usage
        drawText(g, "no data", 6, y + 16, 1, C_DIM);
        drawText(g, "(see bridge window)", 6, y + 27, 1, C_FAINT);
      } else {
        drawText(g, "no PC connection", 6, y + 16, 1, C_DIM);
        drawText(g, "(painel_bridge?)", 6, y + 27, 1, C_FAINT);
      }
      return;
    }
    uint32_t ageS = (millis() - u.receivedAt) / 1000;
    meter(g, y + 13, "5h", u.sessionPct, u.sessionResetIn, ageS);
    meter(g, y + 36, "7d", u.weeklyPct,  u.weeklyResetIn,  ageS);
  }

  void draw(GFXcanvas16& g, uint32_t now) override {
    drawCentered(g, "AI USAGE", 6, 1, C_DIM);
    section(g, 22, "CLAUDE", claudeUsage);
    g.drawFastHLine(6, 82, SCR_W - 12, C_FAINT);
    section(g, 90, "CODEX", codexUsage);
  }
};

DashboardPage pageDashboard;
SunPage       pageSun;
CalendarPage  pageCalendar;
CryptoPage    pageCrypto;
PcPage        pagePc;
UsagePage     pageUsage;
SystemPage    pageSystem;

Page* pages[] = { &pageDashboard, &pageSun, &pageCalendar, &pageCrypto, &pagePc, &pageUsage, &pageSystem };
const uint8_t PAGE_COUNT = sizeof(pages) / sizeof(pages[0]);
uint8_t current = 0;

void goToPage(int idx) {
  current = (idx + PAGE_COUNT) % PAGE_COUNT;
  pages[current]->enter();
}

void drawPageDots(GFXcanvas16& g) {
  const int16_t spacing = 8, y = SCR_H - 5;
  int16_t x = (SCR_W - (PAGE_COUNT - 1) * spacing) / 2;
  for (uint8_t i = 0; i < PAGE_COUNT; i++, x += spacing) {
    if (i == current) g.fillCircle(x, y, 2, C_TEXT);
    else              g.fillCircle(x, y, 1, C_DIM);
  }
}

// ================= MAIN =================
void setup() {
  Serial.begin(115200);

  btnL.begin();
  btnR.begin();
  led.begin();

  SPI.begin(TFT_SCK, -1, TFT_MOSI, TFT_CS);
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(0);   // portrait 128x160
  tft.setSPISpeed(27000000);
  tft.fillScreen(C_BG);

  if (!fb.getBuffer()) {
    Serial.println("failed to allocate the framebuffer");
    while (true) delay(1000);
  }
  fb.setTextWrap(false);
  fb.cp437(true);   // real CP437 glyph codes (° = 0xF8)

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.setSleep(false);   // modem sleep drops UDP packets from the PC
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");
  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);

  pages[current]->enter();
}

void loop() {
  // hold = change page, click = interact with the current page
  pc.poll();
  uint8_t evL = btnL.poll(), evR = btnR.poll();
  if (evL == EV_LONG) { goToPage((int)current - 1); led.flash(); }
  if (evR == EV_LONG)  { goToPage((int)current + 1); led.flash(); }
  if (evL == EV_SHORT) pages[current]->action(false);
  if (evR == EV_SHORT) pages[current]->action(true);

  static uint32_t lastFrame = 0;
  uint32_t now = millis();
  if (now - lastFrame < FRAME_MS) return;
  lastFrame = now;

  led.update(now);
  fb.fillScreen(C_BG);
  pages[current]->draw(fb, now);
  drawPageDots(fb);
  tft.drawRGBBitmap(0, 0, fb.getBuffer(), SCR_W, SCR_H);
}
