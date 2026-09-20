// Simple digital clock for the ESP32-C6-Touch-LCD-1.47 (Waveshare-compatible board).
// Display: JD9853, driven via the Arduino_GFX ST7789-compatible driver.
// Landscape only - auto-flips between the two landscape orientations via the onboard
// QMI8658A IMU. Connects to Wi-Fi and syncs time via NTP; falls back to a fixed start
// time (kept via millis()) if Wi-Fi or NTP sync fails.

#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <time.h>
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

#define PIN_LED       7   // external LED on the breadboard, flashes with the seconds

const char *NTP_SERVER = "pool.ntp.org";
const char *TZ_INFO = "GMT0BST,M3.5.0/1,M10.5.0"; // Europe/London, handles GMT/BST automatically

// Used only if Wi-Fi/NTP sync fails - set to the current time when you flash the board.
const uint8_t START_HOUR = 10;
const uint8_t START_MINUTE = 9;
const uint8_t START_SECOND = 0;

Arduino_DataBus *bus = new Arduino_HWSPI(
    PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_SCK, PIN_LCD_MOSI, PIN_LCD_MISO);

// Confirmed against the physical board (see analog_clock project notes):
// rotation 7 = MADCTL_MV only -> landscape; rotation 5 = MX+MY+MV -> its 180-flip pair.
// No invertDisplay() call needed - ips=false already leaves this panel correctly non-inverted.
Arduino_GFX *gfx = new Arduino_ST7789(
    bus, PIN_LCD_RST, 7 /* rotation */, false /* IPS */,
    172 /* width */, 320 /* height */,
    34 /* col offset 1 */, 0 /* row offset 1 */,
    34 /* col offset 2 */, 0 /* row offset 2 */);

// Off-screen framebuffer so the whole frame redraws each second without flicker.
Arduino_Canvas *canvas = new Arduino_Canvas(320, 172, gfx);

unsigned long startMillis;
bool timeSynced = false;

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
  // else: near the dead zone (board lying flat) - keep the last known orientation.

  if (desiredRotation != currentRotation) {
    currentRotation = desiredRotation;
    gfx->setRotation(currentRotation);
  }
}

void showStatus(const char *line1, const char *line2 = nullptr) {
  canvas->fillScreen(RGB565_BLACK);
  canvas->setTextColor(RGB565_WHITE);
  canvas->setTextSize(1);
  canvas->setCursor(10, 10);
  canvas->print(line1);
  if (line2) {
    canvas->setCursor(10, 25);
    canvas->print(line2);
  }
  canvas->flush();
}

void connectWiFiAndSyncTime() {
  showStatus("Connecting to WiFi...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(300);
  }

  if (WiFi.status() != WL_CONNECTED) {
    showStatus("WiFi failed,", "using fallback time");
    delay(2000);
    return;
  }

  showStatus("Syncing time...");
  configTzTime(TZ_INFO, NTP_SERVER);

  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 10000)) {
    timeSynced = true;
  } else {
    showStatus("NTP sync failed,", "using fallback time");
    delay(2000);
  }
}

void drawClock(uint8_t hh, uint8_t mm, uint8_t ss) {
  canvas->fillScreen(RGB565_BLACK);

  char buf[9];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hh, mm, ss);

  canvas->setTextSize(5);
  canvas->setTextColor(RGB565_WHITE);

  int16_t x1, y1;
  uint16_t w, h;
  canvas->getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
  int16_t cx = (320 - w) / 2 - x1;
  int16_t cy = (172 - h) / 2 - y1;
  canvas->setCursor(cx, cy);
  canvas->print(buf);
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);

  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  canvas->begin();

  initIMU();

  startMillis = millis();

  connectWiFiAndSyncTime();
}

void loop() {
  updateOrientation();

  uint8_t hh, mm, ss;

  if (timeSynced) {
    struct tm timeinfo;
    getLocalTime(&timeinfo);
    hh = timeinfo.tm_hour;
    mm = timeinfo.tm_min;
    ss = timeinfo.tm_sec;
  } else {
    unsigned long elapsedSec = (millis() - startMillis) / 1000;
    uint32_t totalSec = (uint32_t)START_HOUR * 3600 + START_MINUTE * 60 + START_SECOND + elapsedSec;
    hh = (totalSec / 3600) % 24;
    mm = (totalSec / 60) % 60;
    ss = totalSec % 60;
  }

  drawClock(hh, mm, ss);
  canvas->flush();

  digitalWrite(PIN_LED, HIGH);
  delay(100);
  digitalWrite(PIN_LED, LOW);
  delay(900);
}
