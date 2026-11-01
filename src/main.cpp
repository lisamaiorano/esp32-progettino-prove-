// Assistente da scrivania - ESP32-S3 4.0" (ES3C40P)
// Pagina 0: orologio + data con immagine di sfondo
// Pagina 1: griglia di icone delle app (tap = segnaposto per ora)
// Swipe sinistra/destra per cambiare pagina (con giro: 2 pagine -> entrambe le direzioni funzionano)

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <math.h>
#include <LittleFS.h>
#include <JPEGDEC.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// ---------- CONFIGURAZIONE ----------
const char* WIFI_SSID = "TIM-29399329";
const char* WIFI_PASS = "bWJVTK56XbnKS4GYWyKHI0HJ";
const char* TZ_ROMA   = "CET-1CEST,M3.5.0,M10.5.0/3";  // ora italiana con ora legale

// Se i colori dello sfondo sono sbagliati, prova RGB565_LITTLE_ENDIAN
#define JPEG_PIXEL_TYPE RGB565_BIG_ENDIAN

static const int W = 480, H = 320;
static const int SWIPE_MIN = 60;  // pixel minimi per riconoscere uno swipe

// ---------- DISPLAY + TOUCH ----------
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796   _panel;
  lgfx::Bus_SPI        _bus;
  lgfx::Light_PWM      _light;
  lgfx::Touch_FT5x06   _touch;  // compatibile con FT6336U (indirizzo 0x38)
