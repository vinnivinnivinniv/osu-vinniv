#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <cmath>
#include <fstream>
#include <vector>
#include "parser.h"

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
    if (cfg_fps_cap == 120) SDL_SetRenderVSync(renderer, 1);
    else                    SDL_SetRenderVSync(renderer, 0);

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
    if (SDL_LoadWAV(audio_path, &wav_spec, &wav_data, &wav_len)) {
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
    if (SDL_LoadWAV("/sdcard/Android/data/sh.ppy.osuvinniv/files/hit.wav",
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
    if (SDL_LoadWAV("/sdcard/Android/data/sh.ppy.osuvinniv/files/miss.wav",
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

    const Uint64 audio_start_tick = SDL_GetTicks() + 2000;  // 2s lead-in
    bool audio_queued = false;

    SDL_GetWindowSizeInPixels(window, &win_w, &win_h);

    // Playfield geometry (needed by both event loop and draw block)
    const int   keys        = (map.key_count > 0) ? map.key_count : 4;
    const float max_col_w   = 240.0f;
    const float col_w_raw   = (float)win_w / keys;
    const float col_w       = (col_w_raw > max_col_w) ? max_col_w : col_w_raw;
    const float playfield_w = col_w * keys;
    const float playfield_x = ((float)win_w - playfield_w) * 0.5f;
    const float scroll_pps  = cfg_scroll_speed;
    const float judgement_y = win_h * 0.85f;

    // Judgment state
    std::vector<bool>   note_judged(map.notes.size(), false);
    std::vector<double> note_hit_time(map.notes.size(), -1.0);
    bool  column_pressed[16] = {false};
    int   combo = 0, max_combo = 0;
    int   cnt_300 = 0, cnt_100 = 0, cnt_50 = 0, cnt_miss = 0;
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
        double best = 1e9; int best_i = -1;
        for (size_t i = 0; i < map.notes.size(); i++) {
            if (note_judged[i]) continue;
            if (map.notes[i].column != col) continue;
            double d = map.notes[i].time - map_t;
            if (d < -80.0) continue;
            if (d < best) { best = d; best_i = (int)i; }
            if (d > 80.0) break;
        }
        if (best_i < 0) return false;
        double a = best < 0 ? -best : best;
        if (a > 70.0) return false;
        if (map.notes[best_i].is_hold) {
            hold_started[best_i] = true;
            hold_active[best_i]  = true;
            active_holds.push_back(best_i);
        } else {
            note_judged[best_i] = true;
        }
        if      (a <= 22.0) cnt_300++;
        else if (a <= 45.0) cnt_100++;
        else                cnt_50++;
        combo++;
        if (combo > max_combo) max_combo = combo;
        return true;
    };


    Uint64 last_report = SDL_GetTicks();
    int frames = 0;
    int report_count = 0;
    bool running = true;




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

    while (running) {
        Uint64 frame_start = SDL_GetTicksNS() / 1000ULL;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT ||
                e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                running = false;
            }
            if (e.type == SDL_EVENT_KEY_DOWN &&
                e.key.key == SDLK_AC_BACK) {
                running = false;
            }

            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
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
            if (e.type == SDL_EVENT_FINGER_DOWN) {
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
            if (e.type == SDL_EVENT_FINGER_UP || e.type == SDL_EVENT_FINGER_CANCELED) {
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
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                int col = find_col(e.button.x);
                if (col >= 0) {
                    column_pressed[col] = true;
                    play_sound(hit_stream, hit_data, hit_len);
                    judge_col(col, (double)SDL_GetTicks());
                }
            }
            if (e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
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
        if (!audio_queued && audio_stream && current_tick >= audio_start_tick) {
            if (SDL_PutAudioStreamData(audio_stream, wav_data, wav_len)) {
                SDL_ResumeAudioStreamDevice(audio_stream);
                audio_queued = true;
                SDL_Log("AUDIO: playback started");
            }
        }
        // Signed: negative during lead-in, positive during play
        const double now_ms = (double)current_tick - (double)audio_start_tick;

        // Background
        // Complete any holds whose tail has passed
        {
            double nmap = (double)SDL_GetTicks() - (double)audio_start_tick;
            for (size_t i = 0; i < active_holds.size(); ) {
                size_t hi = active_holds[i];
                if (nmap > map.notes[hi].end_time) {
                    hold_active[hi]  = false;
                    note_judged[hi]  = true;
                    release_time[hi] = nmap;
                    active_holds.erase(active_holds.begin() + i);
                } else i++;
            }
        }

        while (miss_cursor < map.notes.size() &&
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
        for (size_t ni = 0; ni < map.notes.size(); ni++) {
            const auto& n = map.notes[ni];
            if (note_judged[ni]) continue;
            if (n.time > now_ms + 4000.0) break;   // notes sorted, stop early

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

                char buf[128];
                int x = 6, y = 6, lh = 14;

                SDL_snprintf(buf, sizeof(buf), "Combo: %d", combo);
                SDL_RenderDebugText(renderer, (float)x, (float)y, buf); y += lh;

                SDL_snprintf(buf, sizeof(buf), "Max: %d", max_combo);
                SDL_RenderDebugText(renderer, (float)x, (float)y, buf); y += lh;

                SDL_snprintf(buf, sizeof(buf), "300:%d 100:%d 50:%d Miss:%d",
                             cnt_300, cnt_100, cnt_50, cnt_miss);
                SDL_RenderDebugText(renderer, (float)x, (float)y, buf); y += lh;

                int total = cnt_300 + cnt_100 + cnt_50;
                double acc = 0.0;
                if (total + cnt_miss > 0)
                    acc = (cnt_300*300.0 + cnt_100*100.0 + cnt_50*50.0)
                        / ((total + cnt_miss) * 300.0) * 100.0;
                SDL_snprintf(buf, sizeof(buf), "Acc: %.2f%%  FPS: %d", acc, hud_fps);
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
        if (cfg_fps_cap > 0 && cfg_fps_cap != 120) {
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

        SDL_RenderPresent(renderer);

        frames++;
        Uint64 now = SDL_GetTicks();
        if (now - last_report >= 1000) {
            report_count++;
            SDL_Log("FPS[%d]: %d  combo=%d max=%d 300=%d 100=%d 50=%d miss=%d", report_count, frames, combo, max_combo, cnt_300, cnt_100, cnt_50, cnt_miss);
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
    if (hud_tex) SDL_DestroyTexture(hud_tex);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
