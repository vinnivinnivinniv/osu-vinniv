#pragma once
#include <cstdint>

// Opaque handle. Implementation in video_decode.cpp.
struct VideoDecoder;

// Decoded video frame. Pixel data is RGBA32 (4 bytes per pixel).
// The data pointer is owned by the decoder and remains valid until the
// next call to video_decoder_read_frame() or video_decoder_close().
struct VideoFrame {
    const uint8_t* data;
    int            width;
    int            height;
    int            stride;   // bytes per row = width * 4
    double         pts_ms;   // presentation timestamp in milliseconds
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
