#pragma once
#include "audio_capturer.h"
#include<atomic>
#include<thread>
#include "common/logger.h"
#include "common/clock.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavdevice/avdevice.h>
}

namespace livepusher {
	//音频采集器
	class FFmpegAudioCapturer :public AudioCapturer {
	public:
		FFmpegAudioCapturer();
		~FFmpegAudioCapturer()override;
		bool open(const std::string& device_name, int sample_rate,int channels) override;
		bool start() override;
		void stop() override;
		void set_callback(AudioFrameCallback cb) override;
		void close() override;

	private:
		void capture_loop();

		static int interrupt_callback_fn(void* opaque);

		//封装器上下文
		AVFormatContext* ctx_ = nullptr;
		int audio_stream_idx_ = -1;//音频流索引


		//音频采集器参数
		int sample_rate_ = 0;//采样率
		int bit_depth_ = 0;//位深度
		int channels_ = 0;//声道数

		//回调函数
		AudioFrameCallback callback_;
		
		//音频采集器运行标志
		std::atomic<bool> running_{ false };

		std::thread capture_thread_;

		Clock clock_;
	};
}