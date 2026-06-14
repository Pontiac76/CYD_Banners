#include "playlist_manager.h"

#include "app_state.h"

String parsedPlaylists[MAX_PLAYLIST_FILES];
int parsedPlaylistCount = 0;

String dirnameOf(const String &path)
{
  int slash = path.lastIndexOf('/');
  return slash <= 0 ? String("") : path.substring(0, slash);
}

String joinPath(const String &baseDir, const String &value)
{
  String v = value;
  v.trim();

  int colon = v.indexOf(':');
  if (colon > 0)
  {
    String maybeKey = v.substring(0, colon);
    maybeKey.trim();
    maybeKey.toUpperCase();
    if (maybeKey == "PLAYLIST")
    {
      v = v.substring(colon + 1);
      v.trim();
    }
  }

  String lower = v;
  lower.toLowerCase();
  if (lower.startsWith("sd://"))
  {
    String path = v.substring(5);
    path.trim();
    return path.startsWith("/") ? path : String("/") + path;
  }
  if (lower.startsWith("lfs://"))
  {
    return v;
  }
  if (v.startsWith("/")) return v;
  if (baseDir == "") return String(PROJECT_ROOT) + "/" + v;
  return baseDir + "/" + v;
}

String lowerExtension(const String &path)
{
  int dot = path.lastIndexOf('.');
  if (dot < 0) return String("");
  String ext = path.substring(dot);
  ext.toLowerCase();
  return ext;
}

unsigned long parseDurationMs(String &value)
{
  int pipe = value.lastIndexOf('|');
  if (pipe < 0) return DEFAULT_SLIDE_MS;
  String durationText = value.substring(pipe + 1);
  durationText.trim();
  value = value.substring(0, pipe);
  value.trim();
  unsigned long seconds = durationText.toInt();
  return seconds > 0 ? seconds * 1000UL : DEFAULT_SLIDE_MS;
}

String normalizeSectionName(String section)
{
  section.trim();
  section.toUpperCase();
  section.replace(":", "");
  section.replace("-", "");
  section.replace(" ", "");
  return section;
}

String currentMacSectionName()
{
  return normalizeSectionName(macAddressText());
}

bool readPlaylistPathLine(String line, String &value)
{
  line.replace("\r", "");
  line.trim();
  if (line == "" || line.startsWith("#")) return false;
  if (line.startsWith("[") && line.endsWith("]")) return false;
  value = line;
  return true;
}

bool addSlide(const String &type, const String &pathOrPayload, const String &displayPathText, unsigned long durationMs)
{
  if (slideCount >= MAX_SLIDES)
  {
    Serial.println("Slide list full; skipping item");
    return false;
  }
  slides[slideCount++] = {type, pathOrPayload, displayPathText, durationMs};
  return true;
}

bool alreadyParsedPlaylist(const String &path)
{
  for (int i = 0; i < parsedPlaylistCount; ++i)
  {
    if (parsedPlaylists[i] == path) return true;
  }
  if (parsedPlaylistCount < MAX_PLAYLIST_FILES) parsedPlaylists[parsedPlaylistCount++] = path;
  return false;
}

void processPathEntry(String value, const String &baseDir);

void parsePlayFile(const String &path)
{
  if (alreadyParsedPlaylist(path)) return;
  if (!fileExistsTracked(path)) return;

  File file = SD.open(path, FILE_READ);
  if (!file)
  {
    noteMissingFile(path);
    return;
  }

  String baseDir = dirnameOf(path);
  while (file.available())
  {
    String value;
    if (readPlaylistPathLine(file.readStringUntil('\n'), value)) processPathEntry(value, baseDir);
  }
  file.close();
}

void processPathEntry(String value, const String &baseDir)
{
  unsigned long durationMs = parseDurationMs(value);
  String path = joinPath(baseDir, value);
  String ext = lowerExtension(path);

  if (ext == ".play" || ext == ".ini")
  {
    parsePlayFile(path);
  }
  else if (ext == ".txt")
  {
    if (fileExistsTracked(path)) addSlide("TEXT", path, displayPath(path), durationMs);
  }
  else if (ext == ".qr")
  {
    if (fileExistsTracked(path)) addSlide("QR", path, displayPath(path), durationMs);
  }
  else if (ext == ".cyd")
  {
    if (fileExistsTracked(path)) addSlide("IMAGE", path, displayPath(path), durationMs);
  }
  else if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp")
  {
    Serial.print("Unsupported source image referenced, server conversion needed: ");
    Serial.println(displayPath(path));
    if (!fileExistsTracked(path)) return;
    addSlide("UNSUPPORTED_IMAGE", path, displayPath(path), durationMs);
  }
  else
  {
    Serial.print("Unknown playlist entry extension: ");
    Serial.println(displayPath(path));
  }
}

bool parseRootSectionsPass(const String &wantedSection, bool includeNoSection)
{
  File file = SD.open(ROOT_PLAYLIST, FILE_READ);
  if (!file)
  {
    noteMissingFile(ROOT_PLAYLIST);
    return false;
  }

  bool matchedAnySectionLine = false;
  bool active = includeNoSection;
  String baseDir = dirnameOf(ROOT_PLAYLIST);

  while (file.available())
  {
    String line = file.readStringUntil('\n');
    line.replace("\r", "");
    line.trim();

    if (line.startsWith("[") && line.endsWith("]"))
    {
      String section = normalizeSectionName(line.substring(1, line.length() - 1));
      active = (section == wantedSection);
      continue;
    }

    if (!active) continue;

    String value;
    if (readPlaylistPathLine(line, value))
    {
      matchedAnySectionLine = true;
      processPathEntry(value, baseDir);
    }
  }

  file.close();
  return matchedAnySectionLine;
}

void parseRootPlaylistFile()
{
  if (!fileExistsTracked(ROOT_PLAYLIST)) return;

  parseRootSectionsPass("GLOBAL", true);
  if (!parseRootSectionsPass(currentMacSectionName(), false))
  {
    parseRootSectionsPass("DEFAULT", false);
  }
}

void rebuildPlaylist()
{
  slideCount = 0;
  parsedPlaylistCount = 0;
  currentSlideIndex = -1;

  sdOk = SD.begin(SD_CS_PIN);
  if (!sdOk || !SD.exists(PROJECT_ROOT))
  {
    if (sdOk)
    {
      noteMissingFile(PROJECT_ROOT);
      SD.end();
    }
    if (fileExistsTracked("LFS://Banners/error.txt")) addSlide("TEXT", "LFS://Banners/error.txt", "LFS://Banners/error.txt", DEFAULT_SLIDE_MS);
    return;
  }

  noteFileExists(PROJECT_ROOT);
  parseRootPlaylistFile();
  SD.end();

  Serial.print("Runtime slides: ");
  Serial.println(slideCount);
}
