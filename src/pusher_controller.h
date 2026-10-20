#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <memory>
#include <functional>
#include <chrono>

#include "common/thread_safe_queue.h"
#include "capture/ffmpeg_video_capturer.h"
#include "capture/ffmpeg_audio_capturer.h"
#include "encode/ffmpeg_video_encode.h"
#include "encode/ffmpeg_audio_encode.h"
#include "muxer/ffmpeg_muxer.h"

struct SwsContext;

namespace livepusher {

// 推流控制器：把「采集→编码→封装→RTMP」整个链路封装成可 start / stop 的对象。
// GUI 只需创建它，点「开始」调 start(url)，点「停止」调 stop()。
// 内部把原来 main.cpp 里 test_full_pipeline() 的顺序逻辑，改造成由线程 + 原子标志驱动。
class PusherController {
public:
    // 状态回调：通知上层当前状态变化（用于 GUI 更新按钮/显示）。
    // 参数：状态字符串（如 "推流中" / "已停止" / "重连中"）。
    using StatusCallback = std::function<void(const std::string&)>;

    // 实时统计快照：由监控线程周期性上报，供 GUI 刷新状态栏。
    struct Stats {
        int bitrate_level = 0;        // 码率档位（0=最高档）
        int target_bitrate = 0;       // 当前目标码率（bps）
        int64_t send_rate = 0;        // 估算发送速率（bps）
        bool disconnected = false;    // 是否断线
        int64_t elapsed_ms = 0;       // 已推流时长（毫秒）
    };
    using StatsCallback = std::function<void(const Stats&)>;

    PusherController();
    ~PusherController();

    PusherController(const PusherController&) = delete;
    PusherController& operator=(const PusherController&) = delete;

    // 开始推流到 url（如 rtmp://...）。内部启动采集、编码、监控线程。
    // 失败返回 false（比如设备打不开），成功返回 true。
    bool start(const std::string& url);

    // 停止推流：关闭采集、队列、编码线程、muxer。
    void stop();

    // 当前是否正在推流。
    bool isRunning() const { return running_.load(); }

    // 注册状态回调。
    void setStatusCallback(StatusCallback cb) { status_cb_ = std::move(cb); }

    // 注册实时统计回调（监控线程每 500ms 调用一次）。
    void setStatsCallback(StatsCallback cb) { stats_cb_ = std::move(cb); }

private:
    // 状态改变时通知上层。
    void notifyStatus(const std::string& s);

    // 采集线程函数（视频）。
    void videoCaptureLoop();
    // 采集线程函数（音频）。
    void audioCaptureLoop();
    // 编码线程函数（视频）。
    void videoEncodeLoop();
    // 编码线程函数（音频）。
    void audioEncodeLoop();
    // 监控线程：断线检测 + 重连 + 弱网码率自适应。
    void monitorLoop();

    // 停止标志：所有循环靠它退出。
    std::atomic<bool> running_{ false };

    // ===== 推流核心对象 =====
    FFmpegMuxer muxer_;
    FFmpegVideoEncoder video_encoder_;
    FFmpegAudioEncoder audio_encoder_;
    FFmpegVideoCapturer video_capturer_;
    FFmpegAudioCapturer audio_capturer_;

    // ===== 队列（采集 → 编码解耦）=====
    ThreadSafeQueue<std::shared_ptr<AVFrame>> video_queue_{ 8 };
    ThreadSafeQueue<std::shared_ptr<AVFrame>> audio_queue_{ 64 };

    // ===== 线程 =====
    std::thread video_capture_thread_;
    std::thread audio_capture_thread_;
    std::thread video_encode_thread_;
    std::thread audio_encode_thread_;
    std::thread monitor_thread_;

    // ===== 跨线程信号 =====
    // 断线重连后强制下一帧 I 帧。
    std::atomic<bool> force_idr_{ false };
    // 音频是否已开始（第一帧到达后置 true），视频在音频开始后才编码，保证音画同步。
    std::atomic<bool> audio_started_{ false };
    // 音频第一帧的真实微秒，作为全局对齐锚点。
    std::atomic<int64_t> audio_origin_us_{ -1 };
    // 视频 pts 帧序号，自增。必须是普通成员（每次 start 重新归 0）。
    int64_t video_pts_ = 0;

    // ===== 弱网：动态码率 =====
    // 三档码率，从高到低（bps）。
    static constexpr int kBitrateLadder[3] = { 1000000, 500000, 300000 };
    static constexpr int kBitrateLevels = 3;
    std::atomic<int> bitrate_level_{ 0 };
    std::atomic<int> target_bitrate_{ kBitrateLadder[0] };

    // 状态回调。
    StatusCallback status_cb_;
    // 实时统计回调。
    StatsCallback stats_cb_;

    // 推流开始时间点（用于统计时长）。普通成员，每次 start 重置。
    std::chrono::steady_clock::time_point start_time_{};
    std::atomic<bool> started_{ false };
};

}  // namespace livepusher
