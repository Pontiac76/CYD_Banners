#include "touch_manager.h"

#include "app_state.h"
#include "display_manager.h"

void handleTouch()
{
  constexpr unsigned long TOUCH_IGNORE_AFTER_BOOT_MS = 1000;
  constexpr unsigned long TOUCH_LOCKOUT_AFTER_TOGGLE_MS = 500;
  constexpr unsigned long TOUCH_MIN_PRESS_MS = 40;
  constexpr unsigned long TOUCH_MAX_PRESS_MS = 5000;

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

    if (pressDurationMs >= TOUCH_MIN_PRESS_MS &&
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
