#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

// Owns a worker thread that streams a preview of the selected song.
// Main thread only ever calls request() / stop() / set_gain() — never
// touches the SDL_AudioStream directly. Generation counter cancels
// in-flight previews when the selection changes.
struct PreviewPlayer {
    PreviewPlayer();
    ~PreviewPlayer();

    PreviewPlayer(const PreviewPlayer&) = delete;
    PreviewPlayer& operator=(const PreviewPlayer&) = delete;

    // Ask to start playing `path`, preview at `preview_ms` (-1 = 40% fallback).
    // Cancels any current preview.
    void request(const std::string& path, int preview_ms, float gain);

    // Stop the current preview but keep the worker alive.
    void stop();

    // Live gain change (applied on next chunk).
    void set_gain(float g);

    // Quit + join. Called automatically by destructor.
    void shutdown();

private:
    void worker_loop();

    std::thread             worker;
    std::atomic<bool>       quit{false};
    std::atomic<uint64_t>   generation{0};
    std::atomic<float>      gain_atomic{0.8f};

    std::mutex              mtx;
    std::condition_variable cv;
    std::string             pending_path;
    int                     pending_preview_ms = -1;
    uint64_t                last_started_gen   = 0;
};
