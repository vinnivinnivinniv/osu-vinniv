#pragma once

#include <string>
#include <vector>

struct SongEntry {
    std::string folder;      // absolute path to songs/<set name>/
    std::string osu_path;    // absolute path to chosen .osu
    std::string title;
    std::string artist;
    std::string version;     // difficulty name
    std::string audio_path;  // absolute path to audio file (or "" if missing)
    std::string bg_path;     // absolute path to bg image (or "" if missing)
    int         key_count;
    double      od;
};

// Walk <root>/*/ and find every .osu file. Each becomes one entry.
// Reads metadata only — no [HitObjects] parsing.
std::vector<SongEntry> scan_songs(const std::string& root);
