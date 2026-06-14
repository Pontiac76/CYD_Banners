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
String lastGoodUpdateSource;

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

constexpr int MAX_MANIFEST_ENTRIES = 96;
ManifestEntry manifestEntries[MAX_MANIFEST_ENTRIES];
int manifestEntryCount = 0;
String backgroundDownloadQueue[MAX_MANIFEST_ENTRIES];
int backgroundDownloadCount = 0;
int backgroundDownloadIndex = 0;
int backgroundDownloadOkCount = 0;
unsigned long nextBackgroundDownloadMs = 0;

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
    if (normalizeSdComparePath(requiredFiles[i]) == sdPath) return true;
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

bool downloadManifestFile(const String &updateSource, const String &relPath)
{
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

void clearBackgroundDownloads()
{
  backgroundDownloadCount = 0;
  backgroundDownloadIndex = 0;
  backgroundDownloadOkCount = 0;
  nextBackgroundDownloadMs = 0;
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

void processBackgroundDownload()
{
  if (backgroundDownloadIndex >= backgroundDownloadCount) return;
  if (WiFi.status() != WL_CONNECTED) return;
  unsigned long now = millis();
  if (long(now - nextBackgroundDownloadMs) < 0) return;
  nextBackgroundDownloadMs = now + 1000UL;

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
  if (downloadManifestFile(source, path)) ++backgroundDownloadOkCount;
  SD.end();
  digitalWrite(TOUCH_CS_PIN, HIGH);

  if (backgroundDownloadIndex >= backgroundDownloadCount)
  {
    Serial.print("Background downloads complete: ok ");
    Serial.print(backgroundDownloadOkCount);
    Serial.print("/");
    Serial.println(backgroundDownloadCount);
    if (backgroundDownloadOkCount == backgroundDownloadCount && lastRemoteSum.length() > 0)
    {
      writeLocalSum(lastRemoteSum);
      updateStatusText = "content current";
    }
  }
}

bool reportManifestPlan(const String &manifestBody, bool verbose)
{
  Serial.println(verbose ? "Manifest planning started" : "Manifest verification started");
  bool mounted = SD.begin(SD_CS_PIN);
  sdOk = mounted;
  if (!mounted)
  {
    Serial.println("Manifest planning skipped: SD mount failed");
    digitalWrite(TOUCH_CS_PIN, HIGH);
    return false;
  }

  manifestEntryCount = 0;
  int sameCount = 0;
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
    ++fileCount;
    int entryIndex = -1;
    if (manifestEntryCount < MAX_MANIFEST_ENTRIES)
    {
      entryIndex = manifestEntryCount++;
      manifestEntries[entryIndex] = {expectedHash, expectedSize, relPath, false};
    }

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
    if (sizeMatches) actualHash = md5File(file);
    file.close();

    if (sizeMatches && actualHash.equalsIgnoreCase(expectedHash))
    {
      ++sameCount;
      if (entryIndex >= 0) manifestEntries[entryIndex].same = true;
      if (verbose)
      {
        Serial.print("PLAN same: SD://banners/");
        Serial.println(relPath);
      }
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
  Serial.print(verbose ? "Manifest planning complete: files " : "Manifest verification complete: files ");
  Serial.print(fileCount);
  Serial.print(", same ");
  Serial.print(sameCount);
  Serial.print(", changed ");
  Serial.print(changedCount);
  Serial.print(", missing ");
  Serial.println(missingCount);
  return changedCount == 0 && missingCount == 0;
}

void downloadPriorityChanges(const String &updateSource)
{
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
      if (downloadManifestFile(updateSource, manifestEntries[i].path))
      {
        ++ok;
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
      for (int r = 0; r < requiredFileCount; ++r)
      {
        Serial.print("  REQUIRED ");
        Serial.println(displayPath(requiredFiles[r]));
      }
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
  clearBackgroundDownloads();
  String url = updateSource + "/manifest.txt";
  HTTPClient http;
  http.setConnectTimeout(1500);
  http.setTimeout(2500);
  if (!http.begin(url))
  {
    Serial.println("Manifest GET failed to begin");
    updateStatusText = "manifest begin failed";
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
    bool contentHealthy = reportManifestPlan(body, true);
    if (!contentHealthy)
    {
      downloadPriorityChanges(updateSource);
      bool nowHealthy = reportManifestPlan(body, false);
      if (nowHealthy && remoteSum.length() > 0)
      {
        writeLocalSum(remoteSum);
        updateStatusText = "content current";
      }
      else
      {
        queueBackgroundDownloads();
      }
    }
    else if (remoteSum.length() > 0)
    {
      writeLocalSum(remoteSum);
      updateStatusText = "content current";
    }
  }
  else
  {
    updateStatusText = String("manifest HTTP ") + code;
  }
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
    processBackgroundDownload();
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
