#include "pusher_controller.h"

#include <vector>
#include <chrono>

#include "common/logger.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
#include <libavdevice/avdevice.h>
#include <libswscale/swscale.h>
}

namespace livepusher {

// 用 avdevice 枚举本机所有 dshow 设备，区分摄像头 / 麦克风。
// 用 device_description（友好名），不用 device_name（@device_pnp_ 长路径）。
static bool listDevices(std::vector<std::string>& out_video,
                        std::vector<std::string>& out_audio) {
    out_video.clear();
    out_audio.clear();

    const AVInputFormat* ifmt = av_find_input_format("dshow");
    if (!ifmt) {
        LOG_ERROR << "dshow demuxer not registered";
        return false;
    }

    AVDeviceInfoList* device_list = nullptr;
    int ret = avdevice_list_input_sources(ifmt, "dummy", nullptr, &device_list);
    if (ret < 0 || !device_list) {
        LOG_ERROR << "list input sources failed";
        if (device_list) avdevice_free_list_devices(&device_list);
        return false;
    }

    for (int i = 0; i < device_list->nb_devices; ++i) {
        const AVDeviceInfo* dev = device_list->devices[i];
        bool is_video = false, is_audio = false;
        for (int j = 0; j < dev->nb_media_types; ++j) {
            if (dev->media_types[j] == AVMEDIA_TYPE_VIDEO) is_video = true;
            if (dev->media_types[j] == AVMEDIA_TYPE_AUDIO) is_audio = true;
        }
        if (is_video) {
            out_video.push_back(dev->device_description);
        } else if (is_audio) {
            out_audio.push_back(dev->device_description);
        }
    }

    avdevice_free_list_devices(&device_list);
    return !out_video.empty();
}

PusherController::PusherController() {
    LOG_INFO << "PusherController created";
}

PusherController::~PusherController() {
    stop();
    LOG_INFO << "PusherController destroyed";
}

void PusherController::notifyStatus(const std::string& s) {
    if (status_cb_) {
        status_cb_(s);
    }
}

// ===== 开始推流 =====
bool PusherController::start(const std::string& url) {
    if (running_.load()) {
        LOG_WARN << "already running, stop first";
        return false;
    }

    // 兜底：确保队列处于可用状态（防止上次异常退出没走 reset）。
    video_queue_.reset();
    audio_queue_.reset();

    avdevice_register_all();

    // 1. 枚举设备，选第一个摄像头和麦克风
    std::vector<std::string> cameras, mics;
    if (!listDevices(cameras, mics)) {
        LOG_ERROR << "no camera found";
        notifyStatus("无摄像头");
        return false;
    }
    const std::string cam_name = cameras.front();
    std::string mic_name = mics.empty() ? "" : mics.front();
    LOG_INFO << "camera=" << cam_name << ", mic=" << mic_name;

    // 2. 打开 muxer（RTMP 或文件）
    if (!muxer_.open(url)) {
        LOG_ERROR << "open url failed: " << url;
        notifyStatus("打开地址失败");
        return false;
    }

    // 3. 打开编码器
    if (!video_encoder_.open(640, 480, 30, 1000000)) {
        LOG_ERROR << "open video encoder failed";
        notifyStatus("视频编码器打开失败");
        return false;
    }
    int v_idx = muxer_.addVideoStream(video_encoder_.getCodecCtx());

    if (!audio_encoder_.open(44100, 1, 128000)) {
        LOG_ERROR << "open audio encoder failed";
        notifyStatus("音频编码器打开失败");
        return false;
    }
    int a_idx = muxer_.addAudioStream(audio_encoder_.getCodecCtx());

    muxer_.writeHeader();

    // 4. 注册断线回调（只通知，重连由 monitor 线程做，避免锁内重入）
    muxer_.setDisconnectCallback([](int last_error) {
        LOG_ERROR << "[DISCONNECT] connection lost, last_error=" << last_error;
        });

    // 5. 编码回调 → 写 muxer
    video_encoder_.set_callback([&, v_idx](std::shared_ptr<AVPacket> pkt) {
        muxer_.writePacket(pkt, v_idx);
        });
    audio_encoder_.set_callback([&, a_idx](std::shared_ptr<AVPacket> pkt) {
        // AAC 编码器 delay，首个包 pts 为负，丢弃。
        if (pkt->pts < 0) {
            return;
        }
        muxer_.writePacket(pkt, a_idx);
        });

    // 6. 复位跨线程状态
    running_.store(true);
    force_idr_.store(false);
    audio_started_.store(false);
    audio_origin_us_.store(-1);
    video_pts_ = 0;
    bitrate_level_.store(0);
    target_bitrate_.store(kBitrateLadder[0]);

    // 7. 启动编码线程（消费队列）
    video_encode_thread_ = std::thread(&PusherController::videoEncodeLoop, this);
    audio_encode_thread_ = std::thread(&PusherController::audioEncodeLoop, this);

    // 8. 设置采集回调：视频做 swscale 格式转换后入队，音频直接入队。
    //    sws_ctx 用 shared_ptr 捕获，线程间共享（只在采集线程创建、复用）。
    auto sws_ctx = std::make_shared<SwsContext*>(nullptr);

    video_capturer_.set_callback([this, sws_ctx](std::shared_ptr<AVFrame> frame) {
        // 音频还没开始：这段视频没有对应声音，丢弃。
        if (!audio_started_.load()) {
            return;
        }

        // 摄像头解码后是 yuvj422p（或 yuv422p），编码器要 yuv420p，需转换。
        if (!*sws_ctx) {
            *sws_ctx = sws_getContext(
                frame->width, frame->height, (AVPixelFormat)frame->format,
                frame->width, frame->height, AV_PIX_FMT_YUV420P,
                SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!*sws_ctx) {
                LOG_ERROR << "sws_getContext failed";
                return;
            }
        }

        std::shared_ptr<AVFrame> yuv(av_frame_alloc(),
            [](AVFrame* f) { av_frame_free(&f); });
        yuv->format = AV_PIX_FMT_YUV420P;
        yuv->width = frame->width;
        yuv->height = frame->height;
        av_frame_get_buffer(yuv.get(), 32);

        sws_scale(*sws_ctx,
                  frame->data, frame->linesize, 0, frame->height,
                  yuv->data, yuv->linesize);

        yuv->pts = video_pts_++;   // 帧序号自增

        if (!video_queue_.try_push(yuv)) {
            LOG_WARN << "video queue full, drop frame";
        }
        });

    audio_capturer_.set_callback([this](std::shared_ptr<AVFrame> frame) {
        audio_started_.store(true);
        if (audio_origin_us_.load() < 0) {
            audio_origin_us_.store(frame->pts);
        }
        if (!audio_queue_.try_push(frame)) {
            LOG_WARN << "audio queue full, drop frame";
        }
        });

    // 9. 打开采集器
    if (!video_capturer_.open("video=" + cam_name, 640, 480, 30)) {
        LOG_ERROR << "open camera failed";
        stop();
        return false;
    }
    if (!mic_name.empty() && !audio_capturer_.open("audio=" + mic_name, 44100, 1)) {
        LOG_ERROR << "open mic failed";
        stop();
        return false;
    }

    // 9. 启动监控线程（断线重连 + 弱网自适应）
    monitor_thread_ = std::thread(&PusherController::monitorLoop, this);

    // 10. 采集器 start（内部开采集线程，回调里入队）
    video_capturer_.start();
    if (!mic_name.empty()) {
        audio_capturer_.start();
    }

    notifyStatus("推流中");
    started_.store(true);
    start_time_ = std::chrono::steady_clock::now();
    LOG_INFO << "===== push started -> " << url << " =====";
    return true;
}

// ===== 停止推流 =====
void PusherController::stop() {
    if (!running_.load()) {
        return;
    }
    LOG_INFO << "===== stopping pusher =====";

    // 1. 先置停止标志，让各线程退出循环
    running_.store(false);

    // 2. 停采集器（内部会 join 采集线程）
    video_capturer_.stop();
    audio_capturer_.stop();

    // 3. 关队列，唤醒编码线程退出
    video_queue_.close();
    audio_queue_.close();

    // 4. 等监控线程退出
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }

