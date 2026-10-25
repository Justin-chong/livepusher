#pragma once

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include <functional>
#include <chrono>

#include "common/thread_safe_queue.h"
#include "capture/ffmpeg_video_capturer.h"
#include "capture/ffmpeg_audio_capturer.h"
#include "capture/ffmpeg_screen_capturer.h"
#include "encode/ffmpeg_video_encode.h"
#include "encode/ffmpeg_audio_encode.h"
#include "muxer/ffmpeg_muxer.h"
#include "muxer/ffmpeg_recorder.h"

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

    // 预览帧回调：把采集到的视频帧（已转 RGB24）回调给 GUI 做本地预览。
    // 参数：RGB 数据指针、宽、高（RGB24 每像素 3 字节，行对齐为 width*3）。
    // 在采集线程里调用，GUI 需自行切回主线程。
    using PreviewFrameCallback =
        std::function<void(const uint8_t* rgb, int width, int height)>;

    PusherController();
    ~PusherController();

    PusherController(const PusherController&) = delete;
    PusherController& operator=(const PusherController&) = delete;

    // 视频源类型。
    enum class VideoSource {
        Camera,   // 摄像头（dshow）
        Screen,   // 屏幕（gdigrab）
    };

    // 开始推流到 url（如 rtmp://...）。内部启动采集、编码、监控线程。
    // video_source 指定视频源（摄像头/屏幕），默认摄像头。
    // record_path 非空时，推流的同时录制一份到该本地 mp4 文件（可选）。
    // 失败返回 false（比如设备打不开），成功返回 true。
    bool start(const std::string& url, VideoSource video_source = VideoSource::Camera,
               const std::string& record_path = "");

    // 停止推流：关闭采集、队列、编码线程、muxer。
    void stop();

    // 当前是否正在推流。
    bool isRunning() const { return running_.load(); }

    // 注册状态回调。
    void setStatusCallback(StatusCallback cb) { status_cb_ = std::move(cb); }

    // 注册实时统计回调（监控线程每 500ms 调用一次）。
    void setStatsCallback(StatsCallback cb) { stats_cb_ = std::move(cb); }

    // 注册预览帧回调（采集线程每帧调用，用于本地预览）。
    void setPreviewCallback(PreviewFrameCallback cb) { preview_cb_ = std::move(cb); }

    // 枚举本机采集设备（dshow），供 GUI 下拉框填充。
    // 成功返回 true，cams/mics 分别是摄像头/麦克风友好名列表。
    static bool listCaptureDevices(std::vector<std::string>& cams,
                                   std::vector<std::string>& mics);

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
    FFmpegRecorder recorder_;   // 本地录制器（可选，start 时传 record_path 才启用）
    FFmpegVideoEncoder video_encoder_;
    FFmpegAudioEncoder audio_encoder_;
    FFmpegVideoCapturer video_capturer_;
    FFmpegAudioCapturer audio_capturer_;
    FFmpegScreenCapturer screen_capturer_;

    // ===== 队列（采集 → 编码解耦）=====
    ThreadSafeQueue<std::shared_ptr<AVFrame>> video_queue_{ 8 };
    ThreadSafeQueue<std::shared_ptr<AVFrame>> audio_queue_{ 64 };

    // ===== 线程 =====
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
    // 视频第一帧的真实微秒（采集帧 pts 是 Clock::now_us() 微秒）。
    // 视频 pts 用「真实时间」而不是「帧序号」：屏幕采集帧率可能掉到 15fps，
    // 若仍按帧序号每帧 +1（假设固定 1/fps 秒），时间就走快，录制 mp4 被"加速"。
    std::atomic<int64_t> video_origin_us_{ -1 };

    // ===== 弱网：动态码率 =====
    // 三档码率，从高到低（bps）。
    static constexpr int kBitrateLadder[3] = { 1000000, 500000, 300000 };
    static constexpr int kBitrateLevels = 3;
    std::atomic<int> bitrate_level_{ 0 };
    std::atomic<int> target_bitrate_{ kBitrateLadder[0] };

    // ===== 弱网：丢帧策略 =====
    // 码率降到最低档仍然拥堵时，主动丢视频 P 帧（每 2 帧丢 1 帧，帧率减半）。
    // 只丢 P 帧、不丢 I 帧、不动音频。
    std::atomic<bool> drop_video_{ false };
    // 丢帧用的计数器（只在编码线程读写，普通成员即可）。
    int drop_counter_ = 0;

    // 状态回调。
    StatusCallback status_cb_;
    // 实时统计回调。
    StatsCallback stats_cb_;
    // 预览帧回调。
    PreviewFrameCallback preview_cb_;

    // 推流开始时间点（用于统计时长）。普通成员，每次 start 重置。
    std::chrono::steady_clock::time_point start_time_{};
    std::atomic<bool> started_{ false };
};

}  // namespace livepusher
