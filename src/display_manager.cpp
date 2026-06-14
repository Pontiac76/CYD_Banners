#include "display_manager.h"

#include "app_state.h"
#include <qrcode.h>

void drawInfoLine(const String &label, const String &value, int y, uint16_t valueColor)
{
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString(label, 6, y, 1);
  tft.setTextColor(valueColor, TFT_BLACK);
  tft.drawString(value, 74, y, 1);
}

constexpr int INFO_VALUE_X = 74;
constexpr int INFO_SLIDE_TIME_Y = 141;

void updateInfoCountdown()
{
  if (!infoScreenVisible) return;

  tft.fillRect(INFO_VALUE_X, INFO_SLIDE_TIME_Y, 100, 10, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(String(currentSlideDurationMs / 1000UL) + "s", INFO_VALUE_X, INFO_SLIDE_TIME_Y, 1);
}

void drawInfoScreen()
{
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(color565(0, 200, 255), TFT_BLACK);
  tft.drawString("CYD Info", tft.width() / 2, 4, 2);

  int y = 24;
  drawInfoLine("MAC", macAddressText(), y, TFT_WHITE); y += 13;
  drawInfoLine("IP", wifiIpText(), y, WiFi.status() == WL_CONNECTED ? TFT_GREEN : TFT_DARKGREY); y += 13;
  drawInfoLine("LittleFS", littlefsOk ? "OK" : "FAIL", y, littlefsOk ? TFT_GREEN : TFT_RED); y += 13;
  bool sdMountedForInfo = sdOk && SD.begin(SD_CS_PIN);
  bool baseFound = sdMountedForInfo && SD.exists(PROJECT_ROOT);
  bool rootFound = sdMountedForInfo && SD.exists(ROOT_PLAYLIST);
  if (sdMountedForInfo) SD.end();
  digitalWrite(TOUCH_CS_PIN, HIGH);

  drawInfoLine("SD", sdOk ? "OK" : "FAIL", y, sdOk ? TFT_GREEN : TFT_RED); y += 13;
  drawInfoLine("Base", baseFound ? "found" : "missing", y, baseFound ? TFT_GREEN : TFT_RED); y += 13;
  drawInfoLine("Root", rootFound ? "playlist.ini" : "missing", y, rootFound ? TFT_GREEN : TFT_RED); y += 13;
  drawInfoLine("Slides", String(slideCount), y, slideCount > 0 ? TFT_GREEN : TFT_RED); y += 13;
  drawInfoLine("Current", currentSlideIndex >= 0 ? String(currentSlideIndex + 1) + "/" + String(slideCount) : "none", y, TFT_WHITE); y += 13;
  drawInfoLine("Missing", String(missingFileCount), y, missingFileCount == 0 ? TFT_GREEN : TFT_YELLOW); y += 13;
  drawInfoLine("SlideTime", "", y, TFT_CYAN); y += 13;
  updateInfoCountdown();

  for (int i = 0; i < missingFileCount && i < 4; ++i)
  {
    String shown = displayPath(missingFiles[i]);
    if (shown.length() > 38) shown = String("...") + shown.substring(shown.length() - 35);
    drawInfoLine(String(i + 1), shown, y, TFT_YELLOW);
    y += 13;
  }

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("touch to return now", tft.width() / 2, tft.height() - 16, 1);
  resetDynamicDrawState();
  drawStatusBar();
}

void drawTextSlide(const String &path)
{
  tft.fillScreen(TFT_BLACK);
  bool sdMountedForRender = false;
  if (!isLfsPath(path))
  {
    sdMountedForRender = SD.begin(SD_CS_PIN);
    if (!sdMountedForRender)
    {
      noteMissingFile(path);
      digitalWrite(TOUCH_CS_PIN, HIGH);
      return;
    }
  }

  bool lfsMountedForRender = false;
  if (isLfsPath(path))
  {
    lfsMountedForRender = LittleFS.begin(false);
    littlefsOk = lfsMountedForRender;
    if (!lfsMountedForRender)
    {
      noteMissingFile(path);
      return;
    }
  }

  File file = isLfsPath(path) ? LittleFS.open(storagePath(path), FILE_READ) : SD.open(storagePath(path), FILE_READ);
  if (!file)
  {
    noteMissingFile(path);
    if (sdMountedForRender) SD.end();
    if (lfsMountedForRender) LittleFS.end();
    digitalWrite(TOUCH_CS_PIN, HIGH);
    return;
  }
  noteFileExists(path);

  int y = 8;
  while (file.available() && y < tft.height() - 10)
  {
    String line = file.readStringUntil('\n');
    line.replace("\r", "");
    int font = 2;
    uint16_t color = TFT_LIGHTGREY;
    if (line.startsWith("1:")) { font = 4; color = TFT_WHITE; line = line.substring(2); }
    else if (line.startsWith("2:")) { font = 2; color = color565(255, 255, 0); line = line.substring(2); }

    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(color, TFT_BLACK);
    tft.drawString(line, tft.width() / 2, y, font);
    y += (font == 4) ? 30 : 18;
  }
  file.close();
  if (sdMountedForRender) SD.end();
  if (lfsMountedForRender) LittleFS.end();
  digitalWrite(TOUCH_CS_PIN, HIGH);
}

String readQrPayload(const String &path)
{
  bool sdMountedForRender = false;
  if (!isLfsPath(path))
  {
    sdMountedForRender = SD.begin(SD_CS_PIN);
    if (!sdMountedForRender)
    {
      noteMissingFile(path);
      return "";
    }
  }

  bool lfsMountedForRender = false;
  if (isLfsPath(path))
  {
    lfsMountedForRender = LittleFS.begin(false);
    littlefsOk = lfsMountedForRender;
    if (!lfsMountedForRender)
    {
      noteMissingFile(path);
      return "";
    }
  }

  File file = isLfsPath(path) ? LittleFS.open(storagePath(path), FILE_READ) : SD.open(storagePath(path), FILE_READ);
  if (!file)
  {
    noteMissingFile(path);
    if (sdMountedForRender) SD.end();
    if (lfsMountedForRender) LittleFS.end();
    return "";
  }
  noteFileExists(path);

  while (file.available())
  {
    String line = file.readStringUntil('\n');
    line.replace("\r", "");
    line.trim();
    if (line != "" && !line.startsWith("#"))
    {
      file.close();
      if (sdMountedForRender) SD.end();
      if (lfsMountedForRender) LittleFS.end();
      digitalWrite(TOUCH_CS_PIN, HIGH);
      return line;
    }
  }
  file.close();
  if (sdMountedForRender) SD.end();
  if (lfsMountedForRender) LittleFS.end();
  digitalWrite(TOUCH_CS_PIN, HIGH);
  return "";
}

void drawQrSlide(const String &path)
{
  String payload = readQrPayload(path);
  if (payload == "")
  {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("Empty QR file", tft.width() / 2, 80, 2);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(displayPath(path), tft.width() / 2, 108, 2);
    return;
  }

  constexpr uint8_t qrVersion = 5;
  constexpr uint8_t quietZoneModules = 4;
  QRCode qrcode;
  uint8_t qrcodeBytes[qrcode_getBufferSize(qrVersion)];
  qrcode_initText(&qrcode, qrcodeBytes, qrVersion, ECC_LOW, payload.c_str());

  tft.fillScreen(TFT_BLACK);
  int totalModules = qrcode.size + quietZoneModules * 2;
  int moduleSize = min(static_cast<int>(tft.width()), static_cast<int>(tft.height()) - 34) / totalModules;
  moduleSize = max(1, moduleSize);
  int qrPixelSize = totalModules * moduleSize;
  int qrX = (tft.width() - qrPixelSize) / 2;
  int qrY = 6;
  int codeX = qrX + quietZoneModules * moduleSize;
  int codeY = qrY + quietZoneModules * moduleSize;

  for (uint8_t y = 0; y < qrcode.size; ++y)
  {
    for (uint8_t x = 0; x < qrcode.size; ++x)
    {
      if (qrcode_getModule(&qrcode, x, y)) tft.fillRect(codeX + x * moduleSize, codeY + y * moduleSize, moduleSize, moduleSize, TFT_WHITE);
    }
  }
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(payload, tft.width() / 2, tft.height() - 24, 2);
}

void drawImagePlaceholder(const String &path)
{
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("CYD image renderer pending", tft.width() / 2, 80, 2);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString(displayPath(path), tft.width() / 2, 108, 2);
}

void drawUnsupportedImagePlaceholder(const String &path)
{
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.drawString("Source image on SD", tft.width() / 2, 70, 2);
  tft.drawString("server conversion needed", tft.width() / 2, 94, 2);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString(displayPath(path), tft.width() / 2, 124, 2);
}

void renderCurrentSlide()
{
  resetDynamicDrawState();
  if (slideCount == 0)
  {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("No playlist slides", tft.width() / 2, 80, 2);
    drawStatusBar();
    return;
  }

  Slide &slide = slides[currentSlideIndex];
  currentSlideDurationMs = slide.durationMs;
  if (slide.type == "TEXT") drawTextSlide(slide.pathOrPayload);
  else if (slide.type == "QR") drawQrSlide(slide.pathOrPayload);
  else if (slide.type == "IMAGE") drawImagePlaceholder(slide.pathOrPayload);
  else if (slide.type == "UNSUPPORTED_IMAGE") drawUnsupportedImagePlaceholder(slide.pathOrPayload);
  drawStatusBar();
}

void advanceSlide(bool force)
{
  if (infoScreenVisible) return;
  unsigned long now = millis();
  if (!force && currentSlideIndex >= 0 && now - slideStartedMs < currentSlideDurationMs) return;
  if (slideCount == 0) return;
  currentSlideIndex = (currentSlideIndex + 1) % slideCount;
  slideStartedMs = now;
  renderCurrentSlide();
}

void drawStatusBar()
{
  constexpr int barHeight = 3;
  unsigned long elapsed = infoScreenVisible ? millis() - infoScreenEnteredMs : millis() - slideStartedMs;
  unsigned long period = infoScreenVisible ? INFO_SCREEN_TIMEOUT_MS : (currentSlideDurationMs > 0 ? currentSlideDurationMs : DEFAULT_SLIDE_MS);
  int barWidth = map(elapsed % period, 0, period - 1, 0, tft.width());
  uint16_t barColor = sdOk ? TFT_GREEN : TFT_RED;
  uint16_t backgroundColor = color565(16, 16, 16);
  int y = tft.height() - barHeight;

  if (lastBarWidth < 0 || lastBarColor != barColor || barWidth < lastBarWidth)
  {
    tft.fillRect(0, y, tft.width(), barHeight, backgroundColor);
    if (barWidth > 0) tft.fillRect(0, y, barWidth, barHeight, barColor);
  }
  else if (barWidth > lastBarWidth)
  {
    tft.fillRect(lastBarWidth, y, barWidth - lastBarWidth, barHeight, barColor);
  }
  lastBarWidth = barWidth;
  lastBarColor = barColor;
}

void drawRuntimeStatus()
{
  drawStatusBar();
}
