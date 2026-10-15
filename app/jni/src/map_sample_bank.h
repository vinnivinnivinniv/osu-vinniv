#pragma once

#include <SDL3/SDL.h>
#include <string>
#include <vector>

struct MapSample {
    std::string name;         // e.g. "soft-hitclap.wav" (lowercase for lookup)
    SDL_AudioStream* stream;
    Uint8* data;
    Uint32 len;
};

class MapSampleBank {
public:
    MapSampleBank() = default;
    ~MapSampleBank();

    // Non-copyable
    MapSampleBank(const MapSampleBank&) = delete;
    MapSampleBank& operator=(const MapSampleBank&) = delete;

    // Load every *.wav / *.ogg in the folder as a sample. Names are stored
    // lowercase to make lookup case-insensitive.
    void load_from_folder(const std::string& folder);

    // Free all streams and buffers.
    void clear();

    // Play <name> if present. force_restart clears the stream first so
    // rapid taps don't queue up. Returns true if played.
    bool play(const std::string& name, float gain, bool force_restart);

    // True if the bank contains <name> (case-insensitive).
    bool has(const std::string& name) const;

private:
    std::vector<MapSample> samples;
};
