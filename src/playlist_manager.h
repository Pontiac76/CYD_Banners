#pragma once

#include <Arduino.h>

extern String parsedPlaylists[];
extern int parsedPlaylistCount;

bool loadCachedPlaylist();
void rebuildPlaylist();
