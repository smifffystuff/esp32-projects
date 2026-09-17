// Simple analog clock for the ESP32-C6-Touch-LCD-1.47 (Waveshare-compatible board).
// Display: JD9853, driven via the Arduino_GFX ST7789-compatible driver.
// Connects to Wi-Fi and syncs time via NTP; falls back to a fixed start time
// (kept via millis()) if Wi-Fi or NTP sync fails.

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
    Serial.println("IMU not found");
    return false;
  }
  imu.setAccelRange(QMI8658_ACCEL_RANGE_8G);
  imu.setAccelODR(QMI8658_ACCEL_ODR_1000HZ);
  imu.setAccelUnit_mg(true);
  imu.enableAccel(true);
  Serial.println("IMU ready");
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

const char *NTP_SERVER = "pool.ntp.org";
const char *TZ_INFO = "GMT0BST,M3.5.0/1,M10.5.0"; // Europe/London, handles GMT/BST automatically

// Used only if Wi-Fi/NTP sync fails - set to the current time when you flash the board.
const uint8_t START_HOUR = 10;
const uint8_t START_MINUTE = 9;
const uint8_t START_SECOND = 0;

Arduino_DataBus *bus = new Arduino_HWSPI(
    PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_SCK, PIN_LCD_MOSI, PIN_LCD_MISO);

// Four MADCTL rotation values, all confirmed against the physical board (not derived
// on paper - the theoretical single-bit pattern from portrait did not carry over to
// landscape once MV was involved, so each was verified by flashing and looking):
//   4 = MX only          -> portrait, cable at bottom
//   6 = MY only          -> portrait, cable at top (180 flip of 4)
//   7 = MV only          -> landscape
//   5 = MX+MY+MV         -> landscape, 180 flip of 7
Arduino_GFX *gfx = new Arduino_ST7789(
    bus, PIN_LCD_RST, 7 /* rotation */, false /* IPS */,
    172 /* width */, 320 /* height */,
    34 /* col offset 1 */, 0 /* row offset 1 */,
    34 /* col offset 2 */, 0 /* row offset 2 */);

// Off-screen framebuffer so the whole face+hands redraw each second without flicker.
// Recreated on the fly when crossing between portrait and landscape (see setLayout()).
Arduino_Canvas *canvas = new Arduino_Canvas(320, 172, gfx);

int16_t CX = 160;
int16_t CY = 86;
int16_t RADIUS = 70;

unsigned long startMillis;
bool timeSynced = false;

// Auto-rotation via the onboard QMI8658A IMU, all 4 orientations. Gravity loads onto
// whichever axis currently faces "up/down" on the panel; the other axis stays near
// zero. Calibrated against the physical board:
//   AY ~ -920mg -> rotation 4 (portrait)     AY ~ +1005mg -> rotation 6 (portrait)
//   AX ~ +990mg  -> rotation 7 (landscape)    AX ~ -987mg  -> rotation 5 (landscape)
const float ORIENTATION_THRESHOLD_MG = 500.0;
uint8_t currentRotation = 7;

bool isLandscapeRotation(uint8_t r) {
  return r == 5 || r == 7;
}

// Resizes the canvas and re-centers the face when crossing between portrait/landscape.
void setLayout(bool landscape) {
  delete canvas;
  if (landscape) {
    canvas = new Arduino_Canvas(320, 172, gfx);
    CX = 160;
    CY = 86;
    RADIUS = 70;
  } else {
    canvas = new Arduino_Canvas(172, 320, gfx);
    CX = 86;
    CY = 160;
    RADIUS = 75;
  }
  canvas->begin();
}

void updateOrientation() {
  if (!imuReady) return;

  float ax, ay, az;
  if (!imu.readAccel(ax, ay, az)) return;

  uint8_t desiredRotation = currentRotation;
  if (fabs(ax) > fabs(ay) && fabs(ax) > ORIENTATION_THRESHOLD_MG) {
    desiredRotation = (ax > 0) ? 7 : 5;
  } else if (fabs(ay) > fabs(ax) && fabs(ay) > ORIENTATION_THRESHOLD_MG) {
    desiredRotation = (ay < 0) ? 4 : 6;
  }
  // else: near the dead zone (board lying flat) - keep the last known orientation.

  if (desiredRotation != currentRotation) {
    bool wasLandscape = isLandscapeRotation(currentRotation);
    bool willBeLandscape = isLandscapeRotation(desiredRotation);
    currentRotation = desiredRotation;
    gfx->setRotation(currentRotation);
    if (wasLandscape != willBeLandscape) {
      setLayout(willBeLandscape);
    }
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

void drawFace() {
  canvas->fillScreen(RGB565_BLACK);
  canvas->drawCircle(CX, CY, RADIUS, RGB565_WHITE);
  for (int i = 0; i < 12; i++) {
    float angle = i * 30 * DEG_TO_RAD;
    int16_t x1 = CX + sin(angle) * (RADIUS - 8);
    int16_t y1 = CY - cos(angle) * (RADIUS - 8);
    int16_t x2 = CX + sin(angle) * RADIUS;
    int16_t y2 = CY - cos(angle) * RADIUS;
    canvas->drawLine(x1, y1, x2, y2, RGB565_WHITE);
  }
}

void drawHand(float angleDeg, int16_t length, uint16_t color) {
  float angle = angleDeg * DEG_TO_RAD;
  int16_t x2 = CX + sin(angle) * length;
  int16_t y2 = CY - cos(angle) * length;
  canvas->drawLine(CX, CY, x2, y2, color);
  // extra offset lines just to give the hand some visible width
  canvas->drawLine(CX + 1, CY, x2 + 1, y2, color);
  canvas->drawLine(CX, CY + 1, x2, y2 + 1, color);
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);

  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);

  canvas->begin();
  // No invertDisplay() call needed: the ST7789 driver's own init sequence already
  // leaves this panel in the correct (non-inverted) state, given ips=false above.

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
    hh = timeinfo.tm_hour % 12;
    mm = timeinfo.tm_min;
    ss = timeinfo.tm_sec;
  } else {
    unsigned long elapsedSec = (millis() - startMillis) / 1000;
    uint32_t totalSec = (uint32_t)START_HOUR * 3600 + START_MINUTE * 60 + START_SECOND + elapsedSec;
    hh = (totalSec / 3600) % 12;
    mm = (totalSec / 60) % 60;
    ss = totalSec % 60;
  }

  drawFace();
  drawHand((hh + mm / 60.0) * 30.0, RADIUS * 0.5, RGB565_WHITE);  // hour hand
  drawHand(mm * 6.0, RADIUS * 0.75, RGB565_WHITE);                // minute hand
  drawHand(ss * 6.0, RADIUS * 0.9, RGB565_RED);                   // second hand
  canvas->flush();

  delay(1000);
}
