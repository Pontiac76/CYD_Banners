#pragma once

#include <Arduino.h>

constexpr int MAX_WIFI_PROFILES = 8;
constexpr int MAX_UPDATE_SOURCES = 4;

struct WifiProfile
{
  String name;
  String ssid;
  String password;
  uint32_t attempts;
};

enum class NetworkHealth
{
  Hunting,
  DisconnectedAfterCycle,
  ConnectedProblem,
  ConnectedGood
};

extern WifiProfile wifiProfiles[MAX_WIFI_PROFILES];
extern int wifiProfileCount;
extern int currentWifiProfile;
extern String updateSources[MAX_UPDATE_SOURCES];
extern int updateSourceCount;
extern bool serviceTokenPresent;
extern String networkStatusText;
extern String updateStatusText;
extern unsigned long lastCallHomeMs;
extern int wifiCompletedCycles;
extern unsigned long configSerialHeartbeatIntervalMs;
extern unsigned long configServerHeartbeatIntervalMs;
extern unsigned long configCallHomeIntervalMs;
extern unsigned long configFirmwareOtaFirstCheckMs;
extern unsigned long configFirmwareOtaCheckIntervalMs;
extern unsigned long configWifiConnectWindowMs;
extern unsigned long configWifiRetryIdleMs;

void networkBegin();
void networkUpdate();
void sendHeartbeatReport();
void processAcceptedManifestScanStep();
NetworkHealth networkHealth();
uint16_t networkStatusBarColor();
String currentWifiSsidText();
bool manifestHasEntries();
int collectManifestMatches(const String &sdPattern, String matches[], int maxMatches);
bool resetLocalContentState();
bool invalidateLocalContentSum(const String &reasonPath);
