#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <vector>
#include "parser.h"
#include "song_select.h"
#include "osz_import.h"
#include "audio_load.h"
#include "preview_player.h"
#include "image_load.h"
#include "map_sample_bank.h"
#include "bundled_sounds.h"
#include <sys/stat.h>

// Write bundled default sounds to the files dir on first launch.
// Files already present are left alone so a user can override them.
static void ensure_bundled_sounds(const char* base_dir) {
    struct Bundled { const char* name; const unsigned char* data; unsigned int len; };
    Bundled items[] = {
        { "hit",       bundled_hit_ogg,       bundled_hit_ogg_len },
        { "clap",      bundled_clap_ogg,      bundled_clap_ogg_len },
        { "whistle",   bundled_whistle_ogg,   bundled_whistle_ogg_len },
        { "finish",    bundled_finish_ogg,    bundled_finish_ogg_len },
        { "miss",      bundled_miss_ogg,      bundled_miss_ogg_len },
        { "countdown", bundled_countdown_ogg, bundled_countdown_ogg_len },
    };
    for (auto& it : items) {
        char path[256];
        SDL_snprintf(path, sizeof(path), "%s/%s.ogg", base_dir, it.name);
        struct stat st;
        if (stat(path, &st) == 0 && st.st_size > 0) continue;
        FILE* f = fopen(path, "wb");
        if (!f) continue;
        fwrite(it.data, 1, it.len, f);
        fclose(f);
        SDL_Log("SOUNDS: wrote bundled %s", path);
    }
}

enum class AppState { SongSelect, Playing, Result, Settings };

// Piecewise-linear OD interpolation. Anchor points are (od0, od5, od10).
static double od_window(double od, double v0, double v5, double v10) {
    if (od < 5.0) return v0 + (od / 5.0) * (v5 - v0);
    return v5 + ((od - 5.0) / 5.0) * (v10 - v5);
}

// Lazer-faithful scoring helpers.
// Perfect = 305, Great = 300 (ManiaScoreProcessor.GetBaseScoreForResult override).
static int lazer_base_score(int tier) {
    // tier: 0=Perfect, 1=Great(300), 2=Good(200), 3=Ok(100), 4=Meh(50)
    switch (tier) {
        case 0: return 305;
        case 1: return 300;
        case 2: return 200;
        case 3: return 100;
        case 4: return 50;
    }
    return 0;
}

// Score formula from ManiaScoreProcessor.ComputeTotalScore.
//   score = 150000 * comboProgress
//         + 850000 * pow(acc, 2 + 2*acc) * accuracyProgress
static long compute_lazer_score(int perfect, int great, int good, int ok, int meh,
                                int total_notes) {
    if (total_notes <= 0) return 0;
    int judged = perfect + great + good + ok + meh;
    double currentBase = perfect * 305.0 + great * 300.0
                       + good * 200.0 + ok * 100.0 + meh * 50.0;
    double maxBase = total_notes * 305.0;
    double acc = maxBase > 0 ? currentBase / maxBase : 1.0;
    double comboProgress    = (double)judged / total_notes;
    double accuracyProgress = (double)judged / total_notes;
    double score = 150000.0 * comboProgress
                 + 850000.0 * std::pow(acc, 2.0 + 2.0 * acc) * accuracyProgress;
    return (long)score;
}

// osu!mania scroll speed -> pixels per millisecond.
// From osu source: timeRange = MAX_TIME_RANGE / speed, with speed 1.0-40.0.
// The visible playfield height corresponds to that time range.
static double osu_speed_to_scroll_pps(double speed, double visible_height_px) {
    const double MAX_TIME_RANGE = 11485.0;
    if (speed < 1.0)  speed = 1.0;
    if (speed > 40.0) speed = 40.0;
    double time_range = MAX_TIME_RANGE / speed;
    return visible_height_px / time_range;
}

