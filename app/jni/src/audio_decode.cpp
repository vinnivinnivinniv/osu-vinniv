#include "audio_decode.h"

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

#include <SDL3/SDL.h>
#include <cstring>
#include <cstdlib>

// stb_vorbis streaming API — declared extern "C" to link against
// third_party/stb/stb_vorbis.c (compiled as a separate TU).
extern "C" {
    typedef struct stb_vorbis stb_vorbis;
    typedef struct {
        unsigned int sample_rate;
        int channels;
        unsigned int setup_memory_required;
        unsigned int setup_temp_memory_required;
        unsigned int temp_memory_required;
        int max_frame_size;
    } stb_vorbis_info;
    typedef struct {
        char* alloc_buffer;
        int   alloc_buffer_length_in_bytes;
    } stb_vorbis_alloc;

    stb_vorbis* stb_vorbis_open_filename(const char* filename, int* error, const stb_vorbis_alloc* alloc);
    stb_vorbis_info stb_vorbis_get_info(stb_vorbis* f);
    void stb_vorbis_close(stb_vorbis* f);
    int  stb_vorbis_seek(stb_vorbis* f, unsigned int sample_number);
    unsigned int stb_vorbis_stream_length_in_samples(stb_vorbis* f);
    int  stb_vorbis_get_samples_short_interleaved(stb_vorbis* f, int channels, short* buffer, int num_shorts);
}

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

bool audio_decoder_open(AudioDecoder& d, const char* path) {
    d = AudioDecoder{};

    if (ends_with_ci(path, ".wav")) {
        SDL_AudioSpec spec = {};
        uint8_t* data = nullptr;
        uint32_t len = 0;
        if (!SDL_LoadWAV(path, &spec, &data, &len)) {
            SDL_Log("PREVIEW: WAV load failed for %s: %s", path, SDL_GetError());
            return false;
        }
        if (spec.format != SDL_AUDIO_S16) {
            SDL_Log("PREVIEW: WAV not S16 (format=%d), skipping %s",
                    (int)spec.format, path);
            SDL_free(data);
            return false;
        }
        d.format       = AudioFormat::WAV;
        d.channels     = spec.channels;
        d.sample_rate  = spec.freq;
        d.wav_data     = data;
        d.wav_len      = len;
        d.wav_pos      = 0;
        d.wav_owned    = true;
        d.total_frames = (int64_t)(len / (spec.channels * 2));
        return true;
    }

    if (ends_with_ci(path, ".mp3")) {
        drmp3* handle = (drmp3*)std::calloc(1, sizeof(drmp3));
        if (!handle) return false;
        if (!drmp3_init_file(handle, path, nullptr)) {
            std::free(handle);
            SDL_Log("PREVIEW: MP3 init failed: %s", path);
            return false;
        }
        d.format       = AudioFormat::MP3;
        d.channels     = (int)handle->channels;
        d.sample_rate  = (int)handle->sampleRate;
        d.mp3_handle   = handle;
        d.total_frames = (int64_t)drmp3_get_pcm_frame_count(handle);
        return true;
    }

    if (ends_with_ci(path, ".ogg")) {
        int err = 0;
        stb_vorbis* handle = stb_vorbis_open_filename(path, &err, nullptr);
        if (!handle) {
            SDL_Log("PREVIEW: OGG open failed (err=%d): %s", err, path);
            return false;
        }
        stb_vorbis_info info = stb_vorbis_get_info(handle);
        unsigned int total = stb_vorbis_stream_length_in_samples(handle);
        d.format       = AudioFormat::OGG;
        d.channels     = info.channels;
        d.sample_rate  = (int)info.sample_rate;
        d.ogg_handle   = handle;
        d.total_frames = (int64_t)total;
        return true;
    }

    SDL_Log("PREVIEW: unsupported extension: %s", path);
    return false;
}

int64_t audio_decoder_seek(AudioDecoder& d, int64_t frame) {
    if (frame < 0) frame = 0;
    if (d.total_frames > 0 && frame >= d.total_frames) frame = 0;

    if (d.format == AudioFormat::WAV) {
        size_t bpf = (size_t)d.channels * 2;
        if (bpf == 0) return -1;
        d.wav_pos = (size_t)frame * bpf;
        if (d.wav_pos > d.wav_len) d.wav_pos = d.wav_len;
        return (int64_t)(d.wav_pos / bpf);
    }
    if (d.format == AudioFormat::MP3) {
        drmp3* h = (drmp3*)d.mp3_handle;
        return drmp3_seek_to_pcm_frame(h, (drmp3_uint64)frame) ? frame : -1;
    }
    if (d.format == AudioFormat::OGG) {
        stb_vorbis* h = (stb_vorbis*)d.ogg_handle;
        return stb_vorbis_seek(h, (unsigned int)frame) ? frame : -1;
    }
    return -1;
}

int64_t audio_decoder_read(AudioDecoder& d, int16_t* out, int64_t frame_count) {
    if (d.format == AudioFormat::WAV) {
        size_t bpf = (size_t)d.channels * 2;
        if (bpf == 0) return 0;
        size_t avail_bytes = d.wav_len - d.wav_pos;
        size_t want_bytes  = (size_t)frame_count * bpf;
        size_t got_bytes   = want_bytes < avail_bytes ? want_bytes : avail_bytes;
        if (got_bytes == 0) return 0;
        memcpy(out, d.wav_data + d.wav_pos, got_bytes);
        d.wav_pos += got_bytes;
        return (int64_t)(got_bytes / bpf);
    }
    if (d.format == AudioFormat::MP3) {
        drmp3* h = (drmp3*)d.mp3_handle;
        return (int64_t)drmp3_read_pcm_frames_s16(h, (drmp3_uint64)frame_count, out);
    }
    if (d.format == AudioFormat::OGG) {
        stb_vorbis* h = (stb_vorbis*)d.ogg_handle;
        int got = stb_vorbis_get_samples_short_interleaved(
            h, d.channels, out, (int)(frame_count * d.channels));
        return got > 0 ? (int64_t)got : 0;
    }
    return 0;
}

void audio_decoder_close(AudioDecoder& d) {
    if (d.format == AudioFormat::WAV) {
        if (d.wav_owned && d.wav_data) SDL_free(d.wav_data);
    } else if (d.format == AudioFormat::MP3) {
        if (d.mp3_handle) {
            drmp3* h = (drmp3*)d.mp3_handle;
            drmp3_uninit(h);
            std::free(h);
        }
    } else if (d.format == AudioFormat::OGG) {
        if (d.ogg_handle) stb_vorbis_close((stb_vorbis*)d.ogg_handle);
    }
    d = AudioDecoder{};
}
