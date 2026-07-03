#include "display_manager.h"

#include "app_state.h"
#include "network_manager.h"
#include "playlist_manager.h"
#include "network_manager.h"
#include <qrcode.h>

unsigned long lastWorkNoticeMs = 0;

void drawWorkNotice(const String &line1, const String &line2)
{
  unsigned long now = millis();
  if (now - lastWorkNoticeMs < 250) return;
  lastWorkNoticeMs = now;

  if (infoScreenVisible)
  {
    constexpr int w = 260;
    constexpr int h = 58;
    int x = (tft.width() - w) / 2;
    int y = (tft.height() - h) / 2;
    tft.fillRect(x, y, w, h, TFT_NAVY);
    tft.drawRect(x, y, w, h, TFT_CYAN);
    tft.drawRect(x + 1, y + 1, w - 2, h - 2, TFT_CYAN);
    tft.setTextDatum(TC_DATUM);
    tft.setTextFont(1);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.drawString(line1, tft.width() / 2, y + 14, 2);
    if (line2.length() > 0)
    {
      String shown = line2;
      if (shown.length() > 34) shown = shown.substring(0, 31) + "...";
      tft.setTextColor(TFT_YELLOW, TFT_NAVY);
      tft.drawString(shown, tft.width() / 2, y + 35, 1);
    }
    return;
  }

  constexpr int h = 34;
  tft.fillRect(0, 0, tft.width(), h, TFT_NAVY);
  tft.drawRect(0, 0, tft.width(), h, TFT_CYAN);
  tft.setTextDatum(TC_DATUM);
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.drawString(line1, tft.width() / 2, 5, 1);
  if (line2.length() > 0)
  {
    String shown = line2;
    if (shown.length() > 38) shown = shown.substring(0, 35) + "...";
    tft.setTextColor(TFT_YELLOW, TFT_NAVY);
    tft.drawString(shown, tft.width() / 2, 18, 1);
  }
}

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
  drawInfoLine("WiFi", currentWifiSsidText(), y, networkStatusBarColor()); y += 13;
  drawInfoLine("Net", networkStatusText, y, networkStatusBarColor()); y += 13;
  drawInfoLine("Update", updateStatusText, y, networkHealth() == NetworkHealth::ConnectedGood ? TFT_GREEN : TFT_YELLOW); y += 13;
  drawInfoLine("LittleFS", littlefsOk ? "OK" : "FAIL", y, littlefsOk ? TFT_GREEN : TFT_RED); y += 13;
  bool sdMountedForInfo = sdOk && SD.begin(SD_CS_PIN);
  bool baseFound = sdMountedForInfo && SD.exists(PROJECT_ROOT);
  bool rootFound = sdMountedForInfo && SD.exists(ROOT_PLAYLIST);
  if (sdMountedForInfo) /* SD stays mounted */
  digitalWrite(TOUCH_CS_PIN, HIGH);

  drawInfoLine("SD", sdOk ? "OK" : "FAIL", y, sdOk ? TFT_GREEN : TFT_RED); y += 13;
  drawInfoLine("Free", sdFreeText(), y, sdOk ? TFT_CYAN : TFT_RED); y += 13;
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
  tft.drawString("touch return | hold 8s reset manifest/sum", tft.width() / 2, tft.height() - 16, 1);
  resetDynamicDrawState();
}

enum class TextJustify
{
  Left,
  Center,
  Right
};

struct TextLineStyle
{
  int font = 2;
  uint16_t color = TFT_LIGHTGREY;
  TextJustify justify = TextJustify::Center;
  bool mono = false;
  bool delimited = false;
};

bool parseTextLineCommand(String &line, TextLineStyle &style)
{
  int colon = line.indexOf(':');
  if (colon <= 0 || colon > 5) return false;

  String command = line.substring(0, colon);
  command.toUpperCase();
  bool recognized = false;
  bool mono = false;
  int heading = 0;
  for (int i = 0; i < command.length(); ++i)
  {
    char c = command.charAt(i);
    if (c == '1')
    {
      heading = 1;
      recognized = true;
    }
    else if (c == '2')
    {
      heading = 2;
      recognized = true;
    }
    else if (c == 'L')
    {
      style.justify = TextJustify::Left;
      recognized = true;
    }
    else if (c == 'R')
    {
      style.justify = TextJustify::Right;
      recognized = true;
    }
    else if (c == 'M')
    {
      mono = true;
      style.mono = true;
      recognized = true;
    }
    else if (c == 'D')
    {
      style.delimited = true;
      recognized = true;
    }
    else
    {
      return false;
    }
  }

  if (!recognized) return false;
  if (heading == 1)
  {
    style.font = 4;
    style.color = TFT_WHITE;
  }
  else if (heading == 2)
  {
    style.font = 2;
    style.color = color565(255, 255, 0);
  }
  else if (mono)
  {
    style.font = 1;
  }

  line = line.substring(colon + 1);
  return true;
}

