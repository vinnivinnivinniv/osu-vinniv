#include "audio_load.h"

#include <cstring>

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

static bool ends_with_ci(const char* s, const char* suf) {
    size_t sl = strlen(s), fl = strlen(suf);
    if (sl < fl) return false;
    for (size_t i = 0; i < fl; i++) {
        char a = s[sl - fl + i];
        char b = suf[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}

bool load_audio_to_pcm(const char* path,
                       SDL_AudioSpec* out_spec,
                       uint8_t** out_data,
                       uint32_t* out_len) {
    *out_data = nullptr;
    *out_len  = 0;

    if (ends_with_ci(path, ".wav")) {
        SDL_AudioSpec spec = {};
        uint8_t* data = nullptr;
        uint32_t len = 0;
        if (!SDL_LoadWAV(path, &spec, &data, &len)) {
            SDL_Log("AUDIO: WAV load failed for %s: %s", path, SDL_GetError());
            return false;
        }
        *out_spec = spec;
        *out_data = data;
        *out_len  = len;
        return true;
    }

    if (ends_with_ci(path, ".mp3")) {
        drmp3_config cfg = {};
        drmp3_uint64 frame_count = 0;
        drmp3_int16* pcm = drmp3_open_file_and_read_pcm_frames_s16(
            path, &cfg, &frame_count, nullptr);
        if (!pcm) {
            SDL_Log("AUDIO: MP3 decode failed for %s", path);
            return false;
        }

        SDL_AudioSpec spec = {};
        spec.format   = SDL_AUDIO_S16;
        spec.channels = cfg.channels;
        spec.freq     = cfg.sampleRate;

        *out_spec = spec;
        *out_data = reinterpret_cast<uint8_t*>(pcm);
        *out_len  = (uint32_t)(frame_count * cfg.channels * sizeof(int16_t));
        return true;
    }

    SDL_Log("AUDIO: no extension match for %s", path);
    return false;
}
