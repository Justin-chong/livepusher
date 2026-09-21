#include <iostream>
#include <thread>
#include <chrono>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "common/thread_safe_queue.h"
#include "common/clock.h"
#include "common/logger.h"
#include "capture/ffmpeg_video_capturer.h"
#include "capture/ffmpeg_audio_capturer.h"
#include "encode/ffmpeg_video_encode.h"
#include "encode/ffmpeg_audio_encode.h"
#include "muxer/ffmpeg_muxer.h"

using namespace livepusher;

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
#include <libavdevice/avdevice.h>
#include <libswscale/swscale.h>
}

using livepusher::ThreadSafeQueue;

void test_thread_safe_queue() {
    std::cout << "\n====== ThreadSafeQueue Test ======" << std::endl;

    ThreadSafeQueue<int> queue(5);

    std::thread producer([&queue]() {
        for (int i = 0; i < 10; ++i) {
            queue.push(i);
            std::cout << "[Producer] push: " << i
                      << " (size: " << queue.size() << ")" << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        queue.close();
        std::cout << "[Producer] queue closed" << std::endl;
    });

    std::thread consumer([&queue]() {
        int value;
        while (queue.pop(value)) {
            std::cout << "[Consumer] pop: " << value
                      << " (size: " << queue.size() << ")" << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::cout << "[Consumer] queue empty and closed, exit" << std::endl;
    });

    producer.join();
    consumer.join();

    std::cout << "====== Queue Test Done ======" << std::endl;
}

void test_clock() {
    std::cout << "\n====== Clock Test ======" << std::endl;

    Clock clock;

    std::cout << "elapsed_us before start: " << clock.elapsed_us() << std::endl;

    clock.start();
    std::cout << "elapsed_us after start: " << clock.elapsed_us() << std::endl;

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::cout << "elapsed_us after 500ms sleep: " << clock.elapsed_us() << std::endl;
    std::cout << "elapsed_ms after 500ms sleep: " << clock.elapsed_ms() << std::endl;

    std::cout << "====== Clock Test Done ======" << std::endl;
}

// 用 FFmpeg 的 avdevice API 枚举本机所有 dshow 视频设备（摄像头）
// 返回值：通过 out_devices 输出设备名列表；函数返回 true 表示枚举成功
// 实现思路：
//   FFmpeg 9.0 里 avdevice_list_input_sources() 是"convenient wrapper"，
//   会自己创建/销毁 ctx。我们只要告诉它 dshow demuxer + 一个 dummy 设备名，
//   它就返回 AVDeviceInfoList，里面的每个设备有 media_types[] 数组标识
//   自己支持哪些媒体类型（视频/音频）。
bool list_dshow_cameras(std::vector<std::string>& out_devices) {
    out_devices.clear();

    // 找到 dshow demuxer（如果还没注册 avdevice，这里会返回 nullptr）
    const AVInputFormat* ifmt = av_find_input_format("dshow");
    if (!ifmt) {
        LOG_ERROR << "[list_dshow_cameras] dshow demuxer not registered. "
                     "Did you forget avdevice_register_all()?";
        return false;
    }

    // list_input_sources 内部会自动用 dummy 字符串 + dshow 触发枚举
    // 然后把结果放进 AVDeviceInfoList
    AVDeviceInfoList* device_list = nullptr;
    int ret = avdevice_list_input_sources(ifmt, "dummy", nullptr, &device_list);
    if (ret < 0 || !device_list) {
        char errbuf[128] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR << "[list_dshow_cameras] list_input_sources failed: " << errbuf;
        if (device_list) avdevice_free_list_devices(&device_list);
        return false;
    }

    LOG_INFO << "=== Found " << device_list->nb_devices
             << " dshow input device(s) ===";
    for (int i = 0; i < device_list->nb_devices; ++i) {
        const AVDeviceInfo* dev = device_list->devices[i];

        // 判断这个设备到底是摄像头（video）、麦克风（audio）还是两者都有
        bool is_video = false, is_audio = false;
        for (int j = 0; j < dev->nb_media_types; ++j) {
            if (dev->media_types[j] == AVMEDIA_TYPE_VIDEO) is_video = true;
            if (dev->media_types[j] == AVMEDIA_TYPE_AUDIO) is_audio = true;
        }

        if (is_video) {
            // 用 device_description（友好名如 "Integrated Camera"），不要用 device_name
            // （dshow 的 device_name 是 @device_pnp_... 长路径，能 open 成功但读不到帧）
            out_devices.push_back(dev->device_description);
            LOG_INFO << "  [cam " << out_devices.size() - 1 << "] "
                     << dev->device_description
                     << (is_audio ? " (video+audio)" : "");
        } else if (is_audio) {
            LOG_INFO << "  [mic] " << dev->device_description << " (skipped, audio only)";
        } else {
            LOG_INFO << "  [dev] " << dev->device_description << " (no media type)";
        }
    }
    LOG_INFO << "=== end ===";

    avdevice_free_list_devices(&device_list);
    return !out_devices.empty();
}

void test_video_capture() {
    LOG_INFO << "====== Video Capture Test ======";

    // 先列本机所有摄像头，让用户看到名字，再挑一个用
    std::vector<std::string> cameras;
    if (!list_dshow_cameras(cameras)) {
        LOG_ERROR << "No dshow camera found on this machine";
        return;
    }

    // 默认选第一个摄像头；如果你想换别的，改这里的下标即可
    const std::string& picked = cameras.front();
    // dshow 的 URL 规范：video=<设备名>（必须带 video= 前缀）
    std::string device_name = "video=" + picked;
    LOG_INFO << "Picking camera: " << device_name;

    FFmpegVideoCapturer capturer;

    if (!capturer.open(device_name, 640, 480, 30)) {
        LOG_ERROR << "Failed to open device: " << device_name;
        return;
    }

    int frame_count = 0;
    auto start_time = std::chrono::steady_clock::now();

    capturer.set_callback([&frame_count](std::shared_ptr<AVFrame> frame) {
        frame_count++;
        if (frame_count <= 5 || frame_count % 30 == 0) {
            LOG_INFO << "Frame " << frame_count << ": "
                     << frame->width << "x" << frame->height
                     << ", pts=" << frame->pts;
        }
    });

    capturer.start();

    std::this_thread::sleep_for(std::chrono::seconds(5));

    capturer.stop();
    capturer.close();

    auto end_time = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time).count();

    LOG_INFO << "Captured " << frame_count << " frames in 5 sec"
             << ", fps=" << (frame_count * 1000 / elapsed_ms);
    LOG_INFO << "====== Video Capture Test Done ======";
}

void test_audio_capture() {
    LOG_INFO << "====== Audio Capture Test ======";

    // 麦克风 dshow 设备名
    std::string mic_name =
        "audio=@device_cm_{33D9A762-90C8-11D0-BD43-00A0C911CE86}\\wave_{8C14BC95-A536-4ECD-BAF7-A9DC0A02B6CD}";

    FFmpegAudioCapturer capturer;

    // 设置回调：打印音频帧信息
    capturer.set_callback([](std::shared_ptr<AVFrame> frame) {
        static int count = 0;
        count++;
        if (count <= 5 || count % 50 == 0) {
            LOG_INFO << "Audio frame " << count
                << ": " << frame->nb_samples << " samples"
                << ", pts=" << frame->pts;
        }
        });

    // 打开麦克风：44100Hz, 单声道
    if (!capturer.open(mic_name, 44100, 1)) {
        LOG_ERROR << "Open mic failed";
        return;
    }

    // 开始采集
    capturer.start();

    // 采集 5 秒
    std::this_thread::sleep_for(std::chrono::seconds(5));

    // 停止并关闭
    capturer.stop();
    capturer.close();

    LOG_INFO << "====== Audio Capture Test Done ======";
}


// 在 main.cpp 里测试
void test_video_encoder() {
    LOG_INFO << "====== Video Encoder Test ======";

    FFmpegVideoEncoder encoder;
    encoder.open(640, 480, 30, 1000000);  // 640x480, 30fps, 1Mbps

    // 设置回调：打印编码出的包信息
    encoder.set_callback([](std::shared_ptr<AVPacket> pkt) {
        LOG_INFO << "Got packet: size=" << pkt->size
            << ", pts=" << pkt->pts
            << ", keyframe=" << (pkt->flags & AV_PKT_FLAG_KEY);
        });

    // 造一帧 YUV420P
    AVFrame* frame = av_frame_alloc();
    frame->width = 640;
    frame->height = 480;
    frame->format = AV_PIX_FMT_YUV420P;
    av_frame_get_buffer(frame, 32);

    // 填数据：Y=128(灰), U=128, V=128
    // YUV420P: Y平面 640x480, U平面 320x240, V平面 320x240
    memset(frame->data[0], 128, frame->linesize[0] * 480);       // Y
    memset(frame->data[1], 128, frame->linesize[1] * 240);       // U
    memset(frame->data[2], 128, frame->linesize[2] * 240);       // V

    // 连续送 30 帧（1秒）
    for (int i = 0; i < 30; i++) {
        frame->pts = i * 33000;  // 每帧 33ms（微秒）
        encoder.encode(std::shared_ptr<AVFrame>(frame, [](AVFrame* f) {
            // 测试用，不释放（只分配了一次）
            }));
    }
    encoder.encode(nullptr);//冲刷
    av_frame_free(&frame);
    encoder.close();
    LOG_INFO << "====== Encoder Test Done ======";
}


void test_audio_encoder() {
    LOG_INFO << "====== Audio Encoder Test ======";

    FFmpegAudioEncoder encoder;
    encoder.open(44100, 1, 128000);  // 44100Hz, 单声道, 128kbps

    encoder.set_callback([](std::shared_ptr<AVPacket> pkt) {
        LOG_INFO << "Got AAC packet: size=" << pkt->size
            << ", pts=" << pkt->pts;
        });

    // 造一帧静音 PCM（1024 个采样点，单声道 16bit = 2048 字节）
    AVFrame* frame = av_frame_alloc();
    frame->sample_rate = 44100;
    frame->ch_layout = AV_CHANNEL_LAYOUT_MONO;
    frame->format = AV_SAMPLE_FMT_FLTP;
    frame->nb_samples = 1024;
    av_frame_get_buffer(frame, 0);

    // 静音：全填 0
    memset(frame->data[0], 0, frame->linesize[0]);

    // 送 50 帧（约 50 * 1024 / 44100 ≈ 1.16 秒）
    for (int i = 0; i < 50; i++) {
        frame->pts = i * 1024;  // 每帧 1024 个采样点
        encoder.encode(std::shared_ptr<AVFrame>(frame, [](AVFrame* f) {
            // 测试用，不释放
            }));
    }

    // 冲洗
    encoder.encode(nullptr);

    av_frame_free(&frame);
    encoder.close();
    LOG_INFO << "====== Audio Encoder Test Done ======";
}

//封装测试
void test_muxer() {
    LOG_INFO << "====== FLV Muxer Test ======";

    // 1. 创建编码器
    FFmpegVideoEncoder video_encoder;
    video_encoder.open(640, 480, 30, 1000000);

    FFmpegAudioEncoder audio_encoder;
    audio_encoder.open(44100, 1, 128000);

    // 2. 创建封装器，打开文件
    FFmpegMuxer muxer;
    muxer.open("test.flv");

    // 3. 添加流
    int v_idx = muxer.addVideoStream(video_encoder.getCodecCtx());
    int a_idx = muxer.addAudioStream(audio_encoder.getCodecCtx());

    // 4. 设置编码回调：编码完直接写进 FLV
    video_encoder.set_callback([&](std::shared_ptr<AVPacket> pkt) {
        muxer.writePacket(pkt, v_idx);
        });
    audio_encoder.set_callback([&](std::shared_ptr<AVPacket> pkt) {
        muxer.writePacket(pkt, a_idx);
        });

    // 5. 写头
    muxer.writeHeader();

    // 6. 造视频帧，编码 30 帧
    AVFrame* vframe = av_frame_alloc();
    vframe->width = 640;
    vframe->height = 480;
    vframe->format = AV_PIX_FMT_YUV420P;
    av_frame_get_buffer(vframe, 32);
    memset(vframe->data[0], 128, vframe->linesize[0] * 480);
    memset(vframe->data[1], 128, vframe->linesize[1] * 240);
    memset(vframe->data[2], 128, vframe->linesize[2] * 240);

    for (int i = 0; i < 30; i++) {
        vframe->pts = i * 33000;
        video_encoder.encode(std::shared_ptr<AVFrame>(vframe, [](AVFrame* f) {}));
    }

    // 7. 造音频帧，编码 30 帧
    AVFrame* aframe = av_frame_alloc();
    aframe->sample_rate = 44100;
    aframe->ch_layout = AV_CHANNEL_LAYOUT_MONO;
    aframe->format = AV_SAMPLE_FMT_FLTP;
    aframe->nb_samples = 1024;
    av_frame_get_buffer(aframe, 0);
    memset(aframe->data[0], 0, aframe->linesize[0]);

    for (int i = 0; i < 30; i++) {
        aframe->pts = i * 1024;
        audio_encoder.encode(std::shared_ptr<AVFrame>(aframe, [](AVFrame* f) {}));
    }

    // 8. 冲洗编码器
    video_encoder.encode(nullptr);
    audio_encoder.encode(nullptr);

    // 9. 清理
    av_frame_free(&vframe);
    av_frame_free(&aframe);
    muxer.close();
    video_encoder.close();
    audio_encoder.close();

    LOG_INFO << "====== FLV Muxer Test Done ======";
}

// 全链路测试：摄像头采集 → MJPEG 解码 → 像素格式转换 → H.264 编码 → FLV 封装
// 数据流：dshow (YUVJ422P) → capturer 解码 → swscale 转 YUV420P → encoder (libx264) → muxer (FLV)
void test_pipeline() {
    LOG_INFO << "====== Video Pipeline Test (capture → encode → mux) ======";

    // 1. 列摄像头，选第一个
    std::vector<std::string> cameras;
    if (!list_dshow_cameras(cameras)) {
        LOG_ERROR << "No dshow camera found";
        return;
    }
    std::string device_name = "video=" + cameras.front();
    LOG_INFO << "Picking camera: " << device_name;

    // 2. 初始化 swscale（懒加载：第一个帧到达时才知道源像素格式）
    // 摄像头给 YUVJ422P（full scale），编码器要 YUV420P（limited）
    // swscale 自动做色彩空间转换
    struct SwsCtxDeleter {
        void operator()(SwsContext* p) { if (p) sws_freeContext(p); }
    };
    std::unique_ptr<SwsContext, SwsCtxDeleter> sws;

    // 3. 编码器（H.264，YUV420P 输入）
    FFmpegVideoEncoder encoder;
    if (!encoder.open(640, 480, 30, 1000000)) {
        LOG_ERROR << "Failed to open encoder";
        return;
    }

    // 4. 封装器（FLV 文件）
    FFmpegMuxer muxer;
    if (!muxer.open("test_pipeline.flv")) {
        LOG_ERROR << "Failed to open muxer";
        return;
    }
    int v_idx = muxer.addVideoStream(encoder.getCodecCtx());

    // 5. 编码回调：直接把 H.264 packet 写进 FLV
    encoder.set_callback([&muxer, v_idx](std::shared_ptr<AVPacket> pkt) {
        muxer.writePacket(pkt, v_idx);
    });

    // 6. 摄像头回调：swscale 转换 → 喂给编码器
    //    注意：此回调运行在 capture_thread 上下文，不是主线程
    std::atomic<int> frame_count{ 0 };
    FFmpegVideoCapturer capturer;
    capturer.set_callback([&](std::shared_ptr<AVFrame> src) {
        // 懒初始化 swscale（第一次拿到帧时创建）
        if (!sws) {
            sws.reset(sws_getContext(
                src->width, src->height, (AVPixelFormat)src->format,
                640, 480, AV_PIX_FMT_YUV420P,
                SWS_BILINEAR, nullptr, nullptr, nullptr));
            LOG_INFO << "Swscale initialized: "
                     << av_get_pix_fmt_name((AVPixelFormat)src->format)
                     << " -> YUV420P";
        }

        // 分配 YUV420P 目标帧
        AVFrame* dst = av_frame_alloc();
        dst->width = 640;
        dst->height = 480;
        dst->format = AV_PIX_FMT_YUV420P;
        if (av_frame_get_buffer(dst, 32) < 0) {
            LOG_ERROR << "Failed to alloc dst frame";
            av_frame_free(&dst);
            return;
        }

        // swscale 做像素格式 + colorspace 转换
        sws_scale(sws.get(),
                  src->data, src->linesize, 0, src->height,
                  dst->data, dst->linesize);

        // pts 用帧序号（编码器 time_base={1,30}，每帧 1 tick）
        int64_t pts = frame_count.fetch_add(1);
        dst->pts = pts;

        // 编码（让 shared_ptr 管理 dst 生命周期）
        encoder.encode(std::shared_ptr<AVFrame>(
            dst, [](AVFrame* f) { if (f) av_frame_free(&f); }));

        // 用 pts（已存到栈变量）而不是 dst->pts，避免 shared_ptr 释放后 use-after-free
        if (pts < 5 || pts % 30 == 0) {
            LOG_INFO << "Captured → Encoded frame " << pts;
        }
    });

    if (!capturer.open(device_name, 640, 480, 30)) {
        LOG_ERROR << "Failed to open capturer";
        return;
    }

    // 7. 写 FLV 文件头（必须在添加流之后、写入包之前）
    muxer.writeHeader();

    // 8. 启动采集 + 跑 5 秒
    capturer.start();
    std::this_thread::sleep_for(std::chrono::seconds(5));

    // 9. 严格逆序关闭
    //    capturer 先停（确保 capture_thread 不再调 encoder.encode）
    capturer.stop();
    capturer.close();

    //    encoder 冲刷（送 nullptr 让编码器吐残留的 packet）
    encoder.encode(nullptr);
    encoder.close();

    //    最后关 muxer（写文件尾 + 关闭文件）
    muxer.close();

    LOG_INFO << "====== Video Pipeline Test Done ======"
             << ", total frames encoded: " << frame_count.load();
}



int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    std::cout << "====== FFmpeg Environment Check ======" << std::endl;
    std::cout << "avformat version: " << avformat_version() << std::endl;
    std::cout << "avcodec  version: " << avcodec_version() << std::endl;
    std::cout << "avutil   version: " << avutil_version() << std::endl;

    const AVCodec* h264_encoder = avcodec_find_encoder_by_name("libx264");
    if (h264_encoder) {
        std::cout << "[OK] libx264 encoder: " << h264_encoder->long_name << std::endl;
    }
    const AVCodec* aac_encoder = avcodec_find_encoder_by_name("aac");
    if (aac_encoder) {
        std::cout << "[OK] aac encoder: " << aac_encoder->long_name << std::endl;
    }

    // 注册 libavdevice 的所有输入/输出设备 demuxer/muxer
    // 不调用这一行，dshow / gdigrab / vfwcap 等设备采集器就不会出现在
    // av_find_input_format() 的查找表里 —— 表现就是"找不到摄像头"
    avdevice_register_all();
    std::cout << "[OK] avdevice registered, version: "
              << avdevice_version() << std::endl;

    //test_thread_safe_queue();
    //test_clock();

    //Logger::instance().set_level(LogLevel::DEBUG);
    //LOG_INFO << "Pusher started";
    //LOG_DEBUG << "Resolution: " << 1920 << "x" << 1080;
    //LOG_WARN << "Video queue full, drop frame";
    //LOG_ERROR << "RTMP connect failed, errno=" << -1;

    //采集器测试
    //test_video_capture();
    //test_audio_capture();

    //编码测试
    //test_video_encoder();
    //test_audio_encoder();
    //test_muxer();


    test_pipeline();
    return 0;
}
