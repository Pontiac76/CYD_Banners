#include "network_manager.h"

#include "app_state.h"
#include "playlist_manager.h"
#include <HTTPClient.h>
#include <LittleFS.h>
#include <MD5Builder.h>

WifiProfile wifiProfiles[MAX_WIFI_PROFILES];
int wifiProfileCount = 0;
int currentWifiProfile = -1;
String updateSources[MAX_UPDATE_SOURCES];
int updateSourceCount = 0;
bool serviceTokenPresent = false;
String serviceToken;
String networkStatusText = "not started";
String updateStatusText = "not checked";
unsigned long lastCallHomeMs = 0;
int wifiCompletedCycles = 0;

namespace
{
constexpr unsigned long WIFI_CONNECT_WINDOW_MS = 9000;
constexpr unsigned long WIFI_RETRY_IDLE_MS = 4000;
constexpr unsigned long CALL_HOME_INTERVAL_MS = 60000;
unsigned long wifiAttemptStartedMs = 0;
unsigned long nextWifiAttemptMs = 0;
bool wifiAttemptActive = false;
bool callHomeProblem = true;
String lastLoggedConnectedSsid;
String lastRemoteSum;
String lastRemoteManifestBody;
String lastGoodUpdateSource;
unsigned long lastWorkNoticeMs = 0;

} // namespace

void drawInfoScreen();
void advanceSlide(bool force);
bool readPlaylistPathLine(String line, String &value);

void serviceUiDuringLongWork()
{
  if (!updateUiLocked) advanceSlide(false);
  yield();
}

