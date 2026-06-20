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
  if (sdOk) /* SD stays mounted */

  writeln("CYD Banners firmware shell");
  write("MAC: "); writeln(macAddressText());
  write("LittleFS: "); writeln(littlefsOk ? "OK" : "FAIL");
  write("SD: "); writeln(sdOk ? "OK" : "FAIL");

  writeln("BOOT: networkBegin starting");
  networkBegin();
  writeln("BOOT: networkBegin complete");
  writeln("BOOT: load generated playlist chunks starting");
  if (loadGeneratedPlaylistChunks())
  {
    writeln("BOOT: generated playlist chunks loaded");
  }
  else if (loadCachedPlaylist())
  {
    writeln("BOOT: cached playlist loaded");
  }
  else
  {
    writeln("BOOT: playlist cache unavailable; rebuilding");
    rebuildPlaylist();
    writeln("BOOT: rebuildPlaylist complete");
  }
  writeln("BOOT: first render starting");
  if (slideCount > 0) advanceSlide(true); else renderCurrentSlide();
  writeln("BOOT: setup complete");
}

void loop()
{
  static unsigned long nextHeartbeatMs = 0;
  unsigned long nowMs = millis();
  if (long(nowMs - nextHeartbeatMs) >= 0)
  {
    write("HEARTBEAT slide=");
    write(currentSlideIndex + 1);
    write("/");
    write(slideCount);
    write(" wifi=");
    write(WiFi.status() == WL_CONNECTED ? "connected" : "not-connected");
    write(" heap=");
    write(ESP.getFreeHeap());
    write(" minHeap=");
    write(ESP.getMinFreeHeap());
    write(" maxAlloc=");
    write(ESP.getMaxAllocHeap());
    write(" stack=");
    write(uxTaskGetStackHighWaterMark(nullptr));
    write(" update=");
    writeln(updateStatusText);
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
  if (!updateUiLocked) advanceSlide(false);

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
