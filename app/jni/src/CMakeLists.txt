cmake_minimum_required(VERSION 3.22)
project(osu_vinniv CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(main SHARED
    main.cpp
    parser.cpp
    song_select.cpp
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
