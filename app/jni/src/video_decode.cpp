// video_decode.cpp — FFmpeg-backed video decoder.
//
// Software decode only. No audio stream is opened. Frames are converted
// to RGBA32 via swscale for direct upload to SDL_TEXTUREACCESS_STREAMING.
//
// FFmpeg headers do NOT self-wrap in extern "C" — the include block
// below is mandatory or the linker emits _Z-mangled references.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
}

#include "video_decode.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Forward decl of SDL_Log so this file doesn't pull in SDL headers.
extern "C" void SDL_Log(const char* fmt, ...);

struct VideoDecoder {
    AVFormatContext* fmt     = nullptr;
    AVCodecContext*  codec   = nullptr;
    AVStream*        stream  = nullptr;
    SwsContext*      sws     = nullptr;
    AVFrame*         frame   = nullptr;
    AVPacket*        packet  = nullptr;
    int              video_idx = -1;
    int              sws_src_fmt = -1;
    double           time_base_ms = 0.0;
    bool             eof = false;

    // RGBA output buffer — reused across frames, grows on dimension change.
    uint8_t* out_buf = nullptr;
    int      out_w = 0, out_h = 0, out_stride = 0;
};

VideoDecoder* video_decoder_open(const char* path) {
    VideoDecoder* d = new VideoDecoder();
    if (!d) return nullptr;

    // Cap stream analysis so first-frame latency stays low. Default
    // is 5 s of content / 5 MB probed — way more than we need to
    // determine stream dimensions and codec parameters.
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "analyzeduration", "500000", 0);  // 500 ms
    av_dict_set(&opts, "probesize", "200000", 0);        // 200 KB

    if (avformat_open_input(&d->fmt, path, nullptr, &opts) < 0) {
        av_dict_free(&opts);
        SDL_Log("VIDEO: avformat_open_input failed: %s", path);
        video_decoder_close(d);
        return nullptr;
    }
    av_dict_free(&opts);

    if (avformat_find_stream_info(d->fmt, nullptr) < 0) {
        SDL_Log("VIDEO: avformat_find_stream_info failed");
        video_decoder_close(d);
        return nullptr;
    }

    d->video_idx = av_find_best_stream(d->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (d->video_idx < 0) {
        SDL_Log("VIDEO: no video stream in %s", path);
        video_decoder_close(d);
        return nullptr;
    }

    d->stream = d->fmt->streams[d->video_idx];

    const AVCodec* codec = avcodec_find_decoder(d->stream->codecpar->codec_id);
    if (!codec) {
        SDL_Log("VIDEO: no decoder for codec_id=%d", (int)d->stream->codecpar->codec_id);
        video_decoder_close(d);
        return nullptr;
    }

    d->codec = avcodec_alloc_context3(codec);
    if (!d->codec
        || avcodec_parameters_to_context(d->codec, d->stream->codecpar) < 0
        || avcodec_open2(d->codec, codec, nullptr) < 0) {
        SDL_Log("VIDEO: avcodec_open2 failed");
        video_decoder_close(d);
        return nullptr;
    }
    d->codec->pkt_timebase = d->stream->time_base;

    d->frame  = av_frame_alloc();
    d->packet = av_packet_alloc();
    if (!d->frame || !d->packet) {
        SDL_Log("VIDEO: av_frame_alloc/av_packet_alloc failed");
        video_decoder_close(d);
        return nullptr;
    }

    d->time_base_ms = av_q2d(d->stream->time_base) * 1000.0;
    return d;
}

void video_decoder_close(VideoDecoder* d) {
    if (!d) return;
    if (d->sws)    sws_freeContext(d->sws);
    if (d->frame)  av_frame_free(&d->frame);
    if (d->packet) av_packet_free(&d->packet);
    if (d->codec)  avcodec_free_context(&d->codec);
    if (d->fmt)    avformat_close_input(&d->fmt);
    free(d->out_buf);
    delete d;
}