void drawStyledTextLine(const String &line, int y, const TextLineStyle &style)
{
  tft.setTextSize(1);
  tft.setTextColor(style.color, TFT_BLACK);

  if (style.delimited && style.justify == TextJustify::Left)
  {
    constexpr int leftX = 6;
    constexpr int labelRightX = 96;
    constexpr int colonX = 101;
    constexpr int valueX = 113;
    int colon = line.indexOf(':');
    if (colon >= 0)
    {
      String label = line.substring(0, colon);
      label.trim();
      String value = line.substring(colon + 1);
      if (value.startsWith(" ")) value = value.substring(1);

      tft.setTextDatum(TR_DATUM);
      tft.drawString(label, labelRightX, y, style.font);
      tft.setTextDatum(TL_DATUM);
      tft.drawString(":", colonX, y, style.font);
      tft.drawString(value, valueX, y, style.font);
      return;
    }

    if (line.startsWith(" "))
    {
      String value = line;
      value.trim();
      tft.setTextDatum(TL_DATUM);
      tft.drawString(value, valueX, y, style.font);
      return;
    }

    tft.setTextDatum(TL_DATUM);
    tft.drawString(line, leftX, y, style.font);
    return;
  }

  int x = tft.width() / 2;
  uint8_t datum = TC_DATUM;
  if (style.justify == TextJustify::Left)
  {
    x = 6;
    datum = TL_DATUM;
  }
  else if (style.justify == TextJustify::Right)
  {
    x = tft.width() - 6;
    datum = TR_DATUM;
  }

  tft.setTextDatum(datum);
  tft.drawString(line, x, y, style.font);
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
    if (sdMountedForRender) /* SD stays mounted */
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
    TextLineStyle style;
    parseTextLineCommand(line, style);

    drawStyledTextLine(line, y, style);
    y += (style.font == 4) ? 30 : (style.font == 1 ? 10 : 18);
  }
  file.close();
  if (sdMountedForRender) /* SD stays mounted */
  if (lfsMountedForRender) LittleFS.end();
  digitalWrite(TOUCH_CS_PIN, HIGH);
}

bool readQrFile(const String &path, String &payload, String textLines[], int &textLineCount, int maxTextLines)
{
  bool sdMountedForRender = false;
  if (!isLfsPath(path))
  {
    sdMountedForRender = SD.begin(SD_CS_PIN);
    if (!sdMountedForRender)
    {
      noteMissingFile(path);
      return false;
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
      return false;
    }
  }

  File file = isLfsPath(path) ? LittleFS.open(storagePath(path), FILE_READ) : SD.open(storagePath(path), FILE_READ);
  if (!file)
  {
    noteMissingFile(path);
    if (sdMountedForRender) /* SD stays mounted */
    if (lfsMountedForRender) LittleFS.end();
    return false;
  }
  noteFileExists(path);

  payload = "";
  textLineCount = 0;
  bool gotPayload = false;
  while (file.available())
  {
    String line = file.readStringUntil('\n');
    line.replace("\r", "");
    if (!gotPayload)
    {
      line.trim();
      if (line == "" || line.startsWith("#")) continue;
      payload = line;
      gotPayload = true;
      continue;
    }
    if (textLineCount < maxTextLines) textLines[textLineCount++] = line;
  }
  file.close();
  if (sdMountedForRender) /* SD stays mounted */
  if (lfsMountedForRender) LittleFS.end();
  digitalWrite(TOUCH_CS_PIN, HIGH);
  return payload != "";
}

void drawQrSlide(const String &path)
{
  constexpr int maxQrTextLines = 3;
  String payload;
  String textLines[maxQrTextLines];
  int textLineCount = 0;
  readQrFile(path, payload, textLines, textLineCount, maxQrTextLines);
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
  if (textLineCount > 0)
  {
    int textY = qrY + qrPixelSize + 4;
    for (int i = 0; i < textLineCount && textY < tft.height() - 6; ++i)
    {
      String line = textLines[i];
      int font = 2;
      uint16_t color = TFT_LIGHTGREY;
      if (i == 0)
      {
        font = 2;
        color = TFT_WHITE;
        if (line.startsWith("1:")) line = line.substring(2);
      }
      else if (i == 1)
      {
        font = 1;
        color = color565(255, 255, 0);
        if (line.startsWith("2:")) line = line.substring(2);
      }
      else
      {
        font = 1;
        color = TFT_LIGHTGREY;
      }
      tft.setTextColor(color, TFT_BLACK);
      tft.drawString(line, tft.width() / 2, textY, font);
      textY += (font == 2) ? 18 : 11;
    }
  }
  else
  {
    String shown = payload;
    if (shown.length() > 38) shown = shown.substring(0, 35) + "...";
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(shown, tft.width() / 2, tft.height() - 24, 2);
  }
}

