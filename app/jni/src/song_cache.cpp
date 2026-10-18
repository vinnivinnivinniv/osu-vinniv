#include "song_cache.h"

#include <SDL3/SDL.h>
#include <sys/stat.h>
#include <cstdlib>
#include <fstream>

static std::string esc(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '|')       out += "\\p";
        else if (c == '\n') out += "\\n";
        else                out.push_back(c);
    }
    return out;
}

static std::string unesc(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            if (s[i+1] == 'p') { out.push_back('|'); i++; continue; }
            if (s[i+1] == 'n') { out.push_back('\n'); i++; continue; }
        }
        out.push_back(s[i]);
    }
    return out;
}

static void mkdir_p(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); i++) {
        cur.push_back(path[i]);
        if (path[i] == '/' && i > 0) mkdir(cur.c_str(), 0777);
    }
    mkdir(path.c_str(), 0777);
}

bool load_song_cache(const std::string& path, time_t expected_mtime,
                     std::vector<SongEntry>& out) {
    std::ifstream f(path);
    if (!f.is_open()) return false;

    std::string line;
    if (!std::getline(f, line)) return false;
    if (line.size() < 3) return false;
    if (line.compare(0, 2, "v5") != 0) return false;
    long long cached_mtime = std::atoll(line.c_str() + 3);
    if ((long long)expected_mtime != cached_mtime) return false;

    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::vector<std::string> parts;
        std::string cur;
        for (char c : line) {
            if (c == '|') { parts.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        parts.push_back(cur);
        if (parts.size() < 11) continue;

        SongEntry e;
        e.folder     = unesc(parts[0]);
        e.osu_path   = unesc(parts[1]);
        e.artist     = unesc(parts[2]);
        e.title      = unesc(parts[3]);
        e.version    = unesc(parts[4]);
        e.key_count  = std::atoi(parts[5].c_str());
        e.od         = std::atof(parts[6].c_str());
        e.audio_path = unesc(parts[7]);
        e.bg_path    = unesc(parts[8]);
        e.preview_time = std::atoi(parts[9].c_str());
        e.video_path = unesc(parts[10]);
        out.push_back(e);
    }
    return true;
}

void save_song_cache(const std::string& path, time_t mtime,
                     const std::vector<SongEntry>& entries) {
    auto slash = path.find_last_of('/');
    if (slash != std::string::npos) mkdir_p(path.substr(0, slash));

    std::ofstream f(path);
    if (!f.is_open()) return;

    f << "v5 " << (long long)mtime << "\n";
    for (const auto& e : entries) {
        f << esc(e.folder)     << "|"
          << esc(e.osu_path)   << "|"
          << esc(e.artist)     << "|"
          << esc(e.title)      << "|"
          << esc(e.version)    << "|"
          << e.key_count       << "|"
          << e.od              << "|"
          << esc(e.audio_path) << "|"
          << esc(e.bg_path)    << "|"
          << e.preview_time    << "|"
          << esc(e.video_path) << "\n";
    }
}
