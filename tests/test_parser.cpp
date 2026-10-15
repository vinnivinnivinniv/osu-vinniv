// Host-side unit tests for the .osu parser.
// Compile with plain g++ — parser.cpp only depends on SDL_Log, which we stub.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// Stub SDL_Log before including parser.cpp
extern "C" void SDL_Log(const char* fmt, ...) { (void)fmt; }

#include "../app/jni/src/parser.cpp"

// ---------- test harness ----------

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        std::printf("FAIL: %s\n", msg); \
        tests_failed++; \
    } \
} while (0)

#define CHECK_EQ_F(a, b, msg) do { \
    tests_run++; \
    double _a = (a), _b = (b); \
    double _d = _a - _b; if (_d < 0) _d = -_d; \
    if (_d > 0.001) { \
        std::printf("FAIL: %s (got %f, want %f)\n", msg, _a, _b); \
        tests_failed++; \
    } \
} while (0)

#define CHECK_EQ_I(a, b, msg) do { \
    tests_run++; \
    long _a = (long)(a), _b = (long)(b); \
    if (_a != _b) { \
        std::printf("FAIL: %s (got %ld, want %ld)\n", msg, _a, _b); \
        tests_failed++; \
    } \
} while (0)

// ---------- helpers ----------

static std::string write_temp(const std::string& content) {
    std::string path = "/tmp/osu_vinniv_test.osu";
    std::ofstream f(path);
    f << content;
    f.close();
    return path;
}

// ---------- test: empty file ----------

static void test_empty_file() {
    auto path = write_temp("");
    Beatmap m;
    bool ok = parse_osu_file(path, m);
    CHECK(!ok, "empty file should fail to parse (no CircleSize)");
}

// ---------- test: minimal valid 4K mania ----------

static void test_minimal_4k() {
    std::string s =
        "osu file format v14\n"
        "\n"
        "[General]\n"
        "Mode: 3\n"
        "\n"
        "[Metadata]\n"
        "Title:Test Song\n"
        "Artist:Test Artist\n"
        "Version:Hard\n"
        "\n"
        "[Difficulty]\n"
        "CircleSize:4\n"
        "OverallDifficulty:8\n"
        "\n"
        "[TimingPoints]\n"
        "1000,428.571428571429,4,2,0,100,1,0\n"
        "\n"
        "[HitObjects]\n"
        "64,192,1000,1,0,0:0:0:0:\n"
        "192,192,1250,1,0,0:0:0:0:\n"
        "320,192,1500,128,0,2000:0:0:0:0:\n"
        "448,192,1750,1,0,0:0:0:0:\n";

    auto path = write_temp(s);
    Beatmap m;
    bool ok = parse_osu_file(path, m);

    CHECK(ok,                          "minimal 4K should parse");
    CHECK_EQ_I(m.key_count, 4,         "key_count");
    CHECK_EQ_F(m.od, 8.0,              "overall difficulty");
    CHECK_EQ_I(m.notes.size(), 4,      "note count");
    CHECK(m.title == "Test Song",      "title");
    CHECK(m.artist == "Test Artist",   "artist");
    CHECK(m.version == "Hard",         "difficulty name");

    // Hold flag on 3rd note
    CHECK(m.notes[2].is_hold,          "note 3 is a hold");
    CHECK_EQ_F(m.notes[2].end_time, 2000.0, "hold end time");

    // Non-hold notes
    CHECK(!m.notes[0].is_hold,         "note 1 is a tap");
    CHECK_EQ_F(m.notes[0].end_time, m.notes[0].time, "tap end == start");

    // Column mapping: x=64,192,320,448 → cols 0,1,2,3 in 4K
    CHECK_EQ_I(m.notes[0].column, 0,   "col mapping for x=64");
    CHECK_EQ_I(m.notes[1].column, 1,   "col mapping for x=192");
    CHECK_EQ_I(m.notes[2].column, 2,   "col mapping for x=320");
    CHECK_EQ_I(m.notes[3].column, 3,   "col mapping for x=448");

    // Sorted by time
    CHECK(m.notes[0].time <= m.notes[1].time, "sorted t0<=t1");
    CHECK(m.notes[1].time <= m.notes[2].time, "sorted t1<=t2");
    CHECK(m.notes[2].time <= m.notes[3].time, "sorted t2<=t3");
}

// ---------- test: 7K column mapping ----------