int video_decoder_read_frame(VideoDecoder* d, VideoFrame* out) {
    if (!d || !out) return -1;

    while (true) {
        // Try to pull a decoded frame first. If EAGAIN, need more input.
        int ret = avcodec_receive_frame(d->codec, d->frame);
        if (ret == 0) {
            // Got a frame. Convert to RGBA32.
            int w = d->frame->width;
            int h = d->frame->height;

            if (!d->out_buf || d->out_w != w || d->out_h != h) {
                free(d->out_buf);
                d->out_w = w; d->out_h = h;
                d->out_stride = w * 4;
                d->out_buf = (uint8_t*)malloc((size_t)d->out_stride * h);
                if (!d->out_buf) { SDL_Log("VIDEO: RGBA buffer alloc failed"); return -1; }
            }

            if (!d->sws || d->sws_src_fmt != (int)d->frame->format) {
                if (d->sws) sws_freeContext(d->sws);
                d->sws = sws_getContext(
                    w, h, (AVPixelFormat)d->frame->format,
                    w, h, AV_PIX_FMT_RGBA,
                    SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (!d->sws) { SDL_Log("VIDEO: sws_getContext failed"); return -1; }
                d->sws_src_fmt = (int)d->frame->format;
            }

            uint8_t* dst_data[4]     = { d->out_buf, nullptr, nullptr, nullptr };
            int      dst_linesize[4] = { d->out_stride, 0, 0, 0 };
            sws_scale(d->sws, d->frame->data, d->frame->linesize,
                      0, h, dst_data, dst_linesize);

            int64_t ts = d->frame->best_effort_timestamp;
            if (ts == AV_NOPTS_VALUE) ts = d->frame->pts;
            int64_t start = d->stream->start_time;
            if (start == AV_NOPTS_VALUE) start = 0;
            out->pts_ms = (double)(ts - start) * d->time_base_ms;
            out->data   = d->out_buf;
            out->width  = w;
            out->height = h;
            out->stride = d->out_stride;

            av_frame_unref(d->frame);
            return 1;
        }

        if (ret == AVERROR_EOF) return 0;
        if (ret != AVERROR(EAGAIN)) {
            SDL_Log("VIDEO: avcodec_receive_frame error %d", ret);
            return -1;
        }

        // Need more input. If we already flushed, EOF.
        if (d->eof) return 0;

        int read = av_read_frame(d->fmt, d->packet);
        if (read < 0) {
            // End of input. Send null packet to flush decoder.
            d->eof = true;
            avcodec_send_packet(d->codec, nullptr);
            continue;
        }

        if (d->packet->stream_index == d->video_idx) {
            int send = avcodec_send_packet(d->codec, d->packet);
            if (send < 0 && send != AVERROR(EAGAIN)) {
                SDL_Log("VIDEO: avcodec_send_packet error %d", send);
                av_packet_unref(d->packet);
                return -1;
            }
        }
        av_packet_unref(d->packet);
    }
}

bool video_decoder_seek_ms(VideoDecoder* d, double ms) {
    if (!d || !d->fmt) return false;
    // Cap to a safe point. If the target is past 90% of duration (or
    // beyond it entirely), restart from 0 to avoid an infinite
    // seek->EOF->seek loop in looping players.
    double dur = video_decoder_duration_ms(d);
    if (dur > 0.0 && ms > dur * 0.9) ms = 0.0;
    if (ms < 0.0) ms = 0.0;
    int64_t ts = (int64_t)(ms / d->time_base_ms);
    int ret = av_seek_frame(d->fmt, d->video_idx, ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) return false;
    avcodec_flush_buffers(d->codec);
    av_frame_unref(d->frame);
    d->eof = false;
    return true;
}

double video_decoder_duration_ms(VideoDecoder* d) {
    if (!d || !d->stream) return 0.0;
    if (d->stream->duration > 0)
        return (double)d->stream->duration * d->time_base_ms;
    if (d->fmt && d->fmt->duration > 0)
        return (double)d->fmt->duration / (double)AV_TIME_BASE * 1000.0;
    return 0.0;
}

double video_decoder_frame_rate(VideoDecoder* d) {
    if (!d || !d->stream) return 0.0;
    return av_q2d(d->stream->avg_frame_rate);
}

int video_decoder_width(VideoDecoder* d) {
    return (d && d->codec) ? d->codec->width : 0;
}

int video_decoder_height(VideoDecoder* d) {
    return (d && d->codec) ? d->codec->height : 0;
}

// Test entry point. Opens the given file, decodes up to 5 frames,
// logs dimensions and pts. Returns true if at least one frame decoded.
extern "C" int video_decoder_probe(const char* path) {
    VideoDecoder* d = video_decoder_open(path);
    if (!d) return 0;

    SDL_Log("VIDEO: opened %s (%dx%d, %.2f fps, %.0f ms)",
            path, video_decoder_width(d), video_decoder_height(d),
            video_decoder_frame_rate(d), video_decoder_duration_ms(d));

    VideoFrame f;
    int count = 0;
    while (count < 5) {
        int r = video_decoder_read_frame(d, &f);
        if (r <= 0) break;
        SDL_Log("VIDEO: frame %d  %dx%d  pts=%.1f ms  px[0]=%02X%02X%02X%02X",
                count, f.width, f.height, f.pts_ms,
                f.data[0], f.data[1], f.data[2], f.data[3]);
        count++;
    }
    SDL_Log("VIDEO: decoded %d frames from %s", count, path);
    video_decoder_close(d);
    return count > 0 ? 1 : 0;
}
