#pragma once

#include <Arduino.h>

void drawInfoScreen();
void updateInfoCountdown();
void drawStatusBar(float progressFraction, uint16_t barColor);
void drawWorkNotice(const String &line1, const String &line2 = String(""));
void renderCurrentSlide();
void advanceSlide(bool force);