static void test_7k_mapping() {
    std::string s =
        "osu file format v14\n"
        "\n"
        "[Difficulty]\n"
        "CircleSize:7\n"
        "OverallDifficulty:5\n"
        "\n"
        "[TimingPoints]\n"
        "1000,428.571428571429,4,2,0,100,1,0\n"
        "\n"
        "[HitObjects]\n"
        "0,192,1000,1,0,0:0:0:0:\n"
        "512,192,1000,1,0,0:0:0:0:\n";

    auto path = write_temp(s);
    Beatmap m;
    bool ok = parse_osu_file(path, m);

    CHECK(ok, "7K parse");
    CHECK_EQ_I(m.key_count, 7, "key_count=7");
    CHECK_EQ_I(m.notes.size(), 2, "2 notes");
    CHECK_EQ_I(m.notes[0].column, 0, "x=0 → col 0");
    CHECK_EQ_I(m.notes[1].column, 6, "x=512 → col 6 (clamped)");
}

// ---------- test: hitsound bitfield ----------

static void test_hitsound_bits() {
    std::string s =
        "osu file format v14\n"
        "\n"
        "[Difficulty]\n"
        "CircleSize:4\n"
        "OverallDifficulty:5\n"
        "\n"
        "[TimingPoints]\n"
        "1000,500,4,2,0,100,1,0\n"
        "\n"
        "[HitObjects]\n"
        "64,192,1000,1,0,0:0:0:0:\n"    // normal (0)
        "192,192,1250,1,2,0:0:0:0:\n"    // whistle (2)
        "320,192,1500,1,4,0:0:0:0:\n"    // finish (4)
        "448,192,1750,1,8,0:0:0:0:\n"    // clap (8)
        "64,192,2000,1,10,0:0:0:0:\n";   // whistle+clap

    auto path = write_temp(s);
    Beatmap m;
    parse_osu_file(path, m);

    CHECK_EQ_I(m.notes.size(), 5, "5 notes");
    CHECK_EQ_I(m.notes[0].hit_sound, 0,  "no additions");
    CHECK_EQ_I(m.notes[1].hit_sound, 2,  "whistle bit");
    CHECK_EQ_I(m.notes[2].hit_sound, 4,  "finish bit");
    CHECK_EQ_I(m.notes[3].hit_sound, 8,  "clap bit");
    CHECK_EQ_I(m.notes[4].hit_sound, 10, "whistle+clap bits");
}

// ---------- test: audio filename ----------

static void test_audio_filename() {
    std::string s =
        "osu file format v14\n"
        "\n"
        "[General]\n"
        "AudioFilename: audio.mp3\n"
        "Mode: 3\n"
        "\n"
        "[Difficulty]\n"
        "CircleSize:4\n"
        "OverallDifficulty:5\n"
        "\n"
        "[TimingPoints]\n"
        "1000,500,4,2,0,100,1,0\n"
        "\n"
        "[HitObjects]\n"
        "64,192,1000,1,0,0:0:0:0:\n";

    auto path = write_temp(s);
    Beatmap m;
    parse_osu_file(path, m);
    CHECK(m.audio_file == "audio.mp3", "audio filename");
}

// ---------- test: unsorted input gets sorted ----------

static void test_sorts_by_time() {
    std::string s =
        "osu file format v14\n"
        "\n"
        "[Difficulty]\n"
        "CircleSize:4\n"
        "OverallDifficulty:5\n"
        "\n"
        "[TimingPoints]\n"
        "1000,428.571428571429,4,2,0,100,1,0\n"
        "\n"
        "[HitObjects]\n"
        "64,192,3000,1,0,0:0:0:0:\n"
        "64,192,1000,1,0,0:0:0:0:\n"
        "64,192,2000,1,0,0:0:0:0:\n";

    auto path = write_temp(s);
    Beatmap m;
    parse_osu_file(path, m);

    CHECK_EQ_I(m.notes.size(), 3, "3 notes");
    CHECK_EQ_F(m.notes[0].time, 1000, "first note at 1000");
    CHECK_EQ_F(m.notes[1].time, 2000, "second at 2000");
    CHECK_EQ_F(m.notes[2].time, 3000, "third at 3000");
}

// ---------- test: BOM tolerance ----------

static void test_utf8_bom() {
    std::string s = "\xEF\xBB\xBFosu file format v14\n"
        "\n"
        "[Difficulty]\n"
        "CircleSize:4\n"
        "OverallDifficulty:5\n"
        "\n"
        "[TimingPoints]\n"
        "1000,428.571428571429,4,2,0,100,1,0\n"
        "\n"
        "[HitObjects]\n"
        "64,192,1000,1,0,0:0:0:0:\n";

    auto path = write_temp(s);
    Beatmap m;
    bool ok = parse_osu_file(path, m);
    CHECK(ok, "parse with UTF-8 BOM");
    CHECK_EQ_I(m.notes.size(), 1, "1 note despite BOM");
}

// ---------- main ----------

int main() {
    test_empty_file();
    test_minimal_4k();
    test_7k_mapping();
    test_sorts_by_time();
    test_utf8_bom();
    test_hitsound_bits();
    test_audio_filename();

    std::printf("\n%d test(s) run, %d failed\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
