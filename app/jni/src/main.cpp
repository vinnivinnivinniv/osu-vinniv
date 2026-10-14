#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <cmath>
#include <vector>
#include "parser.h"

int main(int argc, char* argv[]) {
    SDL_Log("OSUVINNIV_MAIN_ENTERED argc=%d", argc);
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
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
    SDL_SetRenderVSync(renderer, 0);

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


    // Playfield geometry (needed by both event loop and draw block)
    const int   keys        = (map.key_count > 0) ? map.key_count : 4;
    const float max_col_w   = 240.0f;
    const float col_w_raw   = (float)win_w / keys;
    const float col_w       = (col_w_raw > max_col_w) ? max_col_w : col_w_raw;
    const float playfield_w = col_w * keys;
    const float playfield_x = ((float)win_w - playfield_w) * 0.5f;
    const float scroll_pps  = 1.5f;
    const float judgement_y = win_h * 0.85f;

    Uint64 last_report = SDL_GetTicks();
    int frames = 0;
    int report_count = 0;
    bool running = true;

    // Judgment state
    std::vector<bool>   note_judged(map.notes.size(), false);
    std::vector<double> note_hit_time(map.notes.size(), -1.0);
    bool  column_pressed[16] = {false};
    int   combo = 0, max_combo = 0;
    int   cnt_300 = 0, cnt_100 = 0, cnt_50 = 0, cnt_miss = 0;
    size_t miss_cursor = 0;

    while (running) {
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
                    double best = 1e9; int best_i = -1;
                    for (size_t i = 0; i < map.notes.size(); i++) {
                        if (note_judged[i]) continue;
                        if (map.notes[i].column != col) continue;
                        double d = map.notes[i].time - (double)SDL_GetTicks();
                        if (d < -80.0) continue;
                        if (d < best) { best = d; best_i = (int)i; }
                        if (d > 80.0) break;
                    }
                    if (best_i >= 0) {
                        double a = best < 0 ? -best : best;
                        if (a <= 70.0) {
                            note_judged[best_i] = true;
                            note_hit_time[best_i] = (double)SDL_GetTicks();
                            if      (a <= 22.0) cnt_300++;
                            else if (a <= 45.0) cnt_100++;
                            else                cnt_50++;
                            combo++;
                            if (combo > max_combo) max_combo = combo;
                        }
                    }
                }
            }
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                double t = (double)SDL_GetTicks();
                int col = (int)((e.button.x - playfield_x) / col_w);
                if (col >= 0 && col < keys && col < 16) {
                    column_pressed[col] = true;
                    double best = 1e9; int best_i = -1;
                    for (size_t i = 0; i < map.notes.size(); i++) {
                        if (note_judged[i]) continue;
                        if (map.notes[i].column != col) continue;
                        double d = map.notes[i].time - t;
                        if (d < -80.0) continue;
                        if (d < best) { best = d; best_i = (int)i; }
                        if (d > 80.0) break;
                    }
                    if (best_i >= 0) {
                        double a = best < 0 ? -best : best;
                        if (a <= 70.0) {
                            note_judged[best_i] = true;
                            if      (a <= 22.0) cnt_300++;
                            else if (a <= 45.0) cnt_100++;
                            else                cnt_50++;
                            combo++;
                            if (combo > max_combo) max_combo = combo;
                        }
                    }
                }
            }
            if (e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                int col = (int)((e.button.x - playfield_x) / col_w);
                if (col >= 0 && col < keys && col < 16) column_pressed[col] = false;
            }
            if (e.type == SDL_EVENT_KEY_UP) {
                if      (e.key.key == SDLK_D) column_pressed[0] = false;
                else if (e.key.key == SDLK_F) column_pressed[1] = false;
                else if (e.key.key == SDLK_J) column_pressed[2] = false;
                else if (e.key.key == SDLK_K) column_pressed[3] = false;
            }
        }

        SDL_GetWindowSizeInPixels(window, &win_w, &win_h);
        const double now_ms = (double)SDL_GetTicks();

        // Background
        while (miss_cursor < map.notes.size() &&
               map.notes[miss_cursor].time < now_ms - 70.0) {
            if (!note_judged[miss_cursor]) {
                note_judged[miss_cursor] = true;
                cnt_miss++;
                combo = 0;
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
            float y = judgement_y - (float)((n.time - now_ms) * scroll_pps);
            if (y < -50.0f || y > win_h + 50.0f) continue;
            if (n.time > now_ms + 4000.0) break;   // notes sorted, stop early

            float x = playfield_x + n.column * col_w;
            float w = col_w - 4.0f;
            float h = 24.0f;

            if (n.is_hold) {
                float y_end = judgement_y - (float)((n.end_time - now_ms) * scroll_pps);
                float top = (y < y_end) ? y : y_end;
                float bot = (y < y_end) ? y_end : y;
                SDL_SetRenderDrawColor(renderer, 90, 180, 255, 255);
                SDL_FRect body = { x + 2.0f, top, w, bot - top };
                SDL_RenderFillRect(renderer, &body);
            }

            SDL_SetRenderDrawColor(renderer, 240, 240, 240, 255);
            SDL_FRect r = { x + 2.0f, y - h * 0.5f, w, h };
            SDL_RenderFillRect(renderer, &r);
        }

        SDL_RenderPresent(renderer);

        frames++;
        Uint64 now = SDL_GetTicks();
        if (now - last_report >= 1000) {
            report_count++;
            SDL_Log("FPS[%d]: %d  combo=%d max=%d 300=%d 100=%d 50=%d miss=%d", report_count, frames, combo, max_combo, cnt_300, cnt_100, cnt_50, cnt_miss);
            frames = 0;
            last_report = now;

            // Auto-exit after 3 minutes for testing; remove later.
        }
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
