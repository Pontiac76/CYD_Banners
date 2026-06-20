#pragma once

#include <Arduino.h>

extern String parsedPlaylists[];
extern int parsedPlaylistCount;

bool loadCachedPlaylist();
bool loadGeneratedPlaylistChunks();
bool loadGeneratedPlaylistChunk(int chunkIndex);
bool loadNextGeneratedPlaylistChunk();
void rebuildPlaylist();
