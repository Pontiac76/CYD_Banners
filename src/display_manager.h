#pragma once

#include <Arduino.h>

void drawInfoScreen();
void updateInfoCountdown();
void drawStatusBar();
void drawWorkNotice(const String &line1, const String &line2 = String(""));
void drawRuntimeStatus();
void renderCurrentSlide();
void advanceSlide(bool force);
