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

enum class AppState { SongSelect, Playing, Result, Settings };

// Piecewise-linear OD interpolation. Anchor points are (od0, od5, od10).
static double od_window(double od, double v0, double v5, double v10) {
    if (od < 5.0) return v0 + (od / 5.0) * (v5 - v0);
    return v5 + ((od - 5.0) / 5.0) * (v10 - v5);
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
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    // SDL_HINT_TOUCH_MOUSE_EVENTS disabled: we handle FINGER_DOWN/UP directly for multi-touch

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    // ---- settings.ini ----
    float cfg_music_vol    = 0.6f;
    float cfg_hitsound_vol = 1.0f;
    float cfg_scroll_speed = 1.5f;
    int   cfg_fps_cap      = 120;   // 0 = uncapped
    bool  vsync_enabled    = (cfg_fps_cap == 120);
    {
        std::ifstream sf("/sdcard/Android/data/sh.ppy.osuvinniv/files/settings.ini");
        std::string line;
        while (std::getline(sf, line)) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq);
            std::string v = line.substr(eq + 1);
            try {
                if      (k == "music_volume")    cfg_music_vol    = std::stof(v);
                else if (k == "hitsound_volume") cfg_hitsound_vol = std::stof(v);
                else if (k == "scroll_speed")    cfg_scroll_speed = std::stof(v);
                else if (k == "fps_cap")         cfg_fps_cap      = std::stoi(v);
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
    if (load_audio_to_pcm("/sdcard/Android/data/sh.ppy.osuvinniv/files/hit.wav",
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
    if (load_audio_to_pcm("/sdcard/Android/data/sh.ppy.osuvinniv/files/miss.wav",
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
                "/sdcard/Android/data/sh.ppy.osuvinniv/files/%s.wav", e.name);
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
    if (load_audio_to_pcm("/sdcard/Android/data/sh.ppy.osuvinniv/files/countdown.wav",
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
    bool audio_queued = false;

    SDL_GetWindowSizeInPixels(window, &win_w, &win_h);

    // Playfield geometry (recomputed when song changes)
    int   keys        = (map.key_count > 0) ? map.key_count : 4;
    float col_w       = 0.0f;
    float playfield_w = 0.0f;
    float playfield_x = 0.0f;
    const float max_col_w  = 240.0f;
    float scroll_pps = 1.5f;   // recomputed in compute_geometry()
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

    auto judge_col = [&](int col, double t) -> bool {
        double map_t = t - (double)audio_start_tick;
        double od = map.od;
        double p_win  = std::floor(od_window(od, 22.4, 19.4, 13.9)) + 0.5;
        double g_win  = std::floor(od_window(od, 64, 49, 34)) + 0.5;
        double gd_win = std::floor(od_window(od, 97, 82, 67)) + 0.5;
        double ok_win = std::floor(od_window(od, 127, 112, 97)) + 0.5;
        double mh_win = std::floor(od_window(od, 151, 136, 121)) + 0.5;
        double ms_win = std::floor(od_window(od, 188, 173, 158)) + 0.5;

        double best = 1e9; int best_i = -1;
        for (size_t i = 0; i < map.notes.size(); i++) {
            if (note_judged[i]) continue;
            if (map.notes[i].column != col) continue;
            double d = map.notes[i].time - map_t;
            if (d < -ms_win) continue;
            if (d < best) { best = d; best_i = (int)i; }
            if (d > ms_win) break;
        }
        if (best_i < 0) return false;
        double a = best < 0 ? -best : best;
        if (a > ms_win) return false;

        // Layered extras from the note's hitSound bitfield
        int bits = map.notes[best_i].hit_sound;
        if (clap_stream    && (bits & 8)) play_sound(clap_stream,    clap_data,    clap_len);
        if (whistle_stream && (bits & 2)) play_sound(whistle_stream, whistle_data, whistle_len);
        if (finish_stream  && (bits & 4)) play_sound(finish_stream,  finish_data,  finish_len);

        if (map.notes[best_i].is_hold) {
            hold_started[best_i] = true;
            hold_active[best_i]  = true;
            active_holds.push_back(best_i);
        } else {
            note_judged[best_i] = true;
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

    // Auto-import any .osz files sitting in files/inbox/
    import_all_osz(
        "/sdcard/Android/data/sh.ppy.osuvinniv/files/inbox",
        "/sdcard/Android/data/sh.ppy.osuvinniv/files/songs");

    std::vector<SongEntry> songs =
        scan_songs("/sdcard/Android/data/sh.ppy.osuvinniv/files/songs");
    int   selected_index = 0;
    float song_scroll    = 0.0f;

    // Song select drag state
    SDL_FingerID song_drag_id      = (SDL_FingerID)-1;
    float        song_drag_start_y = 0.0f;
    float        song_drag_start_scroll = 0.0f;
    bool         song_drag_moved   = false;

    // Settings state
    bool  settings_dirty = false;
    int   settings_row   = -1;      // which slider is being dragged, -1 = none
    int   settings_fps_preset = 0;  // 0=60, 1=120, 2=240, 3=0 (uncapped)
    SDL_Log("SONGS: found %zu entries", songs.size());

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
                SDL_Log("RESTART: reset to t=0");
            }

            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && !paused && !resuming && !result_shown && app_state == AppState::Playing) {
                int col = -1;
                if      (e.key.key == SDLK_D) col = 0;
                else if (e.key.key == SDLK_F) col = 1;
                else if (e.key.key == SDLK_J) col = 2;
                else if (e.key.key == SDLK_K) col = 3;
                if (col >= 0 && col < keys) {
                    column_pressed[col] = true;
                    play_sound(hit_stream, hit_data, hit_len);
                    judge_col(col, (double)SDL_GetTicks());
                }
            }
            // SONG SELECT DRAG / TAP
            if (app_state == AppState::SongSelect && !songs.empty()) {
                const float list_x = 60.0f;
                const float list_y = 140.0f;
                const float row_h  = 110.0f;
                const float footer_h = 120.0f;
                const float list_h = (float)win_h - list_y - footer_h;
                const float total_h = (float)songs.size() * row_h;
                const float max_scroll = total_h > list_h ? total_h - list_h : 0.0f;

                if (e.type == SDL_EVENT_FINGER_DOWN) {
                    float tx = e.tfinger.x * (float)win_w;
                    float ty = e.tfinger.y * (float)win_h;

                    // Settings button
                    const float sbx = (float)win_w - 320.0f;
                    const float sby = 40.0f;
                    const float sbw = 260.0f;
                    const float sbh = 90.0f;
                    if (tx >= sbx && tx <= sbx + sbw &&
                        ty >= sby && ty <= sby + sbh) {
                        app_state = AppState::Settings;
                        continue;
                    }

                    song_drag_id           = e.tfinger.fingerID;
                    song_drag_start_y      = ty;
                    song_drag_start_scroll = song_scroll;
                    song_drag_moved        = false;
                    continue;
                }

                if (e.type == SDL_EVENT_FINGER_MOTION &&
                    e.tfinger.fingerID == song_drag_id) {
                    float ty = e.tfinger.y * (float)win_h;
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
                    if (!song_drag_moved) {
                        float ty = e.tfinger.y * (float)win_h;
                        int hit = (int)((ty - list_y + song_scroll) / row_h);
                        if (hit >= 0 && hit < (int)songs.size()) {
                            selected_index = hit;
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
                            cnt_perfect = 0; cnt_300 = 0; cnt_200 = 0;
                            cnt_100 = 0; cnt_50 = 0; cnt_miss = 0;
                            miss_cursor = 0;
                            paused = false; resuming = false; result_shown = false;

                            Beatmap new_map;
                            if (!parse_osu_file(s.osu_path, new_map)) {
                                SDL_Log("SONGS: parse failed");
                            } else {
                                map = new_map;
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
                if (e.type == SDL_EVENT_FINGER_DOWN ||
                    e.type == SDL_EVENT_FINGER_MOTION ||
                    e.type == SDL_EVENT_FINGER_UP) {
                    stx = e.tfinger.x * (float)win_w;
                    sty = e.tfinger.y * (float)win_h;
                    SDL_Log("SETTINGS: got type=%d x=%.0f y=%.0f row=%d",
                            (int)e.type, stx, sty, settings_row);
                }

                if (e.type == SDL_EVENT_FINGER_DOWN) {
                    // Back button (bottom-left region)
                    if (stx < 400.0f && sty > (float)win_h - 200.0f) {
                        app_state = AppState::SongSelect;
                        SDL_Log("SETTINGS: back button");
                        continue;
                    }

                    const float row_x  = 80.0f;
                    const float row_w  = (float)win_w - 160.0f;
                    const float row_h  = 100.0f;
                    const float row_y0 = 200.0f;
                    const float gap    = 20.0f;

                    for (int i = 0; i < 4; i++) {
                        float ry = row_y0 + i * (row_h + gap);
                        if (stx >= row_x && stx <= row_x + row_w &&
                            sty >= ry && sty <= ry + row_h) {
                            settings_row = i;
                            SDL_Log("SETTINGS: row %d selected", i);
                            break;
                        }
                    }
                    continue;
                }

                if (e.type == SDL_EVENT_FINGER_MOTION) {
                    if (settings_row < 0) {
                        const float row_x  = 80.0f;
                        const float row_w  = (float)win_w - 160.0f;
                        const float row_h  = 100.0f;
                        const float row_y0 = 200.0f;
                        const float gap    = 20.0f;
                        for (int i = 0; i < 4; i++) {
                            float ry = row_y0 + i * (row_h + gap);
                            if (stx >= row_x && stx <= row_x + row_w &&
                                sty >= ry && sty <= ry + row_h) {
                                settings_row = i;
                                break;
                            }
                        }
                    }

                    if (settings_row >= 0) {
                        const float row_x = 80.0f;
                        const float row_w = (float)win_w - 160.0f;
                        float norm = (stx - row_x) / row_w;
                        if (norm < 0.0f) norm = 0.0f;
                        if (norm > 1.0f) norm = 1.0f;

                        if      (settings_row == 0) cfg_music_vol    = norm * 1.0f;
                        else if (settings_row == 1) cfg_hitsound_vol = norm * 2.0f;
                        else if (settings_row == 2) cfg_scroll_speed = 1.0f + norm * 39.0f;
                        else if (settings_row == 3) {
                            int preset = (int)(norm * 4.0f);
                            if      (preset == 0) cfg_fps_cap = 60;
                            else if (preset == 1) cfg_fps_cap = 120;
                            else if (preset == 2) cfg_fps_cap = 240;
                            else                  cfg_fps_cap = 0;
                        }
                        settings_dirty = true;
                    }
                    continue;
                }

                if (e.type == SDL_EVENT_FINGER_UP) {
                    if (settings_row >= 0) {
                        FILE* f = fopen("/sdcard/Android/data/sh.ppy.osuvinniv/files/settings.ini", "w");
                        if (f) {
                            fprintf(f, "music_volume=%.3f\n",    cfg_music_vol);
                            fprintf(f, "hitsound_volume=%.3f\n", cfg_hitsound_vol);
                            fprintf(f, "scroll_speed=%.3f\n",    cfg_scroll_speed);
                            fprintf(f, "fps_cap=%d\n",           cfg_fps_cap);
                            fclose(f);
                            SDL_Log("SETTINGS: saved");
                        }
                    }
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
                    play_sound(hit_stream, hit_data, hit_len);
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
                    for (size_t i = 0; i < active_holds.size(); ) {
                        size_t hi = active_holds[i];
                        if (map.notes[hi].column != release_col) { i++; continue; }
                        release_time[hi] = now_map;
                        if (now_map < map.notes[hi].end_time - 150.0) {
                            // Released early — break
                            hold_active[hi]  = false;
                            hold_broken[hi]  = true;
                            note_ghost[hi]   = true;
                            note_judged[hi]  = true;
                            cnt_miss++;
                            combo = 0;
                            play_sound(miss_stream, miss_data, miss_len);
                        } else {
                            // Released cleanly at/after tail — LN resolves
                            hold_active[hi]  = false;
                            note_judged[hi]  = true;
                            // combo stays; tail is not a separate +1 in our scoring
                        }
                        active_holds.erase(active_holds.begin() + i);
                    }
                }
            }
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !paused && !resuming && !result_shown && app_state == AppState::Playing) {
                int col = find_col(e.button.x);
                if (col >= 0) {
                    column_pressed[col] = true;
                    play_sound(hit_stream, hit_data, hit_len);
                    judge_col(col, (double)SDL_GetTicks());
                }
            }
            if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && !paused && !resuming && app_state == AppState::Playing) {
                int col = find_col(e.button.x);
                if (col >= 0) column_pressed[col] = false;
            }
            if (e.type == SDL_EVENT_KEY_UP) {
                if      (e.key.key == SDLK_D) column_pressed[0] = false;
                else if (e.key.key == SDLK_F) column_pressed[1] = false;
                else if (e.key.key == SDLK_J) column_pressed[2] = false;
                else if (e.key.key == SDLK_K) column_pressed[3] = false;
            }
        }

        SDL_GetWindowSizeInPixels(window, &win_w, &win_h);

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
        double now_ms = (double)current_tick - (double)audio_start_tick;
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
        if (!result_shown && !paused && !resuming &&
            !map.notes.empty() &&
            now_ms > map.notes.back().end_time + 2000.0)
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
                    active_holds.erase(active_holds.begin() + i);
                } else i++;
            }
        }

        while (app_state == AppState::Playing &&
               miss_cursor < map.notes.size() &&
               map.notes[miss_cursor].time < now_ms - 70.0) {
            size_t i = miss_cursor;
            if (!note_judged[i] && !hold_started[i]) {
                if (map.notes[i].is_hold) {
                    hold_broken[i] = true;
                    release_time[i] = now_ms;
                }
                note_ghost[i] = true;
                note_judged[i] = true;
                cnt_miss++;
                combo = 0;
                play_sound(miss_stream, miss_data, miss_len);
            }
            miss_cursor++;
        }

        SDL_SetRenderDrawColor(renderer, 20, 20, 30, 255);
        SDL_RenderClear(renderer);

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
        SDL_SetRenderDrawColor(renderer, 220, 220, 220, 255);
        SDL_RenderLine(renderer, playfield_x, judgement_y, playfield_x + playfield_w, judgement_y);

        // Notes
        // Visible time window depends on scroll speed. At low speeds a
        // note can be visible 10+ seconds ahead; at high speeds only a
        // fraction of a second.
        double max_time_ahead = (judgement_y + 100.0) / scroll_pps;

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

            float y = judgement_y - (float)((n.time - now_ms) * scroll_pps);
            float y_end = n.is_hold
                ? judgement_y - (float)((n.end_time - now_ms) * scroll_pps)
                : y;

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

                // Tail cap — same color as head, drawn only if on screen.
                if (y_end > -50.0f && y_end < win_h + 50.0f) {
                    SDL_SetRenderDrawColor(renderer, cr, cg, cb, ca);
                    SDL_FRect tail = { x + 2.0f, y_end - h, w, h };
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
                    acc = (cnt_perfect*300.0 + cnt_300*300.0 + cnt_200*200.0
                         + cnt_100*100.0 + cnt_50*50.0)
                        / (total * 300.0) * 100.0;
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
            long total_notes  = (long)map.notes.size();
            double acc = 0.0;
            if (total_judged > 0) {
                acc = (cnt_perfect*300.0 + cnt_300*300.0 + cnt_200*200.0
                     + cnt_100*100.0 + cnt_50*50.0)
                    / (total_judged * 300.0) * 100.0;
            }
            long raw_score = (long)(cnt_perfect)*300*300 + (long)cnt_300*300*300
                           + (long)cnt_200*200*200 + (long)cnt_100*100*100
                           + (long)cnt_50*50*50;
            long max_score = total_notes * 300 * 300;
            long score = max_score > 0 ? (long)((double)raw_score / max_score * 1000000.0) : 0;

            const char* rank = "D";
            if      (acc >= 100.0) rank = "SS";
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
            static int settings_frame_count = 0;
            settings_frame_count++;
            if (settings_frame_count % 60 == 1)
                SDL_Log("SETTINGS_RENDER: frame %d", settings_frame_count);
            SDL_SetRenderDrawColor(renderer, 15, 15, 25, 255);
            SDL_FRect bg_full = { 0, 0, (float)win_w, (float)win_h };
            SDL_RenderFillRect(renderer, &bg_full);

            SDL_SetRenderScale(renderer, 3.0f, 3.0f);
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            SDL_RenderDebugText(renderer, 8.0f, 6.0f, "Settings");
            SDL_SetRenderScale(renderer, 1.0f, 1.0f);

            const float row_x     = 80.0f;
            const float row_w     = (float)win_w - 160.0f;
            const float row_h     = 100.0f;
            const float row_y0    = 200.0f;
            const float row_gap   = 20.0f;
            const float label_gap = 12.0f;

            // Rows: 0=music volume, 1=hitsound volume, 2=scroll speed, 3=fps cap
            const char* labels[4] = {
                "Music Volume",
                "Hitsound Volume",
                "Scroll Speed",
                "FPS Cap"
            };

            float values[4] = {
                cfg_music_vol,
                cfg_hitsound_vol,
                cfg_scroll_speed,
                (float)cfg_fps_cap
            };

            for (int i = 0; i < 4; i++) {
                float ry = row_y0 + i * (row_h + row_gap);

                // Label
                SDL_SetRenderScale(renderer, 3.0f, 3.0f);
                SDL_SetRenderDrawColor(renderer, 220, 220, 240, 255);
                SDL_RenderDebugText(renderer, row_x/3.0f, (ry - 30.0f)/3.0f, labels[i]);
                SDL_SetRenderScale(renderer, 1.0f, 1.0f);

                // Bar background
                SDL_SetRenderDrawColor(renderer, 40, 45, 60, 255);
                SDL_FRect bar = { row_x, ry, row_w, row_h };
                SDL_RenderFillRect(renderer, &bar);

                // Value fill
                float fill = 0.0f;
                if      (i == 0) fill = cfg_music_vol    / 1.0f;
                else if (i == 1) fill = cfg_hitsound_vol / 2.0f;  // 0..2 range
                else if (i == 2) fill = (cfg_scroll_speed - 1.0f) / 39.0f;  // 1.0..40.0
                else if (i == 3) {
                    int preset = 0;
                    if      (cfg_fps_cap == 60)  preset = 0;
                    else if (cfg_fps_cap == 120) preset = 1;
                    else if (cfg_fps_cap == 240) preset = 2;
                    else if (cfg_fps_cap == 0)   preset = 3;
                    fill = (preset + 0.5f) / 4.0f;
                }
                if (fill < 0.0f) fill = 0.0f;
                if (fill > 1.0f) fill = 1.0f;

                SDL_SetRenderDrawColor(renderer, 90, 160, 240, 255);
                SDL_FRect filled = { row_x, ry, row_w * fill, row_h };
                SDL_RenderFillRect(renderer, &filled);

                // Value text on the right side
                char buf[64];
                if      (i == 0) SDL_snprintf(buf, sizeof(buf), "%.2f", cfg_music_vol);
                else if (i == 1) SDL_snprintf(buf, sizeof(buf), "%.2f", cfg_hitsound_vol);
                else if (i == 2) SDL_snprintf(buf, sizeof(buf), "%.2f", cfg_scroll_speed);
                else if (i == 3) {
                    if      (cfg_fps_cap == 0)   SDL_snprintf(buf, sizeof(buf), "uncapped");
                    else                          SDL_snprintf(buf, sizeof(buf), "%d", cfg_fps_cap);
                }

                SDL_SetRenderScale(renderer, 4.0f, 4.0f);
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
                float tx = (row_x + row_w) / 4.0f - 8.0f * 8.0f;
                float ty = ry / 4.0f + (row_h / 4.0f - 8.0f) * 0.5f;
                SDL_RenderDebugText(renderer, tx, ty, buf);
                SDL_SetRenderScale(renderer, 1.0f, 1.0f);
            }

            // Back button
            SDL_SetRenderScale(renderer, 3.0f, 3.0f);
            SDL_SetRenderDrawColor(renderer, 200, 200, 100, 255);
            SDL_RenderDebugText(renderer, 8.0f,
                (float)win_h/3.0f - 30.0f, "back");
            SDL_SetRenderScale(renderer, 1.0f, 1.0f);
        }

        // ---- Song select ----
        if (app_state == AppState::SongSelect) {
            // Full-screen dark bg
            SDL_SetRenderDrawColor(renderer, 15, 15, 25, 255);
            SDL_FRect bg_full = { 0, 0, (float)win_w, (float)win_h };
            SDL_RenderFillRect(renderer, &bg_full);

            // Title bar
            SDL_SetRenderScale(renderer, 3.0f, 3.0f);
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            SDL_RenderDebugText(renderer, 8.0f, 6.0f, "osu!vinniv - song select");

            // Settings button (top-right, drawn)
            {
                const float bx = (float)win_w - 320.0f;
                const float by = 40.0f;
                const float bw = 260.0f;
                const float bh = 90.0f;

                SDL_SetRenderDrawColor(renderer, 40, 50, 70, 255);
                SDL_FRect bg = { bx, by, bw, bh };
                SDL_RenderFillRect(renderer, &bg);

                SDL_SetRenderDrawColor(renderer, 200, 200, 100, 255);
                SDL_RenderRect(renderer, &bg);

                // Text centered — "settings" is 8 chars, at scale 3 each is 24px
                SDL_SetRenderScale(renderer, 3.0f, 3.0f);
                SDL_SetRenderDrawColor(renderer, 220, 220, 140, 255);
                float text_w_px = 8.0f * 24.0f;
                float text_x = (bx + bw * 0.5f - text_w_px * 0.5f) / 3.0f;
                float text_y = (by + bh * 0.5f - 12.0f) / 3.0f;
                SDL_RenderDebugText(renderer, text_x, text_y, "settings");
                SDL_SetRenderScale(renderer, 1.0f, 1.0f);
            }

            const float row_h  = 110.0f;
            const float list_x = 60.0f;
            const float list_y = 140.0f;
            const float list_w = (float)win_w - 120.0f;

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

                for (int i = 0; i < n; i++) {
                    float ry = list_y + i * row_h - song_scroll;
                    if (ry + row_h < 0 || ry > (float)win_h) continue;

                    bool sel = (i == selected_index);

                    SDL_SetRenderDrawColor(renderer,
                        sel ? 60 : 25, sel ? 80 : 30, sel ? 130 : 40, 255);
                    SDL_FRect row = { list_x, ry, list_w, row_h - 6 };
                    SDL_RenderFillRect(renderer, &row);

                    if (sel) {
                        SDL_SetRenderDrawColor(renderer, 200, 220, 255, 255);
                        SDL_RenderRect(renderer, &row);
                    }

                    char line1[256];
                    char line2[128];
                    SDL_snprintf(line1, sizeof(line1), "%s - %s",
                                 songs[i].artist.c_str(), songs[i].title.c_str());
                    SDL_snprintf(line2, sizeof(line2), "[%s]  %dK  OD%.1f",
                                 songs[i].version.c_str(),
                                 songs[i].key_count, songs[i].od);

                    SDL_SetRenderScale(renderer, 3.0f, 3.0f);
                    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
                    SDL_RenderDebugText(renderer, list_x/3.0f + 8.0f,
                                                 ry/3.0f + 6.0f, line1);
                    SDL_SetRenderDrawColor(renderer, 180, 180, 200, 255);
                    SDL_RenderDebugText(renderer, list_x/3.0f + 8.0f,
                                                 ry/3.0f + 28.0f, line2);
                    SDL_SetRenderScale(renderer, 1.0f, 1.0f);
                }

                // SCROLLBAR — only show if content overflows
                float footer_h = 120.0f;
                float list_h   = (float)win_h - list_y - footer_h;
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
    SDL_Quit();
    return 0;
}
