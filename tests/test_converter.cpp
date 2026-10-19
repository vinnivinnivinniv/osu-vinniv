// Host-side converter test. Compares our convert_to_mania() output against
// lazer's -expected-conversion.json goldens (byte-for-byte RNG snapshots +
// note sequences). Any circle divergence is a real port bug.
//
// Build: g++ -std=c++17 -O0 -g tests/test_converter.cpp -o test_converter
// Use:   ./test_converter <file.osu> > <file.txt>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

extern "C" void SDL_Log(const char* fmt, ...) { (void)fmt; }

// Pull in the sources directly (matches test_parser.cpp pattern).
#include "../app/jni/src/parser.cpp"
#include "../app/jni/src/converter.cpp"
#include "../app/jni/src/converter_legacy.cpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <file.osu>\n", argv[0]);
        return 2;
    }

    Beatmap map;
    if (!parse_osu_file(argv[1], map, /*metadata_only=*/false)) {
        std::fprintf(stderr, "parse failed: %s\n", argv[1]);
        return 1;
    }

    // Diagnostic: what does our column heuristic actually compute?
    std::fprintf(stderr,
        "DIAG %s: total=%zu circle=%d slider=%d spinner=%d "
        "CS=%.1f OD=%.1f DR=%.1f AR=%.1f preview_cols=%d\n",
        argv[1],
        map.notes.size(), map.circle_count, map.slider_count, map.spinner_count,
        (double)map.key_count, map.od, map.drain_rate, map.approach_rate,
        preview_column_count(map));

    // Sort note times to verify sort order — if this prints out-of-order
    // that's a diagnostic (parser did not sort map.notes).
    for (size_t i = 0; i < map.notes.size() && i < 10; i++) {
        std::fprintf(stderr, "PRE %zu: t=%.1f x=%.1f type=%d\n",
                     i, map.notes[i].time, (double)map.notes[i].source_x,
                     map.notes[i].source_type);
    }

    // Emit format (one line per source object):
    //   <source_time>|<source_type>|<rng_w>|<rng_x>|<rng_y>|<rng_z>|<notes>
    // where <notes> = "<start>:<col>:<end>;<start>:<col>:<end>;..."
    convert_to_mania(map, [](const ConversionEmit& e) {
        std::printf("%.3f|%d|%u|%u|%u|%u|",
                    e.source_time, e.source_type,
                    e.rng_w, e.rng_x, e.rng_y, e.rng_z);
        for (size_t i = 0; i < e.emitted.size(); i++) {
            if (i) std::printf(";");
            std::printf("%.3f:%d:%.3f",
                        e.emitted[i].time,
                        e.emitted[i].column,
                        e.emitted[i].end_time);
        }
        std::printf("\n");
    });

    return 0;
}
