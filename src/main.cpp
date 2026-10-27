#include <Arduino.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <JPEGDEC.h>

// ====== CONFIGURA QUI ======
const char* WIFI_SSID = "TIM-29399329";
const char* WIFI_PASS = "bWJVTK56XbnKS4GYWyKHI0HJ";
const uint16_t PORT   = 5000;   // deve coincidere con stream.py
// ===========================

// ---- Display ST7796S (pin dal wiki della ES3C40P) ----
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796 _panel;
  lgfx::Bus_SPI      _bus;
  lgfx::Light_PWM    _light;

public:
  LGFX() {
    auto b = _bus.config();
    b.spi_host   = SPI2_HOST;
    b.freq_write = 80000000;   // se compaiono righe/pixel sbagliati, metti 40000000
    b.freq_read  = 16000000;
    b.pin_sclk = 12;
    b.pin_mosi = 11;
    b.pin_miso = 13;
    b.pin_dc   = 46;
    _bus.config(b);
    _panel.setBus(&_bus);

    auto p = _panel.config();
    p.pin_cs  = 10;
    p.pin_rst = -1;
    p.panel_width  = 320;
    p.panel_height = 480;
    p.invert    = true;
    p.rgb_order = false;
    _panel.config(p);

    auto l = _light.config();
    l.pin_bl = 45;
    _light.config(l);
    _panel.setLight(&_light);

    setPanel(&_panel);
  }
};

LGFX tft;
JPEGDEC jpeg;
WiFiServer server(PORT);
WiFiClient client;

static const size_t BUF_SIZE = 128 * 1024;  // max dimensione di un frame JPEG
uint8_t* frameBuf = nullptr;

// Tempo speso a scrivere sul display (microsecondi), per le statistiche
uint32_t pushUs = 0;

// Chiamata da JPEGDEC per ogni blocco decodificato
int drawMCU(JPEGDRAW* d) {
  uint32_t s = micros();
  tft.pushImage(d->x, d->y, d->iWidth, d->iHeight, d->pPixels);
  pushUs += micros() - s;
  return 1;
}

void showMessage(const char* msg, uint32_t color = TFT_WHITE) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(color);
  tft.setTextSize(2);
  tft.setCursor(10, 120);
  tft.print(msg);
}

// Legge esattamente n byte dal client, con timeout.
bool readExact(WiFiClient& c, uint8_t* dst, size_t n, uint32_t timeoutMs) {
  size_t got = 0;
  uint32_t last = millis();
  while (got < n) {
    int avail = c.available();
    if (avail > 0) {
      int r = c.read(dst + got, min((size_t)avail, n - got));
      if (r > 0) {
        got += r;
        last = millis();
      }
    } else {
      if (!c.connected() || millis() - last > timeoutMs) return false;
      delay(1);
    }
  }
  return true;
}

void waitingScreen() {
  String msg = "In attesa del Mac...\n\nIP: " + WiFi.localIP().toString();
  showMessage(msg.c_str(), TFT_GREEN);
  neopixelWrite(42, 0, 20, 0);  // verde = pronto
}

void setup() {
  Serial.begin(115200);

  tft.init();
  tft.setRotation(1);           // orizzontale 480x320 (se capovolto, metti 3)
  tft.setBrightness(200);
  tft.fillScreen(TFT_BLACK);
  neopixelWrite(42, 0, 0, 20);  // blu = avvio

  frameBuf = (uint8_t*)ps_malloc(BUF_SIZE);
  if (!frameBuf) {
    showMessage("PSRAM non disponibile", TFT_RED);
    while (true) delay(1000);
  }

  showMessage("Connessione Wi-Fi...", TFT_YELLOW);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);         // niente risparmio energetico: meno latenza
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(200);

  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  server.begin();
  waitingScreen();
}

uint32_t frames = 0;
uint32_t lastReport = 0;
uint32_t tWait = 0, tRecv = 0, tDec = 0, tScr = 0;
char stats[110] = "";

void loop() {
  // Attendi che lo script sul Mac si colleghi
  if (!client || !client.connected()) {
    client = server.available();
    if (!client) {
      delay(10);
      return;
    }
    client.setNoDelay(true);
    Serial.println("Mac collegato");
    tft.fillScreen(TFT_BLACK);
    frames = 0;
    lastReport = millis();
  }

  // Ogni frame: 4 byte con la lunghezza, poi il JPEG
  uint32_t t0 = millis();
  uint8_t hdr[4];
  if (!readExact(client, hdr, 4, 5000)) {
    client.stop();
    waitingScreen();
    return;
  }
  uint32_t len = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                 ((uint32_t)hdr[2] << 8) | hdr[3];
  uint32_t t1 = millis();
  if (len == 0 || len > BUF_SIZE) {
    Serial.printf("Lunghezza frame non valida: %u\n", (unsigned)len);
    client.stop();
    waitingScreen();
    return;
  }
  if (!readExact(client, frameBuf, len, 2000)) {
    client.stop();
    waitingScreen();
    return;
  }

  uint32_t t2 = millis();
  pushUs = 0;
  tft.startWrite();
  if (jpeg.openRAM(frameBuf, len, drawMCU)) {
    jpeg.setPixelType(RGB565_BIG_ENDIAN);  // colori sbagliati? prova RGB565_LITTLE_ENDIAN
    jpeg.decode(0, 0, 0);
    jpeg.close();
  } else {
    Serial.printf("JPEG non valido, errore %d\n", jpeg.getLastError());
  }
  tft.endWrite();
  uint32_t t3 = millis();

  uint32_t scr = pushUs / 1000;
  uint32_t total = t3 - t2;
  tWait += t1 - t0;
  tRecv += t2 - t1;
  tScr += scr;
  tDec += (total > scr) ? total - scr : 0;
  frames++;

  if (millis() - lastReport >= 2000) {
    snprintf(stats, sizeof(stats),
             "%.1f fps | attesa %u, ricez. %u, decod. %u, schermo %u ms",
             frames * 1000.0f / (millis() - lastReport),
             (unsigned)(tWait / frames), (unsigned)(tRecv / frames),
             (unsigned)(tDec / frames), (unsigned)(tScr / frames));
    Serial.println(stats);
    frames = 0;
    tWait = tRecv = tDec = tScr = 0;
    lastReport = millis();
  }

  // Scritta gialla in basso con le ultime misure
  if (stats[0]) {
    tft.fillRect(0, 306, 480, 14, TFT_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(TFT_YELLOW);
    tft.setCursor(2, 309);
    tft.print(stats);
  }
}