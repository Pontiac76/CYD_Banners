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
  Serial.print("Config loaded: ");
  Serial.println(updateStatusText);
  for (int i = 0; i < updateSourceCount; ++i)
  {
    Serial.print("Update source ");
    Serial.print(i + 1);
    Serial.print(": ");
    Serial.println(updateSources[i]);
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
  Serial.print("WiFi trying [");
  Serial.print(wifiProfiles[currentWifiProfile].name);
  Serial.print("]: ");
  Serial.println(wifiProfiles[currentWifiProfile].ssid);
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
  SD.end();
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
    Serial.print("Local file write failed, SD mount failed: ");
    Serial.println(sdPath);
    return false;
  }

  if (!ensureParentDirs(sdPath))
  {
    SD.end();
    digitalWrite(TOUCH_CS_PIN, HIGH);
    Serial.print("Local file write failed, mkdir failed: ");
    Serial.println(sdPath);
    return false;
  }
  File file = SD.open(sdPath, FILE_WRITE);
  if (!file)
  {
    SD.end();
    digitalWrite(TOUCH_CS_PIN, HIGH);
    Serial.print("Local file write failed, open failed: ");
    Serial.println(sdPath);
    return false;
  }
  file.print(body);
  file.close();
  SD.end();
  digitalWrite(TOUCH_CS_PIN, HIGH);
  return true;
}

bool writeLocalManifest(const String &manifestBody)
{
  bool ok = writeLocalTextFile("/banners/manifest.txt", manifestBody.endsWith("\n") ? manifestBody : manifestBody + "\n");
  if (ok) Serial.println("Local manifest updated: SD://banners/manifest.txt");
  return ok;
}

void queueStaleSdCleanup();

bool writeLocalSum(const String &sum)
{
  bool mounted = SD.begin(SD_CS_PIN);
  sdOk = mounted;
  if (!mounted)
  {
    digitalWrite(TOUCH_CS_PIN, HIGH);
    Serial.println("Local sum write failed: SD mount failed");
    return false;
  }

  if (!SD.exists("/banners")) SD.mkdir("/banners");
  File file = SD.open("/banners/sum.txt", FILE_WRITE);
  if (!file)
  {
    SD.end();
    digitalWrite(TOUCH_CS_PIN, HIGH);
    Serial.println("Local sum write failed: open failed");
    return false;
  }

  file.println(sum);
  file.close();
  SD.end();
  digitalWrite(TOUCH_CS_PIN, HIGH);
  Serial.print("Local sum updated: SD://banners/sum.txt = ");
  Serial.println(sum);
  Serial.println("Local sum update complete; scheduling SD cleanup scan");
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
};

constexpr int MAX_MANIFEST_ENTRIES = 256;
ManifestEntry manifestEntries[MAX_MANIFEST_ENTRIES];
int manifestEntryCount = 0;
String backgroundDownloadQueue[MAX_MANIFEST_ENTRIES];
int backgroundDownloadCount = 0;
int backgroundDownloadIndex = 0;
int backgroundDownloadOkCount = 0;
unsigned long nextBackgroundDownloadMs = 0;
String cleanupDeleteQueue[MAX_MANIFEST_ENTRIES];
int cleanupDeleteCount = 0;
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

bool isPriorityManifestPath(const String &relPath)
{
  if (relPath == "playlist.ini") return true;
  String sdPath = normalizeSdComparePath(String("/banners/") + relPath);
  for (int i = 0; i < parsedPlaylistCount; ++i)
  {
    if (normalizeSdComparePath(parsedPlaylists[i]) == sdPath) return true;
  }
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
    Serial.print("DOWNLOAD validate open failed: ");
    Serial.println(tmpPath);
    return false;
  }
  size_t actualSize = file.size();
  if (actualSize != expectedSize)
  {
    file.close();
    Serial.print("DOWNLOAD validate size failed: ");
    Serial.print(tmpPath);
    Serial.print(" expected ");
    Serial.print(expectedSize);
    Serial.print(" actual ");
    Serial.println(actualSize);
    return false;
  }
  String actualHash = md5File(file);
  file.close();
  if (!actualHash.equalsIgnoreCase(expectedHash))
  {
    Serial.print("DOWNLOAD validate hash failed: ");
    Serial.print(tmpPath);
    Serial.print(" expected ");
    Serial.print(expectedHash);
    Serial.print(" actual ");
    Serial.println(actualHash);
    return false;
  }
  return true;
}

