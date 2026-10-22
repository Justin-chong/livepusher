#pragma once

#include <atomic>
#include <thread>

#include "video_capturer.h"
#include "common/logger.h"
#include "common/clock.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
}

namespace livepusher {

// 屏幕采集器：用 FFmpeg 的 gdigrab 设备捕获 Windows 桌面。
// 与摄像头采集器（dshow）不同：
//  - gdigrab 输出的是原始 BGRA 帧（AV_CODEC_ID_RAWVIDEO），不需要解码。
//  - 打开参数用 "desktop"（全屏）或 "title=xxx"（指定窗口）。
// 因此实现比 FFmpegVideoCapturer 更简洁（省掉了 MJPEG 解码那一段）。
class FFmpegScreenCapturer : public VideoCapturer {
public:
    FFmpegScreenCapturer();
    ~FFmpegScreenCapturer() override;

    bool open(const std::string& device_name, int width, int height, int fps) override;
    bool start() override;
    void stop() override;
    void close() override;
    void set_callback(VideoFrameCallback cb) override;

private:
    void capture_loop();
    // 中断回调：stop() 后让阻塞的 av_read_frame 尽快返回。
    static int interrupt_callback_fn(void* opaque);

    AVFormatContext* ctx_ = nullptr;
    int video_stream_idx_ = -1;

    // BMP 解码器：gdigrab 的 packet 是 BMP 格式，需解码成像素帧。
    const AVCodec* codec_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;

    int width_ = 0;
    int height_ = 0;
    int fps_ = 0;

    std::atomic<bool> running_{ false };
    std::thread capture_thread_;
    VideoFrameCallback callback_;
    Clock clock_;
};

}  // namespace livepusher
