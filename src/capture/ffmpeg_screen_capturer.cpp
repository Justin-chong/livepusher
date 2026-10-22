#include "ffmpeg_screen_capturer.h"

#include <future>
#include <chrono>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
}

namespace livepusher {

FFmpegScreenCapturer::FFmpegScreenCapturer() {
    LOG_INFO << "FFmpegScreenCapturer created";
}

FFmpegScreenCapturer::~FFmpegScreenCapturer() {
    close();
    LOG_INFO << "FFmpegScreenCapturer destroyed";
}

int FFmpegScreenCapturer::interrupt_callback_fn(void* opaque) {
    auto* self = static_cast<FFmpegScreenCapturer*>(opaque);
    return self->running_.load() ? 0 : 1;
}

bool FFmpegScreenCapturer::open(const std::string& device_name,
                                int width, int height, int fps) {
    width_ = width;
    height_ = height;
    fps_ = fps;

    avdevice_register_all();

    // gdigrab：Windows 桌面采集设备。
    // device_name 传 "desktop" 表示全屏；传 "title=窗口标题" 表示指定窗口。
    const AVInputFormat* ifmt = av_find_input_format("gdigrab");
    if (!ifmt) {
        LOG_ERROR << "Cannot find gdigrab demuxer";
        return false;
    }

    AVDictionary* opt = nullptr;
    av_dict_set(&opt, "framerate", std::to_string(fps).c_str(), 0);
    // offset_x/offset_y 可指定抓取区域左上角，这里默认全屏从 (0,0) 开始。
    // draw_mouse=1 表示把鼠标光标也画进画面（屏幕录制通常要）。
    av_dict_set(&opt, "draw_mouse", "1", 0);

    int ret = avformat_open_input(&ctx_, device_name.c_str(), ifmt, &opt);
    av_dict_free(&opt);
    if (ret < 0) {
        char errbuf[128] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR << "Cannot open gdigrab: " << errbuf;
        return false;
    }

    ret = avformat_find_stream_info(ctx_, nullptr);
    if (ret < 0) {
        LOG_ERROR << "Cannot find stream info";
        close();
        return false;
    }

    video_stream_idx_ = av_find_best_stream(
        ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_idx_ < 0) {
        LOG_ERROR << "No video stream";
        close();
        return false;
    }

    // 关键：gdigrab 输出的是 BMP 格式（每个 packet = BMP 文件头 + 像素数据），
    // 不是裸像素！必须用解码器解出 AVFrame，否则直接把 pkt 当像素用会
    // 因为 54 字节 header 偏移导致像素字节错位（画面偏绿/花屏）。
    AVStream* stream = ctx_->streams[video_stream_idx_];
    AVCodecID codec_id = stream->codecpar->codec_id;
    LOG_INFO << "Screen stream codec: " << avcodec_get_name(codec_id)
             << " (id=" << codec_id << ")";
    if (codec_id != AV_CODEC_ID_RAWVIDEO) {
        codec_ = avcodec_find_decoder(codec_id);
        if (!codec_) {
            LOG_ERROR << "No decoder for: " << avcodec_get_name(codec_id);
            close();
            return false;
        }
        codec_ctx_ = avcodec_alloc_context3(codec_);
        if (!codec_ctx_) {
            LOG_ERROR << "Failed to alloc decoder ctx";
            close();
            return false;
        }
        int cret = avcodec_parameters_to_context(codec_ctx_, stream->codecpar);
        if (cret < 0) {
            LOG_ERROR << "Failed to copy codec params";
            close();
            return false;
        }
        cret = avcodec_open2(codec_ctx_, codec_, nullptr);
        if (cret < 0) {
            LOG_ERROR << "Failed to open decoder, ret=" << cret;
            close();
            return false;
        }
        LOG_INFO << "Screen decoder initialized";
    }

    // 屏幕实际分辨率（可能是用户全屏分辨率，比传进来的 width/height 更准）
    if (stream->codecpar->width > 0 && stream->codecpar->height > 0) {
        width_ = stream->codecpar->width;
        height_ = stream->codecpar->height;
    }
    LOG_INFO << "Screen opened: " << width_ << "x" << height_
             << ", fmt=" << av_get_pix_fmt_name(
                 (AVPixelFormat)stream->codecpar->format);

    ctx_->interrupt_callback.callback = &FFmpegScreenCapturer::interrupt_callback_fn;
    ctx_->interrupt_callback.opaque = this;

    LOG_INFO << "Screen device opened: " << device_name;
    return true;
}

bool FFmpegScreenCapturer::start() {
    if (running_.load()) {
        LOG_WARN << "Screen capturer already running";
        return false;
    }
    if (!ctx_) {
        LOG_ERROR << "Screen not opened, call open() first";
        return false;
    }

    clock_.start();
    running_.store(true);
    capture_thread_ = std::thread(&FFmpegScreenCapturer::capture_loop, this);
    LOG_INFO << "Screen capture thread started";
    return true;
}

void FFmpegScreenCapturer::stop() {
    if (!running_.load()) {
        return;
    }
    running_.store(false);

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
        LOG_WARN << "Screen capture thread stuck, detaching";
        if (capture_thread_.joinable()) {
            capture_thread_.detach();
        }
        return;
    }

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
    LOG_INFO << "Screen capture thread stopped";
}

