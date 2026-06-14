#include <Arduino.h>
#include <LittleFS.h>

#include "app_state.h"
#include "display_manager.h"
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

  rebuildPlaylist();
  if (slideCount > 0) advanceSlide(true); else renderCurrentSlide();
}

void loop()
{
  unsigned long nowMs = millis();
  handleTouch();

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
