#pragma once

#include <ctime>
#include <string>
#include <vector>
#include "song_select.h"

bool load_song_cache(const std::string& path, time_t expected_mtime,
                     std::vector<SongEntry>& out);

void save_song_cache(const std::string& path, time_t mtime,
                     const std::vector<SongEntry>& entries);
