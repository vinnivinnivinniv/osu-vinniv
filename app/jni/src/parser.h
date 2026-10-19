#pragma once

#include <string>
#include <vector>

struct Note {
    int column;       // 0..key_count-1
    double time;      // ms
    bool is_hold;
    int  hit_sound;   // bitfield: 1=normal, 2=whistle, 4=finish, 8=clap
    int  source_type = 1;  // raw type field: 1=circle, 2=slider, 8=spinner (bitmask)
    float source_x = 0.f;  // raw X (0..512). Fractional in osu file format v14+.
    float source_y = 0.f;  // raw Y (0..384). Used by converter for 2D pos separation.
    int  sample_bank; // 0=auto, 1=normal, 2=soft, 3=drum (for hitnormal)
    int  sample_bank_addition = 0;  // 0=inherit from sample_bank; else 1/2/3
    std::string sample_suffix;  // custom sample index from field 6, "" if none
    int  sample_volume;         // 0-100, 0 = default
    std::string sample_filename; // full override from field 6, empty if none
    double end_time;  // ms, equals time for taps
};

struct TimingPoint {
    double time        = 0.0;   // ms
    double beat_length = 500.0; // ms/beat from the active uninherited point
    bool   uninherited = true;
    double multiplier  = 1.0;   // Velocity * 1000 / beat_length (lazer's MulCP.Multiplier)
    int    sample_set  = 0;     // field 3: 0=inherit [General], 1=normal, 2=soft, 3=drum
};

struct Beatmap {
    int mode = 0;   // 0=std, 1=taiko, 2=catch, 3=mania
    int key_count = 0;
    double od = 5.0;
    double drain_rate = 5.0;      // HPDrainRate (field 1 of [Difficulty])
    double approach_rate = -1.0;  // ApproachRate; -1 means "inherit from OD" (osu format default)
    // Hit-object type counts, populated during parse. Used by the mania
    // converter for column-count heuristic (EndTimeObjectCount in lazer).
    int circle_count = 0;
    int slider_count = 0;
    int spinner_count = 0;
    double bpm = 0.0;
    std::string title;
    std::string artist;
    std::string version;
    std::string audio_file;
    std::string bg_file;
    std::string video_file;  // [Events] Video,<offset>,"file.mp4" — filename only
    int sample_set = 0;   // 0=auto, 1=normal, 2=soft, 3=drum (from [General])
    int preview_time = -1;  // ms; -1 = unset; from [General] PreviewTime
    std::vector<TimingPoint> timing_points;  // sorted by time; computed multiplier
    double base_beat_length = 500.0;         // lazer GetMostCommonBeatLength
    std::vector<Note> notes;
};

bool parse_osu_file(const std::string& path, Beatmap& out, bool metadata_only = false);
