cmake_minimum_required(VERSION 3.22)
project(rally_android C)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
get_filename_component(ANDROID_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)

add_subdirectory(${ANDROID_DIR}/SDL2 SDL2-build)
add_subdirectory(${ANDROID_DIR}/../port port-build)
