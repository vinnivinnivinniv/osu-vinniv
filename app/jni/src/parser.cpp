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

bool parse_osu_file(const std::string& path, Beatmap& out, bool metadata_only) {
    std::ifstream f(path);
    // Timing-point state — persists across lines, updated as points are seen.
    double tp_cur_beat_length = 500.0;   // default 120 BPM until first uninherited
    double tp_cur_velocity    = 1.0;
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

        if (section == "General") {
            auto pos = t.find(':');
            if (pos == std::string::npos) continue;
            std::string k = trim(t.substr(0, pos));
            std::string v = trim(t.substr(pos + 1));
            if (k == "AudioFilename") out.audio_file = v;
            else if (k == "PreviewTime") {
                try { out.preview_time = std::stoi(v); } catch (...) {}
            }
            else if (k == "Mode") {
                try { out.mode = std::stoi(v); } catch (...) {}
            }
            else if (k == "SampleSet") {
                // "Normal" / "Soft" / "Drum"
                if      (v == "Normal" || v == "normal") out.sample_set = 1;
                else if (v == "Soft"   || v == "soft")   out.sample_set = 2;
                else if (v == "Drum"   || v == "drum")   out.sample_set = 3;
            }
        }
        else if (section == "Metadata") {
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
            // beatLength > 0  -> uninherited (BPM change); resets SV velocity to 1.0
            // beatLength < 0  -> inherited (SV change); SV = -100 / beatLength
            std::vector<std::string> parts;
            std::stringstream ss(t);
            std::string p;
            while (std::getline(ss, p, ',')) parts.push_back(p);
            if (parts.size() >= 2) {
                try {
                    double tp_time  = std::stod(parts[0]);
                    double beat_len = std::stod(parts[1]);
                    bool   uninherited = (beat_len > 0.0);
                    if (uninherited) {
                        if (out.bpm == 0.0) out.bpm = 60000.0 / beat_len;
                        tp_cur_beat_length = beat_len;
                        tp_cur_velocity    = 1.0;
                    } else if (beat_len < 0.0) {
                        tp_cur_velocity = -100.0 / beat_len;
                    }
                    TimingPoint tp;
                    tp.time        = tp_time;
                    tp.beat_length = tp_cur_beat_length;
                    tp.uninherited = uninherited;
                    tp.multiplier  = tp_cur_velocity * 1000.0 / tp_cur_beat_length;
                    out.timing_points.push_back(tp);
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
        else if (section == "Events") {
            // Relevant event types:
            //   0,0,"file.jpg",x,y       background image (first wins)
            //   Video,offset,"file.mp4"  video (NOT a background)
            //   2,start,end              break period
            //   Sample,time,...          storyboard sample
            //   Storyboard layers        ignored
            //
            // We only accept a bg from a line starting with '0,'. Without
            // this guard, the first quoted string on the first Video,
            // line would be stored as bg_file, and the actual background
            // line further down would be skipped by the empty() check.
            if (out.bg_file.empty() &&
                t.size() >= 2 && t[0] == '0' && t[1] == ',') {
                auto q1 = t.find('"');
                if (q1 != std::string::npos) {
                    auto q2 = t.find('"', q1 + 1);
                    if (q2 != std::string::npos && q2 > q1 + 1) {
                        out.bg_file = t.substr(q1 + 1, q2 - q1 - 1);
                    }
                }
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

                // Field 6 (or after endTime: for holds) is:
                //   bank:addition:suffix:volume:filename
                // All fields optional. Parse it if present.
                std::string hs_field;
                if (parts.size() >= 6) {
                    const std::string& tail = parts[5];
                    if (n.is_hold) {
                        // tail is "endTime:bank:addition:suffix:volume:filename"
                        auto c1 = tail.find(':');
                        if (c1 != std::string::npos)
                            hs_field = tail.substr(c1 + 1);
                    } else {
                        // tail IS the hitSample
                        hs_field = tail;
                    }
                }

                if (!hs_field.empty()) {
                    std::vector<std::string> hparts;
                    std::stringstream hss(hs_field);
                    std::string hp;
                    while (std::getline(hss, hp, ':')) hparts.push_back(hp);

                    if (hparts.size() >= 1 && !hparts[0].empty()) {
                        try { n.sample_bank = std::stoi(hparts[0]); }
                        catch (...) { n.sample_bank = 0; }
                    }
                    // Field [1] is the addition-bank: separate from the
                    // normal bank. If empty, additions inherit from field [0].
                    if (hparts.size() >= 2 && !hparts[1].empty()) {
                        try { n.sample_bank_addition = std::stoi(hparts[1]); }
                        catch (...) { n.sample_bank_addition = 0; }
                    }
                    if (hparts.size() >= 3) n.sample_suffix = hparts[2];
                    if (hparts.size() >= 4 && !hparts[3].empty()) {
                        try { n.sample_volume = std::stoi(hparts[3]); }
                        catch (...) { n.sample_volume = 0; }
                    }
                    if (hparts.size() >= 5) n.sample_filename = hparts[4];
                }
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

    std::sort(out.timing_points.begin(), out.timing_points.end(),
              [](const TimingPoint& a, const TimingPoint& b){ return a.time < b.time; });

    // Lazer Beatmap.GetMostCommonBeatLength(). Used as BaseBeatLength when
    // RelativeScaleBeatLengths is true (which mania sets). SliderMultiplier
    // cancels out between DrawableScrollingRuleset and DrawableManiaRuleset.
    // Algorithm: uninherited points only. Each owns the span to the next
    // uninherited point (last one to the final hit object). First point is
    // forced to t=0 (osu-stable compat). Group by beat_length, sum durations,
    // take the largest.
    {
        double last_time = 0.0;
        for (const auto& n : out.notes)
            if (n.end_time > last_time) last_time = n.end_time;
        if (last_time <= 0.0 && !out.timing_points.empty())
            last_time = out.timing_points.back().time;

        std::vector<const TimingPoint*> uninherited;
        for (const auto& tp : out.timing_points)
            if (tp.uninherited) uninherited.push_back(&tp);

        if (!uninherited.empty()) {
            struct Group { double beat_length; double duration; };
            std::vector<Group> groups;
            for (size_t i = 0; i < uninherited.size(); i++) {
                const auto& tp = *uninherited[i];
                if (tp.time > last_time) continue;
                double cur  = (i == 0) ? 0.0 : tp.time;
                double nxt  = (i + 1 == uninherited.size()) ? last_time
                                                            : uninherited[i+1]->time;
                double dur  = nxt - cur;
                if (dur < 0.0) dur = 0.0;
                double bl = std::round(tp.beat_length * 1000.0) / 1000.0;
                bool found = false;
                for (auto& g : groups) {
                    if (std::abs(g.beat_length - bl) < 1e-6) {
                        g.duration += dur;
                        found = true;
                        break;
                    }
                }
                if (!found) groups.push_back({bl, dur});
            }
            double best_bl = 500.0, best_dur = -1.0;
            for (const auto& g : groups) {
                if (g.duration > best_dur) {
                    best_dur = g.duration;
                    best_bl  = g.beat_length;
                }
            }
            out.base_beat_length = best_bl;
        }

        // Recompute multipliers relative to base_beat_length. Old formula
        // baked in 1000.0; recover velocity, then re-apply against base.
        for (auto& tp : out.timing_points) {
            double velocity = tp.multiplier * tp.beat_length / 1000.0;
            tp.multiplier = velocity * out.base_beat_length / tp.beat_length;
        }
    }
    return true;
}
