#pragma once
#include "parser.h"
#include <cstdint>
#include <set>
#include <vector>

// Faithful port of lazer's mania legacy pattern generator (circles only).
// Source: osu.Game.Rulesets.Mania/Beatmaps/Patterns/Legacy/*.cs
//
// Sliders and spinners are NOT converted by this file. Caller should fall
// back to single-note column mapping for non-circles until SliderPatternGenerator
// and SpinnerPatternGenerator are ported.

namespace legacy_mania {

// Bitflags matching lazer PatternType enum values exactly.
enum PatternType : uint32_t {
    PT_None         = 0,
    PT_ForceStack   = 1u << 0,
    PT_ForceNotStack= 1u << 1,
    PT_KeepSingle   = 1u << 2,
    PT_LowProbability=1u << 3,
    PT_Alternate    = 1u << 4,
    PT_ForceSigSlider=1u << 5,
    PT_ForceNotSlider=1u << 6,
    PT_Gathered     = 1u << 7,
    PT_Mirror       = 1u << 8,
    PT_Reverse      = 1u << 9,
    PT_Cycle        = 1u << 10,
    PT_Stair        = 1u << 11,
    PT_ReverseStair = 1u << 12,
};

// Exact port of osu.Game.Utils.LegacyRandom (FastRandom from stable).
struct LegacyRandom {
    uint32_t X, Y, Z, W;
    uint32_t bitBuffer = 0;
    int      bitIndex  = 32;

    explicit LegacyRandom(int32_t seed);
    uint32_t next_uint();
    int32_t  next();
    int32_t  next(int upperBound);
    int32_t  next(int lowerBound, int upperBound);
    double   next_double();
    bool     next_bool();
};

// Port of osu.Game.Rulesets.Mania.Beatmaps.Patterns.Pattern.
struct Pattern {
    std::vector<Note> hit_objects;
    std::set<int>     contained_columns;
    void add(const Note& n);
    bool column_has_object(int column) const;
    int  column_with_objects() const { return (int)contained_columns.size(); }
};

// Port of HitCirclePatternGenerator. Construct once per circle note, call
// Generate() to get 1..N emitted notes at the same time.
struct HitCirclePatternGenerator {
    // Immutable references held for the duration.
    LegacyRandom&   rng;
    const Beatmap&  beatmap;
    const Note&     hit_object;
    Pattern         previous_pattern;

    int             total_columns;
    int             random_start;         // 0 for ≤7K (1 only for 8K dual)
    double          conversion_difficulty;

    // Emitted by constructor.
    uint32_t        convert_type = PT_None;
    PatternType     stair_type   = PT_Stair;

    HitCirclePatternGenerator(LegacyRandom& rng,
                              const Note& note,
                              const Beatmap& beatmap,
                              const Pattern& previous_pattern,
                              int total_columns,
                              double previous_time,
                              float  previous_x,
                              float  previous_y,
                              double density,
                              PatternType last_stair);

    // Returns the pattern (list of column indices at note.time).
    Pattern generate();

private:
    int get_column(float position, bool allow_special = false) const;
    int get_random_column(int lower = -1, int upper = -1);
    int find_available_column(int initial,
                              int lower = -1, int upper = -1,
                              int (*next_col)(HitCirclePatternGenerator&, int, void*) = nullptr,
                              void* ctx = nullptr,
                              const std::vector<const Pattern*>* patterns = nullptr);
    int get_random_note_count(double p2, double p3,
                              double p4 = 0, double p5 = 0, double p6 = 0);
    Pattern generate_random_notes(int note_count);
    Pattern generate_random_pattern(double p2, double p3, double p4, double p5);
    Pattern generate_random_pattern_with_mirrored(double centre_prob, double p2, double p3);
    int  get_random_note_count_checked(double p2, double p3, double p4, double p5);
    int  get_random_note_count_mirrored(double centre_prob, double p2, double p3, bool& add_centre);
    void finalize_stair_type(const Pattern& p);
    void add_to_pattern(Pattern& p, int column);
    bool has_special_column() const;
    double beat_length_at(double time_ms) const;
    bool kiai_at(double time_ms) const;
};

// Conversion difficulty factor (from LegacyPatternGenerator.ConversionDifficulty).
double compute_conversion_difficulty(const Beatmap& map);

}  // namespace legacy_mania
