#include "map_sample_bank.h"

#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <cctype>

static std::string lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

static bool ends_with_ci(const std::string& s, const std::string& suf) {
    if (s.size() < suf.size()) return false;
    for (size_t i = 0; i < suf.size(); i++) {
        char a = (char)std::tolower((unsigned char)s[s.size() - suf.size() + i]);
        char b = (char)std::tolower((unsigned char)suf[i]);
        if (a != b) return false;
    }
    return true;
}

MapSampleBank::~MapSampleBank() {
    clear();
}

void MapSampleBank::clear() {
    for (auto& s : samples) {
        if (s.stream) SDL_DestroyAudioStream(s.stream);
        if (s.data)   SDL_free(s.data);
    }
    samples.clear();
}

void MapSampleBank::load_from_folder(const std::string& folder) {
    clear();

    DIR* d = opendir(folder.c_str());
    if (!d) return;

    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        std::string name = de->d_name;
        if (name == "." || name == "..") continue;
        if (!ends_with_ci(name, ".wav") && !ends_with_ci(name, ".ogg")) continue;

        std::string full = folder + "/" + name;
        SDL_AudioSpec spec = {};
        Uint8* data = nullptr;
        Uint32 len = 0;

        // Only .wav via SDL_LoadWAV here. OGG hitsounds are rare; skip.
        if (!ends_with_ci(name, ".wav")) continue;
        if (!SDL_LoadWAV(full.c_str(), &spec, &data, &len)) continue;

        SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(
            SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!stream) {
            SDL_free(data);
            continue;
        }
        SDL_ResumeAudioStreamDevice(stream);

        MapSample s;
        s.name = lower(name);
        s.stream = stream;
        s.data = data;
        s.len = len;
        samples.push_back(s);
    }
    closedir(d);
}

bool MapSampleBank::has(const std::string& name) const {
    std::string key = lower(name);
    for (const auto& s : samples) {
        if (s.name == key) return true;
    }
    return false;
}

bool MapSampleBank::play(const std::string& name, float gain, bool force_restart) {
    std::string key = lower(name);
    for (auto& s : samples) {
        if (s.name != key) continue;
        if (!s.stream || !s.data) return false;
        SDL_SetAudioStreamGain(s.stream, gain);
        if (force_restart) SDL_ClearAudioStream(s.stream);
        SDL_PutAudioStreamData(s.stream, s.data, (int)s.len);
        return true;
    }
    return false;
}
