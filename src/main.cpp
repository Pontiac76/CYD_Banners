#include <Arduino.h>
#include <LittleFS.h>

#include "app_state.h"
#include "display_manager.h"
#include "network_manager.h"
#include "playlist_manager.h"
#include "touch_manager.h"
#include "scheduler.h"
#include "firmware_ota.h"

void initializeSchedules() {
   unsigned long nowMs = millis();
   if (configSerialHeartbeatIntervalMs > 0) scheduleTask(TASK_SERIAL_HEARTBEAT, nowMs);
   else disableScheduledTask(TASK_SERIAL_HEARTBEAT);
   if (configServerHeartbeatIntervalMs > 0) scheduleTask(TASK_SERVER_HEARTBEAT, nowMs);
   else disableScheduledTask(TASK_SERVER_HEARTBEAT);
   if (configFirmwareOtaCheckIntervalMs > 0) scheduleTask(TASK_FIRMWARE_OTA_CHECK, nowMs + configFirmwareOtaFirstCheckMs);
   else disableScheduledTask(TASK_FIRMWARE_OTA_CHECK);
}

float currentSlideProgressFraction(unsigned long nowMs)
{
    unsigned long period = currentSlideDurationMs > 0 ? currentSlideDurationMs : DEFAULT_SLIDE_MS;
    if (period == 0) return 0.0f;

    unsigned long elapsed = nowMs - slideStartedMs;
    if (elapsed >= period) return 1.0f;

    return (float)elapsed / (float)period;
}

void writeHeartbeat(unsigned long nowMS) {
    // TODO: Should move this into a general function so it keeps this codespace clean
    write("HEARTBEAT CYD slide=");
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
    // Put this here since this will always be a completed function
    if (configSerialHeartbeatIntervalMs > 0) scheduleTask(TASK_SERIAL_HEARTBEAT, nowMS + configSerialHeartbeatIntervalMs);
    else disableScheduledTask(TASK_SERIAL_HEARTBEAT);
}

void setup()
{
  // Setup serial interface
  Serial.begin(115200);
  delay(100);

  // Turn the screen on
  pinMode(BACKLIGHT_PIN, OUTPUT);
  digitalWrite(BACKLIGHT_PIN, HIGH);
  pinMode(TOUCH_CS_PIN, OUTPUT);
  digitalWrite(TOUCH_CS_PIN, HIGH);

  // Setup the screen
  tft.init();
  tft.setRotation(DISPLAY_ROTATION);
  tft.fillScreen(TFT_BLACK);

  // Deal with some touch setup
  touchSpi.begin(TOUCH_SCLK_PIN, TOUCH_MISO_PIN, TOUCH_MOSI_PIN, TOUCH_CS_PIN);
  touch.begin(touchSpi);
  touch.setRotation(DISPLAY_ROTATION);
  digitalWrite(TOUCH_CS_PIN, HIGH);

  // Setup the two file systems, make sure they're available
  littlefsOk = LittleFS.begin(false);
  if (littlefsOk) LittleFS.end();
  sdOk = SD.begin(SD_CS_PIN);

  
  // Write the base header information
  writeln("CYD Banners firmware shell");
  write("MAC: "); writeln(macAddressText());
  write("LittleFS: "); writeln(littlefsOk ? "OK" : "FAIL");
  write("SD: "); writeln(sdOk ? "OK" : "FAIL");

  // Start the networking
  writeln("BOOT: networkBegin starting");
  // TODO: Read through networkBegin to figure out what's being initialized here
  networkBegin();
  initializeSchedules();
  writeln("BOOT: networkBegin complete");
  writeln("BOOT: load generated playlist chunks starting");
  // TODO: Read through loadGeneratedPlayListChunks and loadCachedPlaylist and rebuildPlaylist to understand what's being done here (Beyond the obvious)
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
  // TODO: This pops up the first slide.  I don't know why we're going to render a slide if slideCount=0?
  writeln("BOOT: first render starting");
  if (slideCount > 0) advanceSlide(true); else renderCurrentSlide();
  firmwareOtaBegin();
  writeln("BOOT: setup complete");
}

void loop()
{
  static unsigned long nextHeartbeatMs = 0;
  unsigned long nowMs = millis();

  if (firmwareOtaActive) {
    delay(25);
    return;
  }

  if (isTaskDue(TASK_SERIAL_HEARTBEAT, nowMs)) {
    writeHeartbeat(nowMs);
  }

  if (isTaskDue(TASK_SERVER_HEARTBEAT, nowMs)) {
    sendHeartbeatReport();
    if (configServerHeartbeatIntervalMs > 0) scheduleTask(TASK_SERVER_HEARTBEAT, nowMs + configServerHeartbeatIntervalMs);
    else disableScheduledTask(TASK_SERVER_HEARTBEAT);
  }

  if (isTaskDue(TASK_FIRMWARE_OTA_CHECK, nowMs)) {
    if (!firmwareOtaCheckAndApply()) {
      if (configFirmwareOtaCheckIntervalMs > 0) scheduleTask(TASK_FIRMWARE_OTA_CHECK, nowMs + configFirmwareOtaCheckIntervalMs);
      else disableScheduledTask(TASK_FIRMWARE_OTA_CHECK);
    }
  }

  // TODO: This goes out to a section outside SD access
  handleTouch();
  // TODO: This should be outside SD work
  networkUpdate();

  // TODO: Screen activities are in the section dealing with the UI/Touch
  if (infoScreenVisible && nowMs - infoScreenEnteredMs >= INFO_SCREEN_TIMEOUT_MS)
  {
    infoScreenVisible = false;
    slideStartedMs = nowMs;
    renderCurrentSlide();
  }

  // Update the status bar at the bottom of the screen
  drawStatusBar(currentSlideProgressFraction(nowMs),networkStatusBarColor());
  if (!updateUiLocked) advanceSlide(false);

  delay(25);
}
