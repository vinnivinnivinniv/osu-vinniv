#include "converter.h"
#include "converter_legacy.h"
#include <cmath>

// --- LegacyRandom ---

LegacyRandom::LegacyRandom(int seed)
    : x((uint32_t)seed), y(842502087u), z(3579807591u), w(273326509u) {}

int LegacyRandom::next() {
    uint32_t t = x ^ (x << 11);
    x = y; y = z; z = w;
    w = w ^ (w >> 19) ^ t ^ (t >> 8);
    return (int)(0x7FFFFFFF & w);
}

double LegacyRandom::next_double() {
    // .NET Random uses 1.0 / (int.MaxValue + 1.0) == 1.0 / 2147483648.0
    return next() * (1.0 / 2147483648.0);
}

int LegacyRandom::next_int(int max_value) {
    if (max_value <= 0) return 0;
    return (int)(next_double() * max_value);
}

// --- Column count heuristic (from lazer ManiaBeatmapConverter.getColumnCount) ---

int mania_column_count(double drain_rate, double circle_size, double od,
                       double approach_rate, int total_objects, int slider_count) {
    (void)drain_rate;    // present for parity with lazer signature; unused
    (void)approach_rate; // unused
    double rounded_cs = std::round(circle_size);
    double rounded_od = std::round(od);

    if (total_objects > 0 && slider_count >= 0) {
        double pct = (double)slider_count / (double)total_objects;
        if (pct < 0.2) return 7;
        if (pct < 0.3 || rounded_cs >= 5)
            return rounded_od > 5 ? 7 : 6;
        if (pct > 0.6)
            return rounded_od > 4 ? 5 : 4;
    }
    int n = (int)rounded_od + 1;
    if (n < 4) n = 4;
    if (n > 7) n = 7;
    return n;
}

// --- Conversion ---

void convert_to_mania(Beatmap& map, ConvertEmitFn emit) {
    if (map.mode == 3) return;
    if (map.notes.empty()) return;

    int total = (int)map.notes.size();
    int sliders = map.slider_count + map.spinner_count;
    int cols = mania_column_count(
        map.drain_rate, (double)map.key_count, map.od, map.approach_rate,
        total, sliders);

    int seed = (int)std::round(map.drain_rate + (double)map.key_count) * 20
             + (int)(map.od * 41.2)
             + (int)std::round(map.approach_rate);

    legacy_mania::LegacyRandom rng(seed);

    std::vector<Note> out_notes;
    out_notes.reserve(map.notes.size());

    legacy_mania::Pattern prev_pattern;
    double prev_time  = 0.0;
    float  prev_x     = 0.0f;
    float  prev_y     = 0.0f;
    legacy_mania::PatternType last_stair = legacy_mania::PT_Stair;

    double prev_times[7] = {};
    int    prev_count    = 0;
    double density       = 1e18;

    for (const auto& n : map.notes) {
        if (prev_count < 7) {
            prev_times[prev_count++] = n.time;
        } else {
            for (int i = 0; i < 6; i++) prev_times[i] = prev_times[i + 1];
            prev_times[6] = n.time;
        }
        if (prev_count >= 2) {
            density = (prev_times[prev_count - 1] - prev_times[0]) / prev_count;
        }

        bool is_circle = (n.source_type & 1) != 0;
        std::vector<Note> this_emit;

        if (is_circle) {
            legacy_mania::HitCirclePatternGenerator gen(
                rng, n, map, prev_pattern, cols,
                prev_time, prev_x, prev_y, density, last_stair);

            legacy_mania::Pattern p = gen.generate();
            for (const auto& out : p.hit_objects) {
                out_notes.push_back(out);
                this_emit.push_back(out);
            }

            last_stair  = gen.stair_type;
            prev_pattern = p;
        } else {
            Note out = n;
            int c = (int)((double)n.column / (512.0 / (double)cols));
            if (c < 0) c = 0;
            if (c >= cols) c = cols - 1;
            out.column = c;
            out_notes.push_back(out);
            this_emit.push_back(out);
        }

        if (emit) {
            ConversionEmit e;
            e.source_time = n.time;
            e.source_type = n.source_type;
            e.rng_w = rng.W;
            e.rng_x = rng.X;
            e.rng_y = rng.Y;
            e.rng_z = rng.Z;
            e.emitted = std::move(this_emit);
            emit(e);
        }

        prev_time = n.time;
        prev_x    = n.source_x;
        prev_y    = n.source_y;
    }

    map.notes     = std::move(out_notes);
    map.key_count = cols;
    map.mode      = 3;
}

int preview_column_count(const Beatmap& map) {
    if (map.notes.empty()) return 4;
    return mania_column_count(
        map.drain_rate, (double)map.key_count, map.od, map.approach_rate,
        (int)map.notes.size(),
        map.slider_count + map.spinner_count);
}