namespace
{

void drawWorkNotice(const String &line1, const String &line2 = String(""))
{
  updateStatusText = line2.length() > 0 ? line1 + ": " + line2 : line1;
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

String trimCopy(String value)
{
  value.trim();
  return value;
}

bool parseKeyValue(const String &line, String &key, String &value)
{
  int eq = line.indexOf('=');
  if (eq < 0) return false;
  key = trimCopy(line.substring(0, eq));
  value = trimCopy(line.substring(eq + 1));
  key.toLowerCase();
  return key.length() > 0;
}

void loadWifiProfiles()
{
  wifiProfileCount = 0;
  if (!LittleFS.exists("/wifi.txt"))
  {
    networkStatusText = "LFS:/wifi.txt missing";
    return;
  }

  File file = LittleFS.open("/wifi.txt", FILE_READ);
  if (!file)
  {
    networkStatusText = "LFS:/wifi.txt open failed";
    return;
  }

  int active = -1;
  while (file.available() && wifiProfileCount < MAX_WIFI_PROFILES)
  {
    String line = file.readStringUntil('\n');
    line.replace("\r", "");
    line.trim();
    if (line.length() == 0 || line.startsWith("#") || line.startsWith(";")) continue;
    if (line.startsWith("[") && line.endsWith("]"))
    {
      active = wifiProfileCount++;
      wifiProfiles[active].name = line.substring(1, line.length() - 1);
      wifiProfiles[active].ssid = "";
      wifiProfiles[active].password = "";
      wifiProfiles[active].attempts = 0;
      continue;
    }
    if (active < 0) continue;
    String key, value;
    if (!parseKeyValue(line, key, value)) continue;
    if (key == "ssid") wifiProfiles[active].ssid = value;
    else if (key == "password") wifiProfiles[active].password = value;
  }
  file.close();

  int writeIndex = 0;
  for (int i = 0; i < wifiProfileCount; ++i)
  {
    if (wifiProfiles[i].ssid.length() == 0) continue;
    if (writeIndex != i) wifiProfiles[writeIndex] = wifiProfiles[i];
    ++writeIndex;
  }
  wifiProfileCount = writeIndex;
  networkStatusText = wifiProfileCount > 0 ? String("loaded ") + wifiProfileCount + " WiFi profile(s)" : "no WiFi profiles";
}

void loadPrivateConfig()
{
  updateSourceCount = 0;
  serviceTokenPresent = false;
  serviceToken = "";
  if (!LittleFS.exists("/config.txt"))
  {
    updateStatusText = "LFS:/config.txt missing";
    return;
  }

  File file = LittleFS.open("/config.txt", FILE_READ);
  if (!file)
  {
    updateStatusText = "LFS:/config.txt open failed";
    return;
  }

  while (file.available())
  {
    String line = file.readStringUntil('\n');
    line.replace("\r", "");
    line.trim();
    if (line.length() == 0 || line.startsWith("#") || line.startsWith(";")) continue;
    String key, value;
    if (!parseKeyValue(line, key, value)) continue;
    if (key == "service_token")
    {
      serviceToken = value;
      serviceTokenPresent = value.length() > 0;
    }
    else if (key.startsWith("update_source") && updateSourceCount < MAX_UPDATE_SOURCES && value.startsWith("http")) updateSources[updateSourceCount++] = value;
  }
  file.close();
  updateStatusText = String("token ") + (serviceTokenPresent ? "ok" : "missing") + ", sources " + updateSourceCount;
  write("Config loaded: ");
  writeln(updateStatusText);
  for (int i = 0; i < updateSourceCount; ++i)
  {
    write("Update source ");
    write(i + 1);
    write(": ");
    writeln(updateSources[i]);
  }
}

void startNextWifiAttempt()
{
  if (wifiProfileCount == 0) return;
  currentWifiProfile = (currentWifiProfile + 1) % wifiProfileCount;
  if (currentWifiProfile == 0) ++wifiCompletedCycles;
  wifiProfiles[currentWifiProfile].attempts++;
  WiFi.disconnect(true, true);
  delay(50);
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiProfiles[currentWifiProfile].ssid.c_str(), wifiProfiles[currentWifiProfile].password.c_str());
  wifiAttemptStartedMs = millis();
  wifiAttemptActive = true;
  networkStatusText = String("trying ") + wifiProfiles[currentWifiProfile].name;
  write("WiFi trying [");
  write(wifiProfiles[currentWifiProfile].name);
  write("]: ");
  writeln(wifiProfiles[currentWifiProfile].ssid);
}

String trimBody(String value)
{
  value.replace("\r", "");
  value.trim();
  return value;
}

bool ensureParentDirs(const String &sdPath);

String readLocalSum()
{
  String result;
  bool mounted = SD.begin(SD_CS_PIN);
  sdOk = mounted;
  if (!mounted)
  {
    digitalWrite(TOUCH_CS_PIN, HIGH);
    return result;
  }

  File file = SD.open("/banners/sum.txt", FILE_READ);
  if (file)
  {
    result = trimBody(file.readString());
    file.close();
  }
  /* SD stays mounted */
  digitalWrite(TOUCH_CS_PIN, HIGH);
  return result;
}

bool writeLocalTextFile(const String &sdPath, const String &body)
{
  bool mounted = SD.begin(SD_CS_PIN);
  sdOk = mounted;
  if (!mounted)
  {
    digitalWrite(TOUCH_CS_PIN, HIGH);
    write("Local file write failed, SD mount failed: ");
    writeln(sdPath);
    return false;
  }

  if (!ensureParentDirs(sdPath))
  {
    /* SD stays mounted */
    digitalWrite(TOUCH_CS_PIN, HIGH);
    write("Local file write failed, mkdir failed: ");
    writeln(sdPath);
    return false;
  }
  File file = SD.open(sdPath, FILE_WRITE);
  if (!file)
  {
    /* SD stays mounted */
    digitalWrite(TOUCH_CS_PIN, HIGH);
    write("Local file write failed, open failed: ");
    writeln(sdPath);
    return false;
  }
  size_t written = file.print(body);
  file.flush();
  file.close();
  if (written != body.length())
  {
    write("Local file write failed, short write: ");
    write(sdPath);
    write(" expected ");
    write(body.length());
    write(" wrote ");
    writeln(written);
    digitalWrite(TOUCH_CS_PIN, HIGH);
    return false;
  }
  digitalWrite(TOUCH_CS_PIN, HIGH);
  return true;
}

bool writeLocalManifest(const String &manifestBody)
{
  bool ok = writeLocalTextFile("/banners/manifest.txt", manifestBody.endsWith("\n") ? manifestBody : manifestBody + "\n");
  if (ok) writeln("Local manifest updated: SD://banners/manifest.txt");
  return ok;
}

void queueStaleSdCleanup();

bool writeLocalSum(const String &sum);

bool acceptRemoteManifestAndSum(const String &manifestBody, const String &sum)
{
  if (manifestBody.length() == 0)
  {
    writeln("Accept manifest/sum failed: empty manifest body");
    return false;
  }
  if (!writeLocalManifest(manifestBody))
  {
    writeln("Accept manifest/sum failed: manifest write failed; sum not updated");
    return false;
  }
  if (!writeLocalSum(sum))
  {
    writeln("Accept manifest/sum failed: sum write failed");
    return false;
  }
  return true;
}

bool writeLocalSum(const String &sum)
{
  bool mounted = SD.begin(SD_CS_PIN);
  sdOk = mounted;
  if (!mounted)
  {
    digitalWrite(TOUCH_CS_PIN, HIGH);
    writeln("Local sum write failed: SD mount failed");
    return false;
  }

  if (!SD.exists("/banners")) SD.mkdir("/banners");
  File file = SD.open("/banners/sum.txt", FILE_WRITE);
  if (!file)
  {
    /* SD stays mounted */
    digitalWrite(TOUCH_CS_PIN, HIGH);
    writeln("Local sum write failed: open failed");
    return false;
  }

  file.println(sum);
  file.close();
  /* SD stays mounted */
  digitalWrite(TOUCH_CS_PIN, HIGH);
  write("Local sum updated: SD://banners/sum.txt = ");
  writeln(sum);
  writeln("Local sum update complete; scheduling SD cleanup scan");
  queueStaleSdCleanup();
  return true;
}

String md5File(File &file)
{
  MD5Builder md5;
  md5.begin();
  uint8_t buffer[512];
  while (file.available())
  {
    size_t readCount = file.read(buffer, sizeof(buffer));
    if (readCount > 0) md5.add(buffer, readCount);
  }
  md5.calculate();
  return md5.toString();
}

struct ManifestEntry
{
  String hash;
  size_t size;
  String path;
  bool same;
  bool localKnown;
};

constexpr int MAX_MANIFEST_ENTRIES = 256;
ManifestEntry manifestEntries[MAX_MANIFEST_ENTRIES];
int manifestEntryCount = 0;
String backgroundDownloadQueue[MAX_MANIFEST_ENTRIES];
int backgroundDownloadCount = 0;
int backgroundDownloadIndex = 0;
int backgroundDownloadOkCount = 0;
String retryDownloadQueue[MAX_MANIFEST_ENTRIES];
int retryDownloadCount = 0;
unsigned long nextBackgroundDownloadMs = 0;
String cleanupDeleteQueue[MAX_MANIFEST_ENTRIES];
int cleanupDeleteCount = 0;
String diffClassifyPaths[MAX_MANIFEST_ENTRIES];
bool diffClassifyPriority[MAX_MANIFEST_ENTRIES];
int cleanupDeleteIndex = 0;
unsigned long nextCleanupDeleteMs = 0;
HTTPClient *backgroundHttp = nullptr;
File backgroundOut;
String backgroundActivePath;
String backgroundTmpPath;
String backgroundSdPath;
size_t backgroundExpectedSize = 0;
String backgroundExpectedHash;
int backgroundTotal = 0;
int backgroundWritten = 0;
bool manifestEntriesComplete = true;

bool manifestWildcardMatch(const char *pattern, const char *text)
{
  while (*pattern)
  {
    if (*pattern == '*')
    {
      while (*(pattern + 1) == '*') ++pattern;
      if (*(pattern + 1) == '\0') return true;
      for (const char *scan = text; *scan; ++scan)
      {
        if (manifestWildcardMatch(pattern + 1, scan)) return true;
      }
      return manifestWildcardMatch(pattern + 1, text);
    }
    if (*pattern == '?')
    {
      if (*text == '\0') return false;
      ++pattern;
      ++text;
      continue;
    }
    if (*pattern != *text) return false;
    ++pattern;
    ++text;
  }
  return *text == '\0';
}

bool hasManifestWildcard(const String &path)
{
  return path.indexOf('*') >= 0 || path.indexOf('?') >= 0;
}

bool manifestContainsRelPath(const String &relPath)
{
  for (int i = 0; i < manifestEntryCount; ++i)
  {
    if (manifestEntries[i].path == relPath) return true;
  }
  return false;
}

bool isTrackedMissingRelPath(const String &relPath)
{
  String wanted = String("/banners/") + relPath;
  wanted.toLowerCase();
  for (int i = 0; i < missingFileCount; ++i)
  {
    String tracked = missingFiles[i];
    tracked.toLowerCase();
    if (tracked.startsWith("sd://")) tracked = tracked.substring(5);
    if (!tracked.startsWith("/")) tracked = String("/") + tracked;
    if (tracked == wanted) return true;
  }
  return false;
}

bool internalManifestHasEntries()
{
  return manifestEntryCount > 0;
}

void insertManifestMatchSorted(String matches[], int &matchCount, int maxMatches, const String &path)
{
  if (matchCount >= maxMatches) return;
  int insertAt = matchCount;
  while (insertAt > 0 && path < matches[insertAt - 1])
  {
    matches[insertAt] = matches[insertAt - 1];
    --insertAt;
  }
  matches[insertAt] = path;
  ++matchCount;
}

String normalizeSdComparePath(String path)
{
  path.trim();
  String lower = path;
  lower.toLowerCase();
  if (lower.startsWith("sd://")) path = path.substring(5);
  if (!path.startsWith("/")) path = String("/") + path;
  lower = path;
  lower.toLowerCase();
  if (lower.startsWith("/banners/")) return String("/banners/") + path.substring(9);
  return path;
}

int internalCollectManifestMatches(const String &sdPattern, String matches[], int maxMatches)
{
  String normalizedPattern = normalizeSdComparePath(sdPattern);
  int matchCount = 0;
  for (int i = 0; i < manifestEntryCount; ++i)
  {
    String sdPath = normalizeSdComparePath(String("/banners/") + manifestEntries[i].path);
    if (manifestWildcardMatch(normalizedPattern.c_str(), sdPath.c_str()))
    {
      insertManifestMatchSorted(matches, matchCount, maxMatches, sdPath);
    }
  }
  return matchCount;
}

String manifestMacNoColon()
{
  String mac = macAddressText();
  mac.replace(":", "");
  mac.replace("-", "");
  mac.toUpperCase();
  return mac;
}

bool isOwnGeneratedPlaylistChunk(const String &relPath)
{
  String prefix = String("_generated/playlists/") + manifestMacNoColon() + "/playlist_";
  return relPath.startsWith(prefix) && relPath.endsWith(".ini");
}

bool isPriorityManifestPath(const String &relPath)
{
  if (relPath == "playlist.ini") return true;
  if (isOwnGeneratedPlaylistChunk(relPath)) return true;
  String sdPath = normalizeSdComparePath(String("/banners/") + relPath);
  for (int i = 0; i < requiredFileCount; ++i)
  {
    String requiredPath = normalizeSdComparePath(requiredFiles[i]);
    if (requiredPath == sdPath) return true;
    if (hasManifestWildcard(requiredPath) && manifestWildcardMatch(requiredPath.c_str(), sdPath.c_str())) return true;
  }
  for (int i = 0; i < slideCount; ++i)
  {
    if (normalizeSdComparePath(slides[i].pathOrPayload) == sdPath) return true;
  }
  return false;
}

bool beginSdWithRetry(const char *context, int attempts = 3)
{
  // If another path left SD mounted, do not call SD.begin() again; on this
  // Arduino core that can fail with esp_vfs_fat_register 0x101 even though the
  // filesystem is usable.
  if (SD.exists(PROJECT_ROOT))
  {
    sdOk = true;
    if (strcmp(context, "Pending update check") != 0)
    {
      write(context);
      writeln(": SD already mounted");
    }
    return true;
  }

  /* SD stays mounted */
  digitalWrite(TOUCH_CS_PIN, HIGH);
  delay(25);
  for (int attempt = 1; attempt <= attempts; ++attempt)
  {
    if (SD.begin(SD_CS_PIN))
    {
      sdOk = true;
      if (attempt > 1)
      {
        write(context);
        write(": SD mount ok on retry ");
        writeln(attempt);
      }
      return true;
    }
    sdOk = false;
    write(context);
    write(": SD mount failed attempt ");
    write(attempt);
    write("/");
    writeln(attempts);
    /* SD stays mounted */
    digitalWrite(TOUCH_CS_PIN, HIGH);
    delay(100);
  }
  return false;
}

bool ensureParentDirs(const String &sdPath)
{
  int slash = sdPath.indexOf('/', 1);
  while (slash > 0)
  {
    String dir = sdPath.substring(0, slash);
    if (!SD.exists(dir) && !SD.mkdir(dir)) return false;
    slash = sdPath.indexOf('/', slash + 1);
  }
  return true;
}

bool validateDownloadedFile(const String &tmpPath, size_t expectedSize, const String &expectedHash)
{
  File file = SD.open(tmpPath, FILE_READ);
  if (!file)
  {
    write("DOWNLOAD validate open failed: ");
    writeln(tmpPath);
    return false;
  }
  size_t actualSize = file.size();
  if (actualSize != expectedSize)
  {
    file.close();
    write("DOWNLOAD validate size failed: ");
    write(tmpPath);
    write(" expected ");
    write(expectedSize);
    write(" actual ");
    writeln(actualSize);
    return false;
  }
  String actualHash = md5File(file);
  file.close();
  if (!actualHash.equalsIgnoreCase(expectedHash))
  {
    write("DOWNLOAD validate hash failed: ");
    write(tmpPath);
    write(" expected ");
    write(expectedHash);
    write(" actual ");
    writeln(actualHash);
    return false;
  }
  return true;
}

bool downloadManifestFile(const String &updateSource, const String &relPath, size_t expectedSize, const String &expectedHash, bool visibleWork)
{
  if (visibleWork) drawWorkNotice("Downloading", String("SD://banners/") + relPath);
  write("DOWNLOAD start: SD://banners/");
  write(relPath);
  write(" expected ");
  write(expectedSize);
  write(" bytes from ");
  writeln(updateSource);
  String url = updateSource + "/files/" + relPath;
  String sdPath = String("/banners/") + relPath;
  String tmpPath = sdPath + ".tmp";

  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(3000);
  http.setTimeout(15000);
  http.addHeader("Connection", "close");
  if (!http.begin(url))
  {
    write("DOWNLOAD begin failed: ");
    writeln(url);
    return false;
  }
  int code = http.GET();
  write("DOWNLOAD HTTP ");
  write(code);
  write(": SD://banners/");
  writeln(relPath);
  if (code < 200 || code >= 300)
  {
    write("DOWNLOAD failed HTTP ");
    write(code);
    write(": ");
    writeln(url);
    http.end();
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  stream->setTimeout(15000);
  if (!ensureParentDirs(sdPath))
  {
    write("DOWNLOAD mkdir failed: SD://banners/");
    writeln(relPath);
    http.end();
    return false;
  }
  SD.remove(tmpPath);
  File out = SD.open(tmpPath, FILE_WRITE);
  if (!out)
  {
    write("DOWNLOAD open failed: SD://banners/");
    writeln(relPath);
    http.end();
    return false;
  }

  uint8_t buffer[512];
  int total = http.getSize();
  if (total <= 0 && expectedSize > 0) total = (int)expectedSize;
  int written = 0;
  unsigned long lastProgressMs = millis();
  unsigned long lastProgressLogMs = millis();
  while (http.connected() && (total < 0 || written < total))
  {
    if (visibleWork && total > 0)
    {
      drawWorkNotice("Downloading", String(written / 1024) + "/" + String(total / 1024) + " KiB " + relPath);
    }
    int remaining = total > 0 ? total - written : (int)sizeof(buffer);
    int wanted = total > 0 ? min((int)sizeof(buffer), remaining) : (int)sizeof(buffer);
    int readCount = stream->readBytes(buffer, wanted);
    if (readCount <= 0)
    {
      write("DOWNLOAD read timeout/stall: SD://banners/");
      write(relPath);
      write(" wrote ");
      write(written);
      write("/");
      writeln(total);
      break;
    }
    size_t wrote = out.write(buffer, readCount);
    if (wrote != (size_t)readCount)
    {
      write("DOWNLOAD write failed: SD://banners/");
      writeln(relPath);
      break;
    }
    written += readCount;
    lastProgressMs = millis();
    if (millis() - lastProgressLogMs > 5000UL)
    {
      write("DOWNLOAD progress: SD://banners/");
      write(relPath);
      write(" ");
      write(written);
      write("/");
      writeln(total);
      lastProgressLogMs = millis();
    }
  }
  out.close();
  http.end();

  if (!validateDownloadedFile(tmpPath, expectedSize, expectedHash))
  {
    SD.remove(tmpPath);
    return false;
  }

  SD.remove(sdPath);
  if (!SD.rename(tmpPath, sdPath))
  {
    write("DOWNLOAD rename failed: SD://banners/");
    writeln(relPath);
    return false;
  }
  write("DOWNLOAD ok: SD://banners/");
  write(relPath);
  write(" bytes ");
  writeln(written);
  return true;
}

void writeRetryFile()
{
  SD.remove("/banners/retry.txt");
  if (retryDownloadCount == 0) return;
  ensureParentDirs("/banners/retry.txt");
  File file = SD.open("/banners/retry.txt", FILE_WRITE);
  if (!file)
  {
    writeln("Retry file write failed: open failed");
    return;
  }
  for (int i = 0; i < retryDownloadCount; ++i) file.println(retryDownloadQueue[i]);
  file.close();
}

void enqueueRetryDownload(const String &relPath)
{
  for (int i = 0; i < retryDownloadCount; ++i)
  {
    if (retryDownloadQueue[i] == relPath) return;
  }
  if (retryDownloadCount >= MAX_MANIFEST_ENTRIES)
  {
    write("Retry queue full; dropping: SD://banners/");
    writeln(relPath);
    return;
  }
  retryDownloadQueue[retryDownloadCount++] = relPath;
  write("Retry queued: SD://banners/");
  writeln(relPath);
  writeRetryFile();
}

void removeRetryDownloadAt(int index)
{
  if (index < 0 || index >= retryDownloadCount) return;
  for (int i = index; i < retryDownloadCount - 1; ++i) retryDownloadQueue[i] = retryDownloadQueue[i + 1];
  --retryDownloadCount;
  writeRetryFile();
}

bool downloadManifestFileWithRetry(const String &updateSource, const String &relPath, size_t expectedSize, const String &expectedHash, bool visibleWork, int attempts = 3)
{
  for (int attempt = 1; attempt <= attempts; ++attempt)
  {
    if (attempt > 1)
    {
      write("DOWNLOAD retry ");
      write(attempt);
      write("/");
      write(attempts);
      write(": SD://banners/");
      writeln(relPath);
      delay(500);
    }
    if (downloadManifestFile(updateSource, relPath, expectedSize, expectedHash, visibleWork)) return true;
  }
  write("DOWNLOAD failed after retries: SD://banners/");
  writeln(relPath);
  enqueueRetryDownload(relPath);
  return false;
}

int findManifestEntryIndex(const String &relPath)
{
  for (int i = 0; i < manifestEntryCount; ++i)
  {
    if (manifestEntries[i].path == relPath) return i;
  }
  return -1;
}

void clearCleanupDeletes()
{
  cleanupDeleteCount = 0;
  cleanupDeleteIndex = 0;
  nextCleanupDeleteMs = 0;
}

void queueCleanupPath(const String &sdPath)
{
  if (cleanupDeleteCount >= MAX_MANIFEST_ENTRIES) return;
  for (int i = 0; i < cleanupDeleteCount; ++i)
  {
    if (cleanupDeleteQueue[i] == sdPath) return;
  }
  cleanupDeleteQueue[cleanupDeleteCount++] = sdPath;
}

void scanCleanupDir(const String &dirPath)
{
  File dir = SD.open(dirPath, FILE_READ);
  if (!dir || !dir.isDirectory())
  {
    if (dir) dir.close();
    return;
  }
  while (cleanupDeleteCount < MAX_MANIFEST_ENTRIES)
  {
    File entry = dir.openNextFile();
    if (!entry) break;
    String name = String(entry.name());
    String path = name.startsWith("/") ? name : (dirPath == "/" ? String("/") + name : dirPath + "/" + name);
    if (entry.isDirectory())
    {
      entry.close();
      scanCleanupDir(path);
    }
    else
    {
      entry.close();
      String rel = path;
      if (rel.startsWith("/banners/")) rel = rel.substring(9);
      bool keepLocalIndex = rel == "manifest.txt" || rel == "sum.txt" || rel == "runtime_playlist.cache";
      bool tempFile = path.endsWith(".tmp") || path.indexOf("/tmp/") >= 0 || path.indexOf("/temp/") >= 0;
      if (!keepLocalIndex && (tempFile || !manifestContainsRelPath(rel))) queueCleanupPath(path);
    }
  }
  dir.close();
}

void queueStaleSdCleanup()
{
  writeln("Cleanup scan requested after sum.txt update");
  clearCleanupDeletes();
  if (!manifestEntriesComplete)
  {
    writeln("Cleanup scan skipped: manifest entry list overflow/incomplete");
    return;
  }
  if (manifestEntryCount == 0)
  {
    writeln("Cleanup scan skipped: RAM manifest has no entries");
    return;
  }
  bool mounted = SD.begin(SD_CS_PIN);
  sdOk = mounted;
  if (!mounted)
  {
    digitalWrite(TOUCH_CS_PIN, HIGH);
    writeln("Cleanup scan skipped: SD mount failed");
    return;
  }
  write("Cleanup scan starting: RAM manifest entries ");
  writeln(manifestEntryCount);
  scanCleanupDir("/banners");
  /* SD stays mounted */
  digitalWrite(TOUCH_CS_PIN, HIGH);
  if (cleanupDeleteCount == 0)
  {
    writeln("Cleanup scan complete: no stale/temp files queued");
  }
  if (cleanupDeleteCount > 0)
  {
    write("Cleanup deletes queued: ");
    writeln(cleanupDeleteCount);
    for (int i = 0; i < cleanupDeleteCount; ++i)
    {
      write("  DELETE ");
      writeln(cleanupDeleteQueue[i]);
    }
  }
}

void processCleanupDelete()
{
  if (cleanupDeleteIndex >= cleanupDeleteCount) return;
  unsigned long now = millis();
  if (long(now - nextCleanupDeleteMs) < 0) return;
  nextCleanupDeleteMs = now + 500UL;
  bool mounted = SD.begin(SD_CS_PIN);
  sdOk = mounted;
  if (!mounted)
  {
    digitalWrite(TOUCH_CS_PIN, HIGH);
    writeln("Cleanup delete skipped: SD mount failed");
    return;
  }
  String path = cleanupDeleteQueue[cleanupDeleteIndex++];
  write("Cleanup delete ");
  write(cleanupDeleteIndex);
  write("/");
  write(cleanupDeleteCount);
  write(": ");
  writeln(path);
  bool deleted = SD.remove(path);
  if (deleted)
  {
    write("Cleanup deleted: ");
    writeln(path);
  }
  else
  {
    write("Cleanup delete failed: ");
    writeln(path);
  }
  bool cleanupComplete = cleanupDeleteIndex >= cleanupDeleteCount;
  /* SD stays mounted */
  digitalWrite(TOUCH_CS_PIN, HIGH);
  if (cleanupComplete)
  {
    writeln("Cleanup deletes complete; rebuilding runtime playlist");
    rebuildPlaylist();
  }
}

void clearBackgroundDownloads()
{
  backgroundDownloadCount = 0;
  backgroundDownloadIndex = 0;
  backgroundDownloadOkCount = 0;
  nextBackgroundDownloadMs = 0;
  if (backgroundOut) backgroundOut.close();
  if (backgroundHttp)
  {
    backgroundHttp->end();
    delete backgroundHttp;
    backgroundHttp = nullptr;
  }
  backgroundActivePath = "";
  backgroundTmpPath = "";
  backgroundSdPath = "";
  backgroundExpectedSize = 0;
  backgroundExpectedHash = "";
  backgroundTotal = 0;
  backgroundWritten = 0;
}

bool priorityChangesPending()
{
  for (int i = 0; i < manifestEntryCount; ++i)
  {
    if (manifestEntries[i].same) continue;
    if (isPriorityManifestPath(manifestEntries[i].path)) return true;
  }
  return false;
}

void queueBackgroundDownloads()
{
  clearBackgroundDownloads();
  for (int i = 0; i < manifestEntryCount && backgroundDownloadCount < MAX_MANIFEST_ENTRIES; ++i)
  {
    if (!manifestEntries[i].same && !isPriorityManifestPath(manifestEntries[i].path))
    {
      backgroundDownloadQueue[backgroundDownloadCount++] = manifestEntries[i].path;
    }
  }
  if (backgroundDownloadCount > 0)
  {
    write("Background downloads queued: ");
    writeln(backgroundDownloadCount);
    for (int i = 0; i < backgroundDownloadCount; ++i)
    {
      write("  BACKGROUND ");
      write(i + 1);
      write("/");
      write(backgroundDownloadCount);
      write(" SD://banners/");
      writeln(backgroundDownloadQueue[i]);
    }
  }
}

bool retryDownloadsPending()
{
  return retryDownloadCount > 0;
}

bool backgroundDownloadsPending()
{
  return retryDownloadsPending() || backgroundDownloadIndex < backgroundDownloadCount;
}

bool cleanupDeletesPending()
{
  return cleanupDeleteIndex < cleanupDeleteCount;
}

void finishBackgroundQueueIfDone()
{
  if (retryDownloadsPending() || backgroundDownloadIndex < backgroundDownloadCount) return;
  write("Background downloads complete: ok ");
  write(backgroundDownloadOkCount);
  write("/");
  writeln(backgroundDownloadCount);
  if (lastRemoteSum.length() > 0)
  {
    if (backgroundDownloadOkCount != backgroundDownloadCount)
    {
      writeln("Background downloads had failures; accepting current manifest/sum to avoid repeated full replans");
    }
    if (acceptRemoteManifestAndSum(lastRemoteManifestBody, lastRemoteSum))
    {
      updateStatusText = backgroundDownloadOkCount == backgroundDownloadCount ? "content current" : "content current; bg failures";
    }
    else
    {
      updateStatusText = "manifest accept failed";
    }
  }
}

void processBackgroundDownload()
{
  if (!retryDownloadsPending() && backgroundDownloadIndex >= backgroundDownloadCount) return;
  if (WiFi.status() != WL_CONNECTED) return;
  unsigned long now = millis();
  if (long(now - nextBackgroundDownloadMs) < 0) return;
  nextBackgroundDownloadMs = now + 1500UL;

  bool mounted = SD.begin(SD_CS_PIN);
  sdOk = mounted;
  if (!mounted)
  {
    digitalWrite(TOUCH_CS_PIN, HIGH);
    writeln("Background download skipped: SD mount failed");
    return;
  }

  bool retryItem = retryDownloadsPending();
  String path = retryItem ? retryDownloadQueue[0] : backgroundDownloadQueue[backgroundDownloadIndex++];
  String source = lastGoodUpdateSource.length() > 0 ? lastGoodUpdateSource : (updateSourceCount > 0 ? updateSources[0] : String(""));
  if (source.length() == 0)
  {
    /* SD stays mounted */
    digitalWrite(TOUCH_CS_PIN, HIGH);
    writeln("Background download skipped: no update source");
    return;
  }
  write(retryItem ? "Retry download " : "Background download ");
  write(retryItem ? 1 : backgroundDownloadIndex);
  write("/");
  write(retryItem ? retryDownloadCount : backgroundDownloadCount);
  write(": SD://banners/");
  writeln(path);
  int manifestIndex = findManifestEntryIndex(path);
  if (manifestIndex >= 0 && downloadManifestFileWithRetry(source, path, manifestEntries[manifestIndex].size, manifestEntries[manifestIndex].hash, false))
  {
    if (manifestIndex >= 0) manifestEntries[manifestIndex].same = true;
    if (retryItem) removeRetryDownloadAt(0); else ++backgroundDownloadOkCount;
  }
  /* SD stays mounted */
  digitalWrite(TOUCH_CS_PIN, HIGH);

  finishBackgroundQueueIfDone();
}

bool parseManifestFileLine(const String &lineIn, String &path, String &hash, size_t &size)
{
  String line = lineIn;
  line.replace("\r", "");
  line.trim();
  if (!line.startsWith("FILE ")) return false;
  int hashStart = 5;
  int hashEnd = line.indexOf(' ', hashStart);
  int sizeEnd = hashEnd >= 0 ? line.indexOf(' ', hashEnd + 1) : -1;
  if (hashEnd < 0 || sizeEnd < 0) return false;
  hash = line.substring(hashStart, hashEnd);
  size = line.substring(hashEnd + 1, sizeEnd).toInt();
  path = line.substring(sizeEnd + 1);
  path.trim();
  return path.length() > 0;
}

bool findManifestLineEntry(const String &manifestBody, const String &relPath, String &hash, size_t &size)
{
  int cursor = 0;
  while (cursor < manifestBody.length())
  {
    int next = manifestBody.indexOf('\n', cursor);
    if (next < 0) next = manifestBody.length();
    String path;
    if (parseManifestFileLine(manifestBody.substring(cursor, next), path, hash, size) && path == relPath) return true;
    cursor = next + 1;
  }
  return false;
}

bool findLocalManifestFileEntry(const String &relPath, String &hash, size_t &size)
{
  File file = SD.open("/banners/manifest.txt", FILE_READ);
  if (!file) return false;
  while (file.available())
  {
    String path;
    String line = file.readStringUntil('\n');
    if (parseManifestFileLine(line, path, hash, size) && path == relPath)
    {
      file.close();
      return true;
    }
  }
  file.close();
  return false;
}

String readLocalManifestBody()
{
  String result;
  File file = SD.open("/banners/manifest.txt", FILE_READ);
  if (file)
  {
    size_t size = file.size();
    if (size > 0) result.reserve(size + 1);
    char buffer[256];
    while (file.available())
    {
      size_t count = file.readBytes(buffer, sizeof(buffer));
      if (count == 0) break;
      for (size_t i = 0; i < count; ++i) result += buffer[i];
    }
    file.close();
    write("Local manifest read bytes ");
    write(result.length());
    write(" of file size ");
    writeln(size);
  }
  else
  {
    writeln("Local manifest read failed: open failed");
  }
  return result;
}

bool actualSdFileMatchesManifest(String sdPath, size_t expectedSize, const String &expectedHash)
{
  File file = SD.open(sdPath, FILE_READ);
  if (!file && sdPath.startsWith("/banners/Banners/")) file = SD.open(String("/banners/") + sdPath.substring(17), FILE_READ);
  if (!file && sdPath.startsWith("/banners/banners/")) file = SD.open(String("/banners/") + sdPath.substring(17), FILE_READ);
  if (!file) return false;
  size_t actualSize = file.size();
  if (actualSize != expectedSize)
  {
    file.close();
    return false;
  }
  String actualHash = md5File(file);
  file.close();
  return actualHash.equalsIgnoreCase(expectedHash);
}

const char *UPDATE_ROOT = "/banners/_update";
const char *REMOTE_MANIFEST_TMP = "/banners/manifest.txt.tmp";
const char *REMOTE_SUM_TMP = "/banners/sum.txt.tmp";
const char *DIFF_QUEUE_LIST = "/banners/_update/diff.list";
const char *PRIORITY_QUEUE_LIST = "/banners/_update/p.list";
const char *BACKGROUND_QUEUE_LIST = "/banners/_update/b.list";

const char *queueListPath(const char *queue)
{
  if (strcmp(queue, "diff") == 0) return DIFF_QUEUE_LIST;
  return strcmp(queue, "p") == 0 ? PRIORITY_QUEUE_LIST : BACKGROUND_QUEUE_LIST;
}

String queuePathFor(const char *queue, const String &relPath)
{
  return String(UPDATE_ROOT) + "/stage/" + queue + "/" + relPath;
}

bool appendQueueList(const char *queue, const String &relPath)
{
  const char *listPath = queueListPath(queue);
  ensureParentDirs(listPath);
  File f = SD.open(listPath, FILE_APPEND);
  if (!f) return false;
  f.println(relPath);
  f.close();
  return true;
}

bool readFirstQueueListPath(const char *queue, String &relPath)
{
  File f = SD.open(queueListPath(queue), FILE_READ);
  if (!f) return false;
  while (f.available())
  {
    relPath = f.readStringUntil('\n');
    relPath.replace("\r", "");
    relPath.trim();
    if (relPath.length() > 0)
    {
      f.close();
      return true;
    }
  }
  f.close();
  return false;
}

void removeFirstQueueListPath(const char *queue)
{
  const char *listPath = queueListPath(queue);
  String tmpPath = String(listPath) + ".tmp";
  File in = SD.open(listPath, FILE_READ);
  if (!in) return;
  SD.remove(tmpPath);
  File out = SD.open(tmpPath, FILE_WRITE);
  bool skipped = false;
  while (in.available())
  {
    String line = in.readStringUntil('\n');
    String trimmed = line;
    trimmed.replace("\r", "");
    trimmed.trim();
    if (!skipped && trimmed.length() > 0)
    {
      skipped = true;
      continue;
    }
    if (trimmed.length() > 0) out.println(trimmed);
  }
  in.close();
  out.close();
  SD.remove(listPath);
  SD.rename(tmpPath, listPath);
}

bool writeZeroFile(const String &path)
{
  if (!ensureParentDirs(path)) return false;
  SD.remove(path);
  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;
  f.close();
  return true;
}

bool readManifestFileEntry(const char *manifestPath, const String &relPath, String &hash, size_t &size)
{
  File file = SD.open(manifestPath, FILE_READ);
  if (!file) return false;
  while (file.available())
  {
    String path;
    if (parseManifestFileLine(file.readStringUntil('\n'), path, hash, size) && path == relPath)
    {
      file.close();
      return true;
    }
  }
  file.close();
  return false;
}

bool findPendingManifestEntry(const String &relPath, String &hash, size_t &size)
{
  return readManifestFileEntry(REMOTE_MANIFEST_TMP, relPath, hash, size);
}

bool pathIsInGeneratedPlaylistChunks(const String &relPath)
{
  String dir = String(PROJECT_ROOT) + "/_generated/playlists/" + manifestMacNoColon();
  for (int chunk = 0; chunk < 100; ++chunk)
  {
    char name[32];
    snprintf(name, sizeof(name), "/playlist_%03d.ini", chunk);
    String chunkPath = dir + name;
    if (!SD.exists(chunkPath)) break;
    File file = SD.open(chunkPath, FILE_READ);
    if (!file) continue;
    while (file.available())
    {
      String value;
      if (readPlaylistPathLine(file.readStringUntil('\n'), value))
      {
        String normalized = normalizeSdComparePath(value);
        String wanted = normalizeSdComparePath(String("/banners/") + relPath);
        if (normalized == wanted || (hasManifestWildcard(normalized) && manifestWildcardMatch(normalized.c_str(), wanted.c_str())))
        {
          file.close();
          return true;
        }
      }
    }
    file.close();
  }
  return false;
}

bool pendingPathIsPriority(const String &relPath)
{
  if (relPath == "playlist.ini") return true;
  if (isOwnGeneratedPlaylistChunk(relPath)) return true;
  return pathIsInGeneratedPlaylistChunks(relPath);
}

bool downloadRemoteManifestToTmp(const String &updateSource)
{
  bool mounted = beginSdWithRetry("Pending manifest download");
  if (!mounted) return false;
  String url = updateSource + "/manifest.txt";
  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(3000);
  http.setTimeout(15000);
  http.addHeader("Connection", "close");
  if (!http.begin(url)) return false;
  int code = http.GET();
  write("Manifest GET ");
  write(url);
  write(" -> HTTP ");
  writeln(code);
  if (code < 200 || code >= 300)
  {
    http.end();
    return false;
  }
  SD.remove(REMOTE_MANIFEST_TMP);
  File out = SD.open(REMOTE_MANIFEST_TMP, FILE_WRITE);
  if (!out)
  {
    http.end();
    return false;
  }
  WiFiClient *stream = http.getStreamPtr();
  uint8_t buffer[512];
  int total = http.getSize();
  int written = 0;
  while (http.connected() && (total < 0 || written < total))
  {
    int wanted = total > 0 ? min((int)sizeof(buffer), total - written) : (int)sizeof(buffer);
    int got = stream->readBytes(buffer, wanted);
    if (got <= 0) break;
    out.write(buffer, got);
    written += got;
  }
  out.flush();
  out.close();
  http.end();
  write("Pending manifest saved bytes ");
  writeln(written);
  return written > 0;
}

bool readNextManifestEntry(File &file, String &path, String &hash, size_t &size)
{
  while (file && file.available())
  {
    if (parseManifestFileLine(file.readStringUntil('\n'), path, hash, size)) return true;
    serviceUiDuringLongWork();
  }
  return false;
}

int compareManifestPaths(const String &left, const String &right)
{
  String a = left;
  String b = right;
  a.toLowerCase();
  b.toLowerCase();
  if (a == b) return 0;
  return a < b ? -1 : 1;
}

bool buildDiffQueueFromPendingManifest()
{
  File remote = SD.open(REMOTE_MANIFEST_TMP, FILE_READ);
  if (!remote)
  {
    writeln("Diff build failed: pending manifest missing");
    return false;
  }
  File local = SD.open("/banners/manifest.txt", FILE_READ);

  bool haveRemote = false;
  bool haveLocal = false;
  String remotePath, remoteHash, localPath, localHash;
  size_t remoteSize = 0, localSize = 0;
  haveRemote = readNextManifestEntry(remote, remotePath, remoteHash, remoteSize);
  if (local) haveLocal = readNextManifestEntry(local, localPath, localHash, localSize);

  SD.remove(DIFF_QUEUE_LIST);
  int diffCount = 0;
  int remoteCount = 0;
  unsigned long lastYieldMs = millis();
  while (haveRemote)
  {
    if (millis() - lastYieldMs >= 500UL)
    {
      write("Diff progress uptime=");
      write(millis() / 1000UL);
      write("s remote lines ");
      write(remoteCount);
      write(", diffs ");
      writeln(diffCount);
      serviceUiDuringLongWork();
      delay(1);
      lastYieldMs = millis();
    }

    int cmp = haveLocal ? compareManifestPaths(localPath, remotePath) : 1;
    if (!haveLocal || cmp > 0)
    {
      // Remote path is new/missing locally.
      if (appendQueueList("diff", remotePath)) ++diffCount;
      ++remoteCount;
      haveRemote = readNextManifestEntry(remote, remotePath, remoteHash, remoteSize);
    }
    else if (cmp < 0)
    {
      // Local-only stale path; ignore here. Cleanup handles stale files later.
      haveLocal = readNextManifestEntry(local, localPath, localHash, localSize);
    }
    else
    {
      if (localSize != remoteSize || !localHash.equalsIgnoreCase(remoteHash))
      {
        if (appendQueueList("diff", remotePath)) ++diffCount;
      }
      ++remoteCount;
      haveRemote = readNextManifestEntry(remote, remotePath, remoteHash, remoteSize);
      haveLocal = readNextManifestEntry(local, localPath, localHash, localSize);
    }
  }
  if (local) local.close();
  remote.close();
  write("Diff queue built: ");
  write(diffCount);
  write(" diffs from ");
  write(remoteCount);
  writeln(" remote FILE lines");
  return true;
}

bool downloadToQueueFile(const String &updateSource, const String &relPath, const char *queue, bool visibleWork);

bool findFirstQueueFileRecursive(const String &dirPath, String &relPath, const String &basePrefix)
{
  File dir = SD.open(dirPath, FILE_READ);
  if (!dir || !dir.isDirectory())
  {
    if (dir) dir.close();
    return false;
  }
  File entry = dir.openNextFile();
  while (entry)
  {
    String name = entry.name();
    bool isDir = entry.isDirectory();
    entry.close();
    String childPath = name.startsWith("/") ? name : dirPath + "/" + name;
    if (isDir)
    {
      if (findFirstQueueFileRecursive(childPath, relPath, basePrefix))
      {
        dir.close();
        return true;
      }
    }
    else
    {
      relPath = childPath.substring(basePrefix.length());
      if (relPath.startsWith("/")) relPath = relPath.substring(1);
      dir.close();
      return true;
    }
    entry = dir.openNextFile();
  }
  dir.close();
  return false;
}

bool findFirstQueueFile(const char *queue, String &relPath)
{
  String base = String(UPDATE_ROOT) + "/" + queue;
  return findFirstQueueFileRecursive(base, relPath, base);
}

bool queueHasFiles(const char *queue)
{
  String rel;
  return readFirstQueueListPath(queue, rel);
}

bool processOneQueuedDownload(const String &updateSource, const char *queue, bool visibleWork)
{
  unsigned long locateStartMs = millis();
  String relPath;
  bool fromList = readFirstQueueListPath(queue, relPath);
  if (!fromList) return false;
  unsigned long locateElapsedMs = millis() - locateStartMs;
  if (locateElapsedMs >= 1000UL)
  {
    write("Queue locate slow queue=");
    write(queue);
    write(" source=");
    write(fromList ? "list" : "dir");
    write(" elapsed=");
    write((locateElapsedMs + 500UL) / 1000UL);
    writeln("s");
  }
  write(visibleWork ? "Priority queued download: " : "Background queued download: ");
  writeln(relPath);
  bool ok = downloadToQueueFile(updateSource, relPath, queue, visibleWork);
  if (ok && fromList) removeFirstQueueListPath(queue);
  return ok;
}

void collectDiffRecursive(const String &dirPath, const String &basePrefix, String paths[], bool priority[], int &count)
{
  File dir = SD.open(dirPath, FILE_READ);
  if (!dir || !dir.isDirectory())
  {
    if (dir) dir.close();
    return;
  }
  File entry = dir.openNextFile();
  while (entry)
  {
    String name = entry.name();
    bool isDir = entry.isDirectory();
    entry.close();
    String childPath = name.startsWith("/") ? name : dirPath + "/" + name;
    if (isDir)
    {
      collectDiffRecursive(childPath, basePrefix, paths, priority, count);
    }
    else if (count < MAX_MANIFEST_ENTRIES)
    {
      String relPath = childPath.substring(basePrefix.length());
      if (relPath.startsWith("/")) relPath = relPath.substring(1);
      paths[count] = relPath;
      priority[count] = (relPath == "playlist.ini" || isOwnGeneratedPlaylistChunk(relPath));
      ++count;
    }
    entry = dir.openNextFile();
  }
  dir.close();
}

int findDiffPathIndex(String paths[], int count, const String &relPath)
{
  String wanted = normalizeSdComparePath(String("/banners/") + relPath);
  for (int i = 0; i < count; ++i)
  {
    if (normalizeSdComparePath(String("/banners/") + paths[i]) == wanted) return i;
  }
  return -1;
}

void markPriorityFromGeneratedPlaylists(String paths[], bool priority[], int count)
{
  String dir = String(PROJECT_ROOT) + "/_generated/playlists/" + manifestMacNoColon();
  for (int chunk = 0; chunk < 100; ++chunk)
  {
    char name[32];
    snprintf(name, sizeof(name), "/playlist_%03d.ini", chunk);
    String chunkPath = dir + name;
    if (!SD.exists(chunkPath)) break;
    File file = SD.open(chunkPath, FILE_READ);
    if (!file) continue;
    while (file.available())
    {
      String value;
      if (readPlaylistPathLine(file.readStringUntil('\n'), value))
      {
        int optionSep = value.indexOf('|');
        if (optionSep >= 0) value = value.substring(0, optionSep);
        String normalized = normalizeSdComparePath(value);
        for (int i = 0; i < count; ++i)
        {
          if (priority[i]) continue;
          String wanted = normalizeSdComparePath(String("/banners/") + paths[i]);
          if (normalized == wanted || (hasManifestWildcard(normalized) && manifestWildcardMatch(normalized.c_str(), wanted.c_str()))) priority[i] = true;
        }
      }
      serviceUiDuringLongWork();
    }
    file.close();
  }
}

void promoteBackgroundPriorityRecursive(const String &dirPath, const String &basePrefix, int &promoted)
{
  File dir = SD.open(dirPath, FILE_READ);
  if (!dir || !dir.isDirectory())
  {
    if (dir) dir.close();
    return;
  }
  File entry = dir.openNextFile();
  while (entry)
  {
    String name = entry.name();
    bool isDir = entry.isDirectory();
    entry.close();
    String childPath = name.startsWith("/") ? name : dirPath + "/" + name;
    if (isDir)
    {
      promoteBackgroundPriorityRecursive(childPath, basePrefix, promoted);
    }
    else
    {
      String relPath = childPath.substring(basePrefix.length());
      if (relPath.startsWith("/")) relPath = relPath.substring(1);
      if (pendingPathIsPriority(relPath))
      {
        String pPath = queuePathFor("p", relPath);
        ensureParentDirs(pPath);
        SD.remove(pPath);
        if (SD.rename(childPath, pPath)) ++promoted;
      }
    }
    entry = dir.openNextFile();
  }
  dir.close();
}

void promoteBackgroundPriorityQueue()
{
  int promoted = 0;
  String bBase = String(UPDATE_ROOT) + "/b";
  promoteBackgroundPriorityRecursive(bBase, bBase, promoted);
  if (promoted > 0)
  {
    write("Promoted background files to priority: ");
    writeln(promoted);
  }
}

void classifyDiffQueue()
{
  unsigned long startMs = millis();
  for (int i = 0; i < MAX_MANIFEST_ENTRIES; ++i)
  {
    diffClassifyPaths[i] = "";
    diffClassifyPriority[i] = false;
  }
  int count = 0;
  File diffList = SD.open(DIFF_QUEUE_LIST, FILE_READ);
  while (diffList && diffList.available() && count < MAX_MANIFEST_ENTRIES)
  {
    String relPath = diffList.readStringUntil('\n');
    relPath.replace("\r", "");
    relPath.trim();
    if (relPath.length() > 0)
    {
      diffClassifyPaths[count] = relPath;
      diffClassifyPriority[count] = (relPath == "playlist.ini" || isOwnGeneratedPlaylistChunk(relPath));
      ++count;
    }
  }
  if (diffList) diffList.close();
  markPriorityFromGeneratedPlaylists(diffClassifyPaths, diffClassifyPriority, count);
  SD.remove(PRIORITY_QUEUE_LIST);
  SD.remove(BACKGROUND_QUEUE_LIST);
  ensureParentDirs(PRIORITY_QUEUE_LIST);
  File emptyPriorityList = SD.open(PRIORITY_QUEUE_LIST, FILE_WRITE);
  if (emptyPriorityList) emptyPriorityList.close();
  File emptyBackgroundList = SD.open(BACKGROUND_QUEUE_LIST, FILE_WRITE);
  if (emptyBackgroundList) emptyBackgroundList.close();
  int p = 0, b = 0;
  for (int i = 0; i < count; ++i)
  {
    const char *queue = diffClassifyPriority[i] ? "p" : "b";
    if (appendQueueList(queue, diffClassifyPaths[i]))
    {
      if (diffClassifyPriority[i]) ++p; else ++b;
    }
    serviceUiDuringLongWork();
  }
  SD.remove(DIFF_QUEUE_LIST);
  write("Classified update queue: priority "); write(p); write(", background "); write(b);
  write(" elapsed="); write((millis() - startMs + 500UL) / 1000UL); writeln("s");
}

String readTextFileTrimmed(const char *path)
{
  File f = SD.open(path, FILE_READ);
  if (!f) return String("");
  String s = f.readString();
  f.close();
  return trimBody(s);
}

bool acceptPendingManifestAndSum()
{
  write("Pending accept check uptime=");
  write(millis() / 1000UL);
  writeln("s");
  if (queueHasFiles("p") || queueHasFiles("b")) return false;
  if (!SD.exists(REMOTE_MANIFEST_TMP)) return false;
  String pendingSum = readTextFileTrimmed(REMOTE_SUM_TMP);
  SD.remove("/banners/manifest.txt");
  if (!SD.rename(REMOTE_MANIFEST_TMP, "/banners/manifest.txt"))
  {
    writeln("Pending accept failed: manifest rename failed");
    return false;
  }
  if (pendingSum.length() > 0) writeLocalSum(pendingSum);
  SD.remove(REMOTE_SUM_TMP);
  updateStatusText = "content current";
  write("Pending update accepted: manifest/sum solidified uptime=");
  write(millis() / 1000UL);
  writeln("s");
  return true;
}

bool resumePendingUpdate(const String &updateSource)
{
  bool mounted = beginSdWithRetry("Pending update resume");
  if (!mounted) return false;
  if (!SD.exists(REMOTE_MANIFEST_TMP)) return false;

  if (!queueHasFiles("p") && !queueHasFiles("b"))
  {
    unsigned long planStartMs = millis();
    write("UPDATE plan start uptime=");
    write(planStartMs / 1000UL);
    writeln("s");
    buildDiffQueueFromPendingManifest();
    // Make sure changed/generated playlist chunks land first so classification
    // uses the target playlist set as soon as possible.
    File remote = SD.open(REMOTE_MANIFEST_TMP, FILE_READ);
    if (remote)
    {
      while (remote.available())
      {
        String relPath, hash;
        size_t size = 0;
        if (parseManifestFileLine(remote.readStringUntil('\n'), relPath, hash, size) && isOwnGeneratedPlaylistChunk(relPath))
        {
          String localHash;
          size_t localSize = 0;
          bool same = readManifestFileEntry("/banners/manifest.txt", relPath, localHash, localSize) && localSize == size && localHash.equalsIgnoreCase(hash);
          if (!same) downloadToQueueFile(updateSource, relPath, "p", true);
        }
      }
      remote.close();
    }
    classifyDiffQueue();
    write("UPDATE plan complete elapsed=");
    write((millis() - planStartMs + 500UL) / 1000UL);
    write("s uptime=");
    write(millis() / 1000UL);
    writeln("s");
  }

  // Classification already assigns changed files to p/ or b/. Re-scanning the
  // background queue against every playlist chunk on each resume was very slow
  // and blocked the UI between background downloads.
  bool hadPriorityDownloads = queueHasFiles("p");
  bool priorityDownloadedOk = false;
  if (hadPriorityDownloads)
  {
    updateUiLocked = true;
    drawWorkNotice("Priority downloads", "display paused");
  }

  while (WiFi.status() == WL_CONNECTED && queueHasFiles("p"))
  {
    updateStatusText = "priority downloads";
    if (!processOneQueuedDownload(updateSource, "p", true)) break;
    priorityDownloadedOk = true;
  }
  if (priorityDownloadedOk)
  {
    writeln("Priority content changed; reloading generated playlist and restarting at playlist_000 line 0");
    loadGeneratedPlaylistChunks();
    currentSlideIndex = -1;
    slideStartedMs = 0;
    advanceSlide(true);
  }
  updateUiLocked = false;
  if (queueHasFiles("p"))
  {
    updateStatusText = "priority pending";
    return true;
  }

  if (WiFi.status() == WL_CONNECTED && queueHasFiles("b"))
  {
    serviceUiDuringLongWork();
    updateStatusText = "background downloads queued";
    processOneQueuedDownload(updateSource, "b", false);
    if (!queueHasFiles("p") && !queueHasFiles("b"))
    {
      acceptPendingManifestAndSum();
    }
    return true;
  }

  if (!queueHasFiles("p") && !queueHasFiles("b")) acceptPendingManifestAndSum();
  return true;
}

bool downloadToQueueFile(const String &updateSource, const String &relPath, const char *queue, bool visibleWork)
{
  String hash;
  size_t size = 0;
  String queuePath = queuePathFor(queue, relPath);
  if (!findPendingManifestEntry(relPath, hash, size))
  {
    write("QUEUE manifest lookup failed queue=");
    write(queue);
    write(" path='");
    write(relPath);
    writeln("'; dropping queued item");
    SD.remove(queuePath);
    return true;
  }
  if (visibleWork) drawWorkNotice("Downloading", relPath);
  String url = updateSource + "/files/" + relPath;
  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(3000);
  http.setTimeout(15000);
  http.addHeader("Connection", "close");
  if (!http.begin(url))
  {
    write("QUEUE download begin failed: ");
    writeln(relPath);
    return false;
  }
  int code = http.GET();
  if (code < 200 || code >= 300)
  {
    write("QUEUE download HTTP failed "); write(code); write(" "); writeln(relPath);
    http.end();
    writeZeroFile(queuePath);
    return false;
  }
  ensureParentDirs(queuePath);
  SD.remove(queuePath);
  File out = SD.open(queuePath, FILE_WRITE);
  if (!out)
  {
    write("QUEUE open failed: ");
    writeln(queuePath);
    http.end();
    writeZeroFile(queuePath);
    return false;
  }
  WiFiClient *stream = http.getStreamPtr();
  uint8_t buffer[512];
  int total = http.getSize();
  if (total <= 0 && size > 0) total = (int)size;
  int written = 0;
  while (http.connected() && (total < 0 || written < total))
  {
    int wanted = total > 0 ? min((int)sizeof(buffer), total - written) : (int)sizeof(buffer);
    int got = stream->readBytes(buffer, wanted);
    if (got <= 0) break;
    out.write(buffer, got);
    written += got;
  }
  out.flush();
  out.close();
  http.end();
  if (!validateDownloadedFile(queuePath, size, hash))
  {
    writeZeroFile(queuePath);
    return false;
  }
  String livePath = String("/banners/") + relPath;
  ensureParentDirs(livePath);
  SD.remove(livePath);
  if (!SD.rename(queuePath, livePath))
  {
    write("QUEUE live move failed: "); writeln(relPath);
    return false;
  }
  write("QUEUE download accepted: SD://banners/"); writeln(relPath);
  return true;
}

void printFirstLines(const String &label, const String &body, int maxLines = 5)
{
  write(label);
  writeln(" first lines:");
  int cursor = 0;
  for (int lineNumber = 1; lineNumber <= maxLines; ++lineNumber)
  {
    if (cursor >= body.length())
    {
      write("  ");
      write(lineNumber);
      writeln(": <EOF>");
      continue;
    }
    int next = body.indexOf('\n', cursor);
    if (next < 0) next = body.length();
    String line = body.substring(cursor, next);
    line.replace("\r", "");
    write("  ");
    write(lineNumber);
    write(": ");
    writeln(line);
    cursor = next + 1;
  }
}

bool reportManifestPlan(const String &manifestBody, bool verbose)
{
  writeln(verbose ? "Manifest planning started" : "Manifest verification started");
  bool mounted = beginSdWithRetry(verbose ? "Manifest planning" : "Manifest verification");
  if (!mounted)
  {
    writeln("Manifest planning skipped: SD mount failed");
    manifestEntryCount = 0;
    manifestEntriesComplete = false;
    digitalWrite(TOUCH_CS_PIN, HIGH);
    return false;
  }

  File localManifestInfo = SD.open("/banners/manifest.txt", FILE_READ);
  if (localManifestInfo)
  {
    write("Local manifest file size ");
    writeln(localManifestInfo.size());
    localManifestInfo.close();
  }
  manifestEntryCount = 0;
  manifestEntriesComplete = true;
  int sameCount = 0;
  int manifestSameCount = 0;
  int changedCount = 0;
  int missingCount = 0;
  int fileCount = 0;
  int changedDebugPrinted = 0;
  int cursor = 0;
  while (cursor < manifestBody.length())
  {
    int next = manifestBody.indexOf('\n', cursor);
    if (next < 0) next = manifestBody.length();
    String line = manifestBody.substring(cursor, next);
    line.replace("\r", "");
    line.trim();
    cursor = next + 1;

    if (!line.startsWith("FILE ")) continue;
    int hashStart = 5;
    int hashEnd = line.indexOf(' ', hashStart);
    int sizeEnd = hashEnd >= 0 ? line.indexOf(' ', hashEnd + 1) : -1;
    if (hashEnd < 0 || sizeEnd < 0) continue;

    String expectedHash = line.substring(hashStart, hashEnd);
    size_t expectedSize = line.substring(hashEnd + 1, sizeEnd).toInt();
    String relPath = line.substring(sizeEnd + 1);
    relPath.trim();
    String sdPath = String("/banners/") + relPath;
    bool priorityPath = isPriorityManifestPath(relPath);
    ++fileCount;
    int entryIndex = -1;
    if (manifestEntryCount < MAX_MANIFEST_ENTRIES)
    {
      entryIndex = manifestEntryCount++;
      manifestEntries[entryIndex] = {expectedHash, expectedSize, relPath, false, false};
    }
    else
    {
      manifestEntriesComplete = false;
    }

    String localHash;
    size_t localSize = 0;
    bool localManifestHasEntry = findLocalManifestFileEntry(relPath, localHash, localSize);
    if (entryIndex >= 0) manifestEntries[entryIndex].localKnown = localManifestHasEntry;
    bool localManifestSame = localManifestHasEntry && localSize == expectedSize && localHash.equalsIgnoreCase(expectedHash);
    if (localManifestSame)
    {
      if (!priorityPath)
      {
        ++sameCount;
        ++manifestSameCount;
        if (entryIndex >= 0) manifestEntries[entryIndex].same = true;
        continue;
      }
      bool verifyActualFile = relPath == "playlist.ini" || isTrackedMissingRelPath(relPath);
      if (verifyActualFile)
      {
        File manifestSameFile = SD.open(sdPath, FILE_READ);
        if (!manifestSameFile && relPath.startsWith("Banners/")) manifestSameFile = SD.open(String("/banners/") + relPath.substring(8), FILE_READ);
        if (!manifestSameFile && relPath.startsWith("banners/")) manifestSameFile = SD.open(String("/banners/") + relPath.substring(8), FILE_READ);
        if (manifestSameFile && manifestSameFile.size() == expectedSize)
        {
          manifestSameFile.close();
          ++sameCount;
          ++manifestSameCount;
          if (entryIndex >= 0) manifestEntries[entryIndex].same = true;
          continue;
        }
        if (manifestSameFile) manifestSameFile.close();
        write("PLAN priority local manifest claimed same but SD missing/size mismatch: SD://banners/");
        writeln(relPath);
      }
      else
      {
        ++sameCount;
        ++manifestSameCount;
        if (entryIndex >= 0) manifestEntries[entryIndex].same = true;
        continue;
      }
    }

    if (!localManifestHasEntry && actualSdFileMatchesManifest(sdPath, expectedSize, expectedHash))
    {
      ++sameCount;
      if (entryIndex >= 0) manifestEntries[entryIndex].same = true;
      if (verbose && changedDebugPrinted < 10)
      {
        ++changedDebugPrinted;
        write("PLAN repaired missing local manifest entry from SD file: SD://banners/");
        writeln(relPath);
      }
      continue;
    }

    if (!priorityPath)
    {
      ++changedCount;
      if (verbose)
      {
        write("PLAN background changed by manifest: SD://banners/");
        writeln(relPath);
        if (changedDebugPrinted < 10)
        {
          ++changedDebugPrinted;
          write("  local manifest: ");
          if (localManifestHasEntry)
          {
            write(localHash);
            write(" ");
            write(localSize);
          }
          else
          {
            write("<not found>");
          }
          write(" | remote manifest: ");
          write(expectedHash);
          write(" ");
          writeln(expectedSize);
        }
      }
      continue;
    }

    ++changedCount;
    if (verbose)
    {
      write("PLAN priority changed by manifest: SD://banners/");
      writeln(relPath);
      if (changedDebugPrinted < 10)
      {
        ++changedDebugPrinted;
        write("  local manifest: ");
        if (localManifestHasEntry)
        {
          write(localHash);
          write(" ");
          write(localSize);
        }
        else
        {
          write("<not found>");
        }
        write(" | remote manifest: ");
        write(expectedHash);
        write(" ");
        writeln(expectedSize);
      }
    }
    continue;

    File file = SD.open(sdPath, FILE_READ);
    if (!file && relPath.startsWith("Banners/"))
    {
      sdPath = String("/banners/") + relPath.substring(8);
      file = SD.open(sdPath, FILE_READ);
    }
    if (!file && relPath.startsWith("banners/"))
    {
      sdPath = String("/banners/") + relPath.substring(8);
      file = SD.open(sdPath, FILE_READ);
    }
    if (!file)
    {
      ++missingCount;
      if (verbose)
      {
        write("PLAN missing: SD://banners/");
        writeln(relPath);
      }
      continue;
    }

    size_t actualSize = file.size();
    String actualHash;
    bool sizeMatches = actualSize == expectedSize;
    if (sizeMatches)
    {
      actualHash = md5File(file);
    }
    file.close();

    if (sizeMatches && actualHash.equalsIgnoreCase(expectedHash))
    {
      ++sameCount;
      if (entryIndex >= 0) manifestEntries[entryIndex].same = true;
    }
    else
    {
      ++changedCount;
      if (verbose)
      {
        write("PLAN changed: SD://banners/");
        write(relPath);
        write(" expected size/hash ");
        write(expectedSize);
        write("/");
        write(expectedHash);
        write(" actual ");
        write(actualSize);
        write("/");
        writeln(sizeMatches ? actualHash : String("<size mismatch>"));
      }
    }
  }

  /* SD stays mounted */
  digitalWrite(TOUCH_CS_PIN, HIGH);
  write(verbose ? "Manifest planning complete: files " : "Manifest verification complete: files ");
  write(fileCount);
  write(", same ");
  write(sameCount);
  write(" (manifest ");
  write(manifestSameCount);
  write(")");
  write(", changed ");
  write(changedCount);
  write(", missing ");
  writeln(missingCount);
  return changedCount == 0 && missingCount == 0;
}

void downloadPriorityChanges(const String &updateSource)
{
  drawWorkNotice("Priority downloads", "starting");
  bool mounted = SD.begin(SD_CS_PIN);
  sdOk = mounted;
  if (!mounted)
  {
    digitalWrite(TOUCH_CS_PIN, HIGH);
    writeln("Priority downloads skipped: SD mount failed");
    return;
  }

  int attempted = 0;
  int ok = 0;
  bool playlistDownloaded = false;
  for (int pass = 0; pass < 2; ++pass)
  {
    for (int i = 0; i < manifestEntryCount; ++i)
    {
      if (manifestEntries[i].same) continue;
      String lowerPath = manifestEntries[i].path;
      lowerPath.toLowerCase();
      bool isPlaylist = lowerPath.endsWith(".ini") || lowerPath.endsWith(".play");
      bool shouldDownload = pass == 0
        ? (isPlaylist && (manifestEntries[i].path == "playlist.ini" || isOwnGeneratedPlaylistChunk(manifestEntries[i].path) || isPriorityManifestPath(manifestEntries[i].path)))
        : isPriorityManifestPath(manifestEntries[i].path);
      if (!shouldDownload) continue;

      ++attempted;
      if (downloadManifestFileWithRetry(updateSource, manifestEntries[i].path, manifestEntries[i].size, manifestEntries[i].hash, true))
      {
        ++ok;
        manifestEntries[i].same = true;
        if (isPlaylist) playlistDownloaded = true;
      }
    }
    if (pass == 0)
    {
      writeln(playlistDownloaded ? "Playlist file(s) downloaded; loading generated playlist before priority slide downloads" : "Refreshing runtime playlist before priority slide downloads");
      /* SD stays mounted */
      digitalWrite(TOUCH_CS_PIN, HIGH);
      if (!loadGeneratedPlaylistChunks())
      {
        writeln("Generated playlist chunks unavailable after playlist download; stopping priority pass");
        return;
      }
      write("Priority required files: ");
      writeln(requiredFileCount);
      mounted = SD.begin(SD_CS_PIN);
      sdOk = mounted;
      if (!mounted)
      {
        digitalWrite(TOUCH_CS_PIN, HIGH);
        writeln("Priority downloads stopped: SD remount failed");
        return;
      }
    }
  }

  /* SD stays mounted */
  digitalWrite(TOUCH_CS_PIN, HIGH);
  write("Priority downloads complete: attempted ");
  write(attempted);
  write(", ok ");
  writeln(ok);
}

void fetchManifest(const String &updateSource, const String &remoteSum)
{
  clearBackgroundDownloads();
  bool mounted = beginSdWithRetry("Manifest fetch");
  if (!mounted)
  {
    updateStatusText = "SD mount failed";
    return;
  }
  if (!downloadRemoteManifestToTmp(updateSource))
  {
    updateStatusText = "manifest fetch failed";
    if (infoScreenVisible) drawInfoScreen();
    return;
  }
  if (remoteSum.length() > 0) writeLocalTextFile(REMOTE_SUM_TMP, remoteSum + "\n");
  updateStatusText = "pending manifest saved";
  resumePendingUpdate(updateSource);
  if (infoScreenVisible) drawInfoScreen();
}

void checkCallHome()
{
  lastCallHomeMs = millis();
  callHomeProblem = true;
  loadPrivateConfig();
  if (!serviceTokenPresent)
  {
    updateStatusText = "service token missing";
    writeln("Call-home skipped: service token missing");
    return;
  }
  if (updateSourceCount == 0)
  {
    updateStatusText = "no update sources";
    writeln("Call-home skipped: no update sources");
    return;
  }

  for (int i = 0; i < updateSourceCount; ++i)
  {
    HTTPClient http;
    http.setConnectTimeout(1500);
    http.setTimeout(1500);
    String url = updateSources[i] + "/sum.txt?t=" + serviceToken + "&mac=" + macAddressText();
    String logUrl = updateSources[i] + "/sum.txt?t=<redacted>&mac=" + macAddressText();
    if (!http.begin(url))
    {
      write("Call-home GET ");
      write(logUrl);
      writeln(" -> begin failed");
      continue;
    }
    int code = http.GET();
    String remoteSum;
    if (code >= 200 && code < 300) remoteSum = trimBody(http.getString());
    http.end();

    write("Call-home GET ");
    write(logUrl);
    write(" -> HTTP ");
    write(code);
    if (remoteSum.length() > 0)
    {
      write(", sum ");
      write(remoteSum);
    }
    writeln();

    if (code >= 200 && code < 300)
    {
      callHomeProblem = false;
      lastGoodUpdateSource = updateSources[i];
      String localSum = readLocalSum();
      lastRemoteSum = remoteSum;
      if (remoteSum.length() == 0)
      {
        updateStatusText = "empty sum.txt";
      }
      else if (localSum == remoteSum)
      {
        updateStatusText = "content current";
        writeln("Content sum unchanged; no manifest fetch");
      }
      else
      {
        updateStatusText = "content changed";
        write("Content sum changed: local ");
        write(localSum.length() > 0 ? localSum : String("<missing>"));
        write(" remote ");
        writeln(remoteSum);
        fetchManifest(updateSources[i], remoteSum);
      }
      return;
    }
    updateStatusText = String("source failed HTTP ") + code;
  }
}
}

void networkBegin()
{
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  bool mounted = LittleFS.begin(false);
  littlefsOk = mounted;
  if (mounted)
  {
    loadWifiProfiles();
    loadPrivateConfig();
    LittleFS.end();
  }
  else
  {
    networkStatusText = "LittleFS mount failed";
    updateStatusText = "not checked";
  }
  nextWifiAttemptMs = millis();
}

void networkUpdate()
{
  unsigned long now = millis();
  if (WiFi.status() == WL_CONNECTED)
  {
    wifiAttemptActive = false;
    bool hadBackgroundDownloads = backgroundDownloadsPending();
    processBackgroundDownload();
    bool stillHasBackgroundDownloads = backgroundDownloadsPending();
    if (!hadBackgroundDownloads && !stillHasBackgroundDownloads) processCleanupDelete();
    bool cleanupStillPending = cleanupDeletesPending();
    String connectedSsid = WiFi.SSID();
    if (lastLoggedConnectedSsid != connectedSsid)
    {
      write("WiFi connected [");
      write(currentWifiProfile >= 0 && currentWifiProfile < wifiProfileCount ? wifiProfiles[currentWifiProfile].name : String("unknown"));
      write("]: ");
      write(connectedSsid);
      write(" IP ");
      writeln(WiFi.localIP());
      lastLoggedConnectedSsid = connectedSsid;
    }
    networkStatusText = String("connected ") + (currentWifiProfile >= 0 && currentWifiProfile < wifiProfileCount ? wifiProfiles[currentWifiProfile].name : String("WiFi"));
    if (hadBackgroundDownloads || stillHasBackgroundDownloads || cleanupStillPending)
    {
      if (stillHasBackgroundDownloads) updateStatusText = "downloading background";
      else if (cleanupStillPending) updateStatusText = "cleanup pending";
      return;
    }
    bool pendingMounted = beginSdWithRetry("Pending update check", 1);
    if (pendingMounted && SD.exists(REMOTE_MANIFEST_TMP))
    {
      String source = lastGoodUpdateSource.length() > 0 ? lastGoodUpdateSource : (updateSourceCount > 0 ? updateSources[0] : String(""));
      if (source.length() > 0)
      {
        resumePendingUpdate(source);
        return;
      }
    }
    if (lastCallHomeMs == 0 || now - lastCallHomeMs >= CALL_HOME_INTERVAL_MS)
    {
      bool mounted = LittleFS.begin(false);
      littlefsOk = mounted;
      if (mounted)
      {
        writeln("");
        writeln("Call-home check starting");
        checkCallHome();
        LittleFS.end();
      }
      else
      {
        callHomeProblem = true;
        updateStatusText = "LittleFS mount failed";
        writeln("Call-home skipped: LittleFS mount failed");
      }
    }
    return;
  }

  callHomeProblem = true;
  lastLoggedConnectedSsid = "";
  if (wifiAttemptActive && now - wifiAttemptStartedMs < WIFI_CONNECT_WINDOW_MS) return;
  wifiAttemptActive = false;
  if (long(now - nextWifiAttemptMs) >= 0)
  {
    startNextWifiAttempt();
    nextWifiAttemptMs = now + WIFI_CONNECT_WINDOW_MS + WIFI_RETRY_IDLE_MS;
  }
}

NetworkHealth networkHealth()
{
  if (WiFi.status() == WL_CONNECTED) return callHomeProblem ? NetworkHealth::ConnectedProblem : NetworkHealth::ConnectedGood;
  return wifiCompletedCycles > 1 ? NetworkHealth::DisconnectedAfterCycle : NetworkHealth::Hunting;
}

uint16_t networkStatusBarColor()
{
  switch (networkHealth())
  {
    case NetworkHealth::ConnectedGood: return TFT_GREEN;
    case NetworkHealth::ConnectedProblem: return TFT_YELLOW;
    case NetworkHealth::DisconnectedAfterCycle: return TFT_RED;
    case NetworkHealth::Hunting:
    default: return TFT_WHITE;
  }
}

String currentWifiSsidText()
{
  if (currentWifiProfile >= 0 && currentWifiProfile < wifiProfileCount) return wifiProfiles[currentWifiProfile].name;
  if (WiFi.status() == WL_CONNECTED) return "connected";
  return "none";
}

bool resetLocalContentState()
{
  bool mounted = SD.begin(SD_CS_PIN);
  sdOk = mounted;
  if (!mounted)
  {
    digitalWrite(TOUCH_CS_PIN, HIGH);
    updateStatusText = "reset state SD failed";
    writeln("Local content state reset failed: SD mount failed");
    return false;
  }
  bool manifestRemoved = !SD.exists("/banners/manifest.txt") || SD.remove("/banners/manifest.txt");
  bool sumRemoved = !SD.exists("/banners/sum.txt") || SD.remove("/banners/sum.txt");
  /* SD stays mounted */
  digitalWrite(TOUCH_CS_PIN, HIGH);
  lastRemoteSum = "";
  lastRemoteManifestBody = "";
  clearBackgroundDownloads();
  clearCleanupDeletes();
  manifestEntryCount = 0;
  updateStatusText = manifestRemoved && sumRemoved ? "local state reset; update pending" : "reset state partial; update pending";
  write("Local content state reset: manifest=");
  write(manifestRemoved ? "removed" : "failed");
  write(" sum=");
  writeln(sumRemoved ? "removed" : "failed");
  lastCallHomeMs = 0;
  return manifestRemoved && sumRemoved;
}

bool manifestHasEntries()
{
  return internalManifestHasEntries();
}

int collectManifestMatches(const String &sdPattern, String matches[], int maxMatches)
{
  return internalCollectManifestMatches(sdPattern, matches, maxMatches);
}
