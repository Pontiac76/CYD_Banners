#include "playlist_manager.h"

#include "app_state.h"
#include "display_manager.h"
#include "network_manager.h"

constexpr const char *RUNTIME_PLAYLIST_CACHE = "/banners/runtime_playlist.cache";
String parsedPlaylists[MAX_PLAYLIST_FILES];
int parsedPlaylistCount = 0;

String macNoColon()
{
  String mac = macAddressText();
  mac.replace(":", "");
  mac.replace("-", "");
  mac.toUpperCase();
  return mac;
}
constexpr int WILDCARD_BUFFER_DEPTH = 2;
constexpr int MAX_WILDCARD_MATCHES = 64;
String wildcardMatches[WILDCARD_BUFFER_DEPTH][MAX_WILDCARD_MATCHES];
String wildcardSegments[WILDCARD_BUFFER_DEPTH][10];
int wildcardExpansionDepth = 0;

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

unsigned long parseDurationMs(String &value, bool &explicitDuration)
{
  explicitDuration = false;
  int pipe = value.lastIndexOf('|');
  if (pipe < 0) return DEFAULT_SLIDE_MS;
  explicitDuration = true;
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

bool addSlide(const String &type, const String &pathOrPayload, const String &displayPathText, unsigned long durationMs, bool explicitDuration)
{
  if (slideCount >= MAX_SLIDES)
  {
    writeln("Slide list full; skipping item");
    return false;
  }
  slides[slideCount++] = {type, pathOrPayload, displayPathText, durationMs, explicitDuration};
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
void processResolvedPath(const String &path, unsigned long durationMs, bool explicitDuration);

bool wildcardMatch(const char *pattern, const char *text)
{
  while (*pattern)
  {
    if (*pattern == '*')
    {
      while (*(pattern + 1) == '*') ++pattern;
      if (*(pattern + 1) == '\0') return true;
      for (const char *scan = text; *scan; ++scan)
      {
        if (wildcardMatch(pattern + 1, scan)) return true;
      }
      return wildcardMatch(pattern + 1, text);
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

bool hasWildcard(const String &path)
{
  return path.indexOf('*') >= 0 || path.indexOf('?') >= 0;
}

String basenameOf(const String &path)
{
  int slash = path.lastIndexOf('/');
  return slash >= 0 ? path.substring(slash + 1) : path;
}

void splitPathSegments(const String &path, String segments[], int &segmentCount, int maxSegments)
{
  segmentCount = 0;
  int start = path.startsWith("/") ? 1 : 0;
  while (start <= path.length() && segmentCount < maxSegments)
  {
    int slash = path.indexOf('/', start);
    String segment = slash < 0 ? path.substring(start) : path.substring(start, slash);
    if (segment != "") segments[segmentCount++] = segment;
    if (slash < 0) break;
    start = slash + 1;
  }
}

void insertSorted(String matches[], int &matchCount, int maxMatches, const String &path)
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

void collectWildcardMatches(const String &currentDir, String segments[], int segmentIndex, int segmentCount, String matches[], int &matchCount, int maxMatches)
{
  if (segmentIndex >= segmentCount) return;

  String pattern = segments[segmentIndex];
  bool lastSegment = segmentIndex == segmentCount - 1;
  File dir = SD.open(currentDir, FILE_READ);
  if (!dir || !dir.isDirectory())
  {
    if (dir) dir.close();
    return;
  }

  while (true)
  {
    File entry = dir.openNextFile();
    if (!entry) break;
    String name = basenameOf(String(entry.name()));
    bool nameMatches = wildcardMatch(pattern.c_str(), name.c_str());
    if (nameMatches)
    {
      String entryPath = currentDir == "/" ? String("/") + name : currentDir + "/" + name;
      if (lastSegment)
      {
        if (!entry.isDirectory()) insertSorted(matches, matchCount, maxMatches, entryPath);
      }
      else if (entry.isDirectory())
      {
        collectWildcardMatches(entryPath, segments, segmentIndex + 1, segmentCount, matches, matchCount, maxMatches);
      }
    }
    entry.close();
  }
  dir.close();
}

bool expandWildcardPath(const String &path, unsigned long durationMs, bool explicitDuration)
{
  noteRequiredFile(path);
  drawWorkNotice("Playlist wildcard", displayPath(path));
  if (isLfsPath(path)) return false;

  constexpr int maxSegments = 10;
  int bufferIndex = wildcardExpansionDepth % WILDCARD_BUFFER_DEPTH;
  ++wildcardExpansionDepth;
  String *matches = wildcardMatches[bufferIndex];
  String *segments = wildcardSegments[bufferIndex];
  int segmentCount = 0;
  splitPathSegments(path, segments, segmentCount, maxSegments);
  if (segmentCount == 0)
  {
    --wildcardExpansionDepth;
    return true;
  }

  int matchCount = 0;
  String ext = lowerExtension(path);
  bool useManifestMatches = manifestHasEntries() && ext != ".ini" && ext != ".play";
  if (useManifestMatches)
  {
    matchCount = collectManifestMatches(path, matches, MAX_WILDCARD_MATCHES);
  }
  if (matchCount == 0)
  {
    collectWildcardMatches(path.startsWith("/") ? String("/") : String(PROJECT_ROOT), segments, 0, segmentCount, matches, matchCount, MAX_WILDCARD_MATCHES);
  }

  if (matchCount == 0)
  {
    drawWorkNotice("No wildcard matches", displayPath(path));
    write("Wildcard playlist entry matched no files: ");
    writeln(displayPath(path));
    noteMissingFile(path);
    --wildcardExpansionDepth;
    return true;
  }

  for (int i = 0; i < matchCount; ++i)
  {
    if (i == 0 || i % 5 == 0)
    {
      drawWorkNotice("Adding wildcard matches", String(i + 1) + "/" + String(matchCount) + " " + displayPath(matches[i]));
    }
    String matchPath = matches[i];
    processResolvedPath(matchPath, durationMs, explicitDuration);
  }
  --wildcardExpansionDepth;
  return true;
}

void parsePlayFile(const String &path)
{
  drawWorkNotice("Reading playlist", displayPath(path));
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

void processResolvedPath(const String &path, unsigned long durationMs, bool explicitDuration)
{
  String ext = lowerExtension(path);

  if (ext == ".play" || ext == ".ini")
  {
    parsePlayFile(path);
  }
  else if (ext == ".txt")
  {
    noteRequiredFile(path);
    fileExistsTracked(path);
    addSlide("TEXT", path, displayPath(path), durationMs, explicitDuration);
  }
  else if (ext == ".qr")
  {
    noteRequiredFile(path);
    fileExistsTracked(path);
    addSlide("QR", path, displayPath(path), durationMs, explicitDuration);
  }
  else if (ext == ".cyd")
  {
    noteRequiredFile(path);
    fileExistsTracked(path);
    addSlide("IMAGE", path, displayPath(path), durationMs, explicitDuration);
  }
  else if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp")
  {
    write("Unsupported source image referenced, server conversion needed: ");
    writeln(displayPath(path));
    if (!fileExistsTracked(path)) return;
    addSlide("UNSUPPORTED_IMAGE", path, displayPath(path), durationMs, explicitDuration);
  }
  else
  {
    write("Unknown playlist entry extension: ");
    writeln(displayPath(path));
  }
}

void processPathEntry(String value, const String &baseDir)
{
  bool explicitDuration = false;
  unsigned long durationMs = parseDurationMs(value, explicitDuration);
  String path = joinPath(baseDir, value);
  if (hasWildcard(path) && expandWildcardPath(path, durationMs, explicitDuration)) return;
  processResolvedPath(path, durationMs, explicitDuration);
}

String fieldAt(const String &line, int fieldIndex)
{
  int start = 0;
  for (int i = 0; i < fieldIndex; ++i)
  {
    start = line.indexOf('\t', start);
    if (start < 0) return String("");
    ++start;
  }
  int end = line.indexOf('\t', start);
  return end < 0 ? line.substring(start) : line.substring(start, end);
}

bool writeCachedPlaylist()
{
  File file = SD.open(RUNTIME_PLAYLIST_CACHE, FILE_WRITE);
  if (!file)
  {
    writeln("Runtime playlist cache write failed: open failed");
    return false;
  }
  file.println("CYDPLAY1");
  for (int i = 0; i < parsedPlaylistCount; ++i)
  {
    file.print("P\t");
    file.println(parsedPlaylists[i]);
  }
  for (int i = 0; i < requiredFileCount; ++i)
  {
    file.print("R\t");
    file.println(requiredFiles[i]);
  }
  for (int i = 0; i < slideCount; ++i)
  {
    file.print("S\t");
    file.print(slides[i].type);
    file.print('\t');
    file.print(slides[i].durationMs);
    file.print('\t');
    file.print(slides[i].explicitDuration ? 1 : 0);
    file.print('\t');
    file.print(slides[i].pathOrPayload);
    file.print('\t');
    file.println(slides[i].displayPath);
  }
  file.close();
  write("Runtime playlist cache written: slides ");
  write(slideCount);
  write(", required ");
  write(requiredFileCount);
  write(", playlists ");
  writeln(parsedPlaylistCount);
  return true;
}

bool loadCachedPlaylist()
{
  writeln("PLAYLIST: loading runtime cache");
  bool mounted = SD.begin(SD_CS_PIN);
  sdOk = mounted;
  if (!mounted)
  {
    digitalWrite(TOUCH_CS_PIN, HIGH);
    writeln("PLAYLIST: cache load SD mount failed");
    return false;
  }
  File file = SD.open(RUNTIME_PLAYLIST_CACHE, FILE_READ);
  if (!file)
  {
    /* SD stays mounted */
    digitalWrite(TOUCH_CS_PIN, HIGH);
    writeln("PLAYLIST: runtime cache missing");
    return false;
  }
  String header = file.readStringUntil('\n');
  header.replace("\r", "");
  header.trim();
  if (header != "CYDPLAY1")
  {
    file.close();
    /* SD stays mounted */
    digitalWrite(TOUCH_CS_PIN, HIGH);
    writeln("PLAYLIST: runtime cache bad header");
    return false;
  }

  slideCount = 0;
  parsedPlaylistCount = 0;
  requiredFileCount = 0;
  currentSlideIndex = -1;

  while (file.available())
  {
    String line = file.readStringUntil('\n');
    line.replace("\r", "");
    if (line.length() < 2) continue;
    String kind = fieldAt(line, 0);
    if (kind == "P" && parsedPlaylistCount < MAX_PLAYLIST_FILES)
    {
      parsedPlaylists[parsedPlaylistCount++] = fieldAt(line, 1);
    }
    else if (kind == "R" && requiredFileCount < MAX_REQUIRED_FILES)
    {
      requiredFiles[requiredFileCount++] = fieldAt(line, 1);
    }
    else if (kind == "S" && slideCount < MAX_SLIDES)
    {
      slides[slideCount++] = {fieldAt(line, 1), fieldAt(line, 4), fieldAt(line, 5), static_cast<unsigned long>(fieldAt(line, 2).toInt()), fieldAt(line, 3).toInt() != 0};
    }
  }
  file.close();
  /* SD stays mounted */
  digitalWrite(TOUCH_CS_PIN, HIGH);
  write("PLAYLIST: runtime cache loaded slides ");
  write(slideCount);
  write(", required ");
  write(requiredFileCount);
  write(", playlists ");
  writeln(parsedPlaylistCount);
  return slideCount > 0;
}

int currentGeneratedPlaylistChunk = -1;
bool usingGeneratedPlaylistChunks = false;

bool loadGeneratedPlaylistChunk(int chunkIndex)
{
  bool mounted = SD.begin(SD_CS_PIN);
  sdOk = mounted;
  if (!mounted)
  {
    digitalWrite(TOUCH_CS_PIN, HIGH);
    writeln("PLAYLIST: generated chunk load SD mount failed");
    return false;
  }

  char name[32];
  snprintf(name, sizeof(name), "/playlist_%03d.ini", chunkIndex);
  String chunkPath = String(PROJECT_ROOT) + "/_generated/playlists/" + macNoColon() + name;
  if (!SD.exists(chunkPath))
  {
    digitalWrite(TOUCH_CS_PIN, HIGH);
    return false;
  }

  File file = SD.open(chunkPath, FILE_READ);
  if (!file)
  {
    digitalWrite(TOUCH_CS_PIN, HIGH);
    write("PLAYLIST: generated chunk open failed: ");
    writeln(chunkPath);
    return false;
  }

  slideCount = 0;
  parsedPlaylistCount = 0;
  requiredFileCount = 0;
  currentSlideIndex = -1;
  if (parsedPlaylistCount < MAX_PLAYLIST_FILES) parsedPlaylists[parsedPlaylistCount++] = chunkPath;

  while (file.available())
  {
    String value;
    if (readPlaylistPathLine(file.readStringUntil('\n'), value)) processPathEntry(value, String(PROJECT_ROOT));
  }
  file.close();
  currentGeneratedPlaylistChunk = chunkIndex;
  usingGeneratedPlaylistChunks = true;
  digitalWrite(TOUCH_CS_PIN, HIGH);
  write("PLAYLIST: generated chunk loaded ");
  write(chunkIndex);
  write(" slides ");
  write(slideCount);
  write(", required ");
  writeln(requiredFileCount);
  return slideCount > 0;
}

bool loadGeneratedPlaylistChunks()
{
  writeln("PLAYLIST: loading generated playlist chunk 0");
  return loadGeneratedPlaylistChunk(0);
}

bool loadNextGeneratedPlaylistChunk()
{
  if (!usingGeneratedPlaylistChunks) return false;
  int nextChunk = currentGeneratedPlaylistChunk + 1;
  if (loadGeneratedPlaylistChunk(nextChunk)) return true;
  return loadGeneratedPlaylistChunk(0);
}

void printRuntimePlaylist()
{
  writeln("Runtime playlist:");
  if (slideCount == 0)
  {
    writeln("  <none>");
    return;
  }

  for (int i = 0; i < slideCount; ++i)
  {
    write("  ");
    write(i + 1);
    write(". ");
    write(slides[i].type);
    write(" ");
    write(slides[i].displayPath);
    write(" duration=");
    if (!slides[i].explicitDuration)
    {
      writeln("(Default)");
    }
    else
    {
      write(slides[i].durationMs / 1000UL);
      writeln("s");
    }
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
  writeln("PLAYLIST: rebuild start");
  drawWorkNotice("Building playlist", "starting");
  slideCount = 0;
  parsedPlaylistCount = 0;
  requiredFileCount = 0;
  currentSlideIndex = -1;

  writeln("PLAYLIST: mounting SD");
  sdOk = SD.begin(SD_CS_PIN);
  write("PLAYLIST: SD mount ");
  writeln(sdOk ? "ok" : "failed");
  if (!sdOk || !SD.exists(PROJECT_ROOT))
  {
    if (sdOk)
    {
      noteMissingFile(PROJECT_ROOT);
      /* SD stays mounted */
    }
    if (fileExistsTracked("LFS://Banners/error.txt")) addSlide("TEXT", "LFS://Banners/error.txt", "LFS://Banners/error.txt", DEFAULT_SLIDE_MS, false);
    return;
  }

  writeln("PLAYLIST: project root found");
  noteFileExists(PROJECT_ROOT);
  writeln("PLAYLIST: parse root starting");
  parseRootPlaylistFile();
  writeln("PLAYLIST: parse root complete");
  writeCachedPlaylist();
  /* SD stays mounted */

  drawWorkNotice("Playlist ready", String(slideCount) + " slides");
  write("Runtime slides: ");
  writeln(slideCount);
  // Keep printRuntimePlaylist() available for temporary debugging, but avoid
  // dumping large playlists during normal operation.
}
