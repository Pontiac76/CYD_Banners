#include "firmware_ota.h"

#include "app_state.h"
#include "firmware_version.h"
#include "network_manager.h"

#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>

bool firmwareOtaActive = false;

namespace
{
String jsonStringValue(const String &json, const String &key)
{
  String marker = String("\"") + key + "\"";
  int keyPos = json.indexOf(marker);
  if (keyPos < 0) return "";
  int colon = json.indexOf(':', keyPos + marker.length());
  if (colon < 0) return "";
  int firstQuote = json.indexOf('"', colon + 1);
  if (firstQuote < 0) return "";
  int secondQuote = json.indexOf('"', firstQuote + 1);
  if (secondQuote < 0) return "";
  return json.substring(firstQuote + 1, secondQuote);
}

String jsonNumberValue(const String &json, const String &key)
{
  String marker = String("\"") + key + "\"";
  int keyPos = json.indexOf(marker);
  if (keyPos < 0) return "";
  int colon = json.indexOf(':', keyPos + marker.length());
  if (colon < 0) return "";
  int start = colon + 1;
  while (start < json.length() && isspace(static_cast<unsigned char>(json[start]))) ++start;
  int end = start;
  while (end < json.length() && isdigit(static_cast<unsigned char>(json[end]))) ++end;
  return json.substring(start, end);
}

String absoluteUrl(const String &base, const String &path)
{
  if (path.startsWith("http://") || path.startsWith("https://")) return path;
  if (base.endsWith("/") && path.startsWith("/")) return base.substring(0, base.length() - 1) + path;
  if (!base.endsWith("/") && !path.startsWith("/")) return base + "/" + path;
  return base + path;
}

void drawOtaScreen(const String &line1, const String &line2 = "")
{
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("FIRMWARE UPDATE", tft.width() / 2, 35, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Do not power off", tft.width() / 2, 70, 2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(line1, tft.width() / 2, 115, 2);
  if (line2.length() > 0)
  {
    String shown = line2;
    if (shown.length() > 34) shown = shown.substring(0, 31) + "...";
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(shown, tft.width() / 2, 145, 1);
  }
}

bool downloadAndApplyFirmware(const String &url, const String &md5, int expectedSize)
{
  firmwareOtaActive = true;
  updateUiLocked = true;
  digitalWrite(TOUCH_CS_PIN, HIGH);
  drawOtaScreen("Downloading firmware", md5);
  writeln("Firmware OTA: starting exclusive update mode");
  write("Firmware OTA URL: "); writeln(url);

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(url))
  {
    writeln("Firmware OTA: http.begin failed");
    drawOtaScreen("Update failed", "HTTP begin failed");
    firmwareOtaActive = false;
    updateUiLocked = false;
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK)
  {
    write("Firmware OTA: HTTP "); writeln(code);
    http.end();
    drawOtaScreen("Update failed", String("HTTP ") + code);
    firmwareOtaActive = false;
    updateUiLocked = false;
    return false;
  }

  int contentLength = http.getSize();
  if (expectedSize > 0 && contentLength > 0 && contentLength != expectedSize)
  {
    write("Firmware OTA: size mismatch header="); write(contentLength); write(" expected="); writeln(expectedSize);
    http.end();
    drawOtaScreen("Update failed", "Size mismatch");
    firmwareOtaActive = false;
    updateUiLocked = false;
    return false;
  }

  if (md5.length() == 32) Update.setMD5(md5.c_str());
  if (!Update.begin(expectedSize > 0 ? expectedSize : UPDATE_SIZE_UNKNOWN))
  {
    writeln("Firmware OTA: Update.begin failed");
    http.end();
    drawOtaScreen("Update failed", "Update.begin failed");
    firmwareOtaActive = false;
    updateUiLocked = false;
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  size_t written = Update.writeStream(*stream);
  if (expectedSize > 0 && written != static_cast<size_t>(expectedSize))
  {
    write("Firmware OTA: written mismatch "); write(written); write(" expected "); writeln(expectedSize);
  }

  bool ok = Update.end();
  http.end();
  if (!ok || !Update.isFinished())
  {
    write("Firmware OTA: Update failed error="); writeln(Update.errorString());
    drawOtaScreen("Update failed", Update.errorString());
    firmwareOtaActive = false;
    updateUiLocked = false;
    return false;
  }

  drawOtaScreen("Update complete", "Rebooting...");
  writeln("Firmware OTA: update complete, rebooting");
  delay(1000);
  ESP.restart();
  return true;
}
} // namespace

const char *currentFirmwareVersion()
{
  return CYD_FIRMWARE_VERSION;
}

void firmwareOtaBegin()
{
  write("Firmware version: "); writeln(currentFirmwareVersion());
}

bool firmwareOtaCheckAndApply()
{
  if (firmwareOtaActive) return true;
  if (WiFi.status() != WL_CONNECTED) return false;
  if (updateSourceCount <= 0) return false;

  for (int i = 0; i < updateSourceCount; ++i)
  {
    String latestUrl = absoluteUrl(updateSources[i], "firmware/latest.json");
    HTTPClient http;
    if (!http.begin(latestUrl)) continue;
    int code = http.GET();
    if (code != HTTP_CODE_OK)
    {
      http.end();
      continue;
    }
    String body = http.getString();
    http.end();

    String enabled = jsonStringValue(body, "enabled");
    if (enabled == "false" || enabled == "0") return false;

    String version = jsonStringValue(body, "version");
    String md5 = jsonStringValue(body, "md5");
    String url = jsonStringValue(body, "url");
    int size = jsonNumberValue(body, "size").toInt();
    if (version.length() == 0 || url.length() == 0) continue;

    if (version == currentFirmwareVersion())
    {
      write("Firmware OTA: already current "); writeln(version);
      return false;
    }

    write("Firmware OTA: update available current="); write(currentFirmwareVersion()); write(" remote="); writeln(version);
    return downloadAndApplyFirmware(absoluteUrl(updateSources[i], url), md5, size);
  }
  return false;
}
