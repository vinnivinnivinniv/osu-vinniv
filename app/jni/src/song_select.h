#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct SongEntry {
    std::string folder;      // absolute path to songs/<set name>/
    std::string osu_path;    // absolute path to chosen .osu
    std::string title;
    std::string artist;
    std::string version;     // difficulty name
    std::string audio_path;  // absolute path to audio file (or "" if missing)
    std::string bg_path;     // absolute path to bg image (or "" if missing)
    std::string video_path;  // absolute path to video (or "" if missing)
    int         key_count;
    int         original_mode = 3;  // 0=std, 1=taiko, 2=catch, 3=mania
    double      stars = 0.0;        // placeholder star rating (see read_meta)
    double      bpm_min = 0.0;      // 0 if unknown
    double      bpm_max = 0.0;
    double      od;
    int         preview_time = -1;  // ms; -1 = unset
};

// Walk <root>/*/ and find every .osu file. Each becomes one entry.
// Reads metadata only — no [HitObjects] parsing.
// If progress/total are non-null, a pre-pass counts .osu files and stores
// the value in *total, then *progress is incremented after each read_meta.
std::vector<SongEntry> scan_songs(const std::string& root,
                                  std::atomic<int>* progress = nullptr,
                                  std::atomic<int>* total    = nullptr);

// Async scan. start() spawns a worker that (a) imports all .osz from
// inbox_root, then (b) runs scan_songs(songs_root). Main thread polls
// try_consume() each frame; the scan result becomes available exactly
// once. See docs/scroll.md for the scroll model this sits alongside.
struct SongScanState {
    SongScanState() = default;
    ~SongScanState();

    SongScanState(const SongScanState&) = delete;
    SongScanState& operator=(const SongScanState&) = delete;

    void start(const std::string& songs_root, const std::string& inbox_root);

    // Returns true exactly once, after the worker has finished, moving
    // the scanned entries into `out`. Subsequent calls return false.
    bool try_consume(std::vector<SongEntry>& out);

    bool complete() const { return complete_flag.load(); }

    // Live progress for the UI. total == 0 means "not yet known"
    // (pre-pass running, or cache hit).
    std::atomic<int> progress{0};
    std::atomic<int> total{0};

    void shutdown();

private:
    std::thread            worker;
    std::atomic<bool>      complete_flag{false};
    std::mutex             mtx;
    std::vector<SongEntry> result;
    bool                   consumed = false;  // guarded by mtx
};
