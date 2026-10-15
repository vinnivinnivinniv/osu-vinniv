#pragma once

#include <SDL3/SDL.h>
#include <cstdint>

// Load an audio file (WAV or MP3) fully into memory as interleaved S16 PCM.
// Caller owns *out_data and must SDL_free() it.
bool load_audio_to_pcm(const char* path,
                       SDL_AudioSpec* out_spec,
                       uint8_t** out_data,
                       uint32_t* out_len);
