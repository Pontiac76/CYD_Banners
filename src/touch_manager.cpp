#include "touch_manager.h"

#include "app_state.h"
#include "display_manager.h"
#include "network_manager.h"

void handleTouch()
{
  constexpr unsigned long TOUCH_IGNORE_AFTER_BOOT_MS = 1000;
  constexpr unsigned long TOUCH_LOCKOUT_AFTER_TOGGLE_MS = 500;
  constexpr unsigned long TOUCH_MIN_PRESS_MS = 40;
  constexpr unsigned long TOUCH_MAX_PRESS_MS = 5000;
  constexpr unsigned long TOUCH_RESET_PRESS_MS = 8000;

  static bool pressCandidate = false;
  static unsigned long pressStartedMs = 0;
  static unsigned long lastToggleMs = 0;
  static TS_Point lastPoint;

  bool touchDown = touch.tirqTouched() && touch.touched();
  unsigned long nowMs = millis();

  if (nowMs < TOUCH_IGNORE_AFTER_BOOT_MS)
  {
    touchWasDown = touchDown;
    pressCandidate = false;
    return;
  }

  if (touchDown)
  {
    if (!touchWasDown && nowMs - lastToggleMs >= TOUCH_LOCKOUT_AFTER_TOGGLE_MS)
    {
      pressCandidate = true;
      pressStartedMs = nowMs;
    }

    if (pressCandidate)
    {
      lastPoint = touch.getPoint();
      if (infoScreenVisible && nowMs - pressStartedMs >= TOUCH_RESET_PRESS_MS && nowMs - lastToggleMs >= TOUCH_LOCKOUT_AFTER_TOGGLE_MS)
      {
        pressCandidate = false;
        touchWasDown = true;
        lastToggleMs = nowMs;
        Serial.println("Touch long hold on info screen: reset local content state");
        drawWorkNotice("Reset in progress", "clearing local manifest/sum");
        resetLocalContentState();
        infoScreenVisible = true;
        infoScreenEnteredMs = nowMs;
      }
    }

    touchWasDown = true;
    return;
  }

  // Trigger on release, not press. This avoids immediate bounce/noise toggles
  // while the finger is still down.
  if (touchWasDown && pressCandidate)
  {
    unsigned long pressDurationMs = nowMs - pressStartedMs;
    pressCandidate = false;

    if (infoScreenVisible && pressDurationMs >= TOUCH_RESET_PRESS_MS && nowMs - lastToggleMs >= TOUCH_LOCKOUT_AFTER_TOGGLE_MS)
    {
      // Long-hold reset normally fires while the touch is still down. Keep this
      // release path as a fallback in case the touch controller misses samples.
      lastToggleMs = nowMs;
      Serial.println("Touch long release on info screen: reset local content state");
      drawWorkNotice("Reset in progress", "clearing local manifest/sum");
      resetLocalContentState();
      infoScreenVisible = true;
      infoScreenEnteredMs = nowMs;
    }
    else if (pressDurationMs >= TOUCH_MIN_PRESS_MS &&
        pressDurationMs <= TOUCH_MAX_PRESS_MS &&
        nowMs - lastToggleMs >= TOUCH_LOCKOUT_AFTER_TOGGLE_MS)
    {
      lastToggleMs = nowMs;
      Serial.print("Touch release toggle: z=");
      Serial.print(lastPoint.z);
      Serial.print(" x=");
      Serial.print(lastPoint.x);
      Serial.print(" y=");
      Serial.println(lastPoint.y);

      infoScreenVisible = !infoScreenVisible;
      if (infoScreenVisible)
      {
        infoScreenEnteredMs = nowMs;
        drawInfoScreen();
      }
      else
      {
        slideStartedMs = nowMs;
        renderCurrentSlide();
      }
    }
  }

  touchWasDown = false;
}
