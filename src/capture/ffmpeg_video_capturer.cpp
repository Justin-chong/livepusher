#include "ffmpeg_video_capturer.h"

#include <future>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavutil/imgutils.h>
}

namespace livepusher {

FFmpegVideoCapturer::FFmpegVideoCapturer() {
    LOG_INFO << "FFmpegVideoCapturer created";
}

FFmpegVideoCapturer::~FFmpegVideoCapturer() {
    close();
    LOG_INFO << "FFmpegVideoCapturer destroyed";
}

// av_read_frame 阻塞等待数据时，FFmpeg 会周期性调用此函数
// 返回 0 = 继续等；返回 1 = 立刻中断，av_read_frame 返回 AVERROR_EXIT
// 必须是 static，因为 AVIOInterruptCB 只接受裸函数指针
int FFmpegVideoCapturer::interrupt_callback_fn(void* opaque) {
    auto* self = static_cast<FFmpegVideoCapturer*>(opaque);
    return self->running_.load() ? 0 : 1;
}

bool FFmpegVideoCapturer::open(const std::string& device_name,
                                int width, int height, int fps) {
    width_ = width;
    height_ = height;
    fps_ = fps;

    // 注册所有设备（dshow 等），幂等可多次调用
    avdevice_register_all();

    // 设置期望的分辨率和帧率，通过 AVDictionary 传给 dshow
    AVDictionary* opt = nullptr;
    std::string video_size = std::to_string(width) + "x" + std::to_string(height);
    av_dict_set(&opt, "video_size", video_size.c_str(), 0);

    std::string framerate = std::to_string(fps);
    av_dict_set(&opt, "framerate", framerate.c_str(), 0);

    // 显式指定用 dshow 打开，不传的话 FFmpeg 不认识 "video=xxx"
    const AVInputFormat* ifmt = av_find_input_format("dshow");
    if (!ifmt) {
        LOG_ERROR << "Cannot find dshow demuxer";
        av_dict_free(&opt);
        return false;
    }

    // 打开设备：分配 ctx_、建立与摄像头的连接
    int ret = avformat_open_input(&ctx_, device_name.c_str(), ifmt, &opt);
    av_dict_free(&opt);
    if (ret < 0) {
        char errbuf[128] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR << "Cannot open device: " << errbuf;
        return false;
    }

    // 探测流信息（分辨率、像素格式、帧率等）
    ret = avformat_find_stream_info(ctx_, nullptr);
    if (ret < 0) {
        LOG_ERROR << "Cannot find stream info";
        close();
        return false;
    }

    // 找到视频流索引（dshow 可能同时有视频+音频流）
    video_stream_idx_ = av_find_best_stream(
        ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_idx_ < 0) {
        LOG_ERROR << "No video stream";
        close();
        return false;
    }

    // 关键：dshow 给的可能是压缩 MJPEG bitstream（pkt->size 只有几十 KB），
    // 不是未压缩的 YUVJ422P（应该是 614400 bytes）。codecpar->format 显示
    // yuvj422p 是误导，必须看 codec_id 才知道是不是要解码。
    AVStream* stream = ctx_->streams[video_stream_idx_];
    AVCodecID codec_id = stream->codecpar->codec_id;
    LOG_INFO << "Stream codec: " << avcodec_get_name(codec_id)
             << " (id=" << codec_id << ")";

    if (codec_id != AV_CODEC_ID_RAWVIDEO) {
        // 需要解码：分配 decoder context 并打开
        codec_ = avcodec_find_decoder(codec_id);
        if (!codec_) {
            char errbuf[128] = {0};
            av_strerror(0, errbuf, sizeof(errbuf));  // codec 没找到时没 ret 可翻译
            LOG_ERROR << "No decoder for codec: " << avcodec_get_name(codec_id);
            close();
            return false;
        }
        codec_ctx_ = avcodec_alloc_context3(codec_);
        if (!codec_ctx_) {
            LOG_ERROR << "Failed to alloc codec context";
            close();
            return false;
        }
        int cret = avcodec_parameters_to_context(codec_ctx_, stream->codecpar);
        if (cret < 0) {
            char errbuf[128] = {0};
            av_strerror(cret, errbuf, sizeof(errbuf));
            LOG_ERROR << "Failed to copy codec params: " << errbuf;
            close();
            return false;
        }
        cret = avcodec_open2(codec_ctx_, codec_, nullptr);
        if (cret < 0) {
            char errbuf[128] = {0};
            av_strerror(cret, errbuf, sizeof(errbuf));
            LOG_ERROR << "Failed to open codec: " << errbuf;
            close();
            return false;
        }
        LOG_INFO << "Codec decoder initialized";
    } else {
        LOG_INFO << "Stream is RAWVIDEO, no decoder needed";
    }

    // 注册中断回调：stop() 后 av_read_frame 能被打断
    ctx_->interrupt_callback.callback = &FFmpegVideoCapturer::interrupt_callback_fn;
    ctx_->interrupt_callback.opaque = this;

    LOG_INFO << "Device opened: " << device_name;
    return true;
}

bool FFmpegVideoCapturer::start() {
    if (running_.load()) {
        LOG_WARN << "Capturer already running";
        return false;
    }
    if (!ctx_) {
        LOG_ERROR << "Device not opened, call open() first";
        return false;
    }

    // 时钟归零，开始计时
    clock_.start();
    running_.store(true);

    // 启动采集线程
    capture_thread_ = std::thread(&FFmpegVideoCapturer::capture_loop, this);
    LOG_INFO << "Capture thread started";
    return true;
}

void FFmpegVideoCapturer::stop() {
    if (!running_.load()) {
        return;
    }

    // 先置标志，让 capture_loop 在下一帧处理完后退出
    running_.store(false);

    // 限时 join：dshow 不响应 interrupt_callback，且 av_read_frame 用
    // WaitForSingleObject(INFINITE) 阻塞，如果摄像头一直没送帧，
    // capture_loop 永远卡在 av_read_frame 里，join() 永不返回。
    // 兜底：用 std::async + wait_for(500ms) 限时等线程退出，超时则 detach。
    // detached 线程的资源会在进程退出时被 OS 回收。
    // 类比：工人在降噪仓库里等快递，外面的人敲了 500ms 门没人应，
    // 那就把门钉死、走人，仓库里剩下的东西回头再说。
    bool joined = false;
    {
        auto future = std::async(std::launch::async, [this]() {
            if (capture_thread_.joinable()) {
                capture_thread_.join();
            }
        });

        if (future.wait_for(std::chrono::milliseconds(500))
            == std::future_status::ready) {
            joined = true;
        }
    }

    if (!joined) {
        LOG_WARN << "Capture thread stuck (dshow can't be interrupted), "
                    "detaching. Resources freed on process exit.";
        if (capture_thread_.joinable()) {
            capture_thread_.detach();
        }
        // detached 线程还在用 ctx_，不能 close（否则 UAF）
        return;
    }

    // join 成功，安全关闭设备
    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }
    codec_ = nullptr;
    if (ctx_) {
        avformat_close_input(&ctx_);
        ctx_ = nullptr;
    }
    video_stream_idx_ = -1;
    LOG_INFO << "Capture thread stopped";
}

