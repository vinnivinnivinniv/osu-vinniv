// video_decode.cpp — stub link test for session 3a.
//
// FFmpeg 7.x headers do NOT wrap themselves in extern "C" — they emit
// plain C declarations with no C++ guards. In a .cpp file, this causes
// the compiler to mangle function references as C++ symbols
// (_Z15avcodec_versionv) which the linker can never match against the
// plain C symbols in the static archives.
//
// Every C++ project linking FFmpeg must wrap the includes manually.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
}

extern "C" int video_ffmpeg_link_test_avcodec() {
    return (int)avcodec_version();
}

extern "C" int video_ffmpeg_link_test_avformat() {
    return (int)avformat_version();
}

extern "C" const char* video_ffmpeg_link_test_version() {
    return av_version_info();
}
