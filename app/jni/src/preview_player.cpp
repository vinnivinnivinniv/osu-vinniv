#include "preview_player.h"
#include "audio_decode.h"

#include <SDL3/SDL.h>
#include <chrono>
#include <vector>

PreviewPlayer::PreviewPlayer() {
    worker = std::thread([this]{ worker_loop(); });
}

PreviewPlayer::~PreviewPlayer() {
    shutdown();
}

void PreviewPlayer::request(const std::string& path, int preview_ms, float gain) {
    {
        std::lock_guard<std::mutex> lk(mtx);
        pending_path       = path;
        pending_preview_ms = preview_ms;
    }
    gain_atomic.store(gain);
    generation.fetch_add(1);
    cv.notify_all();
}

void PreviewPlayer::stop() {
    {
        std::lock_guard<std::mutex> lk(mtx);
        pending_path.clear();
        pending_preview_ms = -1;
    }
    generation.fetch_add(1);
    cv.notify_all();
}

void PreviewPlayer::set_gain(float g) {
    gain_atomic.store(g);
}

void PreviewPlayer::shutdown() {
    if (!worker.joinable()) return;
    quit.store(true);
    generation.fetch_add(1);
    cv.notify_all();
    worker.join();
}

void PreviewPlayer::worker_loop() {
    while (true) {
        // ---- wait for a request (or quit) ----
        std::string path;
        int preview_ms = -1;
        uint64_t my_gen = 0;
        {
            std::unique_lock<std::mutex> lk(mtx);
            cv.wait_for(lk, std::chrono::milliseconds(100), [&]{
                return quit.load() || generation.load() != last_started_gen;
            });
            if (quit.load()) return;
            if (generation.load() == last_started_gen) continue;   // timeout
            my_gen = generation.load();
            path       = pending_path;
            preview_ms = pending_preview_ms;
            last_started_gen = my_gen;
        }

        if (path.empty()) {
            SDL_Log("PREVIEW: stopped");
            continue;
        }

        // ---- open decoder ----
        AudioDecoder dec;
        if (!audio_decoder_open(dec, path.c_str())) {
            SDL_Log("PREVIEW: decoder open failed: %s", path.c_str());
            continue;
        }
        int bytes_per_frame = dec.channels * 2;
        if (bytes_per_frame <= 0) { audio_decoder_close(dec); continue; }

        // ---- resolve preview position ----
        int64_t preview_frame = 0;
        if (preview_ms >= 0) {
            preview_frame = (int64_t)dec.sample_rate * preview_ms / 1000;
        } else if (dec.total_frames > 0) {
            preview_frame = (int64_t)(dec.total_frames * 0.4);
        }
        if (dec.total_frames > 0 && preview_frame >= dec.total_frames)
            preview_frame = 0;
        audio_decoder_seek(dec, preview_frame);

        // ---- open SDL stream ----
        SDL_AudioSpec spec = {};
        spec.format   = SDL_AUDIO_S16;
        spec.channels = dec.channels;
        spec.freq     = dec.sample_rate;

        SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(
            SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!stream) {
            SDL_Log("PREVIEW: SDL_OpenAudioDeviceStream failed: %s", SDL_GetError());
            audio_decoder_close(dec);
            continue;
        }
        float cur_gain = gain_atomic.load();
        SDL_SetAudioStreamGain(stream, cur_gain);

        SDL_Log("PREVIEW: start '%s' @ %lld (ch=%d sr=%d)",
                path.c_str(), (long long)preview_frame, dec.channels, dec.sample_rate);

        // ---- stream loop ----
        const int CHUNK_FRAMES = 2048;
        std::vector<int16_t> chunk((size_t)CHUNK_FRAMES * dec.channels);
        int64_t max_queued_bytes = (int64_t)dec.sample_rate * bytes_per_frame * 3 / 2;  // 1.5s
        if (max_queued_bytes < 16384) max_queued_bytes = 16384;

        bool resumed = false;
        while (generation.load() == my_gen && !quit.load()) {
            // backpressure: keep ~1.5s of audio queued
            while (SDL_GetAudioStreamQueued(stream) > max_queued_bytes) {
                if (generation.load() != my_gen || quit.load()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (generation.load() != my_gen || quit.load()) break;

            // live gain change
            float want_gain = gain_atomic.load();
            if (want_gain != cur_gain) {
                SDL_SetAudioStreamGain(stream, want_gain);
                cur_gain = want_gain;
            }

            int64_t got = audio_decoder_read(dec, chunk.data(), CHUNK_FRAMES);
            if (got <= 0) {
                audio_decoder_seek(dec, preview_frame);
                continue;
            }
            SDL_PutAudioStreamData(stream, chunk.data(), (int)(got * bytes_per_frame));
            if (!resumed) {
                SDL_ResumeAudioStreamDevice(stream);
                resumed = true;
            }
        }

        SDL_DestroyAudioStream(stream);
        audio_decoder_close(dec);
        SDL_Log("PREVIEW: teardown");
    }
}
