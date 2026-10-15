#include "song_select.h"
#include <SDL3/SDL.h>
#include "parser.h"
#include "song_cache.h"

#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <fstream>
#include <cstdlib>
#include <ctime>

static bool is_dir(const std::string& p) {
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

static bool ends_with(const std::string& s, const std::string& suf) {
    if (s.size() < suf.size()) return false;
    return s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

static std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

// Parse only the metadata section quickly — we don't need full notes.
// Uses the full parser but ignores HitObjects count.
static bool read_meta(const std::string& osu_path, SongEntry& e) {
    Beatmap m;
    if (!parse_osu_file(osu_path, m, /* metadata_only = */ true)) return false;

    // Only import native mania maps. Std/taiko/catch maps would need a
    // real ruleset converter to play correctly.
    if (m.mode != 3) return false;

    e.osu_path = osu_path;
    e.title    = m.title;
    e.artist   = m.artist;
    e.version  = m.version;
    e.key_count = m.key_count;
    e.od        = m.od;

    // Resolve audio path relative to the .osu folder
    std::string folder = osu_path;
    auto slash = folder.find_last_of('/');
    if (slash != std::string::npos) folder = folder.substr(0, slash);

    // Audio resolution — try declared filename first, then extensions
    // we can decode. BeatmapExporter may rename audio during export
    // (e.g. audio.ogg -> audio.mp3) while the .osu still declares the old name.
    {
        std::vector<std::string> candidates;
        if (!m.audio_file.empty()) {
            candidates.push_back(join(folder, m.audio_file));

            auto dot = m.audio_file.find_last_of('.');
            std::string base = (dot != std::string::npos)
                             ? m.audio_file.substr(0, dot)
                             : m.audio_file;
            candidates.push_back(join(folder, base + ".mp3"));
            candidates.push_back(join(folder, base + ".wav"));
            candidates.push_back(join(folder, base + ".ogg"));
        }
        // Last-resort defaults
        candidates.push_back(join(folder, "audio.mp3"));
        candidates.push_back(join(folder, "audio.wav"));
        candidates.push_back(join(folder, "audio.ogg"));

        struct stat st;
        for (const auto& c : candidates) {
            if (stat(c.c_str(), &st) == 0) {
                e.audio_path = c;
                break;
            }
        }
        if (e.audio_path.empty()) {
            SDL_Log("SONGS: no audio found in %s (declared '%s')",
                    folder.c_str(), m.audio_file.c_str());
        }
    }
    return true;
}

std::vector<SongEntry> scan_songs(const std::string& root) {
    // Cache lives next to the songs dir, in files/cache/
    std::string cache_path = root;
    {
        auto pos = cache_path.find_last_of('/');
        if (pos != std::string::npos) cache_path = cache_path.substr(0, pos);
    }
    cache_path += "/cache/songs.index";

    // Validity: songs dir mtime must match what we stored.
    struct stat root_stat;
    time_t root_mtime = 0;
    if (stat(root.c_str(), &root_stat) == 0) root_mtime = root_stat.st_mtime;

    std::vector<SongEntry> cached;
    if (load_song_cache(cache_path, root_mtime, cached)) {
        SDL_Log("CACHE: loaded %zu entries", cached.size());
        return cached;
    }

    SDL_Log("SCAN: start root=%s (cache miss)", root.c_str());
    std::vector<SongEntry> out;
    DIR* d = opendir(root.c_str());
    if (!d) return out;

    int dir_count = 0;
    int file_count = 0;
    Uint64 t_start = SDL_GetTicks();

    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        std::string name = de->d_name;
        if (name == "." || name == "..") continue;

        std::string set_dir = join(root, name);
        if (!is_dir(set_dir)) continue;

        // Walk .osu files inside this set folder
        DIR* sd = opendir(set_dir.c_str());
        if (!sd) continue;

        dir_count++;
        struct dirent* sde;
        while ((sde = readdir(sd)) != nullptr) {
            std::string fname = sde->d_name;
            if (!ends_with(fname, ".osu")) continue;
            file_count++;
            std::string osu_path = join(set_dir, fname);

            SongEntry e;
            e.folder = set_dir;
            if (read_meta(osu_path, e))
                out.push_back(e);
        }
        closedir(sd);
    }
    closedir(d);

    Uint64 t_end = SDL_GetTicks();
    SDL_Log("SCAN: walked %d dirs, %d osu files, %llu ms",
            dir_count, file_count, (unsigned long long)(t_end - t_start));

    save_song_cache(cache_path, root_mtime, out);
    SDL_Log("CACHE: saved %zu entries", out.size());

    std::sort(out.begin(), out.end(), [](const SongEntry& a, const SongEntry& b) {
        if (a.artist != b.artist) return a.artist < b.artist;
        if (a.title  != b.title)  return a.title  < b.title;
        return a.version < b.version;
    });

    // dedupe by (artist, title, version) — keep the first occurrence
    std::vector<SongEntry> unique;
    unique.reserve(out.size());
    for (auto& e : out) {
        bool dup = false;
        for (auto& u : unique) {
            if (u.artist  == e.artist  &&
                u.title   == e.title   &&
                u.version == e.version) { dup = true; break; }
        }
        if (!dup) unique.push_back(e);
    }
    return unique;
}
