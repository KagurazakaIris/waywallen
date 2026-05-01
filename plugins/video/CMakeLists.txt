cmake_minimum_required(VERSION 3.16)
project(waywallen-video
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "FFmpeg-based video renderer subprocess for waywallen (Iter 0)"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Dependencies — Iter 2: libav* + sw scale come transitively through
# waywallen::ffmpeg_vk (which owns the streaming decoder and GPU YUV→RGB
# pipeline). The plugin only needs Vulkan + threads directly.
# ---------------------------------------------------------------------------

find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)

# waywallen-bridge resolution order (mirrors plugins/image/CMakeLists.txt):
#   1. Target already present (parent plugins/CMakeLists pre-declared it)
#   2. Installed package (distro build)
#   3. In-tree checkout at ../../bridge
if(NOT TARGET waywallen::bridge)
    find_package(waywallen-bridge QUIET)
    if(NOT waywallen-bridge_FOUND)
        set(_ww_bridge_tree "${CMAKE_CURRENT_SOURCE_DIR}/../../bridge")
        if(EXISTS "${_ww_bridge_tree}/CMakeLists.txt")
            message(STATUS "waywallen-video: using in-tree bridge at ${_ww_bridge_tree}")
            add_subdirectory("${_ww_bridge_tree}" waywallen-bridge-build EXCLUDE_FROM_ALL)
        else()
            message(FATAL_ERROR
                "waywallen-bridge not found (no package, no tree at ${_ww_bridge_tree})")
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# Renderer binary
# ---------------------------------------------------------------------------

add_executable(waywallen-video-renderer
    src/main.cpp
)

target_compile_features(waywallen-video-renderer PRIVATE cxx_std_20)
target_compile_options(waywallen-video-renderer PRIVATE
    -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers
)

# waywallen::ffmpeg_vk supplies the shared Vulkan device, streaming
# video decoder, and GPU NV12→RGBA compute pipeline.
if(NOT TARGET waywallen::ffmpeg_vk)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ffmpeg
                     waywallen-ffmpeg-build EXCLUDE_FROM_ALL)
endif()

target_link_libraries(waywallen-video-renderer
    PRIVATE
        waywallen::bridge
        waywallen::ffmpeg_vk
        Vulkan::Vulkan
        Threads::Threads
)

# ---------------------------------------------------------------------------
# Install layout matches the daemon's --plugin scan convention:
#   <prefix>/bin/waywallen-video-renderer
#   <prefix>/share/waywallen/renderers/waywallen-video.toml
#   <prefix>/share/waywallen/sources/video.lua
#
# Iter 5: now that the mpv plugin is gone, this plugin owns the
# `video` source plugin too — without it, the daemon has no way to
# enumerate video files in the user's libraries.
# ---------------------------------------------------------------------------

include(GNUInstallDirs)

install(TARGETS waywallen-video-renderer
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/renderers/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/waywallen/renderers
    FILES_MATCHING PATTERN "*.toml"
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/sources/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/waywallen/sources
    FILES_MATCHING PATTERN "*.lua"
)
