#pragma once

#include <string>
#include <memory>
#include <mutex>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

namespace livepusher {

// 本地录制器：把编码后的 H.264/AAC 包同时写一份到本地 MP4 文件。
// 和 FFmpegMuxer（推流用 FLV/RTMP）是并列关系，共用同一份编码数据，
// 但输出到独立的 mp4 文件。录制和推流并行，互不影响。
class FFmpegRecorder {
public:
    FFmpegRecorder();
    ~FFmpegRecorder();

    FFmpegRecorder(const FFmpegRecorder&) = delete;
    FFmpegRecorder& operator=(const FFmpegRecorder&) = delete;

    // 打开录制文件（.mp4）。成功返回 true。
    bool open(const std::string& path);

    // 添加视频/音频流（复制编码器参数，和推流 muxer 的参数完全一致）。
    int addVideoStream(const AVCodecContext* codec_ctx);
    int addAudioStream(const AVCodecContext* codec_ctx);

    // 写文件头（mp4 的 ftyp/moov 等）。
    void writeHeader();

    // 写一个编码包。内部按流的时间基重排 pts，再写盘。
    // 线程安全：音视频两个编码线程会并发写，内部加锁串行化。
    void writePacket(std::shared_ptr<AVPacket> pkt, int stream_idx);

    // 写尾（必须调用，否则 mp4 的 moov 索引不完整，播放器打不开）并关闭。
    void close();

    // 是否已打开（正在录制）。
    bool isOpen() const { return ctx_ != nullptr; }

private:
    AVFormatContext* ctx_ = nullptr;
    int video_stream_idx_ = -1;
    int audio_stream_idx_ = -1;
    // 互斥锁：av_interleaved_write_frame 非线程安全，音视频线程并发写要串行。
    std::mutex write_mutex_;
};

}  // namespace livepusher
