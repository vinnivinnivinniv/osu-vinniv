// video_player.cpp — async video decode + texture handoff.

#include "video_player.h"
#include "video_decode.h"
#include <algorithm>
#include <chrono>
#include <cstring>

extern "C" void SDL_Log(const char* fmt, ...);

VideoPlayer::VideoPlayer(SDL_Renderer* renderer) : renderer_(renderer) {
    worker_ = std::thread([this]{ worker_loop(); });
}

VideoPlayer::~VideoPlayer() {
    shutdown();
}

void VideoPlayer::shutdown() {
    quit_.store(true);
    generation_.fetch_add(1);
    if (worker_.joinable()) worker_.join();
    for (int i = 0; i < 2; i++) {
        if (textures_[i]) { SDL_DestroyTexture(textures_[i]); textures_[i] = nullptr; }
    }
    tex_w_ = tex_h_ = 0;
}

void VideoPlayer::request(const char* path, bool looping, double start_ms) {
    {
        std::lock_guard<std::mutex> lk(req_mu_);
        req_path_    = path ? path : "";
        req_looping_ = looping;
        req_start_ms_ = start_ms;
        req_gen_     = generation_.fetch_add(1) + 1;
    }
    // Drop the previous request's pending frame so tick() doesn't
    // re-upload it and pin ready_=true on the stale content.
    {
        std::lock_guard<std::mutex> lk(frame_mu_);
        frame_pending_ = false;
    }
    ready_.store(false);
    frames_delivered_.store(0);
}

void VideoPlayer::stop() {
    {
        std::lock_guard<std::mutex> lk(req_mu_);
        req_path_.clear();
        req_gen_ = generation_.fetch_add(1) + 1;
    }
    {
        std::lock_guard<std::mutex> lk(frame_mu_);
        frame_pending_ = false;
    }
    ready_.store(false);
    frames_delivered_.store(0);
}

void VideoPlayer::tick() {
    int w = 0, h = 0;
    {
        std::lock_guard<std::mutex> lk(frame_mu_);
        if (!frame_pending_) return;
        std::swap(frame_data_, upload_data_);
        w = frame_w_;
        h = frame_h_;
        frame_pending_ = false;
    }

    // Ping-pong: render from the texture updated last frame while uploading
    // to the other. Breaks the Adreno UnlockTexture GPU-sync stall caused by
    // re-locking an in-use texture.
    int upload_idx = 1 - tex_index_;
    SDL_Texture* upload_tex = textures_[upload_idx];

    if (!textures_[0] || !textures_[1] || tex_w_ != w || tex_h_ != h) {
        for (int i = 0; i < 2; i++) {
            if (textures_[i]) SDL_DestroyTexture(textures_[i]);
            textures_[i] = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32,
                                             SDL_TEXTUREACCESS_STREAMING, w, h);
            if (textures_[i]) SDL_SetTextureBlendMode(textures_[i], SDL_BLENDMODE_BLEND);
        }
        if (!textures_[0] || !textures_[1]) {
            SDL_Log("VIDEO_PLAYER: texture create failed: %s", SDL_GetError());
            return;
        }
        tex_w_ = w;
        tex_h_ = h;
        upload_tex = textures_[upload_idx];
    }

    void* pixels = nullptr;
    int pitch = 0;
    if (!SDL_LockTexture(upload_tex, nullptr, &pixels, &pitch)) {
        SDL_Log("VIDEO_PLAYER: SDL_LockTexture failed: %s", SDL_GetError());
        return;
    }

    int row_bytes = w * 4;
    const uint8_t* src_px = upload_data_.data();
    if (pitch == row_bytes) {
        memcpy(pixels, src_px, (size_t)row_bytes * h);
    } else {
        uint8_t* dst = (uint8_t*)pixels;
        for (int y = 0; y < h; y++) {
            memcpy(dst + y * pitch, src_px + y * row_bytes, row_bytes);
        }
    }
    SDL_UnlockTexture(upload_tex);
    tex_index_ = upload_idx;
    ready_.store(true);
    frames_delivered_.fetch_add(1);
}


