#include "parser.h"

// Forward-declared to avoid pulling in SDL in this translation unit.
// Linker resolves against real SDL_Log on Android; tests stub it.
extern "C" void SDL_Log(const char* fmt, ...);

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

bool parse_osu_file(const std::string& path, Beatmap& out) {
    std::ifstream f(path);
    if (!f.is_open()) {
        SDL_Log("parse_osu_file: cannot open %s", path.c_str());
        return false;
    }

    std::string section;
    std::string line;
    bool first_line = true;

    while (std::getline(f, line)) {
        if (first_line) {
            first_line = false;
            if (line.size() >= 3 &&
                (unsigned char)line[0] == 0xEF &&
                (unsigned char)line[1] == 0xBB &&
                (unsigned char)line[2] == 0xBF) {
                line = line.substr(3);
            }
        }

        std::string t = trim(line);
        if (t.empty()) continue;
        if (t.size() > 1 && t[0] == '/' && t[1] == '/') continue;

        if (t.front() == '[' && t.back() == ']') {
            section = t.substr(1, t.size() - 2);
            continue;
        }

        if (section == "Metadata") {
            auto pos = t.find(':');
            if (pos == std::string::npos) continue;
            std::string k = trim(t.substr(0, pos));
            std::string v = trim(t.substr(pos + 1));
            if (k == "Title") out.title = v;
            else if (k == "Artist") out.artist = v;
            else if (k == "Version") out.version = v;
        }

        else if (section == "TimingPoints") {
            // Format: time,beatLength,meter,sampleSet,sampleIndex,volume,uninherited,effects
            // We only use the first uninherited point (beatLength > 0).
            std::vector<std::string> parts;
            std::stringstream ss(t);
            std::string p;
            while (std::getline(ss, p, ',')) parts.push_back(p);
            if (parts.size() >= 2 && out.bpm == 0.0) {
                try {
                    double beat_len = std::stod(parts[1]);
                    if (beat_len > 0.0) out.bpm = 60000.0 / beat_len;
                } catch (...) {}
            }
        }
        else if (section == "Difficulty") {
            auto pos = t.find(':');
            if (pos == std::string::npos) continue;
            std::string k = trim(t.substr(0, pos));
            std::string v = trim(t.substr(pos + 1));
            if (k == "CircleSize") {
                try { out.key_count = (int)std::stod(v); } catch (...) {}
            }
            else if (k == "OverallDifficulty") {
                try { out.od = std::stod(v); } catch (...) {}
            }
        }
        else if (section == "HitObjects") {
            std::vector<std::string> parts;
            std::stringstream ss(t);
            std::string p;
            while (std::getline(ss, p, ',')) parts.push_back(p);
            if (parts.size() < 5) continue;

            Note n{};
            try {
                int raw_x = std::stoi(parts[0]);
                n.time    = std::stod(parts[2]);
                int type  = std::stoi(parts[3]);
                n.is_hold = (type & 128) != 0;
                n.end_time = n.time;
                n.column = raw_x;
                if (parts.size() >= 5)
                    n.hit_sound = std::stoi(parts[4]);
                else
                    n.hit_sound = 0;
            } catch (...) { continue; }

            if (n.is_hold && parts.size() >= 6) {
                // Field 5 is "endTime:hitSample" — endTime is the first
                // colon-delimited value.
                const std::string& tail = parts[5];
                auto colon = tail.find(':');
                std::string endStr = (colon == std::string::npos)
                                     ? tail
                                     : tail.substr(0, colon);
                try { n.end_time = std::stod(endStr); }
                catch (...) { n.end_time = n.time; }
            }
            out.notes.push_back(n);
        }
    }

    if (out.key_count <= 0) {
        SDL_Log("parse_osu_file: missing or invalid CircleSize");
        return false;
    }

    for (auto& n : out.notes) {
        int col = (int)std::floor((double)n.column * out.key_count / 512.0);
        if (col < 0) col = 0;
        if (col >= out.key_count) col = out.key_count - 1;
        n.column = col;
    }

    std::sort(out.notes.begin(), out.notes.end(),
              [](const Note& a, const Note& b) { return a.time < b.time; });

    return true;
}
