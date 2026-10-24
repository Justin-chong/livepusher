#include "ffmpeg_recorder.h"
#include "common/logger.h"

namespace livepusher {

FFmpegRecorder::FFmpegRecorder() {
    LOG_INFO << "FFmpegRecorder created";
}

FFmpegRecorder::~FFmpegRecorder() {
    close();
    LOG_INFO << "FFmpegRecorder destroyed";
}

bool FFmpegRecorder::open(const std::string& path) {
    // 已打开就返回 false，避免重复 open 泄漏。
    if (ctx_) {
        LOG_ERROR << "recorder already open";
        return false;
    }

    // 第三个参数 "mp4" 强制用 mp4 格式封装（本地文件）。
    int ret = avformat_alloc_output_context2(&ctx_, nullptr, "mp4", path.c_str());
    if (ret < 0) {
        LOG_ERROR << "recorder alloc output context failed";
        ctx_ = nullptr;
        return false;
    }

    // 打开本地文件 IO。
    if (!(ctx_->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&ctx_->pb, path.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            LOG_ERROR << "recorder avio open failed";
            avformat_free_context(ctx_);
            ctx_ = nullptr;
            return false;
        }
    }

    LOG_INFO << "recorder opened: " << path;
    return true;
}

int FFmpegRecorder::addVideoStream(const AVCodecContext* codec_ctx) {
    AVStream* stream = avformat_new_stream(ctx_, nullptr);
    if (!stream) {
        LOG_ERROR << "recorder create video stream failed";
        return -1;
    }
    int ret = avcodec_parameters_from_context(stream->codecpar, codec_ctx);
    if (ret < 0) {
        LOG_ERROR << "recorder copy video codec params failed";
        return -1;
    }
    stream->time_base = codec_ctx->time_base;
    video_stream_idx_ = stream->index;
    LOG_INFO << "recorder video stream added, index=" << video_stream_idx_;
    return video_stream_idx_;
}

int FFmpegRecorder::addAudioStream(const AVCodecContext* codec_ctx) {
    AVStream* stream = avformat_new_stream(ctx_, nullptr);
    if (!stream) {
        LOG_ERROR << "recorder create audio stream failed";
        return -1;
    }
    int ret = avcodec_parameters_from_context(stream->codecpar, codec_ctx);
    if (ret < 0) {
        LOG_ERROR << "recorder copy audio codec params failed";
        return -1;
    }
    stream->time_base = codec_ctx->time_base;
    audio_stream_idx_ = stream->index;
    LOG_INFO << "recorder audio stream added, index=" << audio_stream_idx_;
    return audio_stream_idx_;
}

void FFmpegRecorder::writeHeader() {
    int ret = avformat_write_header(ctx_, nullptr);
    if (ret < 0) {
        LOG_ERROR << "recorder write header failed";
        return;
    }
    LOG_INFO << "recorder write header ok";
}

//录制的写包不需要应对弱网处理（等到后面再加一个编码器，单独应对录制，因为录制的视频不能降质，现在弱网会降质，因为录制和封装公用一个编码器）
void FFmpegRecorder::writePacket(std::shared_ptr<AVPacket> pkt, int stream_idx) {
    if (!ctx_ || stream_idx < 0) return;

    // 加锁：音视频两个编码线程并发写同一个 mp4 muxer，必须串行。
    std::lock_guard<std::mutex> lock(write_mutex_);

    AVPacket* new_pkt = av_packet_clone(pkt.get());
    if (!new_pkt) return;
    new_pkt->stream_index = stream_idx;

    // pts/dts 换算到录制流的时间基。
    AVStream* stream = ctx_->streams[stream_idx];
    av_packet_rescale_ts(new_pkt, pkt->time_base, stream->time_base);

    int ret = av_interleaved_write_frame(ctx_, new_pkt);
    av_packet_free(&new_pkt);

    if (ret < 0) {
        LOG_ERROR << "recorder write frame failed, ret=" << ret;
    }
}

void FFmpegRecorder::close() {
    if (ctx_) {
        // 关键：写文件尾（mp4 的 moov 索引）。
        // 不写尾的话 mp4 文件不完整，播放器无法打开。
        av_write_trailer(ctx_);

        if (!(ctx_->oformat->flags & AVFMT_NOFILE) && ctx_->pb) {
            avio_closep(&ctx_->pb);
        }
        avformat_free_context(ctx_);
        ctx_ = nullptr;
    }
    video_stream_idx_ = -1;
    audio_stream_idx_ = -1;
    LOG_INFO << "recorder closed";
}

}  // namespace livepusher