    // 5. 等编码线程退出
    if (video_encode_thread_.joinable()) {
        video_encode_thread_.join();
    }
    if (audio_encode_thread_.joinable()) {
        audio_encode_thread_.join();
    }

    // 5.5 线程都退出了，复位队列，让下次 start() 能重新入队
    // （close 是永久性的，不复位的话第二次推流所有帧都会被丢弃）
    video_queue_.reset();
    audio_queue_.reset();

    // 6. 关闭编码器、muxer
    video_encoder_.close();
    audio_encoder_.close();
    muxer_.close();

    notifyStatus("已停止");
    started_.store(false);
    LOG_INFO << "===== pusher stopped =====";
}

// ===== 视频编码线程 =====
void PusherController::videoEncodeLoop() {
    std::shared_ptr<AVFrame> frame;
    while (video_queue_.pop(frame)) {
        if (force_idr_.exchange(false)) {
            frame->pict_type = AV_PICTURE_TYPE_I;
        }

        // 动态码率：目标变了就调编码器
        int want = target_bitrate_.load();
        if (want != video_encoder_.getCodecCtx()->bit_rate) {
            video_encoder_.setBitrate(want);
        }

        video_encoder_.encode(frame);
    }
    video_encoder_.encode(nullptr);  // 冲刷
    LOG_INFO << "video encode thread exit";
}

