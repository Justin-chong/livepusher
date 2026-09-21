#pragma once
#include"audio_encoder.h"

extern "C" {
#include<libavcodec/avcodec.h>
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
		//回调
		EncodedPacketCallback callback_;
		//编码参数,采样率，声道数，码率
		int sample_rate_ = 0;
		int channels_ = 0;
		int bit_rate_ = 0;
	};


}