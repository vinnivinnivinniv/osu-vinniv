#pragma once
#include "parser.h"
#include <cstdint>
#include <functional>

// Port of lazer's ManiaBeatmapConverter (LegacyRandom + column-count heuristic).
// Scope: deterministic column count from difficulty + seeded RNG for column
// assignment. Real pattern generators (HitCircle/Slider/Spinner) are follow-ups
// per docs/mechanics.md. Slider/spinner decomposition is not yet implemented —
// notes stay as flat taps/holds at their existing times.

// Port of System.Random (xorshift128) as used by lazer's LegacyRandom.
// Same seed → identical sequence. Required for faithful conversion once
// pattern generators land.
struct LegacyRandom {
    uint32_t x, y, z, w;
    explicit LegacyRandom(int seed);

    int    next();
    double next_double();
    int    next_int(int max_value);   // [0, max_value)
};

// Column count from difficulty values, matching lazer's ManiaBeatmapConverter
// getColumnCount for non-mania source rulesets. slider_count is used to
// estimate "special object" density — pass -1 if unknown (falls to OD heuristic).
int mania_column_count(double drain_rate, double circle_size, double od,
                       double approach_rate, int total_objects, int slider_count);

// Per-source-object conversion snapshot. Used by test harness to diff
// against lazer's -expected-conversion.json goldens. Populated by
// convert_to_mania only when a non-null emit callback is provided.
struct ConversionEmit {
    double               source_time;    // source object start time (ms)
    int                  source_type;    // 1=circle, 2=slider, 8=spinner
    uint32_t             rng_w, rng_x, rng_y, rng_z;   // state AFTER conversion
    std::vector<Note>    emitted;        // mania notes emitted from this object
};
using ConvertEmitFn = std::function<void(const ConversionEmit&)>;

// Convert a non-mania Beatmap to mania in place. No-op if already mania.
// Currently: computes key_count from difficulty, seeds RNG, assigns columns.
// Not faithful — no pattern generators yet. Playable but not lazer-identical.
// forced_columns > 0 overrides the difficulty-derived column count with
// that value, matching ManiaModKey<N> behavior. Seed derivation is
// unchanged (uses source CircleSize, not forced columns). forced_columns
// of 0 keeps the auto heuristic — the common case for vanilla converts.
void convert_to_mania(Beatmap& map, ConvertEmitFn emit = nullptr,
                      int forced_columns = 0);

// Compute the column count a map WOULD get after conversion (for display
// in the song-select list without needing to convert). Same as
// mania_column_count() with slider_count=0.
int preview_column_count(const Beatmap& map);

