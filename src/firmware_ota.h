#pragma once

#include <Arduino.h>

extern bool firmwareOtaActive;

void firmwareOtaBegin();
bool firmwareOtaCheckAndApply();
const char *currentFirmwareVersion();
