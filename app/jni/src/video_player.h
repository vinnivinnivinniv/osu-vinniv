#pragma once
#include <SDL3/SDL.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Async video player. One worker thread decodes frames at the video's
// native framerate; the main thread calls tick() each frame to pull the
// latest decoded frame into a streaming texture. Same cancellation
// pattern as PreviewPlayer (generation counter) with an added texture
// handoff step.
struct VideoPlayer {
    explicit VideoPlayer(SDL_Renderer* renderer);
    ~VideoPlayer();

    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    // Start decoding the given file. Cancels any current playback.
    // start_ms: seek point. looping: restart on EOF.
    void request(const char* path, bool looping, double start_ms = 0.0);

    // Stop the current playback and free the decoder.
    void stop();

    // Main-thread: pull the latest decoded frame (if any) into the
    // internal texture. Call once per frame before current_texture().
    void tick();

    // Latest texture, or nullptr if nothing ready.
    SDL_Texture* current_texture() const { return textures_[tex_index_]; }
    int  texture_w() const { return tex_w_; }
    int  texture_h() const { return tex_h_; }
    float texture_aspect_ratio() const { return tex_ar_; }

    // True if at least one frame has been uploaded to the texture.
    bool ready() const { return ready_.load(); }

    // Explicit cleanup. Must be called before SDL_Quit() because
    // texture destruction requires a live renderer. Idempotent.
    void shutdown();

    // Total unique frames uploaded to the texture since request().
    // Used by probes and by gameplay to measure actual delivered fps.
    uint64_t frames_delivered() const { return frames_delivered_.load(); }

private:
    void worker_loop();

    SDL_Renderer* renderer_ = nullptr;
    std::thread   worker_;

    std::atomic<bool>     quit_{false};
    std::atomic<uint64_t> generation_{0};
    std::atomic<bool>     ready_{false};
    std::atomic<uint64_t> frames_delivered_{0};

    // Request slot (worker reads under req_mu_)
    std::mutex  req_mu_;
    std::string req_path_;
    bool        req_looping_ = false;
    double      req_start_ms_ = 0.0;
    uint64_t    req_gen_ = 0;

    // Latest decoded frame slot (worker writes, main reads under frame_mu_)
    std::mutex           frame_mu_;
    std::vector<uint8_t> frame_data_;   // worker writes here
    std::vector<uint8_t> upload_data_;  // main drains here; swaps with frame_data_
    int                  frame_w_ = 0;
    int                  frame_h_ = 0;
    int frame_sar_num_ = 1, frame_sar_den_ = 1;
    bool                 frame_pending_ = false;

    // YUV metadata (only valid when frame_is_yuv_ is true)
    bool                 frame_is_yuv_   = false;
    int                  frame_stride_y_ = 0;
    int                  frame_stride_u_ = 0;
    int                  frame_stride_v_ = 0;
    size_t               frame_y_size_   = 0;
    size_t               frame_u_size_   = 0;
    size_t               frame_v_size_   = 0;
    // RGBA metadata (fallback)
    int                  frame_stride_rgba_ = 0;
    // Texture format the ping-pong pair is currently allocated for
    bool                 tex_is_yuv_ = false;

    // Textures (main thread only)
    SDL_Texture* textures_[2] = { nullptr, nullptr };   // ping-pong
    int          tex_index_  = 0;                      // which we rendered last
    int          tex_w_ = 0;
    int          tex_h_ = 0;
    float tex_ar_ = 1.0f;
};

// Test-only. Creates a VideoPlayer, requests the given file, ticks for
// ~2 seconds, logs frame delivery. Returns 1 if at least one frame was
// successfully uploaded to a texture. Remove in session 3e.
int video_player_probe(const char* path, SDL_Renderer* renderer);
