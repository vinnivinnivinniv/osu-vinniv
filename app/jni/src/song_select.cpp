#include "song_select.h"
#include "parser.h"

#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <fstream>

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
    if (!parse_osu_file(osu_path, m)) return false;

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

    if (!m.audio_file.empty()) {
        // Use whatever the .osu declares. Now that dr_mp3 handles mp3
        // on Android, both .wav and .mp3 work.
        std::string declared = join(folder, m.audio_file);

        // Sanity: if declared doesn't exist, try a .wav swap in case the
        // original was converted at import time.
        struct stat st;
        if (stat(declared.c_str(), &st) == 0) {
            e.audio_path = declared;
        } else {
            auto dot = m.audio_file.find_last_of('.');
            std::string base = (dot != std::string::npos)
                             ? m.audio_file.substr(0, dot)
                             : m.audio_file;
            e.audio_path = join(folder, base + ".wav");
        }
    } else {
        e.audio_path = join(folder, "audio.wav");
    }
    return true;
}

std::vector<SongEntry> scan_songs(const std::string& root) {
    std::vector<SongEntry> out;
    DIR* d = opendir(root.c_str());
    if (!d) return out;

    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        std::string name = de->d_name;
        if (name == "." || name == "..") continue;

        std::string set_dir = join(root, name);
        if (!is_dir(set_dir)) continue;

        // Walk .osu files inside this set folder
        DIR* sd = opendir(set_dir.c_str());
        if (!sd) continue;

        struct dirent* sde;
        while ((sde = readdir(sd)) != nullptr) {
            std::string fname = sde->d_name;
            if (!ends_with(fname, ".osu")) continue;
            std::string osu_path = join(set_dir, fname);

            SongEntry e;
            e.folder = set_dir;
            if (read_meta(osu_path, e))
                out.push_back(e);
        }
        closedir(sd);
    }
    closedir(d);

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
