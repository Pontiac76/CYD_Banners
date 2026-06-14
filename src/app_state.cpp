#include "app_state.h"

TFT_eSPI tft;
SPIClass touchSpi(HSPI);
XPT2046_Touchscreen touch(TOUCH_CS_PIN, TOUCH_IRQ_PIN);
bool littlefsOk = false;
bool sdOk = false;
bool infoScreenVisible = false;
bool touchWasDown = false;
unsigned long nextStatusRedrawMs = 0;
unsigned long slideStartedMs = 0;
unsigned long infoScreenEnteredMs = 0;
unsigned long currentSlideDurationMs = DEFAULT_SLIDE_MS;
int currentSlideIndex = -1;
int lastBarWidth = -1;
uint16_t lastBarColor = 0;
String lastUptimeText;
Slide slides[MAX_SLIDES];
int slideCount = 0;
String missingFiles[MAX_MISSING_FILES];
int missingFileCount = 0;
String requiredFiles[MAX_REQUIRED_FILES];
int requiredFileCount = 0;

uint16_t color565(uint8_t r, uint8_t g, uint8_t b)
{
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

bool isLfsPath(const String &path)
{
  String lower = path;
  lower.toLowerCase();
  return lower.startsWith("lfs://");
}

String storagePath(const String &path)
{
  String lower = path;
  lower.toLowerCase();
  if (lower.startsWith("sd://"))
  {
    String p = path.substring(5);
    return p.startsWith("/") ? p : String("/") + p;
  }
  if (lower.startsWith("lfs://"))
  {
    String p = path.substring(6);
    return p.startsWith("/") ? p : String("/") + p;
  }
  return path;
}

String displayPath(const String &sdPath)
{
  String lower = sdPath;
  lower.toLowerCase();
  if (lower.startsWith("sd://") || lower.startsWith("lfs://")) return sdPath;
  String path = sdPath.startsWith("/") ? sdPath.substring(1) : sdPath;
  String pathLower = path;
  pathLower.toLowerCase();
  if (pathLower.startsWith("banners/")) path = String("banners/") + path.substring(8);
  return String("SD://") + path;
}

String macAddressText()
{
  uint64_t mac = ESP.getEfuseMac();
  char text[18];
  snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
           uint8_t(mac >> 40), uint8_t(mac >> 32), uint8_t(mac >> 24),
           uint8_t(mac >> 16), uint8_t(mac >> 8), uint8_t(mac));
  return String(text);
}

String wifiIpText()
{
  return WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("not connected");
}

String formatBytes(uint64_t bytes)
{
  if (bytes >= 1024ULL * 1024ULL * 1024ULL) return String(double(bytes) / double(1024ULL * 1024ULL * 1024ULL), 2) + " GiB";
  if (bytes >= 1024ULL * 1024ULL) return String(double(bytes) / double(1024ULL * 1024ULL), 1) + " MiB";
  if (bytes >= 1024ULL) return String(double(bytes) / 1024.0, 1) + " KiB";
  return String(bytes) + " B";
}

String sdFreeText()
{
  bool mounted = SD.begin(SD_CS_PIN);
  sdOk = mounted;
  if (!mounted)
  {
    digitalWrite(TOUCH_CS_PIN, HIGH);
    return "unavailable";
  }

  uint64_t total = SD.totalBytes();
  uint64_t used = SD.usedBytes();
  digitalWrite(TOUCH_CS_PIN, HIGH);
  if (total == 0) return "unknown";
  uint64_t freeBytes = total > used ? total - used : 0;
  return formatBytes(freeBytes) + " free / " + formatBytes(total);
}

void resetDynamicDrawState()
{
  lastBarWidth = -1;
  lastBarColor = 0;
  lastUptimeText = "";
}

void removeMissingFile(const String &path)
{
  for (int i = 0; i < missingFileCount; ++i)
  {
    if (missingFiles[i] == path)
    {
      for (int j = i; j < missingFileCount - 1; ++j) missingFiles[j] = missingFiles[j + 1];
      --missingFileCount;
      return;
    }
  }
}

void noteFileExists(const String &path)
{
  removeMissingFile(path);
}

void noteMissingFile(const String &path)
{
  Serial.print("Missing file: ");
  Serial.println(displayPath(path));
  for (int i = 0; i < missingFileCount; ++i)
  {
    if (missingFiles[i] == path) return;
  }
  if (missingFileCount >= MAX_MISSING_FILES)
  {
    for (int i = 0; i < MAX_MISSING_FILES - 1; ++i) missingFiles[i] = missingFiles[i + 1];
    missingFileCount = MAX_MISSING_FILES - 1;
  }
  missingFiles[missingFileCount++] = path;
}

void noteRequiredFile(const String &path)
{
  for (int i = 0; i < requiredFileCount; ++i)
  {
    if (requiredFiles[i] == path) return;
  }
  if (requiredFileCount < MAX_REQUIRED_FILES) requiredFiles[requiredFileCount++] = path;
}

bool fileExistsTracked(const String &path)
{
  String p = storagePath(path);
  bool exists = false;

  if (isLfsPath(path))
  {
    bool mounted = LittleFS.begin(false);
    littlefsOk = mounted;
    exists = mounted && LittleFS.exists(p);
    if (mounted) LittleFS.end();
  }
  else
  {
    bool mounted = SD.begin(SD_CS_PIN);
    sdOk = mounted;
    exists = mounted && SD.exists(p);
    digitalWrite(TOUCH_CS_PIN, HIGH);
  }

  if (exists) noteFileExists(path); else noteMissingFile(path);
  return exists;
}
