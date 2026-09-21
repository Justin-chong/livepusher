#pragma once

#include<atomic>
#include<thread>
#include "video_capturer.h"
#include "common/logger.h"
#include "common/clock.h"
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavdevice/avdevice.h>
}

namespace livepusher {

	class FFmpegVideoCapturer :public VideoCapturer {
public:
	FFmpegVideoCapturer();

	~FFmpegVideoCapturer()override;

	bool open(const std::string& device_name, int width, int height, int fps) override;

	bool start() override;

	void stop() override;

	void close() override;

	void set_callback(VideoFrameCallback cb) override;

private:
	void capture_loop();

	// ffmpeg 在 av_read_frame 阻塞时会周期调用这个回调
	// 返回 0 = 继续等待；返回 1 = 立刻中断，av_read_frame 返回错误
	// 必须用 static，因为 AVIOInterruptCB 接受的是裸函数指针（不能用 lambda）
	static int interrupt_callback_fn(void* opaque);

	//封装器上下文;
	AVFormatContext* ctx_ = nullptr;
	int video_stream_idx_ = -1;//视频流索引

	//视频解码器（dshow 给的可能是压缩 MJPEG，需解码）
	const AVCodec* codec_ = nullptr;
	AVCodecContext* codec_ctx_ = nullptr;

	//采集器参数
	int width_ = 0;
	int height_ = 0;
	int fps_ = 0;

	//采集器运行标志
	std::atomic<bool> running_{ false };

	std::thread capture_thread_;

	VideoFrameCallback callback_;

	Clock clock_;

};

}

