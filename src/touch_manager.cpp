#include "touch_manager.h"

#include "app_state.h"
#include "display_manager.h"
#include "network_manager.h"

namespace
{
constexpr unsigned long TOUCH_IGNORE_AFTER_BOOT_MS = 1000;
constexpr unsigned long INFO_SCREEN_TIMEOUT_MS_LOCAL = INFO_SCREEN_TIMEOUT_MS;
constexpr unsigned long INFO_BAR_REDRAW_MS = 50;
constexpr unsigned long TOUCH_RESET_PRESS_MS = 8000;
constexpr uint16_t INFO_MODE_BAR_COLOR = TFT_CYAN;

bool touchCurrentlyDown()
{
  return touch.tirqTouched() && touch.touched();
}

void waitForTouchRelease()
{
  while (touchCurrentlyDown())
  {
    delay(25);
    yield();
  }
  touchWasDown = false;
}

void drawInfoTimeoutBar(unsigned long nowMs, unsigned long enteredMs, unsigned long waitUntilMs)
{
  static int lastWidth = -1;
  static unsigned long lastDrawMs = 0;

  if (long(nowMs - lastDrawMs) < 0 && lastDrawMs != 0) return;
  if (lastDrawMs != 0 && nowMs - lastDrawMs < INFO_BAR_REDRAW_MS) return;
  lastDrawMs = nowMs;

  unsigned long totalMs = waitUntilMs - enteredMs;
  unsigned long remainingMs = long(waitUntilMs - nowMs) > 0 ? waitUntilMs - nowMs : 0;
  int width = totalMs > 0 ? (int)((uint64_t)tft.width() * remainingMs / totalMs) : 0;
  if (width < 0) width = 0;
  if (width > tft.width()) width = tft.width();
  if (width == lastWidth) return;
  lastWidth = width;

  constexpr int barHeight = 4;
  int y = tft.height() - barHeight;
  tft.fillRect(0, y, tft.width(), barHeight, TFT_DARKGREY);
  if (width > 0) tft.fillRect(0, y, width, barHeight, INFO_MODE_BAR_COLOR);
}

void resetInfoTimeoutBarState()
{
  // Force the next modal info bar draw to repaint by drawing a harmless full-width
  // background through the normal dynamic-state reset path.
  lastBarWidth = -1;
}

void showInfoScreenModal()
{
  infoScreenVisible = true;
  infoScreenEnteredMs = millis();
  drawInfoScreen();
  resetInfoTimeoutBarState();

  unsigned long enteredMs = millis();
  unsigned long waitUntilMs = enteredMs + INFO_SCREEN_TIMEOUT_MS_LOCAL;
  unsigned long nextInfoRefreshMs = enteredMs + 1000UL;

  writeln("Info screen active: waiting for timeout or touch release");

  while (long(waitUntilMs - millis()) > 0)
  {
    unsigned long nowMs = millis();
    drawInfoTimeoutBar(nowMs, enteredMs, waitUntilMs);

    if (long(nowMs - nextInfoRefreshMs) >= 0)
    {
      updateInfoCountdown();
      nextInfoRefreshMs = nowMs + 1000UL;
    }

    if (!touchCurrentlyDown())
    {
      delay(25);
      yield();
      continue;
    }

    unsigned long pressStartedMs = millis();
    bool resetDone = false;
    writeln("Info screen touched: waiting for release or long-hold reset");
    while (touchCurrentlyDown() && long(waitUntilMs - millis()) > 0)
    {
      nowMs = millis();
      drawInfoTimeoutBar(nowMs, enteredMs, waitUntilMs);
      if (!resetDone && nowMs - pressStartedMs >= TOUCH_RESET_PRESS_MS)
      {
        resetDone = true;
        writeln("Touch long hold on info screen: reset local content state");
        drawWorkNotice("Reset in progress", "clearing local manifest/sum");
        resetLocalContentState();
        infoScreenVisible = true;
        infoScreenEnteredMs = millis();
        drawInfoScreen();
        resetInfoTimeoutBarState();
      }
      delay(25);
      yield();
    }

    if (resetDone)
    {
      waitForTouchRelease();
      writeln("Info screen reset complete; continuing info timeout");
      continue;
    }

    if (long(waitUntilMs - millis()) > 0)
    {
      writeln("Info screen touch released: resuming normal operation");
      break;
    }
  }

  if (long(waitUntilMs - millis()) <= 0) writeln("Info screen timeout: resuming normal operation");

  infoScreenVisible = false;
  touchWasDown = false;
  slideStartedMs = millis();
  renderCurrentSlide();
}
} // namespace

void handleTouch()
{
  unsigned long nowMs = millis();
  if (nowMs < TOUCH_IGNORE_AFTER_BOOT_MS)
  {
    touchWasDown = touchCurrentlyDown();
    return;
  }

  if (!touchCurrentlyDown())
  {
    touchWasDown = false;
    return;
  }

  writeln("Screen touched: waiting for release");
  waitForTouchRelease();
  writeln("Screen released: showing info screen");
  showInfoScreenModal();
}
