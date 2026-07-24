#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <ArduinoOTA.h>

// Clockface
#include <Clockface.h>
// Commons
#include <WiFiController.h>
#include <CWDateTime.h>
#include <CWPreferences.h>
#include <CWWebServer.h>
#include <StatusController.h>

#ifndef OTA_HOSTNAME
  #define OTA_HOSTNAME "clockwise"
#endif
#ifndef OTA_PASSWORD
  #define OTA_PASSWORD "clockwise"
#endif

// Lowest brightness the display can show legibly in a lit room. Anything
// below this looks off to the eye even though the panel is technically on.
#define MIN_VISIBLE_BRIGHT 12

// Hard pitch-black cutoff for fully turning the display off — only after
// the room is actually dark, not just "dim and cloudy."
#define DARK_OFF_THRESHOLD 5
#define DARK_OFF_HOLD_MS   60000UL

// Rolling LDR average window — absorbs clouds, shadows, headlights.
#define LDR_AVG_SAMPLES 8
#define LDR_POLL_MS     1000UL

#define ESP32_LED_BUILTIN 2

MatrixPanel_I2S_DMA *dma_display = nullptr;

Clockface *clockface;

WiFiController wifi;
CWDateTime cwDateTime;

bool autoBrightEnabled;
long autoBrightMillis = 0;

bool isValidI2SSpeed(uint32_t speed) {
  return speed == 8000000 || speed == 16000000 || speed == 20000000;
}

bool isValidDriver(uint32_t drv) {
  return drv >= 0 && drv <= 5;
}



void displaySetup(bool swapBlueGreen, bool swapBlueRed, uint8_t displayBright, uint8_t displayRotation, uint8_t driver, uint32_t i2cSpeed, uint8_t E_pin)
{
  HUB75_I2S_CFG mxconfig(64, 64, 1);

  if (swapBlueGreen)
  {
    // Swap Blue and Green pins because the panel is RBG instead of RGB.
    mxconfig.gpio.b1 = 26;
    mxconfig.gpio.b2 = 12;
    mxconfig.gpio.g1 = 27;
    mxconfig.gpio.g2 = 13;
  }

  if (swapBlueRed)
  {
    // Swap Blue and Red pins. 
    mxconfig.gpio.b1 = 25;
    mxconfig.gpio.b2 = 14;
    mxconfig.gpio.r1 = 27;
    mxconfig.gpio.r2 = 13;
  }

  mxconfig.gpio.e = E_pin;
  mxconfig.clkphase = false;

  if (isValidDriver(driver)) {
    mxconfig.driver = static_cast<HUB75_I2S_CFG::shift_driver>(driver);
  } else {
    Serial.printf("[ERROR] Invalid driver from config:%d\n", driver);
  }
  if (isValidI2SSpeed(i2cSpeed)) {
    mxconfig.i2sspeed = static_cast<HUB75_I2S_CFG::clk_speed>(i2cSpeed);
  } else {
    Serial.printf("[ERROR] Invalid I2S speed from config:%d\n", i2cSpeed);
  }

  // Full 8-bit color depth (256 levels/channel) for smooth grayscale on the moon.
  // We can afford it because the canvas fetch now runs over plain HTTP (no TLS
  // handshake), so there's no contiguous-heap pressure. The old 4-bit workaround
  // existed only to free heap for HTTPS and posterized the moon into a duotone.
  mxconfig.setPixelColorDepthBits(8);

  // Display Setup
  dma_display = new MatrixPanel_I2S_DMA(mxconfig);
  dma_display->begin();
  dma_display->setBrightness8(displayBright);
  dma_display->clearScreen();
  dma_display->setRotation(displayRotation);
}