int main(int argc, char* argv[]) {
    SDL_Log("OSUVINNIV_MAIN_ENTERED argc=%d", argc);

    // Write bundled default sounds to the files dir on first launch
    ensure_bundled_sounds("/sdcard/Android/data/sh.ppy.osuvinniv/files");
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    // Disable touch-to-mouse synthesis so a single tap does not fire both
    // FINGER_DOWN and MOUSE_BUTTON_DOWN (which double-judged gameplay notes).
    // All UI touch handling uses SDL_EVENT_FINGER_* directly.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    // ---- per-K key bindings (SDL keycodes per key count) ----
    // Defaults match modern osu!mania; overridden by settings.ini keys_Nk=...
    std::vector<SDL_Keycode> bindings[16];
    {
        bindings[1]  = { SDLK_SPACE };
        bindings[2]  = { SDLK_F, SDLK_J };
        bindings[3]  = { SDLK_F, SDLK_SPACE, SDLK_J };
        bindings[4]  = { SDLK_D, SDLK_F, SDLK_J, SDLK_K };
        bindings[5]  = { SDLK_D, SDLK_F, SDLK_SPACE, SDLK_J, SDLK_K };
        bindings[6]  = { SDLK_S, SDLK_D, SDLK_F, SDLK_J, SDLK_K, SDLK_L };
        bindings[7]  = { SDLK_S, SDLK_D, SDLK_F, SDLK_SPACE, SDLK_J, SDLK_K, SDLK_L };
        bindings[8]  = { SDLK_A, SDLK_S, SDLK_D, SDLK_F, SDLK_J, SDLK_K, SDLK_L, SDLK_SEMICOLON };
        bindings[9]  = { SDLK_A, SDLK_S, SDLK_D, SDLK_F, SDLK_SPACE, SDLK_J, SDLK_K, SDLK_L, SDLK_SEMICOLON };
        bindings[10] = { SDLK_A, SDLK_S, SDLK_D, SDLK_F, SDLK_V, SDLK_N, SDLK_J, SDLK_K, SDLK_L, SDLK_SEMICOLON };
    }

    // ---- settings.ini ----
    float cfg_music_vol    = 0.6f;
    float cfg_hitsound_vol = 1.0f;
    float cfg_scroll_speed = 1.5f;
    int   cfg_fps_cap      = 120;   // 0 = uncapped
    float cfg_audio_offset_ms = 0.0f;
    bool  vsync_enabled    = (cfg_fps_cap == 120);

    // ---- settings rows (single table drives render + touch) ----
    struct SettingRow {
        const char* label;
        int         type;          // 0=slider, 1=cycle, 2=keybind
        void*       target;        // float* (slider) or int* (cycle)
        float       min_val;
        float       max_val;
        const char* cycle_labels[4];
        int         cycle_values[4];
        int         cycle_count;
        int         key_count;
    };
    SettingRow setting_rows[] = {
        { "Music Volume",    0, &cfg_music_vol,        0.0f,   1.0f, {nullptr,nullptr,nullptr,nullptr}, {0,0,0,0}, 0, 0 },
        { "Hitsound Volume", 0, &cfg_hitsound_vol,     0.0f,   2.0f, {nullptr,nullptr,nullptr,nullptr}, {0,0,0,0}, 0, 0 },
        { "Scroll Speed",    0, &cfg_scroll_speed,     1.0f,  40.0f, {nullptr,nullptr,nullptr,nullptr}, {0,0,0,0}, 0, 0 },
        { "FPS Cap",         1, &cfg_fps_cap,          0.0f,   0.0f,
            {"60","120","240","Uncapped"}, {60,120,240,0}, 4, 0 },
        { "Audio Offset",    0, &cfg_audio_offset_ms, -200.f, 200.f, {nullptr,nullptr,nullptr,nullptr}, {0,0,0,0}, 0, 0 },
        { "4K Keys",         2, nullptr,               0.0f,   0.0f, {nullptr,nullptr,nullptr,nullptr}, {0,0,0,0}, 0,  4 },
        { "5K Keys",         2, nullptr,               0.0f,   0.0f, {nullptr,nullptr,nullptr,nullptr}, {0,0,0,0}, 0,  5 },
        { "6K Keys",         2, nullptr,               0.0f,   0.0f, {nullptr,nullptr,nullptr,nullptr}, {0,0,0,0}, 0,  6 },
        { "7K Keys",         2, nullptr,               0.0f,   0.0f, {nullptr,nullptr,nullptr,nullptr}, {0,0,0,0}, 0,  7 },
        { "8K Keys",         2, nullptr,               0.0f,   0.0f, {nullptr,nullptr,nullptr,nullptr}, {0,0,0,0}, 0,  8 },
        { "9K Keys",         2, nullptr,               0.0f,   0.0f, {nullptr,nullptr,nullptr,nullptr}, {0,0,0,0}, 0,  9 },
        { "10K Keys",        2, nullptr,               0.0f,   0.0f, {nullptr,nullptr,nullptr,nullptr}, {0,0,0,0}, 0, 10 },
    };
    const int ROW_COUNT = (int)(sizeof(setting_rows) / sizeof(setting_rows[0]));

    // row geometry (shared by render + touch)
    const float settings_row_x   = 80.0f;
    const float settings_row_y0  = 120.0f;
    const float settings_row_h   = 60.0f;
    const float settings_row_gap = 8.0f;

    auto save_settings = [&]() {
        FILE* f = fopen("/sdcard/Android/data/sh.ppy.osuvinniv/files/settings.ini", "w");
        if (!f) return;
        fprintf(f, "music_volume=%.3f\n",    cfg_music_vol);
        fprintf(f, "hitsound_volume=%.3f\n", cfg_hitsound_vol);
        fprintf(f, "scroll_speed=%.3f\n",    cfg_scroll_speed);
        fprintf(f, "fps_cap=%d\n",           cfg_fps_cap);
        fprintf(f, "audio_offset_ms=%.3f\n", cfg_audio_offset_ms);
        for (int nk = 1; nk <= 10; nk++) {
            fprintf(f, "keys_%dk=", nk);
            for (size_t i = 0; i < bindings[nk].size(); i++) {
                if (i) fprintf(f, ",");
                const char* nm = SDL_GetKeyName(bindings[nk][i]);
                fprintf(f, "%s", nm ? nm : "?");
            }
            fprintf(f, "\n");
        }
        fclose(f);
        SDL_Log("SETTINGS: saved");
    };
    {
        std::ifstream sf("/sdcard/Android/data/sh.ppy.osuvinniv/files/settings.ini");
        std::string line;
        while (std::getline(sf, line)) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq);
            std::string v = line.substr(eq + 1);
            try {
                if (k.rfind("keys_", 0) == 0) {
                    int nk = 0;
                    try { nk = std::stoi(k.substr(5)); } catch (...) { nk = 0; }
                    if (nk >= 1 && nk <= 10) {
                        std::vector<SDL_Keycode> parsed;
                        size_t pos = 0;
                        while (pos <= v.size()) {
                            size_t comma = v.find(',', pos);
                            std::string tok = (comma == std::string::npos)
                                ? v.substr(pos)
                                : v.substr(pos, comma - pos);
                            if (!tok.empty()) {
                                SDL_Keycode kc = SDL_GetKeyFromName(tok.c_str());
                                if (kc != SDLK_UNKNOWN) parsed.push_back(kc);
                            }
                            if (comma == std::string::npos) break;
                            pos = comma + 1;
                        }
                        if (parsed.size() == (size_t)nk) bindings[nk] = parsed;
                    }
                }
                else if (k == "music_volume")    cfg_music_vol    = std::stof(v);
                else if (k == "hitsound_volume") cfg_hitsound_vol = std::stof(v);
                else if (k == "scroll_speed")    cfg_scroll_speed = std::stof(v);
                else if (k == "fps_cap")         cfg_fps_cap      = std::stoi(v);
                else if (k == "audio_offset_ms") cfg_audio_offset_ms = std::stof(v);
            } catch (...) {}
        }
        SDL_Log("CFG: music=%.2f hit=%.2f scroll=%.2f",
                cfg_music_vol, cfg_hitsound_vol, cfg_scroll_speed);
    }


    SDL_Window* window = SDL_CreateWindow(
        "osu!vinniv",
        0, 0,
        SDL_WINDOW_FULLSCREEN);
    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        return 1;
    }
    // If fps_cap is 120 (matching panel rate), use vsync for accuracy.
    // Otherwise disable vsync and rely on the software cap below.
    // vsync is only enabled when the FPS cap is 120. Runtime changes
    // can disable it but never re-enable — SDL_SetRenderVSync(renderer, 1)
    // blocks on Android and freezes the event loop.
    if (vsync_enabled) SDL_SetRenderVSync(renderer, 1);
    else               SDL_SetRenderVSync(renderer, 0);

    int win_w = 0, win_h = 0;
    SDL_GetWindowSizeInPixels(window, &win_w, &win_h);
    SDL_Log("Window size: %dx%d", win_w, win_h);

    Beatmap map;
    {
        const char* map_path = "/sdcard/Android/data/sh.ppy.osuvinniv/files/map.osu";
        if (parse_osu_file(map_path, map)) {
            SDL_Log("MAP: %s - %s [%s]", map.artist.c_str(), map.title.c_str(), map.version.c_str());
            SDL_Log("  keys=%d  bpm=%.1f  notes=%zu", map.key_count, map.bpm, map.notes.size());
            // SAMPLE DUMP
            {
                int banks[4] = {0,0,0,0};
                int suffixes = 0, files = 0, vols = 0;
                for (const auto& n : map.notes) {
                    if (n.sample_bank >= 0 && n.sample_bank < 4) banks[n.sample_bank]++;
                    if (!n.sample_suffix.empty()) suffixes++;
                    if (!n.sample_filename.empty()) files++;
                    if (n.sample_volume > 0) vols++;
                }
                SDL_Log("  SampleSet=%d samples: b0=%d b1=%d b2=%d b3=%d sfx=%d fls=%d vol=%d",
                        map.sample_set,
                        banks[0], banks[1], banks[2], banks[3],
                        suffixes, files, vols);
            }
            int holds = 0;
            for (const auto& n : map.notes) if (n.is_hold) holds++;
            SDL_Log("  taps=%zu  holds=%d", map.notes.size() - holds, holds);
            if (!map.notes.empty()) {
                SDL_Log("  first=%.0f ms  last=%.0f ms", map.notes.front().time, map.notes.back().time);
                SDL_Log("  first note: col=%d  t=%.0f  hold=%d  end=%.0f",
                    map.notes.front().column, map.notes.front().time,
                    map.notes.front().is_hold ? 1 : 0, map.notes.front().end_time);
            }
        } else {
            SDL_Log("MAP: parse failed for %s", map_path);
        }
    }


    // Audio
    SDL_AudioSpec wav_spec = {};
    Uint8*  wav_data = nullptr;
    Uint32  wav_len  = 0;
    SDL_AudioStream* audio_stream = nullptr;
    const char* audio_path = "/sdcard/Android/data/sh.ppy.osuvinniv/files/audio.wav";
    if (load_audio_to_pcm(audio_path, &wav_spec, &wav_data, &wav_len)) {
        SDL_Log("AUDIO: loaded %d Hz, %d ch, %u bytes",
                wav_spec.freq, wav_spec.channels, wav_len);
        audio_stream = SDL_OpenAudioDeviceStream(
            SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &wav_spec, nullptr, nullptr);
        if (audio_stream) SDL_SetAudioStreamGain(audio_stream, cfg_music_vol);
        else SDL_Log("AUDIO: stream open failed: %s", SDL_GetError());
    } else {
        SDL_Log("AUDIO: load failed: %s", SDL_GetError());
    }

    // Hitsound stream
    SDL_AudioSpec hit_spec = {};
    Uint8*  hit_data = nullptr;
    Uint32  hit_len  = 0;
    SDL_AudioStream* hit_stream = nullptr;
    if (load_audio_to_pcm("/sdcard/Android/data/sh.ppy.osuvinniv/files/hit.ogg",
                    &hit_spec, &hit_data, &hit_len)) {
        SDL_Log("HIT: loaded %d Hz, %d ch, %u bytes",
                hit_spec.freq, hit_spec.channels, hit_len);
        hit_stream = SDL_OpenAudioDeviceStream(
            SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &hit_spec, nullptr, nullptr);
        if (hit_stream) {
            SDL_SetAudioStreamGain(hit_stream, cfg_hitsound_vol);
            SDL_ResumeAudioStreamDevice(hit_stream);
        }
        else SDL_Log("HIT: stream open failed: %s", SDL_GetError());
    } else {
        SDL_Log("HIT: load failed: %s", SDL_GetError());
    }

    SDL_AudioSpec miss_spec = {};
    Uint8*  miss_data = nullptr;
    Uint32  miss_len  = 0;
    SDL_AudioStream* miss_stream = nullptr;
    if (load_audio_to_pcm("/sdcard/Android/data/sh.ppy.osuvinniv/files/miss.ogg",
                    &miss_spec, &miss_data, &miss_len)) {
        SDL_Log("MISS: loaded %d Hz, %u bytes", miss_spec.freq, miss_len);
        miss_stream = SDL_OpenAudioDeviceStream(
            SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &miss_spec, nullptr, nullptr);
        if (miss_stream) {
            SDL_SetAudioStreamGain(miss_stream, cfg_hitsound_vol);
            SDL_ResumeAudioStreamDevice(miss_stream);
        }
    } else {
        SDL_Log("MISS: load failed: %s", SDL_GetError());
    }

    SDL_AudioStream* clap_stream    = nullptr;
    Uint8*           clap_data      = nullptr;
    Uint32           clap_len       = 0;
    SDL_AudioStream* whistle_stream = nullptr;
    Uint8*           whistle_data   = nullptr;
    Uint32           whistle_len    = 0;
    SDL_AudioStream* finish_stream  = nullptr;
    Uint8*           finish_data    = nullptr;
    Uint32           finish_len     = 0;
    {
        struct Extra { SDL_AudioStream** s; Uint8** d; Uint32* l; const char* name; };
        Extra extras[] = {
            { &clap_stream,    &clap_data,    &clap_len,    "clap" },
            { &whistle_stream, &whistle_data, &whistle_len, "whistle" },
            { &finish_stream,  &finish_data,  &finish_len,  "finish" },
        };
        for (auto& e : extras) {
            char path[128];
            SDL_snprintf(path, sizeof(path),
                "/sdcard/Android/data/sh.ppy.osuvinniv/files/%s.ogg", e.name);
            SDL_AudioSpec spec = {};
            if (load_audio_to_pcm(path, &spec, e.d, e.l)) {
                *e.s = SDL_OpenAudioDeviceStream(
                    SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
                if (*e.s) {
                    SDL_SetAudioStreamGain(*e.s, cfg_hitsound_vol);
                    SDL_ResumeAudioStreamDevice(*e.s);
                    SDL_Log("EXTRA: loaded %s", e.name);
                }
            }
        }
    }

    SDL_AudioSpec cd_spec = {};
    Uint8*  cd_data = nullptr;
    Uint32  cd_len  = 0;
    SDL_AudioStream* countdown_sfx_stream = nullptr;
    if (load_audio_to_pcm("/sdcard/Android/data/sh.ppy.osuvinniv/files/countdown.ogg",
                    &cd_spec, &cd_data, &cd_len)) {
        countdown_sfx_stream = SDL_OpenAudioDeviceStream(
            SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &cd_spec, nullptr, nullptr);
        if (countdown_sfx_stream) {
            SDL_SetAudioStreamGain(countdown_sfx_stream, cfg_hitsound_vol);
            SDL_ResumeAudioStreamDevice(countdown_sfx_stream);
        }
        SDL_Log("COUNTDOWN: loaded %d Hz, %u bytes", cd_spec.freq, cd_len);
    } else {
        SDL_Log("COUNTDOWN: load failed: %s", SDL_GetError());
    }

    Uint64 audio_start_tick = SDL_GetTicks() + 2000;  // 2s lead-in
    double audio_drift_ms   = 0.0;   // smoothed drift between wall clock and audio position
    bool audio_queued = false;

    SDL_GetWindowSizeInPixels(window, &win_w, &win_h);

    // Playfield geometry (recomputed when song changes)
    int   keys        = (map.key_count > 0) ? map.key_count : 4;
    float col_w       = 0.0f;
    float playfield_w = 0.0f;
    float playfield_x = 0.0f;
    const float max_col_w  = 240.0f;
    float scroll_pps = 1.5f;   // recomputed in compute_geometry()

    // ---- Timing-point accumulator (mirrors lazer SequentialScrollAlgorithm) ----
    // raw_positions[i] = sum_{j<i} (t[j+1] - t[j]) * multiplier[j].
    // rp(t) = raw_positions[i] + (t - t[i]) * multiplier[i], where i is the
    // last timing point with t[i] <= t. Pixel offset is
    //   (rp(note) - rp(now)) * judgement_y / time_range.
    std::vector<double> raw_positions;

    auto rebuild_timing_positions = [&]() {
        raw_positions.clear();
        const auto& tps = map.timing_points;
        if (tps.empty()) return;
        raw_positions.resize(tps.size());
        raw_positions[0] = 0.0;
        for (size_t i = 1; i < tps.size(); i++) {
            raw_positions[i] = raw_positions[i-1]
                             + (tps[i].time - tps[i-1].time) * tps[i-1].multiplier;
        }
    };

    auto raw_position_at = [&](double t) -> double {
        if (raw_positions.empty()) return t;
        const auto& tps = map.timing_points;
        int lo = 0, hi = (int)tps.size() - 1, best = 0;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (tps[mid].time <= t) { best = mid; lo = mid + 1; }
            else                    { hi = mid - 1; }
        }
        return raw_positions[best] + (t - tps[best].time) * tps[best].multiplier;
    };
    float judgement_y = 0.0f;

    auto compute_geometry = [&]() {
        keys        = (map.key_count > 0) ? map.key_count : 4;
        float cwr   = (float)win_w / keys;
        col_w       = (cwr > max_col_w) ? max_col_w : cwr;
        playfield_w = col_w * keys;
        playfield_x = ((float)win_w - playfield_w) * 0.5f;
        judgement_y = win_h * 0.85f;

        // osu scroll speed (1.0-40.0) -> pixels per millisecond
        double visible_height = (double)judgement_y;
        scroll_pps = (float)osu_speed_to_scroll_pps(cfg_scroll_speed, visible_height);
    };
    compute_geometry();

    // Judgment state
    std::vector<bool>   note_judged(map.notes.size(), false);
    std::vector<double> note_hit_time(map.notes.size(), -1.0);
    bool  column_pressed[16] = {false};
    int   combo = 0, max_combo = 0;
    int   hold_count = 0;   // taps + 3*holds for score formula
    int   cnt_perfect = 0, cnt_300 = 0, cnt_200 = 0;
    int   cnt_100 = 0, cnt_50 = 0, cnt_miss = 0;
    size_t miss_cursor = 0;
    std::vector<bool>   hold_active(map.notes.size(), false);
    std::vector<bool>   hold_started(map.notes.size(), false);
    std::vector<bool>   hold_broken(map.notes.size(), false);
    std::vector<bool>   note_ghost(map.notes.size(), false);
    std::vector<double> release_time(map.notes.size(), 0.0);
    std::vector<size_t> active_holds;

    // Multi-touch tracking
    struct FingerSlot { SDL_FingerID id; int col; };
    std::vector<FingerSlot> active_fingers;
    active_fingers.reserve(10);

    Uint64 last_miss_sound_tick = 0;   // miss-sound cooldown

    // Latest end time across all notes — what actually determines "song done".
    // notes.back() has the latest head time, not necessarily the latest tail.
    double last_note_end = 0.0;
    for (const auto& n : map.notes) {
        if (n.end_time > last_note_end) last_note_end = n.end_time;
    }

    // OD-scaled miss window (matches lazer's HitWindows.miss_window)
    double od_miss_window = std::floor(od_window(map.od, 188.0, 173.0, 158.0)) + 0.5;
    // Tail windows: lazer's DrawableHoldNoteTail applies RELEASE_WINDOW_LENIENCE
    // (1.5x) on top of the regular hit windows.
    double tw_p  = std::floor(od_window(map.od, 22.4, 19.4, 13.9) * 1.5) + 0.5;
    double tw_g  = std::floor(od_window(map.od,  64.0,  49.0,  34.0) * 1.5) + 0.5;
    double tw_gd = std::floor(od_window(map.od,  97.0,  82.0,  67.0) * 1.5) + 0.5;
    double tw_ok = std::floor(od_window(map.od, 127.0, 112.0,  97.0) * 1.5) + 0.5;

    auto play_miss_sound = [&]() {
        Uint64 now = SDL_GetTicks();
        // Cooldown so a burst of misses doesn't spam the blip.
        if (now - last_miss_sound_tick < 150) return;
        last_miss_sound_tick = now;
        if (miss_stream && miss_data)
            SDL_PutAudioStreamData(miss_stream, miss_data, (int)miss_len);
    };

    auto play_sound = [](SDL_AudioStream* s, Uint8* data, Uint32 len) {
        if (!s || !data) return;
        // Kill any queued samples so the newest press wins instead of
        // stacking into a backlog. Matches the "override" feel.
        SDL_ClearAudioStream(s);
        SDL_PutAudioStreamData(s, data, (int)len);
    };

    auto find_col = [&](float x) -> int {
        int c = (int)((x - playfield_x) / col_w);
        return (c >= 0 && c < keys && c < 16) ? c : -1;
    };


    // Per-map hitsound resolution.
    // The map's SampleSet (or a per-note override) picks the bank:
    //   1 = normal, 2 = soft, 3 = drum. Default = soft.
    // Format: <bank>-<addition><suffix>.wav, e.g. soft-hitclap.wav
    // Per-map hitsound bank (loaded when a song starts)
    MapSampleBank map_bank;

    // Bank resolver: sampleSet from the last timing point at or before t.
    // Matches lazer's SampleControlPoint lookup: each TP's sampleSet applies
    // from its own time until the next TP. Returns 1/2/3; 0 and out-of-range
    // fall back to [General] SampleSet, then to 2 (soft).
    auto bank_at_time = [&](double t) -> int {
        const auto& tps = map.timing_points;
        int best = -1;
        int lo = 0, hi = (int)tps.size() - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (tps[mid].time <= t) { best = mid; lo = mid + 1; }
            else                    { hi = mid - 1; }
        }
        int bank = (best >= 0) ? tps[best].sample_set : 0;
        if (bank == 0) bank = map.sample_set;
        if (bank == 0) bank = 2;
        return bank;
    };

    // Press sound. Mirrors lazer's GameplaySampleTriggerSource.Play():
    // find the earliest unjudged note in the column, or the last judged
    // one if the column is fully resolved, or nothing if the column is
    // empty. Bank resolves at the target note's time (not now_ms).
    auto play_press_sound = [&](int col) {
        int target_i = -1;
        for (size_t i = 0; i < map.notes.size(); i++) {
            if (map.notes[i].column != col) continue;
            if (!note_judged[i]) { target_i = (int)i; break; }
        }
        if (target_i < 0) {
            for (size_t i = map.notes.size(); i-- > 0; ) {
                if (map.notes[i].column == col) { target_i = (int)i; break; }
            }
        }
        if (target_i < 0) return;   // column has no notes at all
        int bank = map.notes[target_i].sample_bank;
        if (bank == 0) bank = bank_at_time(map.notes[target_i].time);
        const char* bank_name = "soft";
        if      (bank == 1) bank_name = "normal";
        else if (bank == 3) bank_name = "drum";
        char buf[64];
        SDL_snprintf(buf, sizeof(buf), "%s-hitnormal.wav", bank_name);
        if (!map_bank.play(buf, cfg_hitsound_vol, true))
            play_sound(hit_stream, hit_data, hit_len);
    };

    // Tail sound helper — resolves bank at a given time.
    auto play_tail_sound = [&](double end_time) {
        int bank = bank_at_time(end_time);
        const char* bank_name = "soft";
        if      (bank == 1) bank_name = "normal";
        else if (bank == 3) bank_name = "drum";
        char buf[64];
        SDL_snprintf(buf, sizeof(buf), "%s-hitnormal.wav", bank_name);
        if (!map_bank.play(buf, cfg_hitsound_vol, true))
            play_sound(hit_stream, hit_data, hit_len);
    };

    // Resolve and play a hitsound sample. Order matches lazer's
    // HitSampleInfo.LookupNames:
    //   1. bank-name<suffix>  (only when custom index >= 2)
    //   2. bank-name
    //   3. name               (bare-name fallback)
    // If filename is set on the note, that overrides everything.
    auto play_note_sample = [&](int bank, const char* addition,
                                 const std::string& suffix, int volume) -> bool {
        if (bank == 0) bank = (map.sample_set > 0 ? map.sample_set : 2);
        const char* bank_name = "soft";
        if      (bank == 1) bank_name = "normal";
        else if (bank == 3) bank_name = "drum";
        // Lazer only appends a suffix when customSampleBank >= 2.
        // Index 0 and 1 mean "use the un-suffixed filename".
        int idx = 0;
        if (!suffix.empty()) { try { idx = std::stoi(suffix); } catch (...) {} }
        char buf[160];
        float gain = volume > 0 ? (volume / 100.0f) * cfg_hitsound_vol : cfg_hitsound_vol;
        if (idx >= 2) {
            SDL_snprintf(buf, sizeof(buf), "%s-%s%d.wav", bank_name, addition, idx);
            if (map_bank.play(buf, gain, true)) return true;
        }
        SDL_snprintf(buf, sizeof(buf), "%s-%s.wav", bank_name, addition);
        if (map_bank.play(buf, gain, true)) return true;
        SDL_snprintf(buf, sizeof(buf), "%s.wav", addition);
        return map_bank.play(buf, gain, true);
    };

    auto judge_col = [&](int col, double t) -> bool {
        double map_t = t - (double)audio_start_tick;
        double od = map.od;
        double p_win  = std::floor(od_window(od, 22.4, 19.4, 13.9)) + 0.5;
        double g_win  = std::floor(od_window(od, 64, 49, 34)) + 0.5;
        double gd_win = std::floor(od_window(od, 97, 82, 67)) + 0.5;
        double ok_win = std::floor(od_window(od, 127, 112, 97)) + 0.5;
        double mh_win = std::floor(od_window(od, 151, 136, 121)) + 0.5;
        double ms_win = std::floor(od_window(od, 188, 173, 158)) + 0.5;

        // Find the EARLIEST unjudged note in this column. Do NOT skip
        // forward past a note that's outside the miss window — a press
        // when the earliest candidate is un-hittable is a no-op, matching
        // lazer's DrawableNote.IsHittable (|now - note.time| <= miss_win).
        // The earlier note gets auto-missed by the miss sweep below.
        int best_i = -1;
        for (size_t i = 0; i < map.notes.size(); i++) {
            if (note_judged[i]) continue;
            if (hold_started[i]) continue;
            if (map.notes[i].column != col) continue;
            best_i = (int)i;
            break;
        }
        if (best_i < 0) return false;
        double d = map.notes[best_i].time - map_t;
        double a = d < 0 ? -d : d;
        if (a > ms_win) return false;

        // Note lock — if the next unjudged note in the same column has
        // already started, this one is unhittable (matches lazer's
        // OrderedHitPolicy.IsHittable).
        for (size_t j = (size_t)best_i + 1; j < map.notes.size(); j++) {
            if (map.notes[j].column != col) continue;
            if (note_judged[j] || hold_started[j]) continue;
            if (map_t >= map.notes[j].time) return false;
            break;
        }

        // Layered extras. Lazer's ConvertHitObjectParser.convertSoundType:
        //   - if Filename is set, it replaces the hitnormal slot only,
        //     and additions still layer on top
        //   - additions use BankForAdditions (= additionBank if set,
        //     else the normal bank)
        {
            int bits = map.notes[best_i].hit_sound;
            // Resolve banks: explicit per-note value wins; else bank_at_time
            // at the note's own time. Mirrors lazer's SampleControlPoint
            // lookup when the note's bank field is null.
            int note_bank_norm = map.notes[best_i].sample_bank;
            if (note_bank_norm == 0) note_bank_norm = bank_at_time(map.notes[best_i].time);
            int note_bank_add  = map.notes[best_i].sample_bank_addition;
            if (note_bank_add == 0) note_bank_add = note_bank_norm;  // inherit
            const std::string& note_suffix = map.notes[best_i].sample_suffix;
            int note_vol = map.notes[best_i].sample_volume;
            const std::string& note_file = map.notes[best_i].sample_filename;

            // Filename override applies to hitnormal only.
            if (!note_file.empty()) {
                float gain = note_vol > 0 ? (note_vol / 100.0f) * cfg_hitsound_vol
                                          : cfg_hitsound_vol;
                if (!map_bank.play(note_file, gain, true))
                    play_sound(hit_stream, hit_data, hit_len);
            }

            if ((bits & 8) && !play_note_sample(note_bank_add, "hitclap",    note_suffix, note_vol))
                play_sound(clap_stream, clap_data, clap_len);
            if ((bits & 2) && !play_note_sample(note_bank_add, "hitwhistle", note_suffix, note_vol))
                play_sound(whistle_stream, whistle_data, whistle_len);
            if ((bits & 4) && !play_note_sample(note_bank_add, "hitfinish",  note_suffix, note_vol))
                play_sound(finish_stream, finish_data, finish_len);
        }

        if (map.notes[best_i].is_hold) {
            hold_started[best_i] = true;
            hold_active[best_i]  = true;
            active_holds.push_back(best_i);
        } else {
            note_judged[best_i] = true;
        }
        // force-miss earlier — any earlier unjudged note in the same column
        // that we didn't pick gets missed (matches lazer's HandleHit).
        for (size_t j = 0; j < (size_t)best_i; j++) {
            if (map.notes[j].column != col) continue;
            if (note_judged[j] || hold_started[j]) continue;
            note_judged[j] = true;
            note_ghost[j] = true;
            cnt_miss++;
            play_miss_sound();
        }

        if      (a <= p_win)  cnt_perfect++;
        else if (a <= g_win)  cnt_300++;
        else if (a <= gd_win) cnt_200++;
        else if (a <= ok_win) cnt_100++;
        else if (a <= mh_win) cnt_50++;
        else                  cnt_miss++;
        combo++;
        if (combo > max_combo) max_combo = combo;
        return true;
    };


    Uint64 last_report = SDL_GetTicks();
    int frames = 0;
    // ---- Song list + state machine ----
    AppState app_state = AppState::SongSelect;

    // Scan runs on a worker thread. Main thread shows "Scanning..." until
    // the worker completes; see scan.try_consume() inside the frame loop.
    std::vector<SongEntry> songs;
    SongScanState scan;
    scan.start("/sdcard/Android/data/sh.ppy.osuvinniv/files/songs",
               "/sdcard/Android/data/sh.ppy.osuvinniv/files/inbox");

    // Song-select list viewport. Single source of truth for render + touch
    // so geometry can never drift between the two.
    struct ListGeom { float x, y, w, h, row_h, footer_h; };
    auto get_list_geom = [&]() -> ListGeom {
        ListGeom g;
        g.row_h    = 90.0f;
        // Right half, 4% margin on the right. Left 40% is reserved for
        // a future local leaderboard panel.
        g.w        = win_w * 0.50f;
        g.x        = win_w - g.w - win_w * 0.04f;
        g.y        = win_h * 0.20f;
        g.footer_h = win_h * 0.15f;
        g.h        = win_h - g.y - g.footer_h;
        if (g.h < 100.0f) g.h = 100.0f;
        return g;
    };

    int   selected_index = 0;
    float song_scroll    = 0.0f;

    // Song select drag state
    SDL_FingerID song_drag_id      = (SDL_FingerID)-1;
    bool         settings_btn_candidate = false;
    float        settings_btn_start_x   = 0.0f;
    float        settings_btn_start_y   = 0.0f;
    float        song_drag_start_y = 0.0f;
    float        song_drag_start_scroll = 0.0f;
    bool         song_drag_moved   = false;

    // Settings state
    bool  settings_dirty = false;
    int   settings_row   = -1;      // which row is being dragged, -1 = none
    int   capture_nk     = 0;       // 0 = not capturing, else target key count
    std::vector<SDL_Keycode> capture_progress;
    int   settings_fps_preset = 0;  // 0=60, 1=120, 2=240, 3=0 (uncapped)
    // Background image cache for song select (declared BEFORE the loader lambda)
    SDL_Texture* song_bg_tex   = nullptr;
    std::string  song_bg_key;   // songs[i].bg_path currently loaded

    PreviewPlayer    preview;
    std::string      preview_key;   // folder|preview_ms of current preview

    // Per-map hitsound bank (loaded when a song starts)
    int          song_bg_w     = 0;
    int          song_bg_h     = 0;

    SDL_Log("SONGS: found %zu entries", songs.size());

    // Load the background image for a given song index.
    auto load_song_bg = [&](int idx) -> bool {
        if (song_bg_tex) { SDL_DestroyTexture(song_bg_tex); song_bg_tex = nullptr; }
        song_bg_w = song_bg_h = 0;
        if (idx < 0 || idx >= (int)songs.size()) return false;

        // Prefer the bg path resolved by the scanner (which used the
        // [Events] declaration), fall back to well-known names.
        std::string found = songs[idx].bg_path;
        if (found.empty()) {
            const char* names[] = {
                "/bg.jpg", "/BG.jpg", "/Bg.jpg", "/bG.jpg",
                "/bg.png", "/BG.png", "/Bg.png", "/bG.png",
                "/bg.jpeg", "/BG.jpeg", "/Bg.jpeg",
                "/background.jpg", "/background.png"
            };
            struct stat st;
            for (const char* n : names) {
                std::string c = songs[idx].folder + n;
                if (stat(c.c_str(), &st) == 0) { found = c; break; }
            }
        }
        if (found.empty()) return false;

        int iw = 0, ih = 0;
        uint8_t* px = nullptr;
        if (!load_image_rgba(found.c_str(), &iw, &ih, &px)) return false;

        song_bg_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                         SDL_TEXTUREACCESS_STATIC, iw, ih);
        if (song_bg_tex) {
            SDL_UpdateTexture(song_bg_tex, NULL, px, iw * 4);
            SDL_SetTextureBlendMode(song_bg_tex, SDL_BLENDMODE_BLEND);
            song_bg_w = iw;
            song_bg_h = ih;
            SDL_Log("BG: loaded %dx%d from %s", iw, ih, found.c_str());
        }
        free(px);
        return song_bg_tex != nullptr;
    };



    int report_count = 0;
    bool   running  = true;
    bool   paused   = false;
    bool   resuming = false;
    bool   result_shown = false;
    Uint64 pause_started_tick = 0;
    Uint64 resume_start_tick  = 0;
    const Uint64 RESUME_DELAY_MS = 3000;




    // ---- HUD texture (3x scale, cached) ----
    const int   HUD_LOGICAL_W = 520;
    const int   HUD_LOGICAL_H = 72;
    const float HUD_SCALE     = 3.0f;
    const int   HUD_TEX_W     = (int)(HUD_LOGICAL_W * HUD_SCALE);
    const int   HUD_TEX_H     = (int)(HUD_LOGICAL_H * HUD_SCALE);

    SDL_Texture* hud_tex = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_TARGET, HUD_TEX_W, HUD_TEX_H);
    if (hud_tex) SDL_SetTextureBlendMode(hud_tex, SDL_BLENDMODE_BLEND);
    else SDL_Log("HUD: texture creation failed: %s", SDL_GetError());

    int  hud_fps = 0;
    int  hud_last_combo = -1, hud_last_max = -1;
    int  hud_last_300 = -1, hud_last_100 = -1, hud_last_50 = -1, hud_last_miss = -1;
    int  hud_last_fps = -1;
    bool hud_dirty = true;

    // Pause menu button geometry
    float menu_btn_x = 0.0f;
    float menu_btn_w = 0.0f;
    float menu_btn_h = 0.0f;
    float menu_btn_y[3] = { 0.0f, 0.0f, 0.0f };


    while (running) {
        if (scan.try_consume(songs)) {
            selected_index = 0;
            song_scroll = 0.0f;
            preview_key.clear();
            song_bg_key.clear();
            SDL_Log("SCAN: consumed %zu entries", songs.size());
            for (int _i = 0; _i < 5 && _i < (int)songs.size(); _i++)
                SDL_Log("SONG[%d]: artist='%s' title='%s' ver='%s'",
                        _i, songs[_i].artist.c_str(),
                        songs[_i].title.c_str(), songs[_i].version.c_str());
        }

        Uint64 frame_start = SDL_GetTicksNS() / 1000ULL;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            // Snapshot the pause state at event entry, since menu handlers
            // can flip it mid-event.
            bool was_paused = paused;
            if (e.type == SDL_EVENT_QUIT ||
                e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                running = false;
            }

            // ---- key capture (settings) — consumes all key events ----
            if (app_state == AppState::Settings && capture_nk > 0) {
                if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                    if (e.key.key == SDLK_ESCAPE || e.key.key == SDLK_AC_BACK) {
                        capture_nk = 0;
                        capture_progress.clear();
                        SDL_Log("SETTINGS: capture canceled");
                    } else {
                        bool dup = false;
                        for (auto k : capture_progress) if (k == e.key.key) { dup = true; break; }
                        if (!dup) {
                            capture_progress.push_back(e.key.key);
                            SDL_Log("SETTINGS: capture %dK [%zu/%d] %s",
                                    capture_nk, capture_progress.size(), capture_nk,
                                    SDL_GetKeyName(e.key.key));
                            if ((int)capture_progress.size() == capture_nk) {
                                bindings[capture_nk] = capture_progress;
                                save_settings();
                                capture_nk = 0;
                                capture_progress.clear();
                            }
                        }
                    }
                }
                continue;
            }
            // BACK -> pause toggle (was quit; quit is in the menu now)
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                e.key.key == SDLK_AC_BACK) {
                if (app_state == AppState::Settings) {
                    app_state = AppState::SongSelect;
                    SDL_Log("SETTINGS: back to song select");
                } else if (app_state == AppState::SongSelect) {
                    running = false;   // exit from song select
                } else if (app_state != AppState::Playing) {
                    // no-op
                } else if (resuming) {
                    resuming = false;
                    paused = true;
                    SDL_Log("PAUSE: canceled countdown (back)");
                } else if (!paused) {
                    paused = true;
                    pause_started_tick = SDL_GetTicks();
                    if (audio_stream) SDL_PauseAudioStreamDevice(audio_stream);
                    for (int i = 0; i < 16; i++) column_pressed[i] = false;
                    active_fingers.clear();
                    SDL_Log("PAUSE: on (back)");
                } else {
                    paused = false;
                    resuming = true;
                    resume_start_tick = SDL_GetTicks();
                    SDL_Log("RESUME: countdown 3s (back)");
                }
            }

            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                e.key.key == SDLK_ESCAPE && result_shown) {
                result_shown = false;
                app_state = AppState::SongSelect;
                SDL_Log("RESULT: back-to-select");
            }
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                (e.key.key == SDLK_R || e.key.key == SDLK_GRAVE) && result_shown) {
                // Retry from result
                std::fill(note_judged.begin(),  note_judged.end(),  false);
                std::fill(note_hit_time.begin(), note_hit_time.end(), -1.0);
                std::fill(hold_active.begin(),  hold_active.end(),  false);
                std::fill(hold_started.begin(), hold_started.end(), false);
                std::fill(hold_broken.begin(),  hold_broken.end(),  false);
                std::fill(note_ghost.begin(),   note_ghost.end(),   false);
                std::fill(release_time.begin(), release_time.end(), 0.0);
                for (int j = 0; j < 16; j++) column_pressed[j] = false;
                active_holds.clear();
                active_fingers.clear();
                combo = 0; max_combo = 0;
                cnt_perfect = 0; cnt_300 = 0; cnt_200 = 0;
                cnt_100 = 0; cnt_50 = 0; cnt_miss = 0;
                miss_cursor = 0;
                if (audio_stream) {
                    SDL_ClearAudioStream(audio_stream);
                    SDL_PauseAudioStreamDevice(audio_stream);
                }
                audio_queued = false;
                audio_start_tick = SDL_GetTicks() + 2000;
                audio_drift_ms = 0.0;
                result_shown = false;
                SDL_Log("RESULT: retry");
            }
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                e.key.key == SDLK_ESCAPE) {
                if (app_state == AppState::Settings) {
                    app_state = AppState::SongSelect;
                    SDL_Log("SETTINGS: esc to song select");
                } else if (app_state != AppState::Playing) {
                    // no pause from song select
                } else if (resuming) {
                    resuming = false;
                    paused = true;
                    SDL_Log("PAUSE: canceled countdown");
                } else if (!paused) {
                    paused = true;
                    pause_started_tick = SDL_GetTicks();
                    if (audio_stream) SDL_PauseAudioStreamDevice(audio_stream);
                    for (int i = 0; i < 16; i++) column_pressed[i] = false;
                    active_fingers.clear();
                    SDL_Log("PAUSE: on");
                } else {
                    paused = false;
                    resuming = true;
                    resume_start_tick = SDL_GetTicks();
                    SDL_Log("RESUME: countdown 3s");
                }
            }
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                (e.key.key == SDLK_R || e.key.key == SDLK_GRAVE)) {
                std::fill(note_judged.begin(),  note_judged.end(),  false);
                std::fill(note_hit_time.begin(), note_hit_time.end(), -1.0);
                std::fill(hold_active.begin(),  hold_active.end(),  false);
                std::fill(hold_started.begin(), hold_started.end(), false);
                std::fill(hold_broken.begin(),  hold_broken.end(),  false);
                std::fill(note_ghost.begin(),   note_ghost.end(),   false);
                std::fill(release_time.begin(), release_time.end(), 0.0);
                for (int i = 0; i < 16; i++) column_pressed[i] = false;
                active_holds.clear();
                active_fingers.clear();
                combo = 0; max_combo = 0;
                cnt_perfect = 0; cnt_300 = 0; cnt_200 = 0;
                cnt_100 = 0; cnt_50 = 0; cnt_miss = 0;
                miss_cursor = 0;
                if (audio_stream) {
                    SDL_ClearAudioStream(audio_stream);
                    SDL_PauseAudioStreamDevice(audio_stream);
                }
                if (hit_stream)  SDL_ClearAudioStream(hit_stream);
                if (miss_stream) SDL_ClearAudioStream(miss_stream);
                audio_queued = false;
                audio_start_tick = SDL_GetTicks() + 2000;
                audio_drift_ms = 0.0;
                SDL_Log("RESTART: reset to t=0");
            }

            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && !paused && !resuming && !result_shown && app_state == AppState::Playing) {
                int col = -1;
                if (keys >= 1 && keys < 16) {
                    const auto& b = bindings[keys];
                    for (size_t i = 0; i < b.size(); i++) {
                        if (b[i] == e.key.key) { col = (int)i; break; }
                    }
                }
                if (col >= 0 && col < keys) {
                    column_pressed[col] = true;
                    play_press_sound(col);
                    judge_col(col, (double)SDL_GetTicks());
                }
            }
            // Song select — settings button works even while scanning
            // (songs empty). Short-circuit before the list-drag guard.
            if (app_state == AppState::SongSelect && e.type == SDL_EVENT_FINGER_DOWN) {
                float bstx = e.tfinger.x * win_w;
                float bsty = e.tfinger.y * win_h;
                if (bstx >= 40.0f && bstx <= 280.0f &&
                    bsty >= 140.0f && bsty <= 220.0f) {
                    app_state = AppState::Settings;
                    SDL_Log("SETTINGS: open (pre-scan)");
                    continue;
                }
                // Random button (below settings)
                if (bstx >= 40.0f && bstx <= 280.0f &&
                    bsty >= 240.0f && bsty <= 320.0f) {
                    if (!songs.empty()) {
                        static bool rng_seeded = false;
                        if (!rng_seeded) { srand((unsigned)SDL_GetTicks()); rng_seeded = true; }
                        selected_index = (int)(rand() % (int)songs.size());
                        ListGeom lg_r = get_list_geom();
                        float row_top = selected_index * lg_r.row_h;
                        float row_bot = row_top + lg_r.row_h;
                        if (row_top < song_scroll) song_scroll = row_top;
                        else if (row_bot > song_scroll + lg_r.h) song_scroll = row_bot - lg_r.h;
                        if (song_scroll < 0.0f) song_scroll = 0.0f;
                        float total_h = (float)songs.size() * lg_r.row_h;
                        float max_s = total_h > lg_r.h ? total_h - lg_r.h : 0.0f;
                        if (song_scroll > max_s) song_scroll = max_s;
                        SDL_Log("SONGS: random -> %d", selected_index);
                    }
                    continue;
                }
            }
            // SONG SELECT DRAG / TAP
            if (app_state == AppState::SongSelect && !songs.empty()) {
                ListGeom lg = get_list_geom();
                const float list_x = lg.x;
                const float list_y = lg.y;
                const float list_w = lg.w;
                const float row_h  = lg.row_h;
                const float list_h = lg.h;
                const float total_h = (float)songs.size() * row_h;
                const float max_scroll = total_h > list_h ? total_h - list_h : 0.0f;

                if (e.type == SDL_EVENT_FINGER_DOWN) {
                    float tx = e.tfinger.x * (float)win_w;
                    float ty = e.tfinger.y * (float)win_h;

                    // Settings button — mark candidate, confirm on FINGER_UP
                    const float sbx = 40.0f;
                    const float sby = 140.0f;
                    const float sbw = 240.0f;
                    const float sbh = 80.0f;
                    if (tx >= sbx && tx <= sbx + sbw &&
                        ty >= sby && ty <= sby + sbh) {
                        settings_btn_candidate = true;
                        settings_btn_start_x   = tx;
                        settings_btn_start_y   = ty;
                        song_drag_id           = e.tfinger.fingerID;
                        song_drag_start_y      = ty;
                        song_drag_start_scroll = song_scroll;
                        song_drag_moved        = false;
                        continue;
                    }

                    settings_btn_candidate = false;
                    song_drag_id           = e.tfinger.fingerID;
                    song_drag_start_y      = ty;
                    song_drag_start_scroll = song_scroll;
                    song_drag_moved        = false;
                    continue;
                }

                if (e.type == SDL_EVENT_FINGER_MOTION &&
                    e.tfinger.fingerID == song_drag_id) {
                    float tx = e.tfinger.x * (float)win_w;
                    float ty = e.tfinger.y * (float)win_h;
                    if (settings_btn_candidate) {
                        float dx = tx - settings_btn_start_x;
                        float dy = ty - settings_btn_start_y;
                        if (dx * dx + dy * dy > 400.0f) {
                            settings_btn_candidate = false;
                        }
                    }
                    float delta = ty - song_drag_start_y;
                    if (delta < -20.0f || delta > 20.0f) song_drag_moved = true;
                    float new_scroll = song_drag_start_scroll - delta;
                    if (new_scroll < 0.0f) new_scroll = 0.0f;
                    if (new_scroll > max_scroll) new_scroll = max_scroll;
                    song_scroll = new_scroll;
                    continue;
                }

                if (e.type == SDL_EVENT_FINGER_UP &&
                    e.tfinger.fingerID == song_drag_id) {
                    if (settings_btn_candidate && !song_drag_moved) {
                        app_state = AppState::Settings;
                        settings_btn_candidate = false;
                        song_drag_id = (SDL_FingerID)-1;
                        continue;
                    }
                    settings_btn_candidate = false;
                    if (!song_drag_moved) {
                        float tx = e.tfinger.x * (float)win_w;
                        float ty = e.tfinger.y * (float)win_h;
                        // Reject taps outside the list viewport (both axes).
                        if (ty < list_y || ty > list_y + list_h) continue;
                        if (tx < list_x || tx > list_x + list_w) continue;
                        int hit = (int)((ty - list_y + song_scroll) / row_h);
                        if (hit >= 0 && hit < (int)songs.size()) {
                            // Tap UX: first tap selects (and previews bg),
                            // second tap on the same row plays.
                            if (hit != selected_index) {
                                selected_index = hit;
                                SDL_Log("SONGS: preview %d", hit);
                                continue;
                            }

                            // second tap plays
                            const auto& s = songs[hit];
                            SDL_Log("SONGS: loading %s [%s]",
                                    s.title.c_str(), s.version.c_str());

                            // Reset gameplay state
                            std::fill(note_judged.begin(),  note_judged.end(),  false);
                            std::fill(note_hit_time.begin(), note_hit_time.end(), -1.0);
                            std::fill(hold_active.begin(),  hold_active.end(),  false);
                            std::fill(hold_started.begin(), hold_started.end(), false);
                            std::fill(hold_broken.begin(),  hold_broken.end(),  false);
                            std::fill(note_ghost.begin(),   note_ghost.end(),   false);
                            std::fill(release_time.begin(), release_time.end(), 0.0);
                            for (int j = 0; j < 16; j++) column_pressed[j] = false;
                            active_holds.clear();
                            active_fingers.clear();
                            combo = 0; max_combo = 0;
                            hold_count = 0;
                            cnt_perfect = 0; cnt_300 = 0; cnt_200 = 0;
                            cnt_100 = 0; cnt_50 = 0; cnt_miss = 0;
                            miss_cursor = 0;
                            paused = false; resuming = false; result_shown = false;

                            Beatmap new_map;
                            if (!parse_osu_file(s.osu_path, new_map)) {
                                SDL_Log("SONGS: parse failed");
                            } else {
                                map = new_map;
                                rebuild_timing_positions();

                                last_note_end = 0.0;
                                hold_count = 0;
                                for (const auto& n : map.notes) {
                                    if (n.end_time > last_note_end) last_note_end = n.end_time;
                                    if (n.is_hold) hold_count++;
                                }
                                note_judged.assign(map.notes.size(),  false);
                                note_hit_time.assign(map.notes.size(), -1.0);
                                hold_active.assign(map.notes.size(),  false);
                                hold_started.assign(map.notes.size(), false);
                                hold_broken.assign(map.notes.size(),  false);
                                note_ghost.assign(map.notes.size(),   false);
                                release_time.assign(map.notes.size(), 0.0);

                                if (audio_stream) { SDL_DestroyAudioStream(audio_stream); audio_stream = nullptr; }
                                if (wav_data)     { SDL_free(wav_data); wav_data = nullptr; }
                                SDL_AudioSpec spec = {};
                                // Load per-map hitsounds from the song folder
                                map_bank.load_from_folder(s.folder);
                                SDL_Log("HITS: bank loaded (soft-hitnormal: %s) from %s",
                                        map_bank.has("soft-hitnormal.wav") ? "yes" : "no",
                                        s.folder.c_str());

                                if (load_audio_to_pcm(s.audio_path.c_str(), &spec, &wav_data, &wav_len)) {
                                    audio_stream = SDL_OpenAudioDeviceStream(
                                        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
                                    if (audio_stream)
                                        SDL_SetAudioStreamGain(audio_stream, cfg_music_vol);
                                    SDL_Log("SONGS: audio %s loaded", s.audio_path.c_str());
                                } else {
                                    SDL_Log("SONGS: audio load failed: %s", SDL_GetError());
                                }

                                audio_queued = false;
                                audio_start_tick = SDL_GetTicks() + 2000;
                audio_drift_ms = 0.0;
                                compute_geometry();
                                app_state = AppState::Playing;
                                SDL_Log("SONGS: entering Playing (%dK, %zu notes)",
                                        map.key_count, map.notes.size());
                            }
                        }
                    }
                    song_drag_id = (SDL_FingerID)-1;
                    continue;
                }
            }
            // SETTINGS TOUCH
            if (app_state == AppState::Settings) {
                float stx = 0.0f, sty = 0.0f;
                bool has_xy = (e.type == SDL_EVENT_FINGER_DOWN ||
                               e.type == SDL_EVENT_FINGER_MOTION ||
                               e.type == SDL_EVENT_FINGER_UP);
                if (has_xy) {
                    stx = e.tfinger.x * (float)win_w;
                    sty = e.tfinger.y * (float)win_h;
                }

                if (e.type == SDL_EVENT_FINGER_DOWN) {
                    // Back button (bottom-left)
                    if (stx < 400.0f && sty > (float)win_h - 120.0f) {
                        capture_nk = 0;
                        capture_progress.clear();
                        app_state = AppState::SongSelect;
                        SDL_Log("SETTINGS: back button");
                        continue;
                    }

                    const float row_x = settings_row_x;
                    const float row_w = (float)win_w - 2.0f * settings_row_x;
                    for (int i = 0; i < ROW_COUNT; i++) {
                        float ry = settings_row_y0 + i * (settings_row_h + settings_row_gap);
                        if (stx >= row_x && stx <= row_x + row_w &&
                            sty >= ry && sty <= ry + settings_row_h) {
                            if (setting_rows[i].type == 2) {
                                capture_nk = setting_rows[i].key_count;
                                capture_progress.clear();
                                settings_row = -1;
                                SDL_Log("SETTINGS: capture %dK", capture_nk);
                            } else {
                                settings_row = i;
                                SDL_Log("SETTINGS: row %d selected", i);
                            }
                            break;
                        }
                    }
                    continue;
                }

                if (e.type == SDL_EVENT_FINGER_MOTION && settings_row >= 0) {
                    const float row_x = settings_row_x;
                    const float row_w = (float)win_w - 2.0f * settings_row_x;
                    float norm = (stx - row_x) / row_w;
                    if (norm < 0.0f) norm = 0.0f;
                    if (norm > 1.0f) norm = 1.0f;

                    auto& row = setting_rows[settings_row];
                    if (row.type == 0) {
                        float* t = (float*)row.target;
                        *t = row.min_val + norm * (row.max_val - row.min_val);
                    } else if (row.type == 1) {
                        int idx = (int)(norm * row.cycle_count);
                        if (idx >= row.cycle_count) idx = row.cycle_count - 1;
                        *((int*)row.target) = row.cycle_values[idx];
                    }
                    settings_dirty = true;
                    continue;
                }

                if (e.type == SDL_EVENT_FINGER_UP) {
                    if (settings_row >= 0) save_settings();
                    settings_row = -1;
                    continue;
                }

                // Any other event while in settings is consumed
                continue;
            }
            if (e.type == SDL_EVENT_FINGER_DOWN && paused) {
                // Pause menu hit-test. Buttons laid out in draw block.
                float tx = e.tfinger.x * (float)win_w;
                float ty = e.tfinger.y * (float)win_h;
                for (int i = 0; i < 3; i++) {
                    if (tx >= menu_btn_x && tx <= menu_btn_x + menu_btn_w &&
                        ty >= menu_btn_y[i] && ty <= menu_btn_y[i] + menu_btn_h) {
                        if (i == 0) {
                            // Continue — start countdown; actual resume happens when it ends
                            paused = false;
                            resuming = true;
                            resume_start_tick = SDL_GetTicks();
                            SDL_Log("RESUME: countdown 3s (continue)");
                        } else if (i == 1) {
                            // Retry — same as pressing R
                            std::fill(note_judged.begin(),  note_judged.end(),  false);
                            std::fill(note_hit_time.begin(), note_hit_time.end(), -1.0);
                            std::fill(hold_active.begin(),  hold_active.end(),  false);
                            std::fill(hold_started.begin(), hold_started.end(), false);
                            std::fill(hold_broken.begin(),  hold_broken.end(),  false);
                            std::fill(note_ghost.begin(),   note_ghost.end(),   false);
                            std::fill(release_time.begin(), release_time.end(), 0.0);
                            for (int j = 0; j < 16; j++) column_pressed[j] = false;
                            active_holds.clear();
                            active_fingers.clear();
                            combo = 0; max_combo = 0;
                            cnt_perfect = 0; cnt_300 = 0; cnt_200 = 0;
                            cnt_100 = 0; cnt_50 = 0; cnt_miss = 0;
                            miss_cursor = 0;
                            if (audio_stream) {
                                SDL_ClearAudioStream(audio_stream);
                                SDL_PauseAudioStreamDevice(audio_stream);
                            }
                            if (hit_stream)  SDL_ClearAudioStream(hit_stream);
                            if (miss_stream) SDL_ClearAudioStream(miss_stream);
                            audio_queued = false;
                            audio_start_tick = SDL_GetTicks() + 2000;
                audio_drift_ms = 0.0;
                            paused = false;
                            SDL_Log("PAUSE: retry");
                        } else {
                            // Return to song select
                            if (audio_stream) {
                                SDL_ClearAudioStream(audio_stream);
                                SDL_PauseAudioStreamDevice(audio_stream);
                            }
                            audio_queued = false;
                            paused = false;
                            resuming = false;
                            result_shown = false;
                            for (int j = 0; j < 16; j++) column_pressed[j] = false;
                            active_fingers.clear();
                            active_holds.clear();
                            app_state = AppState::SongSelect;
                            SDL_Log("PAUSE: return-to-select");
                        }
                        break;
                    }
                }
            }
            if (e.type == SDL_EVENT_FINGER_DOWN && was_paused) {
                continue;
            }
            if (e.type == SDL_EVENT_FINGER_DOWN && !paused && !resuming && !result_shown && app_state == AppState::Playing) {
                float tx = e.tfinger.x * (float)win_w;
                int col = find_col(tx);
                if (col >= 0) {
                    // Replace existing entry for this finger
                    for (auto& f : active_fingers) {
                        if (f.id == e.tfinger.fingerID) { f.col = col; goto finger_done; }
                    }
                    active_fingers.push_back({ e.tfinger.fingerID, col });
                finger_done:
                    column_pressed[col] = true;
                    play_press_sound(col);
                    judge_col(col, (double)SDL_GetTicks());
                }
            }
            if (e.type == SDL_EVENT_FINGER_DOWN && result_shown) {
                // Tap anywhere → retry
                std::fill(note_judged.begin(),  note_judged.end(),  false);
                std::fill(note_hit_time.begin(), note_hit_time.end(), -1.0);
                std::fill(hold_active.begin(),  hold_active.end(),  false);
                std::fill(hold_started.begin(), hold_started.end(), false);
                std::fill(hold_broken.begin(),  hold_broken.end(),  false);
                std::fill(note_ghost.begin(),   note_ghost.end(),   false);
                std::fill(release_time.begin(), release_time.end(), 0.0);
                for (int j = 0; j < 16; j++) column_pressed[j] = false;
                active_holds.clear();
                active_fingers.clear();
                combo = 0; max_combo = 0;
                cnt_perfect = 0; cnt_300 = 0; cnt_200 = 0;
                cnt_100 = 0; cnt_50 = 0; cnt_miss = 0;
                miss_cursor = 0;
                if (audio_stream) {
                    SDL_ClearAudioStream(audio_stream);
                    SDL_PauseAudioStreamDevice(audio_stream);
                }
                audio_queued = false;
                audio_start_tick = SDL_GetTicks() + 2000;
                audio_drift_ms = 0.0;
                result_shown = false;
                SDL_Log("RESULT: tap-retry");
                continue;
            }
            if ((e.type == SDL_EVENT_FINGER_UP || e.type == SDL_EVENT_FINGER_CANCELED) && !paused && !resuming && app_state == AppState::Playing) {
                int release_col = -1;
                for (size_t i = 0; i < active_fingers.size(); i++) {
                    if (active_fingers[i].id == e.tfinger.fingerID) {
                        release_col = active_fingers[i].col;
                        column_pressed[release_col] = false;
                        active_fingers.erase(active_fingers.begin() + i);
                        break;
                    }
                }
                if (release_col >= 0) {
                    double now_map = (double)SDL_GetTicks() - (double)audio_start_tick;
                    // Tail window scaled by 1.5x, matching TailNote.RELEASE_WINDOW_LENIENCE.
                    double tail_win = std::floor(
                        od_window(map.od, 188.0, 173.0, 158.0)) * 1.5 + 0.5;
                    for (size_t i = 0; i < active_holds.size(); ) {
                        size_t hi = active_holds[i];
                        if (map.notes[hi].column != release_col) { i++; continue; }
                        release_time[hi] = now_map;
                        if (now_map < map.notes[hi].end_time - tail_win) {
                            // Released early — break
                            hold_active[hi]  = false;
                            hold_broken[hi]  = true;
                            note_ghost[hi]   = true;
                            note_judged[hi]  = true;
                            // Break: tail resolves as Meh if head hit, else Miss.
                            // (Lazer's GetCappedResult caps the tail at Meh on
                            // break; the head was already scored at press.)
                            if (!hold_started[hi]) {
                                cnt_miss++;
                            } else {
                                cnt_50++;
                            }
                            combo = 0;
                            play_miss_sound();
                        } else {
                            // Released in window — tail tier from
                            // |release - end_time| against 1.5x windows.
                            // Head already scored at press.
                            hold_active[hi]  = false;
                            note_judged[hi]  = true;
                            // Tail sample on release. Bank resolves at end_time.
                            play_tail_sound(map.notes[hi].end_time);
                            double d = now_map - map.notes[hi].end_time;
                            double ta = d < 0 ? -d : d;
                            if      (ta <= tw_p)  cnt_perfect++;
                            else if (ta <= tw_g)  cnt_300++;
                            else if (ta <= tw_gd) cnt_200++;
                            else if (ta <= tw_ok) cnt_100++;
                            else                  cnt_50++;
                            combo++;
                            if (combo > max_combo) max_combo = combo;
                        }
                        active_holds.erase(active_holds.begin() + i);
                    }
                }
            }
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !paused && !resuming && !result_shown && app_state == AppState::Playing) {
                int col = find_col(e.button.x);
                if (col >= 0) {
                    column_pressed[col] = true;
                    play_press_sound(col);
                    judge_col(col, (double)SDL_GetTicks());
                }
            }
            if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && !paused && !resuming && app_state == AppState::Playing) {
                int col = find_col(e.button.x);
                if (col >= 0) column_pressed[col] = false;
            }
            if (e.type == SDL_EVENT_KEY_UP) {
                for (int nk = 1; nk < 16; nk++) {
                    const auto& b = bindings[nk];
                    for (size_t i = 0; i < b.size() && i < 16; i++) {
                        if (b[i] == e.key.key) column_pressed[i] = false;
                    }
                }
            }
        }

        SDL_GetWindowSizeInPixels(window, &win_w, &win_h);

        // Ensure the correct song bg is loaded while in song select.
        // Keyed on folder so scrolling between difficulties doesn't reload.
        if (app_state == AppState::SongSelect &&
            !songs.empty() &&
            songs[selected_index].bg_path != song_bg_key) {
            // Keyed on bg_path, not folder. Two difficulties in the same
            // set can have different bg images (mappers do this).
            if (load_song_bg(selected_index))
                song_bg_key = songs[selected_index].bg_path;
            else
                song_bg_key.clear();
        }
        // Note: we deliberately DON'T free the bg when leaving song select —
        // gameplay uses the same texture. It gets replaced on the next song
        // selection change or freed at app exit.

        // ---- Song preview --------------------------------------------
        if (app_state == AppState::SongSelect && !songs.empty()) {
            const auto& sel = songs[selected_index];
            std::string want_key = sel.folder + "|" + std::to_string(sel.preview_time);
            if (want_key != preview_key) {
                preview.request(sel.audio_path, sel.preview_time, cfg_music_vol);
                preview_key = want_key;
            }
        } else if (!preview_key.empty()) {
            preview.stop();
            preview_key.clear();
        }
        // --------------------------------------------------------------

        const Uint64 current_tick = SDL_GetTicks();
        if (!paused && !resuming && !audio_queued && audio_stream &&
            app_state == AppState::Playing &&
            current_tick >= audio_start_tick) {
            if (SDL_PutAudioStreamData(audio_stream, wav_data, wav_len)) {
                SDL_ResumeAudioStreamDevice(audio_stream);
                audio_queued = true;
                SDL_Log("AUDIO: playback started");
            }
        }
        // Signed: negative during lead-in, positive during play
        // Compute current map time. Once audio is playing, derive from
        // audio playback position, which corrects AAudio startup latency
        // and drift. Falls back to wall clock during lead-in.
        double now_ms = (double)current_tick - (double)audio_start_tick;
        if (audio_queued && audio_stream && wav_len > 0) {
            int queued = SDL_GetAudioStreamQueued(audio_stream);
            if (queued >= 0) {
                long long played = (long long)wav_len - (long long)queued;
                if (played < 0) played = 0;
                int bytes_per_sec = wav_spec.freq * wav_spec.channels *
                                    (SDL_AUDIO_BITSIZE(wav_spec.format) / 8);
                if (bytes_per_sec > 0) {
                    double audio_ms = (double)played / bytes_per_sec * 1000.0;
                    // blend toward audio to smooth jitter
                    double target_drift = audio_ms - now_ms;
                    audio_drift_ms = audio_drift_ms * 0.9 + target_drift * 0.1;
                }
            }
        }
        now_ms += audio_drift_ms + cfg_audio_offset_ms;
        if (paused || resuming)
            now_ms = (double)pause_started_tick - (double)audio_start_tick;

        // Reapply audio gains if sliders moved
        if (settings_dirty) {
            settings_dirty = false;
            if (audio_stream)         SDL_SetAudioStreamGain(audio_stream, cfg_music_vol);
            if (hit_stream)           SDL_SetAudioStreamGain(hit_stream, cfg_hitsound_vol);
            if (miss_stream)          SDL_SetAudioStreamGain(miss_stream, cfg_hitsound_vol);
            if (clap_stream)          SDL_SetAudioStreamGain(clap_stream, cfg_hitsound_vol);
            if (whistle_stream)       SDL_SetAudioStreamGain(whistle_stream, cfg_hitsound_vol);
            if (finish_stream)        SDL_SetAudioStreamGain(finish_stream, cfg_hitsound_vol);
            if (countdown_sfx_stream) SDL_SetAudioStreamGain(countdown_sfx_stream, cfg_hitsound_vol);
        }

        // Result screen trigger: all notes done + 2 seconds grace
        if (app_state == AppState::Playing &&
            !result_shown && !paused && !resuming &&
            !map.notes.empty() &&
            now_ms > last_note_end + 2000.0)
        {
            result_shown = true;
            if (audio_stream) SDL_PauseAudioStreamDevice(audio_stream);
            SDL_Log("RESULT: entered");
        }

        // Countdown finishing → actually resume
        if (resuming) {
            if (SDL_GetTicks() - resume_start_tick >= RESUME_DELAY_MS) {
                audio_start_tick += (SDL_GetTicks() - pause_started_tick);
                if (audio_stream && audio_queued)
                    SDL_ResumeAudioStreamDevice(audio_stream);
                resuming = false;
                SDL_Log("RESUME: go");
            }
        }

        // Background
        // Complete any holds whose tail has passed
        if (!paused) {
            double nmap = (double)SDL_GetTicks() - (double)audio_start_tick;
            for (size_t i = 0; app_state == AppState::Playing && i < active_holds.size(); ) {
                size_t hi = active_holds[i];
                if (nmap > map.notes[hi].end_time) {
                    hold_active[hi]  = false;
                    note_judged[hi]  = true;
                    release_time[hi] = nmap;
                    // Held through: tail resolves as Perfect. Head was
                    // already scored at press. Lazer scores 2 per hold
                    // (head + tail); body is internal state, not a
                    // visible/scored judgement.
                    //
                    // Tail sample on completion (the usual path — players
                    // release a few frames after end_time). Bank resolves
                    // at end_time.
                    play_tail_sound(map.notes[hi].end_time);
                    cnt_perfect++;
                    combo++;
                    if (combo > max_combo) max_combo = combo;
                    active_holds.erase(active_holds.begin() + i);
                } else i++;
            }
        }

        while (app_state == AppState::Playing &&
               miss_cursor < map.notes.size() &&
               map.notes[miss_cursor].time < now_ms - od_miss_window) {
            size_t i = miss_cursor;
            if (!note_judged[i] && !hold_started[i]) {
                bool is_hold_miss = map.notes[i].is_hold;
                if (is_hold_miss) {
                    hold_broken[i] = true;
                    release_time[i] = now_ms;
                }
                note_ghost[i] = true;
                note_judged[i] = true;
                cnt_miss++;   // 1 miss regardless of tap/hold
                combo = 0;
                play_miss_sound();
            }
            miss_cursor++;
        }

        SDL_SetRenderDrawColor(renderer, 20, 20, 30, 255);
        SDL_RenderClear(renderer);

        // GAMEPLAY BG — draw the current song's bg behind the playfield.
        if (song_bg_tex && song_bg_w > 0 && song_bg_h > 0) {
            float tex_ar = (float)song_bg_w / song_bg_h;
            float win_ar = (float)win_w / win_h;
            SDL_FRect dst;
            if (win_ar > tex_ar) {
                float w = (float)win_w;
                float h = w / tex_ar;
                dst = { 0.0f, (win_h - h) * 0.5f, w, h };
            } else {
                float h = (float)win_h;
                float w = h * tex_ar;
                dst = { (win_w - w) * 0.5f, 0.0f, w, h };
            }
            SDL_SetTextureAlphaMod(song_bg_tex, 100);
            SDL_RenderTexture(renderer, song_bg_tex, NULL, &dst);
        }

        // COLUMN STRIP — dark strip under just the note columns so
        // notes stay readable over any bg.
        {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 170);
            SDL_FRect strip = { playfield_x, 0.0f, playfield_w, (float)win_h };
            SDL_RenderFillRect(renderer, &strip);
        }

        // Column separators
        for (int c = 0; c < keys; c++) {
            if (column_pressed[c]) {
                SDL_SetRenderDrawColor(renderer, 40, 60, 110, 255);
                SDL_FRect bg = { playfield_x + c * col_w, 0, col_w, (float)win_h };
                SDL_RenderFillRect(renderer, &bg);
            }
        }

        SDL_SetRenderDrawColor(renderer, 60, 60, 80, 255);
        for (int c = 1; c < keys; c++) {
            float x = playfield_x + c * col_w;
            SDL_RenderLine(renderer, x, 0, x, (float)win_h);
        }

        // Judgement line
        // JUDGEMENT LINE THICK
        SDL_SetRenderDrawColor(renderer, 220, 220, 220, 255);
        SDL_FRect jline = { playfield_x, judgement_y - 1.0f, playfield_w, 2.0f };
        SDL_RenderFillRect(renderer, &jline);

        // Notes
        // Visible time window depends on scroll speed. At low speeds a
        // note can be visible 10+ seconds ahead; at high speeds only a
        // fraction of a second.
        double time_range = 11485.0 / (double)cfg_scroll_speed;
        double rp_now     = raw_position_at(now_ms);
        double max_time_ahead = time_range * 6.0;

        for (size_t ni = 0; ni < map.notes.size(); ni++) {
            const auto& n = map.notes[ni];
            if (note_judged[ni]) continue;
            if (n.time > now_ms + max_time_ahead) break;   // notes sorted, stop early

            // Render skip: resolved notes not marked ghost are gone.
            if (note_judged[ni] && !note_ghost[ni]) continue;

            // Killed LNs: broken or missed hold notes fade out 250ms after resolution.
            if (n.is_hold && note_ghost[ni]) {
                double since_release = now_ms - release_time[ni];
                if (since_release > 250.0) continue;
            }

            double rp_note = raw_position_at(n.time);
            float y = judgement_y - (float)((rp_note - rp_now) * judgement_y / time_range);
            // Tail uses the same SV-aware accumulator as the head.
            // Mixing formulas (old flat (end-now)*scroll_pps) made the tail
            // drift by the accumulated multiplier difference on SV/BPM maps.
            double rp_end = n.is_hold ? raw_position_at(n.end_time) : rp_note;
            float y_end = judgement_y - (float)((rp_end - rp_now) * judgement_y / time_range);

            // Sticky head: while a hold is active, pin the head to the
            // judgement line and let the body shrink from the bottom.
            float head_y = y;
            if (n.is_hold && hold_active[ni]) head_y = judgement_y;

            float top = (head_y < y_end) ? head_y : y_end;
            float bot = (head_y < y_end) ? y_end : head_y;
            if (bot < -50.0f || top > win_h + 50.0f) continue;

            float x = playfield_x + n.column * col_w;
            float w = col_w - 4.0f;
            float h = 24.0f;

            // Color scheme by state — mono cap color, shared by head and tail.
            Uint8 br = 90,  bg = 180, bb = 255, ba = 255;   // body
            Uint8 cr = 240, cg = 240, cb = 240, ca = 255;   // cap (head + tail)

            if (note_ghost[ni]) {
                br = 60;  bg = 65;  bb = 75;  ba = 90;
                cr = 100; cg = 100; cb = 100; ca = 130;
            } else if (hold_active[ni]) {
                br = 130; bg = 220; bb = 255; ba = 255;
                cr = 255; cg = 245; cb = 170; ca = 255;
            }

            if (n.is_hold) {
                // Body
                SDL_SetRenderDrawColor(renderer, br, bg, bb, ba);
                SDL_FRect body = { x + 2.0f, top, w, bot - top };
                SDL_RenderFillRect(renderer, &body);

                // Tail cap — same anchor convention as head (centered on
                // y_end, not bottom-aligned). Mixing the two put the tail
                // 12px above the head whenever a hold spawned.
                if (y_end > -50.0f && y_end < win_h + 50.0f) {
                    SDL_SetRenderDrawColor(renderer, cr, cg, cb, ca);
                    SDL_FRect tail = { x + 2.0f, y_end - h * 0.5f, w, h };
                    SDL_RenderFillRect(renderer, &tail);
                }
            }

            // Head cap (skip if scrolled off)
            if (head_y > -50.0f && head_y < win_h + 50.0f) {
                SDL_SetRenderDrawColor(renderer, cr, cg, cb, ca);
                SDL_FRect r = { x + 2.0f, head_y - h * 0.5f, w, h };
                SDL_RenderFillRect(renderer, &r);
            }
        }

        // ---- HUD (cached to texture) ----
        if (hud_tex) {
            // FPS only updates once per second (when the log report fires).
            // Everything else triggers a rebuild only on actual value change.
            bool changed =
                combo    != hud_last_combo || max_combo != hud_last_max ||
                cnt_300  != hud_last_300   || cnt_100   != hud_last_100 ||
                cnt_50   != hud_last_50    || cnt_miss  != hud_last_miss ||
                hud_fps  != hud_last_fps;

            if (changed) {
                hud_last_combo = combo;   hud_last_max  = max_combo;
                hud_last_300   = cnt_300; hud_last_100  = cnt_100;
                hud_last_50    = cnt_50;  hud_last_miss = cnt_miss;
                hud_last_fps   = hud_fps;

                SDL_SetRenderTarget(renderer, hud_tex);
                SDL_SetRenderScale(renderer, HUD_SCALE, HUD_SCALE);

                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
                SDL_RenderClear(renderer);

                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);

                char buf[160];
                int x = 6, y = 6, lh = 14;

                SDL_snprintf(buf, sizeof(buf), "Combo: %d   Max: %d",
                             combo, max_combo);
                SDL_RenderDebugText(renderer, (float)x, (float)y, buf); y += lh;

                SDL_snprintf(buf, sizeof(buf), "P:%d 300:%d 200:%d 100:%d 50:%d M:%d",
                             cnt_perfect, cnt_300, cnt_200, cnt_100, cnt_50, cnt_miss);
                SDL_RenderDebugText(renderer, (float)x, (float)y, buf); y += lh;

                int total = cnt_perfect + cnt_300 + cnt_200 + cnt_100 + cnt_50 + cnt_miss;
                double acc = 0.0;
                if (total > 0)
                    acc = (cnt_perfect*305.0 + cnt_300*300.0 + cnt_200*200.0
                         + cnt_100*100.0 + cnt_50*50.0)
                        / (total * 305.0) * 100.0;
                SDL_snprintf(buf, sizeof(buf), "Acc: %.2f%%   FPS: %d", acc, hud_fps);
                SDL_RenderDebugText(renderer, (float)x, (float)y, buf);

                SDL_SetRenderScale(renderer, 1.0f, 1.0f);
                SDL_SetRenderTarget(renderer, NULL);
            }

            // Blit — 60px inset from top-left for rounded corners
            SDL_FRect dst = { 60.0f, 60.0f,
                              (float)HUD_TEX_W, (float)HUD_TEX_H };
            SDL_RenderTexture(renderer, hud_tex, NULL, &dst);
        }

        // Frame cap: target cfg_fps, sleep the remainder of the frame.
        // Set to 0 in settings.ini to disable (uncapped).
        // Software cap only when vsync is off. With vsync on,
        // SDL_RenderPresent blocks until vblank; adding our own delay
        // would occasionally miss the next vblank and halve the rate.
        if (cfg_fps_cap > 0 && !vsync_enabled) {
            Uint64 frame_end_target = frame_start + (1000000ULL / cfg_fps_cap);
            Uint64 now_us = SDL_GetTicksNS() / 1000ULL;
            if (now_us < frame_end_target) {
                Uint64 remaining_us = frame_end_target - now_us;
                if (remaining_us > 1500) {
                    SDL_DelayNS((remaining_us - 1500) * 1000ULL);
                }
                while (SDL_GetTicksNS() / 1000ULL < frame_end_target) {
                    // tight loop
                }
            }
        }

        // ---- Resume countdown ----
        static int last_tick_second = -1;
        if (resuming) {
            Uint64 elapsed = SDL_GetTicks() - resume_start_tick;
            int seconds_left = 3 - (int)(elapsed / 1000);
            if (seconds_left < 1) seconds_left = 1;

            // Fire countdown sfx when the second changes
            if (seconds_left != last_tick_second) {
                if (countdown_sfx_stream && cd_data) {
                    SDL_ClearAudioStream(countdown_sfx_stream);
                    SDL_PutAudioStreamData(countdown_sfx_stream, cd_data, (int)cd_len);
                }
                last_tick_second = seconds_left;
            }

            char buf[8];
            SDL_snprintf(buf, sizeof(buf), "%d", seconds_left);

            // Big centered number
            SDL_SetRenderScale(renderer, 12.0f, 12.0f);
            SDL_SetRenderDrawColor(renderer, 255, 255, 200, 255);
            float cw = (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE;
            float tx = ((float)win_w / 12.0f - cw) * 0.5f;
            float ty = ((float)win_h / 12.0f - cw) * 0.5f;
            SDL_RenderDebugText(renderer, tx, ty, buf);
            SDL_SetRenderScale(renderer, 1.0f, 1.0f);
        }

        // ---- RESULT SCREEN ----
        if (result_shown) {
            // Dim
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 200);
            SDL_FRect full = { 0, 0, (float)win_w, (float)win_h };
            SDL_RenderFillRect(renderer, &full);

            // Score + accuracy
            long total_judged = (long)(cnt_perfect + cnt_300 + cnt_200 + cnt_100 + cnt_50 + cnt_miss);
            // Total judgements = taps + 2*holds (head + tail per hold).
            // = map.notes.size() + hold_count.
            long total_notes  = (long)map.notes.size() + (long)hold_count;
            double acc = 0.0;
            if (total_judged > 0) {
                acc = (cnt_perfect*305.0 + cnt_300*300.0 + cnt_200*200.0
                     + cnt_100*100.0 + cnt_50*50.0)
                    / (total_judged * 305.0) * 100.0;
            }
            long score = compute_lazer_score(cnt_perfect, cnt_300, cnt_200,
                                              cnt_100, cnt_50, (int)total_notes);

            // Lazer ManiaScoreProcessor.RankFromScore: SS (X) iff base rank
            // is S (>= 95% acc) AND no Good/Ok/Meh/Miss judgements exist.
            bool any_imperfect = (cnt_200 > 0 || cnt_100 > 0 ||
                                  cnt_50  > 0 || cnt_miss > 0);
            const char* rank = "D";
            if      (acc >= 95.0 && !any_imperfect) rank = "SS";
            else if (acc >= 95.0)  rank = "S";
            else if (acc >= 90.0)  rank = "A";
            else if (acc >= 80.0)  rank = "B";
            else if (acc >= 70.0)  rank = "C";

            // Panel — 8 lines, 5x scale
            const float scale = 5.0f;
            const int   lh_log = 20;
            SDL_SetRenderScale(renderer, scale, scale);

            float panel_logical_w = 60;
            float panel_logical_h = lh_log * 8;
            float px = ((float)win_w / scale - panel_logical_w) * 0.5f;
            float py = ((float)win_h / scale - panel_logical_h) * 0.5f;

            char line[64];

            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);

            SDL_snprintf(line, sizeof(line), "Rank:  %s", rank);
            SDL_RenderDebugText(renderer, px, py, line); py += lh_log;

            SDL_snprintf(line, sizeof(line), "Score: %ld", score);
            SDL_RenderDebugText(renderer, px, py, line); py += lh_log;

            SDL_snprintf(line, sizeof(line), "Acc:   %.2f%%", acc);
            SDL_RenderDebugText(renderer, px, py, line); py += lh_log;

            SDL_snprintf(line, sizeof(line), "Max:   %d", max_combo);
            SDL_RenderDebugText(renderer, px, py, line); py += lh_log;

            SDL_snprintf(line, sizeof(line), "P:%d 300:%d 200:%d", cnt_perfect, cnt_300, cnt_200);
            SDL_RenderDebugText(renderer, px, py, line); py += lh_log;

            SDL_snprintf(line, sizeof(line), "100:%d 50:%d M:%d", cnt_100, cnt_50, cnt_miss);
            SDL_RenderDebugText(renderer, px, py, line); py += lh_log;

            py += lh_log / 2;
            SDL_SetRenderDrawColor(renderer, 200, 200, 100, 255);
            SDL_RenderDebugText(renderer, px, py, "Tap / R to retry");

            SDL_SetRenderScale(renderer, 1.0f, 1.0f);
        }

        // ---- SETTINGS SCREEN ----
        if (app_state == AppState::Settings) {
            SDL_SetRenderDrawColor(renderer, 15, 15, 25, 255);
            SDL_FRect bg_full = { 0, 0, (float)win_w, (float)win_h };
            SDL_RenderFillRect(renderer, &bg_full);

            SDL_SetRenderScale(renderer, 3.0f, 3.0f);
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            SDL_RenderDebugText(renderer, 8.0f, 6.0f, "Settings");
            SDL_SetRenderScale(renderer, 1.0f, 1.0f);

            const float row_x = settings_row_x;
            const float row_w = (float)win_w - 2.0f * settings_row_x;

            for (int i = 0; i < ROW_COUNT; i++) {
                const auto& row = setting_rows[i];
                float ry = settings_row_y0 + i * (settings_row_h + settings_row_gap);
                bool capturing_this = (row.type == 2 && capture_nk == row.key_count);

                // Row background
                if (capturing_this) SDL_SetRenderDrawColor(renderer, 90, 60, 40, 255);
                else                SDL_SetRenderDrawColor(renderer, 40, 45, 60, 255);
                SDL_FRect bar = { row_x, ry, row_w, settings_row_h };
                SDL_RenderFillRect(renderer, &bar);

                // Fill / highlight
                char buf[128];
                buf[0] = 0;
                float fill = 0.0f;
                if (row.type == 0) {
                    float v = *(float*)row.target;
                    fill = (v - row.min_val) / (row.max_val - row.min_val);
                    if (row.target == (void*)&cfg_audio_offset_ms)
                        SDL_snprintf(buf, sizeof(buf), "%+.0f ms", v);
                    else
                        SDL_snprintf(buf, sizeof(buf), "%.2f", v);
                } else if (row.type == 1) {
                    int v = *(int*)row.target;
                    int idx = 0;
                    for (int k = 0; k < row.cycle_count; k++)
                        if (row.cycle_values[k] == v) idx = k;
                    fill = (idx + 0.5f) / row.cycle_count;
                    SDL_snprintf(buf, sizeof(buf), "%s", row.cycle_labels[idx]);
                } else if (row.type == 2) {
                    // build binding display string
                    int nk = row.key_count;
                    const auto& b = bindings[nk];
                    int len = 0;
                    if (capturing_this) {
                        len += SDL_snprintf(buf + len, sizeof(buf) - len, "[%zu/%d] ",
                                            capture_progress.size(), nk);
                        for (int k = 0; k < nk && len < (int)sizeof(buf) - 16; k++) {
                            if ((size_t)k < capture_progress.size()) {
                                const char* nm = SDL_GetKeyName(capture_progress[k]);
                                len += SDL_snprintf(buf + len, sizeof(buf) - len, "%s ", nm ? nm : "?");
                            } else {
                                len += SDL_snprintf(buf + len, sizeof(buf) - len, "_ ");
                            }
                        }
                    } else {
                        for (size_t k = 0; k < b.size() && len < (int)sizeof(buf) - 16; k++) {
                            const char* nm = SDL_GetKeyName(b[k]);
                            len += SDL_snprintf(buf + len, sizeof(buf) - len, "%s ", nm ? nm : "?");
                        }
                    }
                }
                if (fill < 0.0f) fill = 0.0f;
                if (fill > 1.0f) fill = 1.0f;
                if (row.type != 2) {
                    SDL_SetRenderDrawColor(renderer, 90, 160, 240, 255);
                    SDL_FRect filled = { row_x, ry, row_w * fill, settings_row_h };
                    SDL_RenderFillRect(renderer, &filled);
                }

                // Label (small, top-left)
                SDL_SetRenderScale(renderer, 2.0f, 2.0f);
                SDL_SetRenderDrawColor(renderer, 200, 200, 220, 255);
                SDL_RenderDebugText(renderer, (row_x + 8.0f) / 2.0f, (ry + 4.0f) / 2.0f, row.label);

                // Value / binding (3x, bottom-left)
                SDL_SetRenderScale(renderer, 3.0f, 3.0f);
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
                SDL_RenderDebugText(renderer, (row_x + 8.0f) / 3.0f, (ry + 26.0f) / 3.0f, buf);
                SDL_SetRenderScale(renderer, 1.0f, 1.0f);
            }

            // Back button
            SDL_SetRenderScale(renderer, 3.0f, 3.0f);
            SDL_SetRenderDrawColor(renderer, 200, 200, 100, 255);
            SDL_RenderDebugText(renderer, 8.0f, (float)win_h / 3.0f - 30.0f, "back");
            SDL_SetRenderScale(renderer, 1.0f, 1.0f);
        }
        // ---- Song select ----
        if (app_state == AppState::SongSelect) {
            // BG render — scale-to-cover, then dim overlay
            SDL_SetRenderDrawColor(renderer, 15, 15, 25, 255);
            SDL_FRect bg_full = { 0, 0, (float)win_w, (float)win_h };
            SDL_RenderFillRect(renderer, &bg_full);

            if (song_bg_tex && song_bg_w > 0 && song_bg_h > 0) {
                float tex_ar = (float)song_bg_w / song_bg_h;
                float win_ar = (float)win_w / win_h;
                SDL_FRect dst;
                if (win_ar > tex_ar) {
                    float w = (float)win_w;
                    float h = w / tex_ar;
                    dst = { 0.0f, (win_h - h) * 0.5f, w, h };
                } else {
                    float h = (float)win_h;
                    float w = h * tex_ar;
                    dst = { (win_w - w) * 0.5f, 0.0f, w, h };
                }
                SDL_SetTextureAlphaMod(song_bg_tex, 180);
                SDL_RenderTexture(renderer, song_bg_tex, NULL, &dst);

                // Slight further dim so list rows stay legible
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 100);
                SDL_RenderFillRect(renderer, &bg_full);
            }

            // Title bar
            SDL_SetRenderScale(renderer, 3.0f, 3.0f);
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            SDL_RenderDebugText(renderer, 8.0f, 6.0f, "osu!vinniv - song select");
            SDL_SetRenderScale(renderer, 1.0f, 1.0f);

            if (!scan.complete()) {
                int p = scan.progress.load();
                int t = scan.total.load();

                char sbuf[64];
                if (t > 0) SDL_snprintf(sbuf, sizeof(sbuf), "Scanning %d / %d", p, t);
                else       SDL_snprintf(sbuf, sizeof(sbuf), "Scanning...");

                // Centered text at 3x scale
                SDL_SetRenderScale(renderer, 3.0f, 3.0f);
                SDL_SetRenderDrawColor(renderer, 220, 220, 240, 255);
                int n = (int)strlen(sbuf);
                SDL_RenderDebugText(renderer,
                                    (float)win_w / 6.0f - (float)n * 4.0f,
                                    (float)win_h / 6.0f - 20.0f, sbuf);
                SDL_SetRenderScale(renderer, 1.0f, 1.0f);

                // Progress bar at 1x scale
                float bar_w = (float)win_w * 0.6f;
                float bar_h = 12.0f;
                float bx = ((float)win_w - bar_w) * 0.5f;
                float by = (float)win_h * 0.5f + 20.0f;

                SDL_SetRenderDrawColor(renderer, 40, 45, 60, 255);
                SDL_FRect bar_bg = { bx, by, bar_w, bar_h };
                SDL_RenderFillRect(renderer, &bar_bg);

                if (t > 0) {
                    float frac = (float)p / (float)t;
                    if (frac < 0.0f) frac = 0.0f;
                    if (frac > 1.0f) frac = 1.0f;
                    SDL_SetRenderDrawColor(renderer, 90, 160, 240, 255);
                    SDL_FRect bar_fill = { bx, by, bar_w * frac, bar_h };
                    SDL_RenderFillRect(renderer, &bar_fill);
                }
            }

            // Settings button (upper-left, below the title bar)
            {
                const float bx = 40.0f;
                const float by = 140.0f;
                const float bw = 240.0f;
                const float bh = 80.0f;

                SDL_SetRenderDrawColor(renderer, 40, 50, 70, 255);
                SDL_FRect bg = { bx, by, bw, bh };
                SDL_RenderFillRect(renderer, &bg);

                SDL_SetRenderDrawColor(renderer, 200, 200, 100, 255);
                SDL_RenderRect(renderer, &bg);

                // Text centered — "settings" is 8 chars, at scale 2 each is 16px
                SDL_SetRenderScale(renderer, 2.0f, 2.0f);
                SDL_SetRenderDrawColor(renderer, 220, 220, 140, 255);
                float text_w_px = 8.0f * 16.0f;
                float text_x = (bx + bw * 0.5f - text_w_px * 0.5f) / 2.0f;
                float text_y = (by + bh * 0.5f - 8.0f) / 2.0f;
                SDL_RenderDebugText(renderer, text_x, text_y, "settings");
                SDL_SetRenderScale(renderer, 1.0f, 1.0f);
            }

            // Random button (upper-left, below settings)
            {
                const float bx = 40.0f;
                const float by = 240.0f;
                const float bw = 240.0f;
                const float bh = 80.0f;

                SDL_SetRenderDrawColor(renderer, 40, 50, 70, 255);
                SDL_FRect bg = { bx, by, bw, bh };
                SDL_RenderFillRect(renderer, &bg);

                SDL_SetRenderDrawColor(renderer, 100, 200, 100, 255);
                SDL_RenderRect(renderer, &bg);

                SDL_SetRenderScale(renderer, 2.0f, 2.0f);
                SDL_SetRenderDrawColor(renderer, 180, 240, 180, 255);
                float text_w_px = 6.0f * 16.0f;   // "random" is 6 chars
                float text_x = (bx + bw * 0.5f - text_w_px * 0.5f) / 2.0f;
                float text_y = (by + bh * 0.5f - 8.0f) / 2.0f;
                SDL_RenderDebugText(renderer, text_x, text_y, "random");
                SDL_SetRenderScale(renderer, 1.0f, 1.0f);
            }

            ListGeom lg = get_list_geom();
            const float row_h  = lg.row_h;
            const float list_x = lg.x;
            const float list_y = lg.y;
            const float list_w = lg.w;
            const float list_h = lg.h;

            if (songs.empty()) {
                SDL_SetRenderScale(renderer, 3.0f, 3.0f);
                SDL_SetRenderDrawColor(renderer, 200, 120, 120, 255);
                SDL_RenderDebugText(renderer, 12.0f, 60.0f,
                    "no songs found - push .osu files to files/songs/");
                SDL_SetRenderScale(renderer, 1.0f, 1.0f);
            } else {
                int n = (int)songs.size();
                if (selected_index < 0) selected_index = 0;
                if (selected_index >= n) selected_index = n - 1;

                // Two-pass: rows (clipped), then text (unclipped but bounded
                // by the same y-range gate). Keeps render scale changes away
                // from the clip rect, which SDL3 handles unreliably when they
                // interleave.
                SDL_Rect list_clip = { (int)list_x, (int)list_y,
                                       (int)list_w, (int)list_h };
                SDL_SetRenderClipRect(renderer, &list_clip);
                for (int i = 0; i < n; i++) {
                    float ry = list_y + i * row_h - song_scroll;
                    if (ry + row_h < list_y || ry > list_y + list_h) continue;
                    bool sel = (i == selected_index);
                    SDL_SetRenderDrawColor(renderer,
                        sel ? 60 : 25, sel ? 80 : 30, sel ? 130 : 40, 255);
                    SDL_FRect row = { list_x, ry, list_w, row_h - 6 };
                    SDL_RenderFillRect(renderer, &row);
                    if (sel) {
                        SDL_SetRenderDrawColor(renderer, 200, 220, 255, 255);
                        SDL_RenderRect(renderer, &row);
                    }
                }
                SDL_SetRenderClipRect(renderer, nullptr);

                // Text pass — no clip (SDL3 clip+scale interleave is buggy),
                // instead gate on BOTH top and bottom so text only draws when
                // it fully fits inside the panel. Rows partially scrolled off
                // show their bg (clipped) but no text (cleaner than overflow).
                const float TEXT_BOTTOM = 68.0f;  // line2 baseline (52) + glyph (16)
                for (int i = 0; i < n; i++) {
                    float ry = list_y + i * row_h - song_scroll;
                    if (ry + row_h < list_y || ry > list_y + list_h) continue;
                    if (ry + 14.0f < list_y) continue;                    // top text clipped
                    if (ry + TEXT_BOTTOM > list_y + list_h) continue;    // bottom text clipped

                    char line1[256];
                    char line2[256];
                    // [NK] badge prefix on line1 (lazer parity), title after.
                    // line2: artist, difficulty name, OD.
                    SDL_snprintf(line1, sizeof(line1), "[%dK] %s",
                                 songs[i].key_count, songs[i].title.c_str());
                    SDL_snprintf(line2, sizeof(line2), "%s   [%s]  OD%.1f",
                                 songs[i].artist.c_str(),
                                 songs[i].version.c_str(),
                                 songs[i].od);

                    SDL_SetRenderScale(renderer, 3.0f, 3.0f);
                    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
                    SDL_RenderDebugText(renderer,
                        (list_x + 14.0f) / 3.0f, (ry + 14.0f) / 3.0f, line1);
                    SDL_SetRenderScale(renderer, 2.0f, 2.0f);
                    SDL_SetRenderDrawColor(renderer, 180, 180, 200, 255);
                    SDL_RenderDebugText(renderer,
                        (list_x + 14.0f) / 2.0f, (ry + 52.0f) / 2.0f, line2);
                    SDL_SetRenderScale(renderer, 1.0f, 1.0f);
                }

                // SCROLLBAR — only show if content overflows.
                // Use lg.h (the actual viewport height) — do NOT recompute
                // from win_h here; that shadowing bug made the track extend
                // below the panel.
                SDL_SetRenderClipRect(renderer, nullptr);
                float total_h  = (float)n * row_h;
                if (total_h > list_h) {
                    float track_x = list_x + list_w + 8.0f;
                    float track_w = 8.0f;
                    float thumb_h = list_h * (list_h / total_h);
                    float scroll_max = total_h - list_h;
                    float thumb_y = list_y +
                        (song_scroll / scroll_max) * (list_h - thumb_h);

                    SDL_SetRenderDrawColor(renderer, 40, 40, 55, 255);
                    SDL_FRect track = { track_x, list_y, track_w, list_h };
                    SDL_RenderFillRect(renderer, &track);

                    SDL_SetRenderDrawColor(renderer, 140, 160, 200, 255);
                    SDL_FRect thumb = { track_x, thumb_y, track_w, thumb_h };
                    SDL_RenderFillRect(renderer, &thumb);
                }
            }

            // Footer hint
            SDL_SetRenderScale(renderer, 3.0f, 3.0f);
            SDL_SetRenderDrawColor(renderer, 200, 200, 100, 255);
            SDL_RenderDebugText(renderer, 8.0f,
                (float)win_h/3.0f - 20.0f, "tap a row to play");
            SDL_SetRenderScale(renderer, 1.0f, 1.0f);
        }

        // ---- Pause menu ----
        if (paused) {
            // Dim the whole screen
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
            SDL_FRect full = { 0, 0, (float)win_w, (float)win_h };
            SDL_RenderFillRect(renderer, &full);

            // Layout
            menu_btn_w = 360.0f;
            menu_btn_h = 72.0f;
            menu_btn_x = ((float)win_w - menu_btn_w) * 0.5f;
            float total_h = 3 * menu_btn_h + 2 * 24.0f;
            float top_y   = ((float)win_h - total_h) * 0.5f;
            menu_btn_y[0] = top_y;
            menu_btn_y[1] = top_y + menu_btn_h + 24.0f;
            menu_btn_y[2] = top_y + 2 * (menu_btn_h + 24.0f);

            const char* labels[3] = { "Continue", "Retry", "Quit" };

            for (int i = 0; i < 3; i++) {
                // Button background
                SDL_SetRenderDrawColor(renderer, 40, 40, 60, 240);
                SDL_FRect bg = { menu_btn_x, menu_btn_y[i], menu_btn_w, menu_btn_h };
                SDL_RenderFillRect(renderer, &bg);

                // Border
                SDL_SetRenderDrawColor(renderer, 200, 200, 220, 255);
                SDL_RenderRect(renderer, &bg);

                // Label centered — use hud font at 3x scale
                SDL_SetRenderScale(renderer, 3.0f, 3.0f);
                float text_x = menu_btn_x / 3.0f + (menu_btn_w / 3.0f - (float)strlen(labels[i]) * SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE) * 0.5f;
                float text_y = menu_btn_y[i] / 3.0f + (menu_btn_h / 3.0f - SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE) * 0.5f;
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
                SDL_RenderDebugText(renderer, text_x, text_y, labels[i]);
                SDL_SetRenderScale(renderer, 1.0f, 1.0f);
            }
        }

        SDL_RenderPresent(renderer);

        frames++;
        Uint64 now = SDL_GetTicks();
        if (now - last_report >= 1000) {
            report_count++;
            SDL_Log("FPS[%d]: %d  combo=%d max=%d P=%d 300=%d 200=%d 100=%d 50=%d miss=%d", report_count, frames, combo, max_combo, cnt_perfect, cnt_300, cnt_200, cnt_100, cnt_50, cnt_miss);
            hud_fps = frames;      // update HUD's FPS display once per second
            frames = 0;
            last_report = now;

            // Auto-exit after 3 minutes for testing; remove later.
        }
    }

    if (audio_stream) SDL_DestroyAudioStream(audio_stream);
    if (wav_data)     SDL_free(wav_data);
    if (hit_stream)   SDL_DestroyAudioStream(hit_stream);
    if (hit_data)     SDL_free(hit_data);
    if (miss_stream)  SDL_DestroyAudioStream(miss_stream);
    if (miss_data)    SDL_free(miss_data);
    if (countdown_sfx_stream) SDL_DestroyAudioStream(countdown_sfx_stream);
    if (cd_data)              SDL_free(cd_data);
    if (clap_stream)    SDL_DestroyAudioStream(clap_stream);
    if (clap_data)      SDL_free(clap_data);
    if (whistle_stream) SDL_DestroyAudioStream(whistle_stream);
    if (whistle_data)   SDL_free(whistle_data);
    if (finish_stream)  SDL_DestroyAudioStream(finish_stream);
    if (finish_data)    SDL_free(finish_data);
    if (hud_tex) SDL_DestroyTexture(hud_tex);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    scan.shutdown();
    SDL_Quit();
    return 0;
}
