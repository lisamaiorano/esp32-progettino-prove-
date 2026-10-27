#include <Arduino.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <HTTPClient.h>
#include <TJpg_Decoder.h>

// ====== CONFIGURA QUI ======
const char* WIFI_SSID = "TIM-29399329";
const char* WIFI_PASS = "bWJVTK56XbnKS4GYWyKHI0HJ";
const char* FRAME_URL = "http://192.168.1.179:8000/frame.jpg";
// ===========================

// ---- Display ST7796S 320x480 (pin dal wiki della ES3C40P) ----
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796 _panel;
  lgfx::Bus_SPI      _bus;
  lgfx::Light_PWM    _light;

public:
  LGFX() {
    auto b = _bus.config();
    b.spi_host   = SPI2_HOST;
    b.freq_write = 80000000;
    b.freq_read  = 16000000;
    b.pin_sclk = 12;
    b.pin_mosi = 11;
    b.pin_miso = 13;
    b.pin_dc   = 46;
    _bus.config(b);
    _panel.setBus(&_bus);

    auto p = _panel.config();
    p.pin_cs  = 10;
    p.pin_rst = -1;          // reset condiviso con EN
    p.panel_width  = 320;
    p.panel_height = 480;
    p.invert    = true;      // colori invertiti? metti false
    p.rgb_order = false;     // rosso e blu scambiati? metti true
    _panel.config(p);

    auto l = _light.config();
    l.pin_bl = 45;           // backlight
    _light.config(l);
    _panel.setLight(&_light);

    setPanel(&_panel);
  }
};

LGFX tft;

// ---- Streaming ----
static const size_t BUF_SIZE = 128 * 1024;  // max dimensione di un frame JPEG
uint8_t* frameBuf = nullptr;

bool onBlock(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bmp) {
  tft.pushImage(x, y, w, h, bmp);
  return true;
}

void showMessage(const char* msg, uint32_t color = TFT_WHITE) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(color);
  tft.setTextSize(2);
  tft.setCursor(10, 200);
  tft.print(msg);
}

void setupStream() {
  frameBuf = (uint8_t*)ps_malloc(BUF_SIZE);
  if (!frameBuf) {
    showMessage("PSRAM non disponibile", TFT_RED);
    Serial.println("ps_malloc fallita: controlla platformio.ini");
    while (true) delay(1000);
  }

  showMessage("Connessione Wi-Fi...", TFT_YELLOW);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(200);

  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
  showMessage("Connesso!", TFT_GREEN);
  neopixelWrite(42, 0, 20, 0);  // LED RGB verde = connesso
  delay(500);

  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);   // colori sbagliati? metti false
  TJpgDec.setCallback(onBlock);
}

void streamFrame() {
  if (WiFi.status() != WL_CONNECTED) {
    neopixelWrite(42, 20, 0, 0);  // LED rosso = Wi-Fi perso
    WiFi.reconnect();
    delay(1000);
    return;
  }

  HTTPClient http;
  http.setReuse(true);
  http.begin(FRAME_URL);
  int code = http.GET();

  if (code == 200) {
    int len = http.getSize();
    if (len > 0 && (size_t)len <= BUF_SIZE) {
      WiFiClient* stream = http.getStreamPtr();
      size_t got = stream->readBytes(frameBuf, len);
      if (got == (size_t)len) {
        TJpgDec.drawJpg(0, 0, frameBuf, len);
      }
    }
  } else {
    Serial.printf("HTTP error: %d\n", code);
    delay(500);
  }
  http.end();
}

void setup() {
  Serial.begin(115200);

  tft.init();
  tft.setRotation(1);
  tft.setBrightness(200);
  tft.fillScreen(TFT_BLACK);
  neopixelWrite(42, 0, 0, 20);  // LED RGB blu = avvio

  setupStream();
}

void loop() {
  streamFrame();
}