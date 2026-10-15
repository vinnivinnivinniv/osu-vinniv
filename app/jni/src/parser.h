#pragma once

#include <string>
#include <vector>

struct Note {
    int column;       // 0..key_count-1
    double time;      // ms
    bool is_hold;
    int  hit_sound;   // bitfield: 1=normal, 2=whistle, 4=finish, 8=clap
    int  sample_bank; // 0=auto, 1=normal, 2=soft, 3=drum (from field 6)
    std::string sample_suffix;  // custom suffix from field 6
    int  sample_volume;         // 0-100, 0 = default
    std::string sample_filename; // full override from field 6, empty if none
    double end_time;  // ms, equals time for taps
};

struct Beatmap {
    int mode = 0;   // 0=std, 1=taiko, 2=catch, 3=mania
    int key_count = 0;
    double od = 5.0;
    double bpm = 0.0;
    std::string title;
    std::string artist;
    std::string version;
    std::string audio_file;
    std::string bg_file;
    int sample_set = 0;   // 0=auto, 1=normal, 2=soft, 3=drum (from [General])
    std::vector<Note> notes;
};

bool parse_osu_file(const std::string& path, Beatmap& out, bool metadata_only = false);