void FFmpegScreenCapturer::close() {
    stop();
}

void FFmpegScreenCapturer::set_callback(VideoFrameCallback cb) {
    callback_ = std::move(cb);
}

void FFmpegScreenCapturer::capture_loop() {
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        LOG_ERROR << "Failed to alloc AVPacket";
        return;
    }

    if (!ctx_ || video_stream_idx_ < 0) {
        LOG_ERROR << "ctx or stream invalid";
        av_packet_free(&pkt);
        return;
    }

    AVStream* stream = ctx_->streams[video_stream_idx_];
    int src_w = stream->codecpar->width;
    int src_h = stream->codecpar->height;

    int frame_count = 0;
    while (running_.load()) {
        int ret = av_read_frame(ctx_, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EXIT) {
                LOG_INFO << "screen av_read_frame interrupted";
            } else {
                LOG_ERROR << "screen av_read_frame failed, ret=" << ret;
            }
            break;
        }

        if (pkt->stream_index != video_stream_idx_) {
            av_packet_unref(pkt);
            continue;
        }

        // BMP 解码：gdigrab 的 packet 是 BMP 格式，解码后得到真正的像素帧。
        int dret = avcodec_send_packet(codec_ctx_, pkt);
        if (dret < 0 && dret != AVERROR(EAGAIN)) {
            LOG_ERROR << "screen decode send failed, ret=" << dret;
            av_packet_unref(pkt);
            continue;
        }

        std::shared_ptr<AVFrame> frame(
            av_frame_alloc(),
            [](AVFrame* f) { av_frame_free(&f); });
        dret = avcodec_receive_frame(codec_ctx_, frame.get());
        if (dret == AVERROR(EAGAIN)) {
            av_packet_unref(pkt);
            continue;
        }
        if (dret < 0) {
            LOG_ERROR << "screen decode receive failed, ret=" << dret;
            av_packet_unref(pkt);
            continue;
        }

        frame->pts = Clock::now_us();

        if (frame_count < 5 || frame_count % 50 == 0) {
            LOG_INFO << "Screen frame " << frame_count
                     << ": " << frame->width << "x" << frame->height
                     << ", fmt=" << av_get_pix_fmt_name(
                         (AVPixelFormat)frame->format);
        }
        frame_count++;

        if (callback_) callback_(frame);

        av_packet_unref(pkt);

        if (!running_.load()) break;
    }

    av_packet_free(&pkt);
    LOG_INFO << "Screen capture loop exited, total: " << frame_count;
}

}  // namespace livepusher
