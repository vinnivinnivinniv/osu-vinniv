#pragma once
#include <cstdint>
#include <cstddef>

// Streaming audio decoder. Opens a file, exposes seek + chunked read.
// Backed by dr_mp3 (streaming), stb_vorbis (streaming), or SDL_LoadWAV
// (whole-file buffer; WAVs are small).
enum class AudioFormat { NONE, WAV, MP3, OGG };

struct AudioDecoder {
    AudioFormat format = AudioFormat::NONE;
    int         channels = 0;
    int         sample_rate = 0;
    int64_t     total_frames = -1;   // -1 if unknown

    // WAV backing buffer (one-shot decode into memory)
    uint8_t*    wav_data  = nullptr;
    size_t      wav_len   = 0;
    size_t      wav_pos   = 0;
    bool        wav_owned = false;

    // Streaming handles (typed void* to keep headers light)
    void*       mp3_handle = nullptr;   // drmp3*
    void*       ogg_handle = nullptr;   // stb_vorbis*
};

bool    audio_decoder_open (AudioDecoder& d, const char* path);
int64_t audio_decoder_seek (AudioDecoder& d, int64_t frame);
int64_t audio_decoder_read (AudioDecoder& d, int16_t* out, int64_t frame_count);
void    audio_decoder_close(AudioDecoder& d);
