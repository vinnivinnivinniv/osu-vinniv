#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <cmath>
#include "parser.h"

int main(int argc, char* argv[]) {
    SDL_Log("OSUVINNIV_MAIN_ENTERED argc=%d", argc);
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");

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


    Uint64 last_report = SDL_GetTicks();
    int frames = 0;
    int report_count = 0;
    bool running = true;

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
        }

        SDL_GetWindowSizeInPixels(window, &win_w, &win_h);

        // Playfield layout
        const float judgement_y = win_h * 0.85f;
        const int   keys        = (map.key_count > 0) ? map.key_count : 4;
        const float max_col_w   = 120.0f;   // cap column width
        const float col_w_raw   = (float)win_w / keys;
        const float col_w       = (col_w_raw > max_col_w) ? max_col_w : col_w_raw;
        const float playfield_w = col_w * keys;
        const float playfield_x = ((float)win_w - playfield_w) * 0.5f;
        const float scroll_pps  = 1.5f;
        const double now_ms     = (double)SDL_GetTicks();

        // Background
        SDL_SetRenderDrawColor(renderer, 20, 20, 30, 255);
        SDL_RenderClear(renderer);

        // Column separators
        SDL_SetRenderDrawColor(renderer, 60, 60, 80, 255);
        for (int c = 1; c < keys; c++) {
            float x = playfield_x + c * col_w;
            SDL_RenderLine(renderer, x, 0, x, (float)win_h);
        }

        // Judgement line
        SDL_SetRenderDrawColor(renderer, 220, 220, 220, 255);
        SDL_RenderLine(renderer, playfield_x, judgement_y, playfield_x + playfield_w, judgement_y);

        // Notes
        for (const auto& n : map.notes) {
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
            SDL_Log("FPS[%d]: %d  (window %dx%d)", report_count, frames, win_w, win_h);
            frames = 0;
            last_report = now;

            // Auto-exit after 3 minutes for testing; remove later.
        }
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