bool downloadManifestFile(const String &updateSource, const String &relPath, size_t expectedSize, const String &expectedHash, bool visibleWork)
{
  if (visibleWork) drawWorkNotice("Downloading", String("SD://banners/") + relPath);
  String url = updateSource + "/files/" + relPath;
  String sdPath = String("/banners/") + relPath;
  String tmpPath = sdPath + ".tmp";

  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(6000);
  if (!http.begin(url))
  {
    Serial.print("DOWNLOAD begin failed: ");
    Serial.println(url);
    return false;
  }
  int code = http.GET();
  if (code < 200 || code >= 300)
  {
    Serial.print("DOWNLOAD failed HTTP ");
    Serial.print(code);
    Serial.print(": ");
    Serial.println(url);
    http.end();
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  if (!ensureParentDirs(sdPath))
  {
    Serial.print("DOWNLOAD mkdir failed: SD://banners/");
    Serial.println(relPath);
    http.end();
    return false;
  }
  SD.remove(tmpPath);
  File out = SD.open(tmpPath, FILE_WRITE);
  if (!out)
  {
    Serial.print("DOWNLOAD open failed: SD://banners/");
    Serial.println(relPath);
    http.end();
    return false;
  }

  uint8_t buffer[512];
  int total = http.getSize();
  int written = 0;
  while (http.connected() && (total < 0 || written < total))
  {
    if (visibleWork && total > 0)
    {
      drawWorkNotice("Downloading", String(written / 1024) + "/" + String(total / 1024) + " KiB " + relPath);
    }
    size_t available = stream->available();
    if (!available)
    {
      delay(1);
      continue;
    }
    int readCount = stream->readBytes(buffer, min(available, sizeof(buffer)));
    if (readCount <= 0) break;
    out.write(buffer, readCount);
    written += readCount;
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
    Serial.print("DOWNLOAD rename failed: SD://banners/");
    Serial.println(relPath);
    return false;
  }
  Serial.print("DOWNLOAD ok: SD://banners/");
  Serial.print(relPath);
  Serial.print(" bytes ");
  Serial.println(written);
  return true;
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
  Serial.println("Cleanup scan requested after sum.txt update");
  clearCleanupDeletes();
  if (!manifestEntriesComplete)
  {
    Serial.println("Cleanup scan skipped: manifest entry list overflow/incomplete");
    return;
  }
  if (manifestEntryCount == 0)
  {
    Serial.println("Cleanup scan skipped: RAM manifest has no entries");
    return;
  }
  bool mounted = SD.begin(SD_CS_PIN);
  sdOk = mounted;
  if (!mounted)
  {
    digitalWrite(TOUCH_CS_PIN, HIGH);
    Serial.println("Cleanup scan skipped: SD mount failed");
    return;
  }
  Serial.print("Cleanup scan starting: RAM manifest entries ");
  Serial.println(manifestEntryCount);
  scanCleanupDir("/banners");
  SD.end();
  digitalWrite(TOUCH_CS_PIN, HIGH);
  if (cleanupDeleteCount == 0)
  {
    Serial.println("Cleanup scan complete: no stale/temp files queued");
  }
  if (cleanupDeleteCount > 0)
  {
    Serial.print("Cleanup deletes queued: ");
    Serial.println(cleanupDeleteCount);
    for (int i = 0; i < cleanupDeleteCount; ++i)
    {
      Serial.print("  DELETE ");
      Serial.println(cleanupDeleteQueue[i]);
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
    Serial.println("Cleanup delete skipped: SD mount failed");
    return;
  }
  String path = cleanupDeleteQueue[cleanupDeleteIndex++];
  Serial.print("Cleanup delete ");
  Serial.print(cleanupDeleteIndex);
  Serial.print("/");
  Serial.print(cleanupDeleteCount);
  Serial.print(": ");
  Serial.println(path);
  bool deleted = SD.remove(path);
  if (deleted)
  {
    Serial.print("Cleanup deleted: ");
    Serial.println(path);
  }
  else
  {
    Serial.print("Cleanup delete failed: ");
    Serial.println(path);
  }
  bool cleanupComplete = cleanupDeleteIndex >= cleanupDeleteCount;
  SD.end();
  digitalWrite(TOUCH_CS_PIN, HIGH);
  if (cleanupComplete)
  {
    Serial.println("Cleanup deletes complete; rebuilding runtime playlist");
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
    String lowerPath = manifestEntries[i].path;
    lowerPath.toLowerCase();
    if (lowerPath.endsWith(".ini") || lowerPath.endsWith(".play") || isPriorityManifestPath(manifestEntries[i].path)) return true;
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
    Serial.print("Background downloads queued: ");
    Serial.println(backgroundDownloadCount);
    for (int i = 0; i < backgroundDownloadCount; ++i)
    {
      Serial.print("  BACKGROUND ");
      Serial.print(i + 1);
      Serial.print("/");
      Serial.print(backgroundDownloadCount);
      Serial.print(" SD://banners/");
      Serial.println(backgroundDownloadQueue[i]);
    }
  }
}

bool backgroundDownloadsPending()
{
  return backgroundDownloadIndex < backgroundDownloadCount;
}

bool cleanupDeletesPending()
{
  return cleanupDeleteIndex < cleanupDeleteCount;
}

void finishBackgroundQueueIfDone()
{
  if (backgroundDownloadIndex < backgroundDownloadCount) return;
  Serial.print("Background downloads complete: ok ");
  Serial.print(backgroundDownloadOkCount);
  Serial.print("/");
  Serial.println(backgroundDownloadCount);
  if (lastRemoteSum.length() > 0)
  {
    if (backgroundDownloadOkCount != backgroundDownloadCount)
    {
      Serial.println("Background downloads had failures; accepting current manifest/sum to avoid repeated full replans");
    }
    if (lastRemoteManifestBody.length() > 0) writeLocalManifest(lastRemoteManifestBody);
    writeLocalSum(lastRemoteSum);
    updateStatusText = backgroundDownloadOkCount == backgroundDownloadCount ? "content current" : "content current; bg failures";
  }
}

void processBackgroundDownload()
{
  if (backgroundDownloadIndex >= backgroundDownloadCount) return;
  if (WiFi.status() != WL_CONNECTED) return;
  unsigned long now = millis();
  if (long(now - nextBackgroundDownloadMs) < 0) return;
  nextBackgroundDownloadMs = now + 1500UL;

  bool mounted = SD.begin(SD_CS_PIN);
  sdOk = mounted;
  if (!mounted)
  {
    digitalWrite(TOUCH_CS_PIN, HIGH);
    Serial.println("Background download skipped: SD mount failed");
    return;
  }

  String path = backgroundDownloadQueue[backgroundDownloadIndex++];
  String source = lastGoodUpdateSource.length() > 0 ? lastGoodUpdateSource : (updateSourceCount > 0 ? updateSources[0] : String(""));
  if (source.length() == 0)
  {
    SD.end();
    digitalWrite(TOUCH_CS_PIN, HIGH);
    Serial.println("Background download skipped: no update source");
    return;
  }
  Serial.print("Background download ");
  Serial.print(backgroundDownloadIndex);
  Serial.print("/");
  Serial.print(backgroundDownloadCount);
  Serial.print(": SD://banners/");
  Serial.println(path);
  int manifestIndex = findManifestEntryIndex(path);
  if (manifestIndex >= 0 && downloadManifestFile(source, path, manifestEntries[manifestIndex].size, manifestEntries[manifestIndex].hash, false)) ++backgroundDownloadOkCount;
  SD.end();
  digitalWrite(TOUCH_CS_PIN, HIGH);

  finishBackgroundQueueIfDone();
}

bool findManifestLineEntry(const String &manifestBody, const String &relPath, String &hash, size_t &size)
{
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
    String linePath = line.substring(sizeEnd + 1);
    linePath.trim();
    if (linePath != relPath) continue;
    hash = line.substring(hashStart, hashEnd);
    size = line.substring(hashEnd + 1, sizeEnd).toInt();
    return true;
  }
  return false;
}

String readLocalManifestBody()
{
  String result;
  File file = SD.open("/banners/manifest.txt", FILE_READ);
  if (file)
  {
    result = file.readString();
    file.close();
  }
  return result;
}

bool reportManifestPlan(const String &manifestBody, bool verbose)
{
  Serial.println(verbose ? "Manifest planning started" : "Manifest verification started");
  drawWorkNotice(verbose ? "Manifest planning" : "Manifest verify", "starting");
  bool mounted = SD.begin(SD_CS_PIN);
  sdOk = mounted;
  if (!mounted)
  {
    Serial.println("Manifest planning skipped: SD mount failed");
    manifestEntryCount = 0;
    manifestEntriesComplete = false;
    digitalWrite(TOUCH_CS_PIN, HIGH);
    return false;
  }

  String localManifestBody = readLocalManifestBody();
  manifestEntryCount = 0;
  manifestEntriesComplete = true;
  int sameCount = 0;
  int manifestSameCount = 0;
  int changedCount = 0;
  int missingCount = 0;
  int fileCount = 0;
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
    String lowerRelPath = relPath;
    lowerRelPath.toLowerCase();
    bool priorityPath = lowerRelPath.endsWith(".ini") || lowerRelPath.endsWith(".play") || isPriorityManifestPath(relPath);
    ++fileCount;
    if (priorityPath && (fileCount == 1 || fileCount % 5 == 0))
    {
      drawWorkNotice(verbose ? "Manifest planning" : "Manifest verify", String(fileCount) + " files, checking " + relPath);
    }
    int entryIndex = -1;
    if (manifestEntryCount < MAX_MANIFEST_ENTRIES)
    {
      entryIndex = manifestEntryCount++;
      manifestEntries[entryIndex] = {expectedHash, expectedSize, relPath, false};
    }
    else
    {
      manifestEntriesComplete = false;
    }

    String localHash;
    size_t localSize = 0;
    bool localManifestSame = findManifestLineEntry(localManifestBody, relPath, localHash, localSize) && localSize == expectedSize && localHash.equalsIgnoreCase(expectedHash);
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
        Serial.print("PLAN priority local manifest claimed same but SD missing/size mismatch: SD://banners/");
        Serial.println(relPath);
      }
      else
      {
        ++sameCount;
        ++manifestSameCount;
        if (entryIndex >= 0) manifestEntries[entryIndex].same = true;
        continue;
      }
    }

    if (!priorityPath)
    {
      ++changedCount;
      if (verbose)
      {
        Serial.print("PLAN background changed by manifest: SD://banners/");
        Serial.println(relPath);
      }
      continue;
    }

    ++changedCount;
    if (verbose)
    {
      Serial.print("PLAN priority changed by manifest: SD://banners/");
      Serial.println(relPath);
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
        Serial.print("PLAN missing: SD://banners/");
        Serial.println(relPath);
      }
      continue;
    }

    size_t actualSize = file.size();
    String actualHash;
    bool sizeMatches = actualSize == expectedSize;
    if (sizeMatches)
    {
      if (actualSize > 32768) drawWorkNotice("MD5 checking", relPath);
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
        Serial.print("PLAN changed: SD://banners/");
        Serial.print(relPath);
        Serial.print(" expected size/hash ");
        Serial.print(expectedSize);
        Serial.print("/");
        Serial.print(expectedHash);
        Serial.print(" actual ");
        Serial.print(actualSize);
        Serial.print("/");
        Serial.println(sizeMatches ? actualHash : String("<size mismatch>"));
      }
    }
  }

  SD.end();
  digitalWrite(TOUCH_CS_PIN, HIGH);
  drawWorkNotice(verbose ? "Manifest planning" : "Manifest verify", "complete");
  Serial.print(verbose ? "Manifest planning complete: files " : "Manifest verification complete: files ");
  Serial.print(fileCount);
  Serial.print(", same ");
  Serial.print(sameCount);
  Serial.print(" (manifest ");
  Serial.print(manifestSameCount);
  Serial.print(")");
  Serial.print(", changed ");
  Serial.print(changedCount);
  Serial.print(", missing ");
  Serial.println(missingCount);
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
    Serial.println("Priority downloads skipped: SD mount failed");
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
      bool shouldDownload = pass == 0 ? isPlaylist : (!isPlaylist && isPriorityManifestPath(manifestEntries[i].path));
      if (!shouldDownload) continue;

      ++attempted;
      if (downloadManifestFile(updateSource, manifestEntries[i].path, manifestEntries[i].size, manifestEntries[i].hash, true))
      {
        ++ok;
        manifestEntries[i].same = true;
        if (isPlaylist) playlistDownloaded = true;
      }
    }
    if (pass == 0)
    {
      Serial.println(playlistDownloaded ? "Playlist file(s) downloaded; rebuilding runtime playlist before priority slide downloads" : "Refreshing runtime playlist before priority slide downloads");
      SD.end();
      digitalWrite(TOUCH_CS_PIN, HIGH);
      rebuildPlaylist();
      Serial.print("Priority required files: ");
      Serial.println(requiredFileCount);
      mounted = SD.begin(SD_CS_PIN);
      sdOk = mounted;
      if (!mounted)
      {
        digitalWrite(TOUCH_CS_PIN, HIGH);
        Serial.println("Priority downloads stopped: SD remount failed");
        return;
      }
    }
  }

  SD.end();
  digitalWrite(TOUCH_CS_PIN, HIGH);
  Serial.print("Priority downloads complete: attempted ");
  Serial.print(attempted);
  Serial.print(", ok ");
  Serial.println(ok);
}

