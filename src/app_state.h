#pragma once

#include <Arduino.h>
#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <XPT2046_Touchscreen.h>

constexpr uint8_t DISPLAY_ROTATION = 1;
constexpr int BACKLIGHT_PIN = 21;
constexpr int SD_CS_PIN = 5;
constexpr int TOUCH_CS_PIN = 33;
constexpr int TOUCH_IRQ_PIN = 36;
constexpr int TOUCH_SCLK_PIN = 25;
constexpr int TOUCH_MISO_PIN = 39;
constexpr int TOUCH_MOSI_PIN = 32;
constexpr unsigned long STATUS_REDRAW_MS = 1000;
constexpr unsigned long DEFAULT_SLIDE_MS = 10000;
constexpr unsigned long INFO_SCREEN_TIMEOUT_MS = 60000;
constexpr int MAX_SLIDES = 48;
constexpr int MAX_PLAYLIST_FILES = 16;
constexpr int MAX_MISSING_FILES = 8;
constexpr int MAX_REQUIRED_FILES = 96;
constexpr const char *PROJECT_ROOT = "/banners";
constexpr const char *ROOT_PLAYLIST = "/banners/playlist.ini";

struct Slide
{
  String type;
  String pathOrPayload;
  String displayPath;
  unsigned long durationMs;
};

extern TFT_eSPI tft;
extern SPIClass touchSpi;
extern XPT2046_Touchscreen touch;
extern bool littlefsOk;
extern bool sdOk;
extern bool infoScreenVisible;
extern bool touchWasDown;
extern unsigned long nextStatusRedrawMs;
extern unsigned long slideStartedMs;
extern unsigned long infoScreenEnteredMs;
extern unsigned long currentSlideDurationMs;
extern int currentSlideIndex;
extern int lastBarWidth;
extern uint16_t lastBarColor;
extern String lastUptimeText;
extern Slide slides[MAX_SLIDES];
extern int slideCount;
extern String missingFiles[MAX_MISSING_FILES];
extern int missingFileCount;
extern String requiredFiles[MAX_REQUIRED_FILES];
extern int requiredFileCount;

uint16_t color565(uint8_t r, uint8_t g, uint8_t b);
String displayPath(const String &sdPath);
String macAddressText();
String wifiIpText();
String sdFreeText();
void resetDynamicDrawState();
void noteFileExists(const String &path);
void noteMissingFile(const String &path);
bool isLfsPath(const String &path);
String storagePath(const String &path);
bool fileExistsTracked(const String &path);
void noteRequiredFile(const String &path);
