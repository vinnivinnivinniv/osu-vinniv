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
        // Prefer .wav over the declared extension — SDL3 can't decode MP3,
        // and we convert to wav during import.
        std::string declared = join(folder, m.audio_file);
        auto dot = m.audio_file.find_last_of('.');
        std::string base = (dot != std::string::npos)
                         ? m.audio_file.substr(0, dot)
                         : m.audio_file;
        std::string wav_path = join(folder, base + ".wav");

        struct stat st;
        if (stat(wav_path.c_str(), &st) == 0) {
            e.audio_path = wav_path;
        } else {
            e.audio_path = declared;  // fall back to declared (may fail if mp3)
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
    return out;
}