void drawImagePlaceholder(const String &path)
{
  if (!fileExistsTracked(path))
  {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("Missing image", tft.width() / 2, 80, 2);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(displayPath(path), tft.width() / 2, 108, 2);
    return;
  }

  bool mounted = SD.begin(SD_CS_PIN);
  sdOk = mounted;
  if (!mounted)
  {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("SD mount failed", tft.width() / 2, 80, 2);
    digitalWrite(TOUCH_CS_PIN, HIGH);
    return;
  }

  File file = SD.open(storagePath(path), FILE_READ);
  if (!file)
  {
    /* SD stays mounted */
    digitalWrite(TOUCH_CS_PIN, HIGH);
    noteMissingFile(path);
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("Missing image", tft.width() / 2, 80, 2);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(displayPath(path), tft.width() / 2, 108, 2);
    return;
  }

  char magic[8];
  if (file.readBytes(magic, 8) != 8 || memcmp(magic, "CYDIMG1\0", 8) != 0)
  {
    file.close();
    /* SD stays mounted */
    digitalWrite(TOUCH_CS_PIN, HIGH);
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.drawString("Bad CYD image", tft.width() / 2, 80, 2);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(displayPath(path), tft.width() / 2, 108, 2);
    return;
  }

  uint8_t dims[4];
  if (file.read(dims, 4) != 4)
  {
    file.close();
    /* SD stays mounted */
    digitalWrite(TOUCH_CS_PIN, HIGH);
    return;
  }
  uint16_t imageW = dims[0] | (uint16_t(dims[1]) << 8);
  uint16_t imageH = dims[2] | (uint16_t(dims[3]) << 8);
  int drawX = (tft.width() - imageW) / 2;
  int drawY = (tft.height() - imageH) / 2;

  tft.fillScreen(TFT_BLACK);
  uint16_t *row = new uint16_t[imageW];
  if (!row)
  {
    file.close();
    /* SD stays mounted */
    digitalWrite(TOUCH_CS_PIN, HIGH);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("Image row alloc failed", tft.width() / 2, 80, 2);
    return;
  }

  bool oldSwap = tft.getSwapBytes();
  tft.setSwapBytes(true);
  for (uint16_t y = 0; y < imageH; ++y)
  {
    size_t wanted = size_t(imageW) * 2;
    size_t got = file.read(reinterpret_cast<uint8_t *>(row), wanted);
    if (got != wanted) break;
    if (drawY + y >= 0 && drawY + y < tft.height())
    {
      tft.pushImage(drawX, drawY + y, imageW, 1, row);
    }
  }

  tft.setSwapBytes(oldSwap);
  delete[] row;
  file.close();
  /* SD stays mounted */
  digitalWrite(TOUCH_CS_PIN, HIGH);
  noteFileExists(path);
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
    return;
  }

  Slide &slide = slides[currentSlideIndex];
  currentSlideDurationMs = slide.durationMs;
  if (slide.type == "TEXT") drawTextSlide(slide.pathOrPayload);
  else if (slide.type == "QR") drawQrSlide(slide.pathOrPayload);
  else if (slide.type == "IMAGE") drawImagePlaceholder(slide.pathOrPayload);
  else if (slide.type == "UNSUPPORTED_IMAGE") drawUnsupportedImagePlaceholder(slide.pathOrPayload);
}

void advanceSlide(bool force)
{
  if (infoScreenVisible) return;
  processAcceptedManifestScanStep();
  unsigned long now = millis();
  if (!force && currentSlideIndex >= 0 && now - slideStartedMs < currentSlideDurationMs) return;
  if (slideCount == 0) return;
  if (currentSlideIndex >= 0 && currentSlideIndex + 1 >= slideCount)
  {
    loadNextGeneratedPlaylistChunk();
  }
  currentSlideIndex = (currentSlideIndex + 1) % slideCount;
  slideStartedMs = now;
  renderCurrentSlide();
}

void drawStatusBar(float progressFraction, uint16_t barColor)
{
  constexpr int barHeight = 3;
  uint16_t backgroundColor = color565(8, 8, 8);
  int y = tft.height() - barHeight;

  if (progressFraction < 0.0f) progressFraction = 0.0f;
  if (progressFraction > 1.0f) progressFraction = 1.0f;

  int barWidth = (int)(tft.width() * progressFraction);

  tft.fillRect(0, y, barWidth, barHeight, barColor);
  tft.fillRect(barWidth, y, tft.width(), barHeight, backgroundColor);
}

