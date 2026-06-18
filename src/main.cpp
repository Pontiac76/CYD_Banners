#include <Arduino.h>
#include <LittleFS.h>

#include "app_state.h"
#include "display_manager.h"
#include "network_manager.h"
#include "playlist_manager.h"
#include "touch_manager.h"

void setup()
{
  Serial.begin(115200);
  delay(100);
  pinMode(BACKLIGHT_PIN, OUTPUT);
  digitalWrite(BACKLIGHT_PIN, HIGH);
  pinMode(TOUCH_CS_PIN, OUTPUT);
  digitalWrite(TOUCH_CS_PIN, HIGH);

  tft.init();
  tft.setRotation(DISPLAY_ROTATION);
  tft.fillScreen(TFT_BLACK);

  touchSpi.begin(TOUCH_SCLK_PIN, TOUCH_MISO_PIN, TOUCH_MOSI_PIN, TOUCH_CS_PIN);
  touch.begin(touchSpi);
  touch.setRotation(DISPLAY_ROTATION);
  digitalWrite(TOUCH_CS_PIN, HIGH);

  littlefsOk = LittleFS.begin(false);
  if (littlefsOk) LittleFS.end();
  sdOk = SD.begin(SD_CS_PIN);
  if (sdOk) SD.end();

  Serial.println("CYD Banners firmware shell");
  Serial.print("MAC: "); Serial.println(macAddressText());
  Serial.print("LittleFS: "); Serial.println(littlefsOk ? "OK" : "FAIL");
  Serial.print("SD: "); Serial.println(sdOk ? "OK" : "FAIL");

  Serial.println("BOOT: networkBegin starting");
  networkBegin();
  Serial.println("BOOT: networkBegin complete");
  Serial.println("BOOT: rebuildPlaylist starting");
  rebuildPlaylist();
  Serial.println("BOOT: rebuildPlaylist complete");
  Serial.println("BOOT: first render starting");
  if (slideCount > 0) advanceSlide(true); else renderCurrentSlide();
  Serial.println("BOOT: setup complete");
}

void loop()
{
  static unsigned long nextHeartbeatMs = 0;
  unsigned long nowMs = millis();
  if (long(nowMs - nextHeartbeatMs) >= 0)
  {
    Serial.print("HEARTBEAT uptime=");
    Serial.print(nowMs / 1000UL);
    Serial.print("s slide=");
    Serial.print(currentSlideIndex + 1);
    Serial.print("/");
    Serial.print(slideCount);
    Serial.print(" wifi=");
    Serial.print(WiFi.status() == WL_CONNECTED ? "connected" : "not-connected");
    Serial.print(" update=");
    Serial.println(updateStatusText);
    nextHeartbeatMs = nowMs + 15000UL;
  }
  handleTouch();
  networkUpdate();

  if (infoScreenVisible && nowMs - infoScreenEnteredMs >= INFO_SCREEN_TIMEOUT_MS)
  {
    infoScreenVisible = false;
    slideStartedMs = nowMs;
    renderCurrentSlide();
  }

  drawStatusBar();
  advanceSlide(false);

  if (long(nowMs - nextStatusRedrawMs) >= 0)
  {
    if (infoScreenVisible)
    {
      updateInfoCountdown();
    }
    else
    {
      drawRuntimeStatus();
    }
    nextStatusRedrawMs = nowMs + STATUS_REDRAW_MS;
  }

  delay(25);
}
