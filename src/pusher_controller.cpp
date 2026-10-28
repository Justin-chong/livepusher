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
bool PusherController::listCaptureDevices(std::vector<std::string>& out_video,
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
bool PusherController::start(const std::string& url, VideoSource video_source,
                             const std::string& record_path) {
    if (running_.load()) {
        LOG_WARN << "already running, stop first";
        return false;
    }

    // 兜底：确保队列处于可用状态（防止上次异常退出没走 reset）。
    video_queue_.reset();
    audio_queue_.reset();
    //Initialize libavdevice and register all the input and output devices.
    avdevice_register_all();

    // 1. 根据视频源确定采集设备名。
    //    屏幕采集走 gdigrab（device_name="desktop"），不需要枚举设备。
    //    摄像头采集才需要枚举 dshow 设备。
    const bool is_screen = (video_source == VideoSource::Screen);
    std::string cam_name;
    std::string mic_name;
    if (is_screen) {
        // 屏幕采集时，麦克风仍然照常用（直播屏幕 + 解说声音）
        std::vector<std::string> cams, mics;
        listCaptureDevices(cams, mics);
        mic_name = mics.empty() ? "" : mics.front();
        LOG_INFO << "video source=screen, mic=" << mic_name;
    } else {
        std::vector<std::string> cameras, mics;
        if (!listCaptureDevices(cameras, mics)) {
            LOG_ERROR << "no camera found";
            notifyStatus("无摄像头");
            return false;
        }
        cam_name = cameras.front();
        mic_name = mics.empty() ? "" : mics.front();
        LOG_INFO << "camera=" << cam_name << ", mic=" << mic_name;
    }

    // 2. 打开 muxer（RTMP 或文件）
    if (!muxer_.open(url)) {
        LOG_ERROR << "open url failed: " << url;
        notifyStatus("打开地址失败");
        return false;
    }

    // 3. 打开编码器。分辨率按视频源选：摄像头用 640x480，屏幕用 1280x800
    //    （你的屏幕 2560x1600 是 16:10，等比缩小一半，画面不变形且更清晰）。
    int enc_width = 640, enc_height = 480;
    if (is_screen) {
        enc_width = 1280;
        enc_height = 800;
    }
    if (!video_encoder_.open(enc_width, enc_height, 30, 1000000)) {
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

    // 3.5 本地录制（可选）：record_path 非空时，创建独立的 mp4 录制器。
    //     录制器复制推流 muxer 的编码器参数（codecpar），所以音视频编码完全一致。
    //     因为录制和推流同时开始，pts 从 0 起算，天然对齐，无需偏移。
    const bool do_record = !record_path.empty();
    if (do_record) {
        if (!recorder_.open(record_path)) {
            LOG_ERROR << "open recorder failed: " << record_path;
            notifyStatus("录制文件打开失败");
            // 录制失败不阻断推流，只放弃录制。
        } else {
            recorder_.addVideoStream(video_encoder_.getCodecCtx());
            recorder_.addAudioStream(audio_encoder_.getCodecCtx());
            recorder_.writeHeader();
            LOG_INFO << "recording enabled -> " << record_path;
        }
    }

    // 4. 注册断线回调（只通知，重连由 monitor 线程做，避免锁内重入）
    muxer_.setDisconnectCallback([](int last_error) {
        LOG_ERROR << "[DISCONNECT] connection lost, last_error=" << last_error;
        });

    // 5. 编码回调 → 写 muxer（推流），同时写 recorder（本地录制）。
    //    同一个 AVPacket 喂给两个 muxer，不重复编码，CPU 几乎零开销（只多磁盘 IO）。
    video_encoder_.set_callback([&, v_idx](std::shared_ptr<AVPacket> pkt) {
        muxer_.writePacket(pkt, v_idx);
        if (recorder_.isOpen()) {
            recorder_.writePacket(pkt, v_idx);
        }
        });
    audio_encoder_.set_callback([&, a_idx](std::shared_ptr<AVPacket> pkt) {
        // AAC 编码器 delay，首个包 pts 为负，丢弃。
        if (pkt->pts < 0) {
            return;
        }
        muxer_.writePacket(pkt, a_idx);
        if (recorder_.isOpen()) {
            recorder_.writePacket(pkt, a_idx);
        }
        });

    // 6. 复位跨线程状态
    running_.store(true);
    force_idr_.store(false);
    audio_started_.store(false);
    audio_origin_us_.store(-1);
    video_origin_us_.store(-1);
    bitrate_level_.store(0);
    target_bitrate_.store(kBitrateLadder[0]);
    drop_video_.store(false);
    drop_counter_ = 0;

    // 7. 启动编码线程（消费队列）
    video_encode_thread_ = std::thread(&PusherController::videoEncodeLoop, this);
    audio_encode_thread_ = std::thread(&PusherController::audioEncodeLoop, this);

    // 8. 设置采集回调：视频做 swscale 格式转换后入队，音频直接入队。
    //    sws_ctx 用 shared_ptr 捕获，线程间共享（只在采集线程创建、复用）。
    //    摄像头帧是 yuvj422p，屏幕帧是 bgra——sws_getContext 会按帧的实际格式
    //    自动适配，所以这段转换逻辑对两者通用。
    auto sws_ctx = std::make_shared<SwsContext*>(nullptr);
    // 预览用的 YUV→RGB 转换上下文（独立于编码用的 yuv420p 转换）。
    auto preview_sws = std::make_shared<SwsContext*>(nullptr);

    // 通用视频帧处理：转 yuv420p 入编码队列 + 转 RGB 做本地预览。
    // 关键：目标尺寸用【编码器的分辨率】，而不是源帧分辨率。
    // 源可能是摄像头 640x480，也可能是屏幕 2560x1600——统一缩放到编码器
    // 尺寸再编码。否则编码器按自己的宽高读帧，大画面只能读到左上角一块。
    auto handle_video_frame = [this, sws_ctx, preview_sws](
                                  std::shared_ptr<AVFrame> frame) {
        // 音频还没开始：这段视频没有对应声音，丢弃。
        if (!audio_started_.load()) {
            return;
        }

        const int enc_w = video_encoder_.getCodecCtx()->width;
        const int enc_h = video_encoder_.getCodecCtx()->height;

        // 转 yuv420p（编码器要求），源格式（yuvj422p/bgra）由 sws 自动适配，
        // 同时缩放到编码器分辨率。
        if (!*sws_ctx) {
            *sws_ctx = sws_getContext(
                frame->width, frame->height, (AVPixelFormat)frame->format,
                enc_w, enc_h, AV_PIX_FMT_YUV420P,
                SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!*sws_ctx) {
                LOG_ERROR << "sws_getContext failed";
                return;
            }
        }

        std::shared_ptr<AVFrame> yuv(av_frame_alloc(),
            [](AVFrame* f) { av_frame_free(&f); });
        yuv->format = AV_PIX_FMT_YUV420P;
        yuv->width = enc_w;
        yuv->height = enc_h;
        av_frame_get_buffer(yuv.get(), 32);

        sws_scale(*sws_ctx,
                  frame->data, frame->linesize, 0, frame->height,
                  yuv->data, yuv->linesize);

        // 视频 pts 用「真实时间」换算成帧单位，而不是帧序号自增。
        // 原因：帧序号自增隐含假设每帧固定 1/fps 秒，但屏幕采集帧率会掉
        // （2560x1600 全屏 + 解码 + 缩放 CPU 吃紧，可能只有 15fps）。
        // 若仍每帧 +1，实际过了 1 秒但 pts 只走了 0.5 秒 → 录制/播放被"加速"。
        // 用采集帧的真实微秒 pts 换算：相邻帧间隔自动反映真实帧率。
        if (video_origin_us_.load() < 0) {
            video_origin_us_.store(frame->pts);
        }
        int fps = video_encoder_.getCodecCtx()->framerate.num;
        int64_t delta_us = frame->pts - video_origin_us_.load();
        // 微秒 → 帧单位（time_base = {1, fps}）：pts = 微秒 * fps / 1e6
        yuv->pts = delta_us * fps / 1000000;

        //丢的是新帧，虽然会保证数据完整性，但是因为旧帧一直积累，新帧被丢弃，就会导致延迟积累
        //优化点：将丢新帧 改为 丢旧帧（适合直播，低延迟）
        if (!video_queue_.try_push(yuv)) {
            LOG_WARN << "video queue full, drop frame";
        }

        // ===== 本地预览：把同一帧额外转成 RGB24 回调给 GUI =====
        // 预览也缩到编码器尺寸，避免大分辨率 swscale 的 CPU 浪费
        // （反正窗口显示时还要缩小）。
        //采集帧(yuvj422p / bgra)
        //    ├─→ 转 YUV420P → video_queue_ → 编码线程（慢，走完整编码）
        //    └─→ 转 RGB24   → 直接回调 GUI（快，不过编码器）

        if (preview_cb_) {
            if (!*preview_sws) {
                *preview_sws = sws_getContext(
                    frame->width, frame->height, (AVPixelFormat)frame->format,
                    enc_w, enc_h, AV_PIX_FMT_RGB24,
                    SWS_BILINEAR, nullptr, nullptr, nullptr);
            }
            if (*preview_sws) {
                std::shared_ptr<AVFrame> rgb(av_frame_alloc(),
                    [](AVFrame* f) { av_frame_free(&f); });
                rgb->format = AV_PIX_FMT_RGB24;
                rgb->width = enc_w;
                rgb->height = enc_h;
                av_frame_get_buffer(rgb.get(), 32);

                sws_scale(*preview_sws,
                          frame->data, frame->linesize, 0, frame->height,
                          rgb->data, rgb->linesize);
                //跨线程不能直接传QImage，
                preview_cb_(rgb->data[0], enc_w, enc_h);
            }
        }
    };

    // 摄像头和屏幕共用同一个帧处理逻辑
    //注册采集回调
    video_capturer_.set_callback(handle_video_frame);
    screen_capturer_.set_callback(handle_video_frame);

    //注册音频采集回调
    audio_capturer_.set_callback([this](std::shared_ptr<AVFrame> frame) {
        audio_started_.store(true);
        if (audio_origin_us_.load() < 0) {
            audio_origin_us_.store(frame->pts);
        }
        if (!audio_queue_.try_push(frame)) {
            LOG_WARN << "audio queue full, drop frame";
        }
        });

    // 9. 打开采集器（根据视频源选摄像头或屏幕）
    if (is_screen) {
        // 屏幕采集：gdigrab，device_name="desktop"（全屏）。分辨率由 gdigrab 自动探测。
        if (!screen_capturer_.open("desktop", 0, 0, 30)) {
            LOG_ERROR << "open screen failed";
            stop();
            return false;
        }
    } else {
        if (!video_capturer_.open("video=" + cam_name, 640, 480, 30)) {
            LOG_ERROR << "open camera failed";
            stop();
            return false;
        }
    }
    if (!mic_name.empty() && !audio_capturer_.open("audio=" + mic_name, 44100, 1)) {
        LOG_ERROR << "open mic failed";
        stop();
        return false;
    }

    // 9. 启动监控线程（断线重连 + 弱网自适应）
    monitor_thread_ = std::thread(&PusherController::monitorLoop, this);

    // 10. 采集器 start（内部开采集线程，回调里入队）
    if (is_screen) {
        screen_capturer_.start();
    } else {
        video_capturer_.start();
    }
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
    screen_capturer_.stop();
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

    // 6. 关闭编码器、muxer、录制器
    video_encoder_.close();
    audio_encoder_.close();
    muxer_.close();
    // 录制器写尾（mp4 必须写 moov 索引才能播放）。在编码线程退出后调用，
    // 确保没有线程还在写录制器。
    recorder_.close();

    notifyStatus("已停止");
    started_.store(false);
    LOG_INFO << "===== pusher stopped =====";
}

//视频编码（循环取队列帧 + 做控制逻辑 + 调 encode）
void PusherController::videoEncodeLoop() {
    std::shared_ptr<AVFrame> frame;
    while (video_queue_.pop(frame)) {
        // 弱网丢帧：码率降到最低档仍拥堵时，主动丢视频 P 帧（帧率减半）。
        // 只丢 P 帧，不丢 I 帧（I 帧是参考锚点，丢了后面全花）；不动音频。
        if (drop_video_.load()) {
            ++drop_counter_;
            bool is_keyframe = (frame->pict_type == AV_PICTURE_TYPE_I);
            // 每 2 帧丢 1 帧，且关键帧永不丢
            if (drop_counter_ % 2 == 0 && !is_keyframe) {
                continue;   // 跳过这帧，不编码
            }
        }
        // 强制 I 帧
        if (force_idr_.exchange(false)) {
            frame->pict_type = AV_PICTURE_TYPE_I;
        }

        // 动态码率：目标变了就调编码器
        int want = target_bitrate_.load();
        if (want != video_encoder_.getCodecCtx()->bit_rate) {
            video_encoder_.setBitrate(want);
        }
        //调用编码器编码函数，只做"送帧编码取包回调"
        video_encoder_.encode(frame);
    }
    video_encoder_.encode(nullptr);  // 冲刷
    LOG_INFO << "video encode thread exit";
}

// 音频编码
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
    //只要推流器没退出，就一直循环，每500ms调用一次，获取发送字节增量/时间间隔=发送速率
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

        // ---- 阶段1：发送速率监控（提前降档 + 最低档丢帧）----
        //false 代表没断，则可以计算发送速率
        if (!muxer_.isDisconnected()) {
            int64_t send_rate = muxer_.estimateSendRate();
            int cur_bitrate = target_bitrate_.load();
            if (send_rate > 0 && cur_bitrate > 0) {
                //发送速率<目标速率*0.8  降档处理
                if (send_rate < static_cast<int64_t>(cur_bitrate * kCongestRatio)) {
                    ++low_rate_count;
                    //降档降到底
                    if (low_rate_count >= kLowRateSamplesToDowngrade) {
                        //bitrate_level_开始是0挡
                        if (bitrate_level_.load() < kBitrateLevels - 1) {
                            // 还能降档：继续降码率
                            int nl = bitrate_level_.load() + 1;
                            bitrate_level_.store(nl);
                            target_bitrate_.store(kBitrateLadder[nl]);
                            last_downgrade_time = steady_clock::now();
                            low_rate_count = 0;
                            LOG_INFO << "[BITRATE] congestion downgrade -> level " << nl
                                     << " (" << kBitrateLadder[nl] / 1000 << " kbps)";
                        } else {
                            // 已到最低档仍拥堵：启用丢帧（每 2 帧丢 1 帧 P 帧）
                            if (!drop_video_.load()) {
                                drop_video_.store(true);
                                LOG_WARN << "[DROP] 最低码率仍拥堵，启用丢帧（帧率减半）";
                            }
                            low_rate_count = 0;
                        }
                    }
                } 
                //速率够
                else {
                    low_rate_count = 0;
                    // 带宽恢复：取消丢帧
                    if (drop_video_.load()) {
                        drop_video_.store(false);
                        LOG_INFO << "[DROP] 带宽恢复，取消丢帧";
                    }
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