void fetchManifest(const String &updateSource, const String &remoteSum)
{
  drawWorkNotice("Fetching manifest", "starting");
  clearBackgroundDownloads();
  String url = updateSource + "/manifest.txt";
  HTTPClient http;
  http.setConnectTimeout(1500);
  http.setTimeout(2500);
  if (!http.begin(url))
  {
    Serial.println("Manifest GET failed to begin");
    updateStatusText = "manifest begin failed";
    if (infoScreenVisible) drawInfoScreen();
    return;
  }

  int code = http.GET();
  String body;
  if (code >= 200 && code < 300) body = http.getString();
  http.end();

  Serial.print("Manifest GET ");
  Serial.print(url);
  Serial.print(" -> HTTP ");
  Serial.print(code);
  if (body.length() > 0)
  {
    Serial.print(", bytes ");
    Serial.print(body.length());
  }
  Serial.println();

  if (code >= 200 && code < 300)
  {
    updateStatusText = "manifest fetched";
    lastRemoteManifestBody = body;
    bool contentHealthy = reportManifestPlan(body, true);
    if (!contentHealthy && manifestEntryCount == 0)
    {
      Serial.println("Manifest planning produced no entries; deferring update until next call-home");
      updateStatusText = "manifest plan deferred";
      if (infoScreenVisible) drawInfoScreen();
      return;
    }
    if (!contentHealthy)
    {
      downloadPriorityChanges(updateSource);
      Serial.println("Priority downloads finished; rebuilding runtime playlist before background downloads");
      rebuildPlaylist();
      bool priorityHealthy = !priorityChangesPending();
      if (priorityHealthy && remoteSum.length() > 0)
      {
        queueBackgroundDownloads();
        writeLocalManifest(body);
        writeLocalSum(remoteSum);
        updateStatusText = backgroundDownloadsPending() ? "background downloads queued" : "content current";
      }
      else
      {
        Serial.println("Priority changes remain after download attempt; leaving local sum unchanged");
        queueBackgroundDownloads();
      }
    }
    else if (remoteSum.length() > 0)
    {
      writeLocalManifest(body);
      writeLocalSum(remoteSum);
      updateStatusText = "content current";
    }
  }
  else
  {
    updateStatusText = String("manifest HTTP ") + code;
  }
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
    Serial.println("Call-home skipped: service token missing");
    return;
  }
  if (updateSourceCount == 0)
  {
    updateStatusText = "no update sources";
    Serial.println("Call-home skipped: no update sources");
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
      Serial.print("Call-home GET ");
      Serial.print(logUrl);
      Serial.println(" -> begin failed");
      continue;
    }
    int code = http.GET();
    String remoteSum;
    if (code >= 200 && code < 300) remoteSum = trimBody(http.getString());
    http.end();

    Serial.print("Call-home GET ");
    Serial.print(logUrl);
    Serial.print(" -> HTTP ");
    Serial.print(code);
    if (remoteSum.length() > 0)
    {
      Serial.print(", sum ");
      Serial.print(remoteSum);
    }
    Serial.println();

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
        Serial.println("Content sum unchanged; no manifest fetch");
      }
      else
      {
        drawWorkNotice("Content changed", "fetching updates");
        updateStatusText = "content changed";
        Serial.print("Content sum changed: local ");
        Serial.print(localSum.length() > 0 ? localSum : String("<missing>"));
        Serial.print(" remote ");
        Serial.println(remoteSum);
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
      Serial.print("WiFi connected [");
      Serial.print(currentWifiProfile >= 0 && currentWifiProfile < wifiProfileCount ? wifiProfiles[currentWifiProfile].name : String("unknown"));
      Serial.print("]: ");
      Serial.print(connectedSsid);
      Serial.print(" IP ");
      Serial.println(WiFi.localIP());
      lastLoggedConnectedSsid = connectedSsid;
    }
    networkStatusText = String("connected ") + (currentWifiProfile >= 0 && currentWifiProfile < wifiProfileCount ? wifiProfiles[currentWifiProfile].name : String("WiFi"));
    if (hadBackgroundDownloads || stillHasBackgroundDownloads || cleanupStillPending)
    {
      if (stillHasBackgroundDownloads) updateStatusText = "downloading background";
      else if (cleanupStillPending) updateStatusText = "cleanup pending";
      return;
    }
    if (lastCallHomeMs == 0 || now - lastCallHomeMs >= CALL_HOME_INTERVAL_MS)
    {
      bool mounted = LittleFS.begin(false);
      littlefsOk = mounted;
      if (mounted)
      {
        Serial.println("Call-home check starting");
        checkCallHome();
        LittleFS.end();
      }
      else
      {
        callHomeProblem = true;
        updateStatusText = "LittleFS mount failed";
        Serial.println("Call-home skipped: LittleFS mount failed");
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
    Serial.println("Local content state reset failed: SD mount failed");
    return false;
  }
  bool manifestRemoved = !SD.exists("/banners/manifest.txt") || SD.remove("/banners/manifest.txt");
  bool sumRemoved = !SD.exists("/banners/sum.txt") || SD.remove("/banners/sum.txt");
  SD.end();
  digitalWrite(TOUCH_CS_PIN, HIGH);
  lastRemoteSum = "";
  lastRemoteManifestBody = "";
  clearBackgroundDownloads();
  clearCleanupDeletes();
  manifestEntryCount = 0;
  updateStatusText = manifestRemoved && sumRemoved ? "local state reset; update pending" : "reset state partial; update pending";
  Serial.print("Local content state reset: manifest=");
  Serial.print(manifestRemoved ? "removed" : "failed");
  Serial.print(" sum=");
  Serial.println(sumRemoved ? "removed" : "failed");
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
