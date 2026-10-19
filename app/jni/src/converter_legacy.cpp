#include "converter_legacy.h"
#include <algorithm>
#include <cmath>

namespace legacy_mania {

// --- LegacyRandom ---

static const double INT_TO_REAL = 1.0 / 2147483648.0;
static const uint32_t INT_MASK = 0x7FFFFFFFu;

LegacyRandom::LegacyRandom(int32_t seed)
    : X((uint32_t)seed), Y(842502087u), Z(3579807591u), W(273326509u) {}

uint32_t LegacyRandom::next_uint() {
    uint32_t t = X ^ (X << 11);
    X = Y; Y = Z; Z = W;
    return W = W ^ (W >> 19) ^ t ^ (t >> 8);
}

int32_t LegacyRandom::next() {
    return (int32_t)(INT_MASK & next_uint());
}

int32_t LegacyRandom::next(int upperBound) {
    return (int32_t)(next_double() * upperBound);
}

int32_t LegacyRandom::next(int lowerBound, int upperBound) {
    return (int32_t)(lowerBound + next_double() * (upperBound - lowerBound));
}

double LegacyRandom::next_double() {
    return INT_TO_REAL * next();
}

bool LegacyRandom::next_bool() {
    if (bitIndex == 32) {
        bitBuffer = next_uint();
        bitIndex = 1;
        return (bitBuffer & 1) == 1;
    }
    bitIndex++;
    return ((bitBuffer >>= 1) & 1) == 1;
}

// --- Pattern ---

void Pattern::add(const Note& n) {
    hit_objects.push_back(n);
    contained_columns.insert(n.column);
}

bool Pattern::column_has_object(int column) const {
    return contained_columns.count(column) > 0;
}

// --- Conversion difficulty ---

double compute_conversion_difficulty(const Beatmap& map) {
    if (map.notes.empty()) return 0.0;
    double first_time = map.notes.front().time;
    double last_time  = 0.0;
    for (const auto& n : map.notes)
        if (n.time > last_time) last_time = n.time;

    // Drain time in seconds. No break time parsing — treated as 0.
    int drain_time = (int)((last_time - first_time) / 1000.0);
    if (drain_time == 0) drain_time = 10000;

    double ar_clamped = std::clamp(map.approach_rate, 4.0, 7.0);
    double base = (map.drain_rate + ar_clamped) / 1.5;
    double density_term = (double)map.notes.size() / (double)drain_time * 9.0;
    double result = (base + density_term) / 38.0 * 5.0 / 1.15;
    return std::min(result, 12.0);
}

// --- HitCirclePatternGenerator: constructor ---

HitCirclePatternGenerator::HitCirclePatternGenerator(
    LegacyRandom& rng_,
    const Note& note,
    const Beatmap& beatmap_,
    const Pattern& previous_pattern_,
    int total_columns_,
    double previous_time,
    float  previous_x,
    float  previous_y,
    double density,
    PatternType last_stair)
    : rng(rng_),
      beatmap(beatmap_),
      hit_object(note),
      previous_pattern(previous_pattern_),
      total_columns(total_columns_),
      random_start(total_columns_ == 8 ? 1 : 0),
      conversion_difficulty(compute_conversion_difficulty(beatmap_)),
      stair_type(last_stair)
{
    // Direct port of HitCirclePatternGenerator ctor body.
    // Lazer: positionSeparation = ||(x,y) - (prev_x, prev_y)||,
    // not |dx| alone. Two notes at the same X but different Y have
    // nonzero separation and skip the pos_sep < 20 branches.
    float  position_x = note.source_x;
    float  position_y = note.source_y;
    float  dx = position_x - previous_x;
    float  dy = position_y - previous_y;
    float  pos_separation = std::sqrt(dx*dx + dy*dy);
    double time_separation = note.time - previous_time;

    double beat_len = beat_length_at(note.time);
    bool kiai = kiai_at(note.time);

    if (time_separation <= 80) {
        convert_type |= PT_ForceNotStack | PT_KeepSingle;
    } else if (time_separation <= 95) {
        convert_type |= PT_ForceNotStack | PT_KeepSingle | (uint32_t)last_stair;
    } else if (time_separation <= 105) {
        convert_type |= PT_ForceNotStack | PT_LowProbability;
    } else if (time_separation <= 125) {
        convert_type |= PT_ForceNotStack;
    } else if (time_separation <= 135 && pos_separation < 20) {
        convert_type |= PT_Cycle | PT_KeepSingle;
    } else if (time_separation <= 150 && pos_separation < 20) {
        convert_type |= PT_ForceStack | PT_LowProbability;
    } else if (pos_separation < 20 && density >= beat_len / 2.5) {
        convert_type |= PT_Reverse | PT_LowProbability;
    } else if (density < beat_len / 2.5 || kiai) {
        // High density — no flag set.
    } else {
        convert_type |= PT_LowProbability;
    }

    if (!(convert_type & PT_KeepSingle)) {
        bool has_finish = (note.hit_sound & 4) != 0;
        bool has_clap   = (note.hit_sound & 8) != 0;
        if (has_finish && total_columns != 8)
            convert_type |= PT_Mirror;
        else if (has_clap)
            convert_type |= PT_Gathered;
    }
}

// --- Helper implementations ---

int HitCirclePatternGenerator::get_column(float position, bool allow_special) const {
    if (allow_special && total_columns == 8) {
        const float divisor = 512.f / 7.f;
        int c = (int)std::floor(position / divisor);
        return std::clamp(c, 0, 6) + 1;
    }
    float divisor = 512.f / (float)total_columns;
    int c = (int)std::floor(position / divisor);
    return std::clamp(c, 0, total_columns - 1);
}

int HitCirclePatternGenerator::get_random_column(int lower, int upper) {
    if (lower < 0) lower = random_start;
    if (upper < 0) upper = total_columns;
    return rng.next(lower, upper);
}

int HitCirclePatternGenerator::get_random_note_count(double p2, double p3,
                                                     double p4, double p5, double p6) {
    p2 = std::clamp(p2, 0.0, 1.0);
    p3 = std::clamp(p3, 0.0, 1.0);
    p4 = std::clamp(p4, 0.0, 1.0);
    p5 = std::clamp(p5, 0.0, 1.0);
    p6 = std::clamp(p6, 0.0, 1.0);
    double val = rng.next_double();
    if (val >= 1.0 - p6) return 6;
    if (val >= 1.0 - p5) return 5;
    if (val >= 1.0 - p4) return 4;
    if (val >= 1.0 - p3) return 3;
    if (val >= 1.0 - p2) return 2;
    return 1;
}

bool HitCirclePatternGenerator::has_special_column() const {
    return (hit_object.hit_sound & 4) != 0 && (hit_object.hit_sound & 8) != 0;
}

double HitCirclePatternGenerator::beat_length_at(double time_ms) const {
    // Find last uninherited timing point at or before time_ms.
    double best = 500.0;
    for (const auto& tp : beatmap.timing_points) {
        if (tp.time > time_ms) break;
        if (tp.uninherited) best = tp.beat_length;
    }
    return best > 0 ? best : 500.0;
}

bool HitCirclePatternGenerator::kiai_at(double /*time_ms*/) const {
    // Kiai not parsed yet — default false. Deviation from lazer.
    return false;
}

void HitCirclePatternGenerator::finalize_stair_type(const Pattern& p) {
    for (const auto& obj : p.hit_objects) {
        if ((convert_type & PT_Stair) && obj.column == total_columns - 1)
            stair_type = PT_ReverseStair;
        if ((convert_type & PT_ReverseStair) && obj.column == random_start)
            stair_type = PT_Stair;
    }
}

void HitCirclePatternGenerator::add_to_pattern(Pattern& p, int column) {
    Note n = {};
    n.time        = hit_object.time;
    n.end_time    = hit_object.time;
    n.column      = column;
    n.hit_sound   = hit_object.hit_sound;
    n.sample_bank = hit_object.sample_bank;
    n.sample_bank_addition = hit_object.sample_bank_addition;
    n.sample_suffix  = hit_object.sample_suffix;
    n.sample_volume  = hit_object.sample_volume;
    n.sample_filename = hit_object.sample_filename;
    n.source_type = 1;   // output is circle
    p.add(n);
}

// --- HitCirclePatternGenerator: Generate ---

Pattern HitCirclePatternGenerator::generate() {
    Pattern pattern;

    if (total_columns == 1) {
        add_to_pattern(pattern, 0);
        return pattern;
    }

    int last_column = previous_pattern.hit_objects.empty()
                    ? 0 : previous_pattern.hit_objects.front().column;

    // Reverse: copy previous pattern's columns in reversed order.
    if ((convert_type & PT_Reverse) && !previous_pattern.hit_objects.empty()) {
        for (int i = random_start; i < total_columns; i++) {
            if (previous_pattern.column_has_object(i))
                add_to_pattern(pattern, random_start + total_columns - i - 1);
        }
        return pattern;
    }

    // Cycle: reverse-only for single-note previous pattern.
    if ((convert_type & PT_Cycle) && previous_pattern.hit_objects.size() == 1
        && (total_columns != 8 || last_column != 0)
        && (total_columns % 2 == 0 || last_column != total_columns / 2)) {
        int column = random_start + total_columns - last_column - 1;
        add_to_pattern(pattern, column);
        return pattern;
    }

    // ForceStack: repeat previous pattern's columns.
    if ((convert_type & PT_ForceStack) && !previous_pattern.hit_objects.empty()) {
        for (int i = random_start; i < total_columns; i++) {
            if (previous_pattern.column_has_object(i))
                add_to_pattern(pattern, i);
        }
        return pattern;
    }

    // Stair / ReverseStair (only when previous pattern is single note).
    if (previous_pattern.hit_objects.size() == 1) {
        if (convert_type & PT_Stair) {
            int target = last_column + 1;
            if (target == total_columns) target = random_start;
            add_to_pattern(pattern, target);
            return pattern;
        }
        if (convert_type & PT_ReverseStair) {
            int target = last_column - 1;
            if (target == random_start - 1) target = total_columns - 1;
            add_to_pattern(pattern, target);
            return pattern;
        }
    }

    if (convert_type & PT_KeepSingle) {
        { Pattern _p = generate_random_notes(1); finalize_stair_type(_p); return _p; }
    }

    if (convert_type & PT_Mirror) {
        if (conversion_difficulty > 6.5)
            { Pattern _p = generate_random_pattern_with_mirrored(0.12, 0.38, 0.12); finalize_stair_type(_p); return _p; }
        if (conversion_difficulty > 4.0)
            { Pattern _p = generate_random_pattern_with_mirrored(0.12, 0.17, 0.0); finalize_stair_type(_p); return _p; }
        { Pattern _p = generate_random_pattern_with_mirrored(0.12, 0.0, 0.0); finalize_stair_type(_p); return _p; }
    }

    if (conversion_difficulty > 6.5) {
        if (convert_type & PT_LowProbability)
            { Pattern _p = generate_random_pattern(0.78, 0.42, 0.0, 0.0); finalize_stair_type(_p); return _p; }
        { Pattern _p = generate_random_pattern(1.0, 0.62, 0.0, 0.0); finalize_stair_type(_p); return _p; }
    }
    if (conversion_difficulty > 4.0) {
        if (convert_type & PT_LowProbability)
            { Pattern _p = generate_random_pattern(0.35, 0.08, 0.0, 0.0); finalize_stair_type(_p); return _p; }
        { Pattern _p = generate_random_pattern(0.52, 0.15, 0.0, 0.0); finalize_stair_type(_p); return _p; }
    }
    Pattern result;
    if (conversion_difficulty > 2.0) {
        if (convert_type & PT_LowProbability)
            result = generate_random_pattern(0.18, 0.0, 0.0, 0.0);
        else
            result = generate_random_pattern(0.45, 0.0, 0.0, 0.0);
    } else {
        result = generate_random_pattern(0.0, 0.0, 0.0, 0.0);
    }
    finalize_stair_type(result);
    return result;
}

// --- generate_random_notes ---

Pattern HitCirclePatternGenerator::generate_random_notes(int note_count) {
    Pattern pattern;
    bool allow_stacking = !(convert_type & PT_ForceNotStack);
    if (!allow_stacking) {
        int cap = total_columns - random_start - previous_pattern.column_with_objects();
        if (note_count > cap) note_count = cap;
        if (note_count < 0) note_count = 0;
    }

    int next_column = get_column(hit_object.source_x, true);
    for (int i = 0; i < note_count; i++) {
        // Inline find_available_column with getNextColumn logic.
        auto get_next = [&](int last) -> int {
            if (convert_type & PT_Gathered) {
                last++;
                if (last == total_columns) last = random_start;
                return last;
            }
            return get_random_column();
        };

        // isValid lambda
        auto is_valid = [&](int col) -> bool {
            if (pattern.column_has_object(col)) return false;
            if (!allow_stacking && previous_pattern.column_has_object(col)) return false;
            return true;
        };

        int col = next_column;
        if (!is_valid(col)) {
            bool has_valid = false;
            for (int k = random_start; k < total_columns; k++)
                if (is_valid(k)) { has_valid = true; break; }
            if (!has_valid) break;   // safety: shouldn't happen
            do { col = get_next(col); } while (!is_valid(col));
        }
        add_to_pattern(pattern, col);
        next_column = col;
    }
    return pattern;
}

// --- generate_random_pattern ---

Pattern HitCirclePatternGenerator::generate_random_pattern(double p2, double p3,
                                                          double p4, double p5) {
    Pattern pattern;
    // Column-count caps by TotalColumns (from getRandomNoteCount in C#).
    double q2 = p2, q3 = p3, q4 = p4, q5 = p5;
    switch (total_columns) {
        case 2: q2 = 0; q3 = 0; q4 = 0; q5 = 0; break;
        case 3: q2 = std::min(q2, 0.1); q3 = 0; q4 = 0; q5 = 0; break;
        case 4: q2 = std::min(q2, 0.23); q3 = std::min(q3, 0.04); q4 = 0; q5 = 0; break;
        case 5: q3 = std::min(q3, 0.15); q4 = std::min(q4, 0.03); q5 = 0; break;
    }
    // CLAP forces 2 notes.
    if (hit_object.hit_sound & 8) q2 = 1.0;

    int count = get_random_note_count(q2, q3, q4, q5);
    Pattern inner = generate_random_notes(count);
    for (const auto& n : inner.hit_objects) pattern.add(n);

    if (random_start > 0 && has_special_column())
        add_to_pattern(pattern, 0);
    return pattern;
}

// --- generate_random_pattern_with_mirrored ---

Pattern HitCirclePatternGenerator::generate_random_pattern_with_mirrored(
    double centre_prob, double p2, double p3)
{
    if (convert_type & PT_ForceNotStack) {
        return generate_random_pattern(0.5 + p2 / 2.0, p2, (p2 + p3) / 2.0, p3);
    }

    Pattern pattern;
    bool add_to_centre = false;
    int note_count = get_random_note_count_mirrored(centre_prob, p2, p3, add_to_centre);

    int column_limit = (total_columns % 2 == 0 ? total_columns : total_columns - 1) / 2;
    int next_column = get_random_column(0, column_limit);

    for (int i = 0; i < note_count; i++) {
        // Find available within [0, column_limit)
        auto is_valid = [&](int col) -> bool { return !pattern.column_has_object(col); };
        int col = next_column;
        if (!is_valid(col)) {
            int tries = 0;
            do { col = get_random_column(0, column_limit); tries++; }
            while (!is_valid(col) && tries < 32);
        }
        // Add normal + mirrored
        add_to_pattern(pattern, col);
        add_to_pattern(pattern, random_start + total_columns - col - 1);
        next_column = col;
    }

    if (add_to_centre) add_to_pattern(pattern, total_columns / 2);
    if (random_start > 0 && has_special_column()) add_to_pattern(pattern, 0);
    return pattern;
}

// --- get_random_note_count_mirrored ---

int HitCirclePatternGenerator::get_random_note_count_mirrored(
    double centre_prob, double p2, double p3, bool& add_centre)
{
    double q_centre = centre_prob;
    double q2 = p2, q3 = p3;
    switch (total_columns) {
        case 2: q_centre = 0; q2 = 0; q3 = 0; break;
        case 3: q_centre = std::min(q_centre, 0.03); q2 = 0; q3 = 0; break;
        case 4: q_centre = 0; q2 = 1.0 - std::max((1.0 - q2) * 2.0, 0.8); q3 = 0; break;
        case 5: q_centre = std::min(q_centre, 0.03); q3 = 0; break;
        case 6: q_centre = 0;
                q2 = 1.0 - std::max((1.0 - q2) * 2.0, 0.5);
                q3 = 1.0 - std::max((1.0 - q3) * 2.0, 0.85); break;
    }
    q2 = std::clamp(q2, 0.0, 1.0);
    q3 = std::clamp(q3, 0.0, 1.0);
    double centre_val = rng.next_double();
    int count = get_random_note_count(q2, q3);
    add_centre = (total_columns % 2 != 0) && count != 3 && centre_val > 1.0 - q_centre;
    return count;
}

}  // namespace legacy_mania
