#pragma once
#include"audio_encoder.h"

extern "C" {
#include<libavcodec/avcodec.h>
#include<libswresample/swresample.h>
}

namespace livepusher {
	class FFmpegAudioEncoder:public AudioEncoder {
	public:
		FFmpegAudioEncoder();
		~FFmpegAudioEncoder() override;
		bool open(int sample_rate,int channels,int bit_rate) override;
		void close() override;
		void encode(std::shared_ptr<AVFrame> frame) override;
		void set_callback(EncodedPacketCallback cb) override;

		AVCodecContext* getCodecCtx() { return ctx_; }

	private:
		//音频编码器上下文
		AVCodecContext* ctx_ = nullptr;
		//音频编码器
		const AVCodec* codec_ = nullptr;
		//重采样器，用来把采集的 s16 转成编码器要的 fltp
		SwrContext* swr_ = nullptr;
		//回调
		EncodedPacketCallback callback_;
		//采样率、声道数、码率，来自参数
		int sample_rate_ = 0;
		int channels_ = 0;
		int bit_rate_ = 0;
		//已送出的采样点总数，用来给音频 pts 累加
		int64_t total_samples_ = 0;
		//音频自己的时间原点（第一帧的绝对微秒），让 pts 从这个原点开始
		//避免采集源不同导致的初始时间偏移被平移进每一帧
		int64_t audio_origin_us_ = -1;
	};


}