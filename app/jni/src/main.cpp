#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <GLES3/gl3.h>
#include <cmath>
#include "parser.h"

int main(int argc, char* argv[]) {
    SDL_Log("OSUVINNIV_MAIN_ENTERED argc=%d", argc);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window* window = SDL_CreateWindow(
        "osu!vinniv",
        0, 0,
        SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return 1;
    }

    SDL_GLContext gl = SDL_GL_CreateContext(window);
    if (!gl) {
        SDL_Log("SDL_GL_CreateContext failed: %s", SDL_GetError());
        return 1;
    }

    // Disable vsync so we measure raw loop throughput.
    SDL_GL_SetSwapInterval(0);

    SDL_Log("GL_VERSION:  %s", glGetString(GL_VERSION));
    SDL_Log("GL_RENDERER: %s", glGetString(GL_RENDERER));
    SDL_Log("GL_VENDOR:   %s", glGetString(GL_VENDOR));

    int win_w = 0, win_h = 0;
    SDL_GetWindowSizeInPixels(window, &win_w, &win_h);
    SDL_Log("Window size: %dx%d", win_w, win_h);

    {
        Beatmap map;
        const char* map_path = "/sdcard/Android/data/sh.ppy.osuvinniv/files/map.osu";
        if (parse_osu_file(map_path, map)) {
            SDL_Log("MAP: %s - %s [%s]", map.artist.c_str(), map.title.c_str(), map.version.c_str());
            SDL_Log("  keys=%d  notes=%zu", map.key_count, map.notes.size());
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

        // Cycle hue so we can see it's alive.
        float t = SDL_GetTicks() / 1000.0f;
        float r = 0.5f + 0.5f * sinf(t);
        float g = 0.5f + 0.5f * sinf(t + 2.094f);
        float b = 0.5f + 0.5f * sinf(t + 4.188f);

        SDL_GetWindowSizeInPixels(window, &win_w, &win_h);
        glViewport(0, 0, win_w, win_h);
        glClearColor(r, g, b, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        SDL_GL_SwapWindow(window);

        frames++;
        Uint64 now = SDL_GetTicks();
        if (now - last_report >= 1000) {
            report_count++;
            SDL_Log("FPS[%d]: %d  (window %dx%d)", report_count, frames, win_w, win_h);
            frames = 0;
            last_report = now;

            // Stop after 10 seconds so we get a bounded sample.
            if (report_count >= 10) running = false;
        }
    }

    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