void FFmpegVideoCapturer::close() {
    stop();
}

void FFmpegVideoCapturer::set_callback(VideoFrameCallback cb) {
    callback_ = std::move(cb);
}

// 采集线程主循环：在独立线程中运行
// 职责：从摄像头读 YUV 帧 → 包装成 AVFrame → 打时间戳 → 回调给上层
void FFmpegVideoCapturer::capture_loop() {
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        LOG_ERROR << "Failed to allocate AVPacket";
        return;
    }

    // 空指针检查，定位崩溃点
    if (!ctx_) {
        LOG_ERROR << "ctx_ is null";
        av_packet_free(&pkt);
        return;
    }
    if (video_stream_idx_ < 0) {
        LOG_ERROR << "video_stream_idx_ invalid: " << video_stream_idx_;
        av_packet_free(&pkt);
        return;
    }

    AVStream* stream = ctx_->streams[video_stream_idx_];
    if (!stream || !stream->codecpar) {
        LOG_ERROR << "stream or codecpar is null";
        av_packet_free(&pkt);
        return;
    }

    int src_width = stream->codecpar->width;
    int src_height = stream->codecpar->height;
    AVPixelFormat src_fmt = (AVPixelFormat)stream->codecpar->format;
    LOG_INFO << "Capture params: " << src_width << "x" << src_height
             << ", fmt=" << av_get_pix_fmt_name(src_fmt);

    int frame_count = 0;

    while (running_.load()) {
        // 从摄像头读一包数据
        int ret = av_read_frame(ctx_, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EXIT) {
                LOG_INFO << "av_read_frame interrupted";
            } else {
                LOG_ERROR << "av_read_frame failed, ret=" << ret;
            }
            break;
        }

        // 只处理视频流，音频包直接丢
        if (pkt->stream_index != video_stream_idx_) {
            av_packet_unref(pkt);
            continue;
        }

        // 如果 dshow 给的是压缩帧（如 MJPEG），先解码
        if (codec_ctx_) {
            int dret = avcodec_send_packet(codec_ctx_, pkt);
            if (dret < 0 && dret != AVERROR(EAGAIN)) {
                LOG_ERROR << "avcodec_send_packet failed, ret=" << dret;
                av_packet_unref(pkt);
                continue;
            }

            // 收解码后的 YUV 帧
            std::shared_ptr<AVFrame> frame(
                av_frame_alloc(),
                [](AVFrame* f) { av_frame_free(&f); }
            );

            dret = avcodec_receive_frame(codec_ctx_, frame.get());
            if (dret == AVERROR(EAGAIN)) {
                // 还没解码出帧，等下一个 packet
                av_packet_unref(pkt);
                continue;
            }
            if (dret < 0) {
                LOG_ERROR << "avcodec_receive_frame failed, ret=" << dret;
                av_packet_unref(pkt);
                continue;
            }

            // 帧解码成功，打 PTS + 回调
            frame->pts = clock_.elapsed_us();
            if (frame_count < 5 || frame_count % 30 == 0) {
                LOG_INFO << "Frame " << frame_count
                         << ": " << frame->width << "x" << frame->height
                         << ", fmt=" << av_get_pix_fmt_name((AVPixelFormat)frame->format)
                         << ", pts=" << frame->pts;
            }
            frame_count++;

            if (callback_) callback_(frame);
        } else {
            // RAWVIDEO 路径（如果以后 dshow 输出未压缩数据）
            if (frame_count < 5 || frame_count % 30 == 0) {
                LOG_INFO << "packet received [" << frame_count
                         << "]: size=" << pkt->size
                         << ", stream_index=" << pkt->stream_index;
            }
            frame_count++;
        }

        av_packet_unref(pkt);

        // [已注释] image_copy 块（被 MJPEG 解码路径替代）
        // int src_linesize[4] = { 0 };
        // av_image_fill_linesizes(src_linesize, src_fmt, src_width);
        // const uint8_t* src_data[4] = { nullptr };
        // av_image_fill_pointers((uint8_t**)src_data, src_fmt, src_height,
        //     pkt->data, src_linesize);
        // av_image_copy(frame->data, frame->linesize,
        //     src_data, src_linesize,
        //     src_fmt, src_width, src_height);
        // frame->pts = clock_.elapsed_us();

        // 每帧处理完检查 running_（dshow 不响应 interrupt_callback，这是唯一出口）
        if (!running_.load()) {
            break;
        }
    }

    av_packet_free(&pkt);
    LOG_INFO << "Capture loop exited, total frames: " << frame_count;
}

}  // namespace livepusher
