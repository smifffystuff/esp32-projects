// Remote-controlled display for the ESP32-C6-Touch-LCD-1.47 (Waveshare-compatible board).
// Display: JD9853, driven via the Arduino_GFX ST7789-compatible driver.
// Connects to Wi-Fi, shows its IP address, then runs a tiny web server: POST some text
// to it (as form field "text") and it's shown full-screen. Landscape only, auto-flips
// between the two landscape orientations via the onboard QMI8658A IMU.

#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <QMI8658.h>
#include "secrets.h"

#define IMU_SDA 18
#define IMU_SCL 19

QMI8658 imu;
bool imuReady = false;

bool initIMU() {
  imuReady = imu.begin(IMU_SDA, IMU_SCL, QMI8658_ADDRESS_HIGH);
  if (!imuReady) {
    return false;
  }
  imu.setAccelRange(QMI8658_ACCEL_RANGE_8G);
  imu.setAccelODR(QMI8658_ACCEL_ODR_1000HZ);
  imu.setAccelUnit_mg(true);
  imu.enableAccel(true);
  return true;
}

// Pins per the board's documented wiring (JD9853 LCD over SPI).
#define PIN_LCD_SCK   1
#define PIN_LCD_MOSI  2
#define PIN_LCD_MISO  3
#define PIN_LCD_CS    14
#define PIN_LCD_DC    15
#define PIN_LCD_RST   22
#define PIN_LCD_BL    23
#define PIN_SD_CS     4   // shares the SPI bus with the LCD - must be held HIGH (deselected)

Arduino_DataBus *bus = new Arduino_HWSPI(
    PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_SCK, PIN_LCD_MOSI, PIN_LCD_MISO);

// Confirmed against the physical board (see analog_clock/digital_clock project notes):
// rotation 7 = MADCTL_MV only -> landscape; rotation 5 = MX+MY+MV -> its 180-flip pair.
// No invertDisplay() call needed - ips=false already leaves this panel correctly non-inverted.
Arduino_GFX *gfx = new Arduino_ST7789(
    bus, PIN_LCD_RST, 7 /* rotation */, false /* IPS */,
    172 /* width */, 320 /* height */,
    34 /* col offset 1 */, 0 /* row offset 1 */,
    34 /* col offset 2 */, 0 /* row offset 2 */);

Arduino_Canvas *canvas = new Arduino_Canvas(320, 172, gfx);

WebServer server(80);
String currentMessage = "Ready";

// Scrolling ticker state - recomputed whenever the message changes via setMessage().
const uint8_t TICKER_TEXT_SIZE = 4;
const int32_t SCROLL_STEP = 4;
const unsigned long SCROLL_INTERVAL_MS = 30;
int32_t scrollX = 320;
int16_t textX1 = 0;
uint16_t textWidth = 0;
unsigned long lastScrollUpdate = 0;

void setMessage(const String &msg) {
  currentMessage = msg;
  canvas->setTextSize(TICKER_TEXT_SIZE);
  int16_t y1;
  uint16_t h;
  canvas->getTextBounds(currentMessage, 0, 0, &textX1, &y1, &textWidth, &h);
  scrollX = 320; // start just off the right edge
}

// Auto-flip between the two landscape orientations via the IMU's X axis (gravity loads
// onto X when held landscape). Thresholds calibrated against the physical board in the
// analog_clock project: AX ~+990mg -> rotation 7, AX ~-987mg -> rotation 5.
const float ORIENTATION_THRESHOLD_MG = 500.0;
uint8_t currentRotation = 7;

void updateOrientation() {
  if (!imuReady) return;

  float ax, ay, az;
  if (!imu.readAccel(ax, ay, az)) return;

  uint8_t desiredRotation = currentRotation;
  if (ax > ORIENTATION_THRESHOLD_MG) {
    desiredRotation = 7;
  } else if (ax < -ORIENTATION_THRESHOLD_MG) {
    desiredRotation = 5;
  }

  if (desiredRotation != currentRotation) {
    currentRotation = desiredRotation;
    gfx->setRotation(currentRotation);
  }
}

void drawStaticMessage(const char *line1, const char *line2 = nullptr) {
  canvas->fillScreen(RGB565_BLACK);
  canvas->setTextColor(RGB565_WHITE);

  canvas->setTextSize(3);
  int16_t x1, y1;
  uint16_t w, h;
  canvas->getTextBounds(line1, 0, 0, &x1, &y1, &w, &h);
  int16_t cx = (320 - w) / 2 - x1;
  int16_t cy = line2 ? 50 - y1 : (172 - h) / 2 - y1;
  canvas->setCursor(cx, cy);
  canvas->print(line1);

  if (line2) {
    canvas->setTextSize(1);
    canvas->getTextBounds(line2, 0, 0, &x1, &y1, &w, &h);
    cx = (320 - w) / 2 - x1;
    canvas->setCursor(cx, 10);
    canvas->print(line2);
  }

  canvas->flush();
}

void drawTicker() {
  canvas->fillScreen(RGB565_BLACK);
  canvas->setTextColor(RGB565_WHITE);

  canvas->setTextSize(1);
  canvas->setCursor(5, 5);
  canvas->print(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "WiFi disconnected");

  canvas->setTextSize(TICKER_TEXT_SIZE);
  canvas->setCursor(scrollX - textX1, 60);
  canvas->print(currentMessage);

  canvas->flush();
}

void handlePostText() {
  if (server.hasArg("text")) {
    setMessage(server.arg("text"));
    server.send(200, "text/plain", "OK\n");
  } else {
    server.send(400, "text/plain", "Missing 'text' field\n");
  }
}

void handleRoot() {
  server.send(200, "text/plain",
      "POST text=<your message> to /text to update the display\n");
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);

  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);

  canvas->begin();
  canvas->setTextWrap(false); // ticker text must run off-screen, not wrap to a new line

  initIMU();

  drawStaticMessage("Connecting to WiFi...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(300);
  }

  if (WiFi.status() != WL_CONNECTED) {
    drawStaticMessage("WiFi failed");
    return;
  }

  setMessage("Ready");

  server.on("/", HTTP_GET, handleRoot);
  server.on("/text", HTTP_POST, handlePostText);
  server.begin();
}

void loop() {
  server.handleClient();
  updateOrientation();

  unsigned long now = millis();
  if (now - lastScrollUpdate >= SCROLL_INTERVAL_MS) {
    lastScrollUpdate = now;
    scrollX -= SCROLL_STEP;
    if (scrollX < -(int32_t)textWidth) {
      scrollX = 320;
    }
    drawTicker();
  }
}
