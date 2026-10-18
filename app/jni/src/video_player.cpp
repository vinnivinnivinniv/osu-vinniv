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
    quit_.store(true);
    generation_.fetch_add(1);
    if (worker_.joinable()) worker_.join();
    if (texture_) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }
}

void VideoPlayer::request(const char* path, bool looping, double start_ms) {
    {
        std::lock_guard<std::mutex> lk(req_mu_);
        req_path_    = path ? path : "";
        req_looping_ = looping;
        req_start_ms_ = start_ms;
        req_gen_     = generation_.fetch_add(1) + 1;
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
    ready_.store(false);
}

void VideoPlayer::tick() {
    std::vector<uint8_t> local_data;
    int w = 0, h = 0;

    {
        std::lock_guard<std::mutex> lk(frame_mu_);
        if (!frame_pending_) return;
        local_data.swap(frame_data_);
        w = frame_w_;
        h = frame_h_;
        frame_pending_ = false;
    }

    if (!texture_ || tex_w_ != w || tex_h_ != h) {
        if (texture_) SDL_DestroyTexture(texture_);
        texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32,
                                     SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!texture_) {
            SDL_Log("VIDEO_PLAYER: texture create failed: %s", SDL_GetError());
            return;
        }
        tex_w_ = w;
        tex_h_ = h;
        SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_BLEND);
    }

    SDL_UpdateTexture(texture_, nullptr, local_data.data(), w * 4);
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
                video_decoder_seek_ms(dec, 0.0);
                playback_start_tick = SDL_GetTicks();
                continue;
            }
            // EOF, hold last frame. Wait for a new request.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Pacing: sleep until this frame's pts is due relative to playback start.
        Uint64 due = playback_start_tick + (Uint64)f.pts_ms;
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
