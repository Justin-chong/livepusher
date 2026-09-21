cmake_minimum_required(VERSION 3.15)
project(live_pusher CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC：源文件为 UTF-8 编码（解决中文注释导致的 C4819 警告和语法错误）
if(MSVC)
    add_compile_options(/utf-8)
endif()

# FFmpeg 开发库路径
set(FFMPEG_ROOT "D:/library/ffmpeg-9.0.1-full_build-shared")

# 头文件路径
include_directories(
    ${FFMPEG_ROOT}/include
    ${CMAKE_SOURCE_DIR}/src
)

# 库文件路径
link_directories(${FFMPEG_ROOT}/lib)

# 收集源文件
file(GLOB_RECURSE SOURCES "src/*.cpp")

add_executable(live_pusher ${SOURCES} "src/common/clock.h" "src/common/log_level.h" "src/common/logger.h" "src/encode/video_encoder.h"  "src/encode/ffmpeg_video_encode.h" "src/encode/ffmpeg_video_encode.cpp" "src/capture/audio_capturer.h" "src/capture/ffmpeg_audio_capturer.h" "src/capture/ffmpeg_audio_capturer.cpp" "src/encode/audio_encoder.h" "src/encode/ffmpeg_audio_encode.h" "src/encode/ffmpeg_audio_encode.cpp" "src/muxer/muxer.h" "src/muxer/ffmpeg_muxer.h" "src/muxer/ffmpeg_muxer.cpp")

# 链接 FFmpeg 全套库 + Windows 网络/安全库（RTMP 推流需要）
target_link_libraries(live_pusher
    avformat
    avcodec
    avutil
    swscale
    swresample
    avdevice
    ws2_32
    secur32
)

# 编译后自动把 FFmpeg bin 目录下的 DLL 复制到 exe 同目录
add_custom_command(TARGET live_pusher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${FFMPEG_ROOT}/bin"
    $<TARGET_FILE_DIR:live_pusher>
)
