cmake_minimum_required(VERSION 3.22)
project(osu_vinniv CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MINIZ_DIR   ${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/miniz)
set(DRLIBS_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/dr_libs)
set(STB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/stb)
set(FFMPEG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../ffmpeg)

add_library(main SHARED
    main.cpp
    parser.cpp
    song_select.cpp
    song_cache.cpp
    image_load.cpp
    map_sample_bank.cpp
    osz_import.cpp
    audio_load.cpp
    audio_decode.cpp
    preview_player.cpp
    video_decode.cpp
    video_player.cpp
    ${MINIZ_DIR}/miniz.c
    ${MINIZ_DIR}/miniz_tdef.c
    ${MINIZ_DIR}/miniz_tinfl.c
    ${MINIZ_DIR}/miniz_zip.c
    ${STB_DIR}/stb_vorbis.c
)


target_include_directories(main PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MINIZ_DIR}
    ${DRLIBS_DIR}
    ${STB_DIR}
    ${FFMPEG_DIR}/include
)

find_library(GLESv3 GLESv3 REQUIRED)
find_library(log log REQUIRED)
find_library(android android REQUIRED)

# Link order for static libs: dependents first, dependencies last.
# avformat -> avcodec -> swresample/swscale -> avutil.
target_link_libraries(main
    SDL3::SDL3
    # --start-group/--end-group: libav* have circular deps
    # (avformat<->avcodec, swscale<->avutil). Without this, symbols
    # can go unresolved even when they exist in the archive.
    -Wl,--start-group
    ${FFMPEG_DIR}/lib/libavformat.a
    ${FFMPEG_DIR}/lib/libavcodec.a
    ${FFMPEG_DIR}/lib/libswresample.a
    ${FFMPEG_DIR}/lib/libswscale.a
    ${FFMPEG_DIR}/lib/libavutil.a
    -Wl,--end-group
    m    # libm — FFmpeg math
    z    # libz — mov/avi demuxers use zlib for atoms
    ${GLESv3}
    ${log}
    ${android}
)

# Dead-strip unused symbols from static libs so APK size is honest.
# --exclude-libs,ALL keeps FFmpeg symbols out of the exported dynsym.
target_link_options(main PRIVATE
    -Wl,--gc-sections
    -Wl,--exclude-libs,ALL
)
