#include "song_select.h"
#include <SDL3/SDL.h>
#include "parser.h"
#include "song_cache.h"
#include "converter.h"
#include "osz_import.h"
#include <atomic>
#include <mutex>
#include <thread>

#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <fstream>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <cctype>
#include <cstring>

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
    // Full parse (not metadata-only) so we have notes for star
    // computation. Slower scan but accurate density-based ratings.
    if (!parse_osu_file(osu_path, m, /* metadata_only = */ false)) return false;

    // Accept all modes. Display filter (Show converts toggle in main.cpp)
    // hides non-mania entries by default. Playback of converts uses a
    // placeholder 4K converter in main.cpp until the real converter lands.

    e.osu_path = osu_path;
    e.title    = m.title;
    e.artist   = m.artist;
    e.version  = m.version;
    e.key_count = m.key_count;
    e.od        = m.od;
    e.preview_time = m.preview_time;
    e.original_mode = m.mode;
    if (m.mode != 3) {
        // Compute the column count the converter will actually use.
        // Same heuristic runs at play time; this keeps display and
        // play consistent.
        e.key_count = preview_column_count(m);
    }

    // Placeholder star rating. NOT the real osu! diffcalc (see
    // docs/mechanics.md for the port plan). Rough approximation from
    // note density + OD + key count. Real diffcalc is weeks of work.
    if (m.notes.size() >= 2) {
        double first_t = m.notes.front().time;
        double last_t  = 0.0;
        for (const auto& n : m.notes) {
            if (n.end_time > last_t) last_t = n.end_time;
        }
        double dur_s = (last_t - first_t) / 1000.0;
        if (dur_s < 1.0) dur_s = 1.0;
        double nps = (double)m.notes.size() / dur_s;
        double s = 0.5
                 + std::sqrt(nps) * 1.3
                 + (m.od - 5.0) * 0.1
                 + (e.key_count - 4) * 0.08;
        if (s < 0.5)  s = 0.5;
        if (s > 12.0) s = 12.0;
        e.stars = s;
    }

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
            candidates.push_back(join(folder, base + ".ogg"));
            candidates.push_back(join(folder, base + ".mp3"));
            candidates.push_back(join(folder, base + ".wav"));
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
        // Background resolution. Some maps declare a video (mp4/webm)
        // in [Events], either via Video,offset,"..." or as 0,0,"...".
        // stbi cannot decode those, so skip video extensions and try
        // extension variants of the declared filename plus known defaults.
        {
            auto is_video = [](const std::string& s) -> bool {
                auto dot = s.find_last_of('.');
                if (dot == std::string::npos) return false;
                std::string ext = s.substr(dot);
                for (auto& c : ext) c = (char)::tolower(c);
                return ext == ".mp4" || ext == ".webm" || ext == ".avi"
                    || ext == ".flv" || ext == ".mov" || ext == ".mkv";
            };
            auto is_video_basename = [&](const std::string& s) -> bool {
                std::string lc = s;
                for (auto& c : lc) c = (char)::tolower(c);
                return lc.rfind("bga.", 0) == 0
                    || lc.rfind("video.", 0) == 0
                    || lc.rfind("bgvideo.", 0) == 0;
            };

            std::vector<std::string> bg_candidates;
            if (!m.bg_file.empty() && !is_video(m.bg_file)) {
                bg_candidates.push_back(join(folder, m.bg_file));
                // Try same basename with common image extensions
                auto dot = m.bg_file.find_last_of('.');
                if (dot != std::string::npos) {
                    std::string base = m.bg_file.substr(0, dot);
                    for (const char* ext : {".jpg",".png",".jpeg",".JPG",".PNG"})
                        bg_candidates.push_back(join(folder, base + ext));
                }
            }
            bg_candidates.push_back(join(folder, "bg.jpg"));
            bg_candidates.push_back(join(folder, "BG.jpg"));
            bg_candidates.push_back(join(folder, "bg.png"));
            bg_candidates.push_back(join(folder, "BG.png"));
            bg_candidates.push_back(join(folder, "cover.jpg"));
            bg_candidates.push_back(join(folder, "cover.png"));

            struct stat bst;
            for (const auto& c : bg_candidates) {
                if (is_video(c)) continue;
                if (is_video_basename(c.substr(c.find_last_of('/') + 1))) continue;
                if (stat(c.c_str(), &bst) == 0) { e.bg_path = c; break; }
            }

            // Last-resort: scan the folder for any image file. Some mappers
            // name the bg arbitrarily (e.g. "vi.jpg", "Myon.jpg") with no
            // matching Events line we recognise. First match wins.
            if (e.bg_path.empty()) {
                auto has_ext = [](const std::string& s, const char* suf) -> bool {
                    size_t sl = strlen(suf);
                    if (s.size() < sl) return false;
                    std::string tail = s.substr(s.size() - sl);
                    for (auto& c : tail) c = (char)::tolower(c);
                    return tail == suf;
                };
                DIR* bd = opendir(folder.c_str());
                if (bd) {
                    struct dirent* bde;
                    while ((bde = readdir(bd)) != nullptr) {
                        std::string n = bde->d_name;
                        if (!has_ext(n, ".jpg") && !has_ext(n, ".png") &&
                            !has_ext(n, ".jpeg")) continue;
                        std::string c = join(folder, n);
                        if (stat(c.c_str(), &bst) == 0) { e.bg_path = c; break; }
                    }
                    closedir(bd);
                }
            }
        }

        // Video resolution — same folder as the .osu, using declared filename.
        // Extension-swap fallback matches the bg resolver behaviour.
        if (!m.video_file.empty()) {
            std::vector<std::string> vcandidates;
            vcandidates.push_back(join(folder, m.video_file));
            auto dot = m.video_file.find_last_of('.');
            if (dot != std::string::npos) {
                std::string base = m.video_file.substr(0, dot);
                for (const char* ext : {".mp4", ".avi", ".flv", ".MP4", ".AVI", ".FLV"})
                    vcandidates.push_back(join(folder, base + ext));
            }
            struct stat vst;
            for (const auto& c : vcandidates) {
                if (stat(c.c_str(), &vst) == 0) { e.video_path = c; break; }
            }
        }

        if (e.audio_path.empty()) {
            SDL_Log("SONGS: no audio found in %s (declared '%s')",
                    folder.c_str(), m.audio_file.c_str());
            // List what audio-ish files are actually in the folder
            DIR* ad = opendir(folder.c_str());
            if (ad) {
                struct dirent* ade;
                while ((ade = readdir(ad)) != nullptr) {
                    std::string n = ade->d_name;
                    if (ends_with(n, ".mp3") || ends_with(n, ".wav") ||
                        ends_with(n, ".ogg") || ends_with(n, ".MP3") ||
                        ends_with(n, ".WAV") || ends_with(n, ".OGG")) {
                        SDL_Log("SONGS:   actual audio file: %s", n.c_str());
                    }
                }
                closedir(ad);
            }
        } else {
            SDL_Log("SONGS: audio resolution -> %s", e.audio_path.c_str());
        }
    }
    return true;
}