void automaticBrightControl()
{
  if (!autoBrightEnabled) return;
  if (millis() - autoBrightMillis < LDR_POLL_MS) return;
  autoBrightMillis = millis();

  static uint16_t samples[LDR_AVG_SAMPLES] = {0};
  static uint8_t  sampleIdx = 0;
  static bool     samplesPrimed = false;
  static uint32_t darkSinceMs = 0;
  static uint8_t  lastApplied = 255;

  uint16_t raw = analogRead(ClockwiseParams::getInstance()->ldrPin);
  samples[sampleIdx] = raw;
  sampleIdx = (sampleIdx + 1) % LDR_AVG_SAMPLES;
  if (sampleIdx == 0) samplesPrimed = true;

  // Until the ring is full, average only the populated entries so the first
  // few seconds after boot don't read as "pitch black" from the zeroed slots.
  uint8_t  count = samplesPrimed ? LDR_AVG_SAMPLES : sampleIdx ? sampleIdx : 1;
  uint32_t sum = 0;
  for (uint8_t i = 0; i < count; i++) sum += samples[i];
  uint16_t avg = sum / count;

  uint16_t ldrMin = ClockwiseParams::getInstance()->autoBrightMin;
  uint16_t ldrMax = ClockwiseParams::getInstance()->autoBrightMax;
  uint8_t  maxB   = ClockwiseParams::getInstance()->displayBright;

  // Hard pitch-black cutoff — sustained darkness only, not transient.
  if (avg < DARK_OFF_THRESHOLD) {
    if (darkSinceMs == 0) darkSinceMs = millis();
    if (millis() - darkSinceMs > DARK_OFF_HOLD_MS) {
      if (lastApplied != 0) {
        Serial.printf("[Bright] Pitch-black hold met (avg=%u) -> off\n", avg);
        dma_display->setBrightness8(0);
        lastApplied = 0;
      }
      return;
    }
  } else {
    darkSinceMs = 0;
  }

  // Clamp the averaged reading into the user-configured curve range, then
  // map continuously into [MIN_VISIBLE_BRIGHT, displayBright]. The floor
  // ensures any lit room produces a legible panel — there is NO "below
  // ldrMin -> off" shortcut anymore; that's what DARK_OFF_THRESHOLD is for.
  uint16_t clamped = constrain(avg, ldrMin, ldrMax);
  uint8_t  target  = (ldrMax > ldrMin)
                   ? map(clamped, ldrMin, ldrMax, MIN_VISIBLE_BRIGHT, maxB)
                   : maxB;
  if (target < MIN_VISIBLE_BRIGHT) target = MIN_VISIBLE_BRIGHT;

  if (abs((int)target - (int)lastApplied) >= 4) {
    Serial.printf("[Bright] LDR raw=%u avg=%u -> bright=%u\n", raw, avg, target);
    dma_display->setBrightness8(target);
    lastApplied = target;
  }
}

void setupOTA()
{
  // Hostname is set BEFORE begin() so it's used both for the _arduino._tcp
  // mDNS record and for the AUTH challenge string. ArduinoOTA reuses the
  // mDNS instance already started by WiFiController, so no MDNS.begin here.
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    // Sketch updates only — we don't ship SPIFFS images, so SPIFFS path
    // means an unmount before flash, which would brick the running clock if
    // the upload aborts. Treat it as a hard error.
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
    Serial.printf("[OTA] Start: %s\n", type.c_str());
    if (dma_display) dma_display->clearScreen();
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static unsigned int lastPct = 255;
    unsigned int pct = (progress * 100) / total;
    if (pct != lastPct) {
      Serial.printf("[OTA] Progress: %u%%\n", pct);
      lastPct = pct;
    }
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("[OTA] End — rebooting");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    const char *msg = "Unknown";
    switch (error) {
      case OTA_AUTH_ERROR:    msg = "Auth failed";    break;
      case OTA_BEGIN_ERROR:   msg = "Begin failed";   break;
      case OTA_CONNECT_ERROR: msg = "Connect failed"; break;
      case OTA_RECEIVE_ERROR: msg = "Receive failed"; break;
      case OTA_END_ERROR:     msg = "End failed";     break;
    }
    Serial.printf("[OTA] Error %u: %s\n", error, msg);
  });

  ArduinoOTA.begin();
  Serial.printf("[OTA] Ready: %s.local @ %s\n",
                OTA_HOSTNAME, WiFi.localIP().toString().c_str());
}

void setup()
{
  Serial.begin(115200);
  pinMode(ESP32_LED_BUILTIN, OUTPUT);

  StatusController::getInstance()->blink_led(5, 100);

  ClockwiseParams::getInstance()->load();

  pinMode(ClockwiseParams::getInstance()->ldrPin, INPUT);

  uint8_t driver = ClockwiseParams::getInstance()->driver;
  uint32_t i2cSpeed = ClockwiseParams::getInstance()->i2cSpeed;
  uint8_t E_pin = ClockwiseParams::getInstance()->E_pin;
  
  displaySetup(ClockwiseParams::getInstance()->swapBlueGreen, ClockwiseParams::getInstance()->swapBlueRed, ClockwiseParams::getInstance()->displayBright, ClockwiseParams::getInstance()->displayRotation, driver, i2cSpeed, E_pin);
  clockface = new Clockface(dma_display);

  autoBrightEnabled = (ClockwiseParams::getInstance()->autoBrightMax > 0);

  StatusController::getInstance()->clockwiseLogo();
  delay(1000);

  StatusController::getInstance()->wifiConnecting();
  if (wifi.begin())
  {
    setupOTA();
    StatusController::getInstance()->ntpConnecting();
    cwDateTime.begin(ClockwiseParams::getInstance()->timeZone.c_str(),
        ClockwiseParams::getInstance()->use24hFormat,
        ClockwiseParams::getInstance()->ntpServer.c_str(),
        ClockwiseParams::getInstance()->manualPosix.c_str());
    clockface->setup(&cwDateTime);
  }
}

void loop()
{
  wifi.handleImprovWiFi();

  if (wifi.isConnected())
  {
    ArduinoOTA.handle();
    ClockwiseWebServer::getInstance()->handleHttpRequest();
    ezt::events();
  }

  if (wifi.connectionSucessfulOnce)
  {
    clockface->update();
  }

  automaticBrightControl();
}
