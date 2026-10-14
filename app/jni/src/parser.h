#pragma once

#include <string>
#include <vector>

struct Note {
    int column;       // 0..key_count-1
    double time;      // ms
    bool is_hold;
    double end_time;  // ms, equals time for taps
};

struct Beatmap {
    int key_count = 0;
    double bpm = 0.0;
    std::string title;
    std::string artist;
    std::string version;
    std::vector<Note> notes;
};

bool parse_osu_file(const std::string& path, Beatmap& out);
