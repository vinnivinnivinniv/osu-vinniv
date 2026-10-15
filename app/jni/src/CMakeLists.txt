cmake_minimum_required(VERSION 3.22)
project(osu_vinniv CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MINIZ_DIR   ${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/miniz)
set(DRLIBS_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/dr_libs)

add_library(main SHARED
    main.cpp
    parser.cpp
    song_select.cpp
    song_cache.cpp
    image_load.cpp
    osz_import.cpp
    audio_load.cpp
    ${MINIZ_DIR}/miniz.c
    ${MINIZ_DIR}/miniz_tdef.c
    ${MINIZ_DIR}/miniz_tinfl.c
    ${MINIZ_DIR}/miniz_zip.c
)

set(STB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/stb)

target_include_directories(main PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MINIZ_DIR}
    ${DRLIBS_DIR}
    ${STB_DIR}
)

find_library(GLESv3 GLESv3 REQUIRED)
find_library(log log REQUIRED)
find_library(android android REQUIRED)

target_link_libraries(main
    SDL3::SDL3
    ${GLESv3}
    ${log}
    ${android}
)