std::vector<SongEntry> scan_songs(const std::string& root,
                                  std::atomic<int>* progress,
                                  std::atomic<int>* total) {
    if (progress) progress->store(0);
    if (total)    total->store(0);
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
        // Cache file is written before the cold-path sort, so it holds
        // scan order. Sort here so warm loads match cold loads.
        std::sort(cached.begin(), cached.end(), [](const SongEntry& a, const SongEntry& b) {
            if (a.title  != b.title)  return a.title  < b.title;
            if (a.artist != b.artist) return a.artist < b.artist;
            double sa = std::round(a.stars * 100.0) / 100.0;
            double sb = std::round(b.stars * 100.0) / 100.0;
            if (sa != sb) return sa < sb;
            return a.version < b.version;
        });
        return cached;
    }

    SDL_Log("SCAN: start root=%s (cache miss)", root.c_str());
    std::vector<SongEntry> out;
    DIR* d = opendir(root.c_str());
    if (!d) return out;

    // Optional pre-pass: count .osu files so the UI can render a
    // determinate progress bar. Directory I/O only — no parsing.
    if (total) {
        int c = 0;
        DIR* cd = opendir(root.c_str());
        if (cd) {
            struct dirent* cde;
            while ((cde = readdir(cd)) != nullptr) {
                std::string n = cde->d_name;
                if (n == "." || n == "..") continue;
                std::string sd = join(root, n);
                if (!is_dir(sd)) continue;
                DIR* csd = opendir(sd.c_str());
                if (!csd) continue;
                struct dirent* csde;
                while ((csde = readdir(csd)) != nullptr) {
                    if (ends_with(csde->d_name, ".osu")) c++;
                }
                closedir(csd);
            }
            closedir(cd);
        }
        total->store(c);
        SDL_Log("SCAN: pre-pass counted %d .osu files", c);
    }

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
            if (progress) progress->fetch_add(1);
        }
        closedir(sd);
    }
    closedir(d);

    Uint64 t_end = SDL_GetTicks();
    SDL_Log("SCAN: walked %d dirs, %d osu files, %llu ms",
            dir_count, file_count, (unsigned long long)(t_end - t_start));

    save_song_cache(cache_path, root_mtime, out);
    SDL_Log("CACHE: saved %zu entries", out.size());

    // Primary sort: title (lazer default), then artist, then difficulty.
    // Title-first sort. Groups all difficulties of a song together
    // (Agate 2.5, Agate 4.5, Beowulf 1.5, Beowulf 9.5) instead of
    // interleaving songs by star tier. Within a title, difficulty
    // ascends by star rating.
    std::sort(out.begin(), out.end(), [](const SongEntry& a, const SongEntry& b) {
        if (a.title  != b.title)  return a.title  < b.title;
        if (a.artist != b.artist) return a.artist < b.artist;
        double sa = std::round(a.stars * 100.0) / 100.0;
        double sb = std::round(b.stars * 100.0) / 100.0;
        if (sa != sb) return sa < sb;
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

// ---- SongScanState -------------------------------------------------------

SongScanState::~SongScanState() { shutdown(); }

void SongScanState::start(const std::string& songs_root, const std::string& inbox_root) {
    if (worker.joinable()) return;
    worker = std::thread([this, songs_root, inbox_root]() {
        Uint64 t0 = SDL_GetTicks();
        SDL_Log("SCAN: worker start");
        import_all_osz(inbox_root, songs_root);
        auto entries = scan_songs(songs_root, &progress, &total);
        size_t n = entries.size();
        {
            std::lock_guard<std::mutex> lk(mtx);
            result = std::move(entries);
        }
        complete_flag.store(true);
        SDL_Log("SCAN: worker done in %llu ms (%zu entries)",
                (unsigned long long)(SDL_GetTicks() - t0), n);
    });
}

bool SongScanState::try_consume(std::vector<SongEntry>& out) {
    if (!complete_flag.load()) return false;
    std::lock_guard<std::mutex> lk(mtx);
    if (consumed) return false;
    consumed = true;
    out = std::move(result);
    return true;
}

void SongScanState::shutdown() {
    if (!worker.joinable()) return;
    worker.join();
}