public:
  LGFX() {
    {
      auto c = _bus.config();
      c.spi_host = SPI2_HOST; c.spi_mode = 0;
      c.freq_write = 80000000; c.freq_read = 16000000;
      c.spi_3wire = false; c.use_lock = true; c.dma_channel = SPI_DMA_CH_AUTO;
      c.pin_sclk = 12; c.pin_mosi = 11; c.pin_miso = 13; c.pin_dc = 46;
      _bus.config(c);
      _panel.setBus(&_bus);
    }
    {
      auto c = _panel.config();
      c.pin_cs = 10; c.pin_rst = -1; c.pin_busy = -1;
      c.panel_width = 320; c.panel_height = 480;
      c.memory_width = 320; c.memory_height = 480;
      c.offset_x = 0; c.offset_y = 0; c.offset_rotation = 0;
      c.readable = false;
      c.invert = true;       // <-- COPIA qui il valore che usi in main_v2.cpp
      c.rgb_order = false;   // <-- COPIA qui il valore che usi in main_v2.cpp
      c.dlen_16bit = false; c.bus_shared = false;
      _panel.config(c);
    }
    {
      auto c = _light.config();
      c.pin_bl = 45; c.invert = false; c.freq = 44100; c.pwm_channel = 7;
      _light.config(c);
      _panel.setLight(&_light);
    }
    {
      auto c = _touch.config();
      c.x_min = 0; c.x_max = 319; c.y_min = 0; c.y_max = 479;
      c.pin_int = 17; c.bus_shared = false; c.offset_rotation = 0;
      c.i2c_port = 1; c.i2c_addr = 0x38;
      c.pin_sda = 16; c.pin_scl = 15; c.freq = 400000;
      _touch.config(c);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

LGFX tft;
LGFX_Sprite bgSpr(&tft), pageA(&tft), pageB(&tft);
LGFX_Sprite *front = &pageA, *back = &pageB;
JPEGDEC jpeg;

// ---------- COLORI ----------
static inline uint16_t C(uint8_t r, uint8_t g, uint8_t b) { return lgfx::color565(r, g, b); }
const uint16_t WHITE = 0xFFFF, BLACK = 0x0000;

// ---------- PAGINE ----------
enum { PAGE_CLOCK, PAGE_APPS, PAGE_COUNT };
int page = PAGE_CLOCK;
time_t lastSec = 0;
String toastText;
uint32_t toastUntil = 0;

const char* GIORNI[] = {"Dom", "Lun", "Mar", "Mer", "Gio", "Ven", "Sab"};  // niente accenti: i font base non li hanno
const char* MESI[]   = {"gennaio", "febbraio", "marzo", "aprile", "maggio", "giugno",
                        "luglio", "agosto", "settembre", "ottobre", "novembre", "dicembre"};

// ---------- SFONDO ----------
int jpegDraw(JPEGDRAW* d) {
  bgSpr.pushImage(d->x, d->y, d->iWidth, d->iHeight, d->pPixels);
  return 1;
}

void gradientBackground() {
  for (int y = 0; y < H; y++) {
    uint8_t k = map(y, 0, H - 1, 0, 255);
    bgSpr.drawFastHLine(0, y, W, C(10 + k / 12, 20 + k / 8, 60 + k / 3));
  }
}

bool loadBackground() {
  if (!LittleFS.begin(false)) return false;
  File f = LittleFS.open("/bg.jpg", "r");
  if (!f) return false;
  size_t n = f.size();
  uint8_t* buf = (uint8_t*)ps_malloc(n);
  if (!buf) { f.close(); return false; }
  f.read(buf, n);
  f.close();
  bool ok = false;
  if (jpeg.openRAM(buf, n, jpegDraw)) {
    jpeg.setPixelType(JPEG_PIXEL_TYPE);
    ok = jpeg.decode(0, 0, 0);
    jpeg.close();
  }
  free(buf);
  return ok;
}

// ---------- TESTO ----------
void shadowText(LGFX_Sprite& s, const char* txt, int x, int y, const lgfx::IFont* font,
                float size, textdatum_t datum, uint16_t col) {
  s.setFont(font);
  s.setTextSize(size);
  s.setTextDatum(datum);
  s.setTextColor(BLACK);
  s.drawString(txt, x + 2, y + 2);
  s.setTextColor(col);
  s.drawString(txt, x, y);
}

void drawDots(LGFX_Sprite& s, int cur) {
  const int gap = 18;
  int x0 = W / 2 - (PAGE_COUNT - 1) * gap / 2;
  for (int i = 0; i < PAGE_COUNT; i++)
    s.fillCircle(x0 + i * gap, 306, 4, i == cur ? WHITE : C(120, 120, 120));
}

// ---------- ICONE (disegnate con primitive, nessun file) ----------
typedef void (*IconFn)(LGFX_Sprite&, int, int, uint16_t);

void iconMeteo(LGFX_Sprite& s, int cx, int cy, uint16_t bg) {
  s.fillCircle(cx + 8, cy - 10, 11, C(255, 214, 10));
  s.fillCircle(cx - 10, cy + 8, 12, WHITE);
  s.fillCircle(cx + 6, cy + 4, 15, WHITE);
  s.fillCircle(cx + 20, cy + 12, 10, WHITE);
  s.fillRect(cx - 10, cy + 8, 30, 14, WHITE);
}
void iconTimer(LGFX_Sprite& s, int cx, int cy, uint16_t bg) {
  s.fillCircle(cx, cy + 4, 24, WHITE);
  s.fillCircle(cx, cy + 4, 19, bg);
  s.fillRect(cx - 5, cy - 26, 10, 6, WHITE);
  s.drawLine(cx, cy + 4, cx, cy - 10, WHITE);
  s.drawLine(cx + 1, cy + 4, cx + 1, cy - 10, WHITE);
  s.drawLine(cx, cy + 4, cx + 9, cy + 9, WHITE);
}
void iconNote(LGFX_Sprite& s, int cx, int cy, uint16_t bg) {
  s.fillRoundRect(cx - 18, cy - 24, 36, 48, 4, WHITE);
  for (int i = 0; i < 4; i++) s.fillRect(cx - 12, cy - 14 + i * 10, 24, 3, bg);
}
void iconMusica(LGFX_Sprite& s, int cx, int cy, uint16_t bg) {
  s.fillCircle(cx - 8, cy + 14, 9, WHITE);
  s.fillRect(cx, cy - 22, 4, 36, WHITE);
  s.fillRect(cx, cy - 22, 18, 6, WHITE);
}
void iconAI(LGFX_Sprite& s, int cx, int cy, uint16_t bg) {
  s.fillRoundRect(cx - 24, cy - 20, 48, 34, 10, WHITE);
  s.fillTriangle(cx - 12, cy + 12, cx - 2, cy + 12, cx - 14, cy + 24, WHITE);
  for (int i = -1; i <= 1; i++) s.fillCircle(cx + i * 12, cy - 3, 3, bg);
}
void iconImpostazioni(LGFX_Sprite& s, int cx, int cy, uint16_t bg) {
  for (int k = 0; k < 8; k++) {
    float a = k * M_PI / 4;
    s.fillCircle(cx + cosf(a) * 20, cy + sinf(a) * 20, 5, WHITE);
  }
  s.fillCircle(cx, cy, 18, WHITE);
  s.fillCircle(cx, cy, 8, bg);
}

struct App { const char* name; IconFn icon; uint16_t color; };
App apps[6];
const int APP_CX[3] = {80, 240, 400};
const int APP_CY[2] = {100, 220};
const int TILE = 84;

void initApps() {
  apps[0] = {"Meteo",    iconMeteo,         C(10, 132, 255)};
  apps[1] = {"Timer",    iconTimer,         C(255, 149, 0)};
  apps[2] = {"Note",     iconNote,          C(230, 180, 0)};
  apps[3] = {"Musica",   iconMusica,        C(255, 55, 95)};
  apps[4] = {"Assistente", iconAI,          C(94, 92, 230)};
  apps[5] = {"Impostazioni", iconImpostazioni, C(110, 110, 120)};
}

// ---------- RENDER PAGINE ----------
void renderClock(LGFX_Sprite& s) {
  bgSpr.pushSprite(&s, 0, 0);

  struct tm t;
  char hm[8], ss[4], date[48];
  if (getLocalTime(&t, 0)) {
    snprintf(hm, sizeof(hm), "%02d:%02d", t.tm_hour, t.tm_min);
    snprintf(ss, sizeof(ss), "%02d", t.tm_sec);
    snprintf(date, sizeof(date), "%s %d %s %d", GIORNI[t.tm_wday], t.tm_mday, MESI[t.tm_mon], t.tm_year + 1900);
  } else {
    strcpy(hm, "--:--"); strcpy(ss, "--"); strcpy(date, "sincronizzazione...");
  }

  s.setFont(&fonts::Font7);
  s.setTextSize(2);
  int wHm = s.textWidth(hm);
  int total = wHm + 12 + 44;
  int left = (W - total) / 2;
  shadowText(s, hm, left, 125, &fonts::Font7, 2, textdatum_t::middle_left, WHITE);
  shadowText(s, ss, left + wHm + 12, 160, &fonts::FreeSansBold18pt7b, 1, textdatum_t::bottom_left, C(220, 220, 220));
  shadowText(s, date, W / 2, 235, &fonts::FreeSansBold18pt7b, 1, textdatum_t::middle_center, WHITE);

  // pallino stato Wi-Fi in alto a destra
  s.fillCircle(W - 14, 14, 5, WiFi.status() == WL_CONNECTED ? C(52, 199, 89) : C(255, 69, 58));
  drawDots(s, PAGE_CLOCK);
}

void renderApps(LGFX_Sprite& s) {
  bgSpr.pushSprite(&s, 0, 0);
  s.fillRectAlpha(0, 0, W, H, 150, BLACK);
  shadowText(s, "Applicazioni", 16, 12, &fonts::FreeSansBold12pt7b, 1, textdatum_t::top_left, WHITE);

  for (int i = 0; i < 6; i++) {
    int cx = APP_CX[i % 3], cy = APP_CY[i / 3];
    s.fillRoundRect(cx - TILE / 2, cy - TILE / 2, TILE, TILE, 18, apps[i].color);
    apps[i].icon(s, cx, cy, apps[i].color);
    shadowText(s, apps[i].name, cx, cy + TILE / 2 + 14, &fonts::FreeSans9pt7b, 1, textdatum_t::middle_center, WHITE);
  }

  if (millis() < toastUntil) {
    s.fillRoundRect(80, 262, 320, 32, 10, C(30, 30, 30));
    s.setFont(&fonts::FreeSans9pt7b);
    s.setTextSize(1);
    s.setTextDatum(textdatum_t::middle_center);
    s.setTextColor(WHITE);
    s.drawString(toastText.c_str(), W / 2, 278);
  }
  drawDots(s, PAGE_APPS);
}

void renderPage(int p, LGFX_Sprite& s) {
  if (p == PAGE_CLOCK) renderClock(s);
  else renderApps(s);
}

void redrawCurrent() {
  renderPage(page, *front);
  front->pushSprite(0, 0);
}

// ---------- TRANSIZIONE ----------
void goToPage(int next, int dir) {  // dir > 0: la nuova pagina entra da destra
  renderPage(next, *back);
  const int steps = 10;
  tft.startWrite();
  for (int i = 1; i <= steps; i++) {
    float t = i / (float)steps;
    t = 1 - (1 - t) * (1 - t);  // ease-out
    int off = (int)(t * W);
    if (dir > 0) { front->pushSprite(-off, 0); back->pushSprite(W - off, 0); }
    else         { front->pushSprite(off, 0);  back->pushSprite(-W + off, 0); }
  }
  tft.endWrite();
  std::swap(front, back);
  page = next;
}

// ---------- TOUCH ----------
void onTap(int x, int y) {
  if (page != PAGE_APPS) return;
  for (int i = 0; i < 6; i++) {
    int cx = APP_CX[i % 3], cy = APP_CY[i / 3];
    if (abs(x - cx) < TILE / 2 + 6 && abs(y - cy) < TILE / 2 + 6) {
      toastText = String(apps[i].name) + ": in arrivo";
      toastUntil = millis() + 1500;
      redrawCurrent();
      return;
    }
  }
}

void handleTouch() {
  static bool down = false;
  static int sx, sy, lx, ly;
  static uint32_t t0;
  int32_t x, y;
  if (tft.getTouch(&x, &y)) {
    if (!down) { down = true; sx = x; sy = y; t0 = millis(); }
    lx = x; ly = y;
  } else if (down) {
    down = false;
    int dx = lx - sx, dy = ly - sy;
    if (abs(dx) > SWIPE_MIN && abs(dx) > abs(dy) * 3 / 2) {
      if (dx < 0) goToPage((page + 1) % PAGE_COUNT, +1);              // swipe verso sinistra
      else        goToPage((page + PAGE_COUNT - 1) % PAGE_COUNT, -1);  // swipe verso destra
    } else if (abs(dx) < 15 && abs(dy) < 15 && millis() - t0 < 500) {
      onTap(sx, sy);
    }
  }
}

// ---------- SETUP / LOOP ----------
void setup() {
  Serial.begin(115200);

  // reset del touch controller (IO18) prima di inizializzare il display
  pinMode(18, OUTPUT);
  digitalWrite(18, LOW); delay(10);
  digitalWrite(18, HIGH); delay(100);

  tft.init();
  tft.setRotation(1);  // 480x320 orizzontale (usa 3 se risulta capovolto)
  tft.setBrightness(200);
  tft.fillScreen(BLACK);

  for (LGFX_Sprite* s : {&bgSpr, &pageA, &pageB}) {
    s->setPsram(true);
    s->setColorDepth(16);
    if (!s->createSprite(W, H)) Serial.println("Sprite: memoria insufficiente!");
  }

  if (!loadBackground()) {
    Serial.println("bg.jpg non trovato/decodificabile: uso sfondo sfumato");
    gradientBackground();
  }
  bgSpr.fillRectAlpha(0, 0, W, H, 70, BLACK);  // leggero scurimento per leggere meglio il testo

  initApps();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  configTzTime(TZ_ROMA, "pool.ntp.org", "time.google.com");

  redrawCurrent();
}

void loop() {
  handleTouch();

  if (page == PAGE_CLOCK) {
    time_t now = time(nullptr);
    if (now != lastSec) { lastSec = now; redrawCurrent(); }
  } else if (toastUntil && millis() > toastUntil) {
    toastUntil = 0;
    redrawCurrent();
  }
  delay(5);
}