// ===== 音频编码线程 =====
void PusherController::audioEncodeLoop() {
    std::shared_ptr<AVFrame> frame;
    while (audio_queue_.pop(frame)) {
        audio_encoder_.encode(frame);
    }
    audio_encoder_.encode(nullptr);  // 冲刷
    LOG_INFO << "audio encode thread exit";
}

// ===== 视频采集回调（由采集线程调用）=====
void PusherController::videoCaptureLoop() {
    // 本函数实际不会被直接调用 —— 采集由 FFmpegVideoCapturer 内部线程驱动，
    // 帧通过 set_callback 进入。这里保留占位以满足语义完整性。
}

// ===== 音频采集回调（由采集线程调用）=====
void PusherController::audioCaptureLoop() {
    // 同 videoCaptureLoop，保留占位。
}

// ===== 监控线程：断线重连 + 弱网自适应 =====
void PusherController::monitorLoop() {
    using namespace std::chrono;

    auto last_downgrade_time = steady_clock::now();
    static constexpr int kStableSecondsToUpgrade = 3;
    static constexpr int kLowRateSamplesToDowngrade = 4;
    static constexpr double kCongestRatio = 0.8;
    int low_rate_count = 0;

    while (running_.load()) {
        // ---- 阶段2：断线 → 降档 + 重连 ----
        if (muxer_.isDisconnected()) {
            LOG_ERROR << "[RECONNECT] disconnected, trying reconnect...";

            if (bitrate_level_.load() < kBitrateLevels - 1) {
                int nl = bitrate_level_.load() + 1;
                bitrate_level_.store(nl);
                target_bitrate_.store(kBitrateLadder[nl]);
                last_downgrade_time = steady_clock::now();
                low_rate_count = 0;
                LOG_INFO << "[BITRATE] disconnect downgrade -> level " << nl
                         << " (" << kBitrateLadder[nl] / 1000 << " kbps)";
            }

            if (muxer_.reconnect(video_encoder_.getCodecCtx(),
                                 audio_encoder_.getCodecCtx())) {
                force_idr_.store(true);
                notifyStatus("重连成功");
                LOG_INFO << "[RECONNECT] reconnect ok";
            } else {
                notifyStatus("重连失败");
                LOG_ERROR << "[RECONNECT] reconnect failed";
            }
        }

        // ---- 阶段1：发送速率监控（提前降档）----
        if (!muxer_.isDisconnected()) {
            int64_t send_rate = muxer_.estimateSendRate();
            int cur_bitrate = target_bitrate_.load();
            if (send_rate > 0 && cur_bitrate > 0) {
                if (send_rate < static_cast<int64_t>(cur_bitrate * kCongestRatio)) {
                    ++low_rate_count;
                    if (low_rate_count >= kLowRateSamplesToDowngrade
                        && bitrate_level_.load() < kBitrateLevels - 1) {
                        int nl = bitrate_level_.load() + 1;
                        bitrate_level_.store(nl);
                        target_bitrate_.store(kBitrateLadder[nl]);
                        last_downgrade_time = steady_clock::now();
                        low_rate_count = 0;
                        LOG_INFO << "[BITRATE] congestion downgrade -> level " << nl
                                 << " (" << kBitrateLadder[nl] / 1000 << " kbps)";
                    }
                } else {
                    low_rate_count = 0;
                }
            }
        }

        // ---- 恢复升档 ----
        if (bitrate_level_.load() > 0) {
            auto stable_for = duration_cast<seconds>(
                steady_clock::now() - last_downgrade_time).count();
            if (stable_for >= kStableSecondsToUpgrade) {
                int nl = bitrate_level_.load() - 1;
                bitrate_level_.store(nl);
                target_bitrate_.store(kBitrateLadder[nl]);
                last_downgrade_time = steady_clock::now();
                low_rate_count = 0;
                LOG_INFO << "[BITRATE] recover upgrade -> level " << nl
                         << " (" << kBitrateLadder[nl] / 1000 << " kbps)";
            }
        }

        // ---- 上报实时统计（给 GUI 状态栏）----
        if (stats_cb_) {
            Stats s;
            s.bitrate_level = bitrate_level_.load();
            s.target_bitrate = target_bitrate_.load();
            s.disconnected = muxer_.isDisconnected();
            s.send_rate = muxer_.isDisconnected()
                ? 0 : muxer_.estimateSendRate();
            if (started_.load()) {
                s.elapsed_ms = duration_cast<milliseconds>(
                    steady_clock::now() - start_time_).count();
            }
            stats_cb_(s);
        }

        std::this_thread::sleep_for(milliseconds(500));
    }
    LOG_INFO << "monitor thread exit";
}

}  // namespace livepusher
