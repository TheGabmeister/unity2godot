cmake_minimum_required(VERSION 3.16)
project(unity2godot LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ufbx (single-file C library)
add_library(ufbx STATIC thirdparty/ufbx/ufbx.c)
target_include_directories(ufbx PUBLIC thirdparty/ufbx)

# Main executable
add_executable(unity2godot src/main.cpp)
target_link_libraries(unity2godot PRIVATE ufbx)
