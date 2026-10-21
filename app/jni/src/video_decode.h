#pragma once
#include <cstdint>

// Opaque handle. Implementation in video_decode.cpp.
struct VideoDecoder;

// Decoded video frame. Pixel data is RGBA32 (4 bytes per pixel).
// The data pointer is owned by the decoder and remains valid until the
// next call to video_decoder_read_frame() or video_decoder_close().
struct VideoFrame {
    // RGBA path (fallback for exotic codecs: VP9, AV1, hardware formats
    // that swscale must normalize). Width * 4 bytes per row.
    const uint8_t* data_rgba   = nullptr;
    int            stride_rgba = 0;

    // YUV420P path (preferred — h264/mpeg4 come out of the decoder in
    // this format natively; skipping swscale saves both worker CPU and
    // texture upload bandwidth: 1.5 bytes/pixel vs 4 bytes/pixel).
    const uint8_t* data_y = nullptr;
    const uint8_t* data_u = nullptr;
    const uint8_t* data_v = nullptr;
    int            stride_y = 0;
    int            stride_u = 0;
    int            stride_v = 0;

    int            width  = 0;
    int            height = 0;
    int            sar_num = 1;
    int            sar_den = 1;
    double         pts_ms = 0.0;
    bool           is_yuv = false;
};

// extern "C" wrapper — keeps symbols unmangled so they can be dumped
// with nm -D and are callable from C contexts (future JNI bridges).
// Declarations here determine linkage; the .cpp definitions inherit it.
extern "C" {

// Open a video file. Returns nullptr on failure.
// Software decode only. No audio stream is opened.
VideoDecoder* video_decoder_open(const char* path);
void          video_decoder_close(VideoDecoder* d);

// Read the next frame. Returns:
//    1 = frame decoded and written to *out
//    0 = end of stream
//   -1 = error
int  video_decoder_read_frame(VideoDecoder* d, VideoFrame* out);

// Seek to a timestamp. Returns false if the stream can't seek.
bool video_decoder_seek_ms(VideoDecoder* d, double ms);

// Metadata. Valid only after a successful open.
double video_decoder_duration_ms(VideoDecoder* d);
double video_decoder_frame_rate(VideoDecoder* d);
int    video_decoder_width(VideoDecoder* d);
int    video_decoder_height(VideoDecoder* d);

// Test-only probe. Opens the path, decodes up to 5 frames, logs dims
// and pts. Returns 1 if at least one frame decoded. Remove in session 4.
int video_decoder_probe(const char* path);

}  // extern "C"