void VideoPlayer::worker_loop() {
    std::string cur_path;
    uint64_t    cur_gen = 0;
    bool        cur_looping = false;
    double      cur_start_ms = 0.0;

    VideoDecoder* dec = nullptr;
    Uint64 playback_start_tick = 0;
    // Reference pts: the first frame decoded after open/seek. All
    // subsequent frames have their pts measured relative to this
    // value. Reset on every seek. Without this, a video seeking to
    // start_ms=59208 has f.pts_ms=59208 and paces 59 seconds late.
    double base_pts_ms = -1.0;

    while (!quit_.load()) {
        // Pick up request changes
        {
            std::lock_guard<std::mutex> lk(req_mu_);
            if (req_gen_ != cur_gen) {
                cur_gen       = req_gen_;
                cur_path      = req_path_;
                cur_looping   = req_looping_;
                cur_start_ms  = req_start_ms_;
                if (dec) { video_decoder_close(dec); dec = nullptr; }
            }
        }

        if (cur_path.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        if (!dec) {
            dec = video_decoder_open(cur_path.c_str());
            if (!dec) {
                SDL_Log("VIDEO_PLAYER: open failed: %s", cur_path.c_str());
                std::lock_guard<std::mutex> lk(req_mu_);
                cur_path.clear();
                continue;
            }
            if (cur_start_ms > 0.0) video_decoder_seek_ms(dec, cur_start_ms);
            playback_start_tick = SDL_GetTicks();
            base_pts_ms = -1.0;   // reset reference after seek
            SDL_Log("VIDEO_PLAYER: playing %s (%dx%d, %.2f fps)",
                    cur_path.c_str(),
                    video_decoder_width(dec),
                    video_decoder_height(dec),
                    video_decoder_frame_rate(dec));
        }

        VideoFrame f;
        int r = video_decoder_read_frame(dec, &f);

        if (r < 0) {
            SDL_Log("VIDEO_PLAYER: decode error");
            video_decoder_close(dec);
            dec = nullptr;
            std::lock_guard<std::mutex> lk(req_mu_);
            cur_path.clear();
            continue;
        }

        if (r == 0) {
            if (cur_looping) {
                // Loop back to the requested start point (preview_time
                // in song select), not to absolute 0.
                video_decoder_seek_ms(dec, cur_start_ms);
                playback_start_tick = SDL_GetTicks();
                base_pts_ms = -1.0;   // reset reference after loop seek
                continue;
            }
            // EOF, hold last frame. Wait for a new request.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Gen check before doing anything with this frame. Covers the
        // fast path (due <= now) where the pacing block below would
        // otherwise be skipped entirely.
        {
            std::lock_guard<std::mutex> lk(req_mu_);
            if (req_gen_ != cur_gen) continue;
        }

        // Capture baseline pts from the very first frame after a seek.
        // This makes f.pts_ms after seek-to-59208 relative to 59208, so
        // pacing starts immediately instead of sleeping 59 seconds.
        if (base_pts_ms < 0.0) base_pts_ms = f.pts_ms;
        double rel_pts = f.pts_ms - base_pts_ms;
        if (rel_pts < 0.0) rel_pts = 0.0;   // guard: first frame may be at or slightly before seek target

        // Pacing: sleep until this frame's pts is due relative to playback start.
        Uint64 due = playback_start_tick + (Uint64)rel_pts;
        Uint64 now = SDL_GetTicks();
        if (due > now) {
            Uint64 target = due;
            while (SDL_GetTicks() < target) {
                if (quit_.load()) break;
                {
                    std::lock_guard<std::mutex> lk(req_mu_);
                    if (req_gen_ != cur_gen) break;
                }
                Uint64 rem = target - SDL_GetTicks();
                if (rem > 5) rem = 5;
                std::this_thread::sleep_for(std::chrono::milliseconds(rem));
            }
            if (quit_.load()) continue;
            {
                std::lock_guard<std::mutex> lk(req_mu_);
                if (req_gen_ != cur_gen) continue;
            }
        }

        // Post to shared frame slot
        {
            std::lock_guard<std::mutex> lk(frame_mu_);
            size_t bytes = (size_t)f.stride * f.height;
            frame_data_.resize(bytes);
            memcpy(frame_data_.data(), f.data, bytes);
            frame_w_ = f.width;
            frame_h_ = f.height;
            frame_pending_ = true;
        }
    }

    if (dec) video_decoder_close(dec);
}

// Test entry point. Runs a VideoPlayer for 2 seconds against the given
// file, logs frame delivery and texture dimensions.
int video_player_probe(const char* path, SDL_Renderer* renderer) {
    if (!path || !renderer) return 0;

    VideoPlayer vp(renderer);
    vp.request(path, false, 0.0);

    Uint64 t0 = SDL_GetTicks();
    int iterations = 0;
    int last_w = 0, last_h = 0;
    bool got_first = false;

    while (SDL_GetTicks() - t0 < 2000) {
        vp.tick();
        iterations++;

        if (!got_first && vp.ready()) {
            got_first = true;
            last_w = vp.texture_w();
            last_h = vp.texture_h();
            SDL_Log("VIDEO_PLAYER: first texture %dx%d at t=%llu ms",
                    last_w, last_h,
                    (unsigned long long)(SDL_GetTicks() - t0));
        }

        SDL_Delay(8);
    }

    Uint64 elapsed = SDL_GetTicks() - t0;
    uint64_t frames = vp.frames_delivered();
    SDL_Log("VIDEO_PLAYER: probe finished — %d iterations, %llu frames in %llu ms (%.1f fps)",
            iterations,
            (unsigned long long)frames,
            (unsigned long long)elapsed,
            frames * 1000.0 / (double)elapsed);
    return got_first ? 1 : 0;
}
