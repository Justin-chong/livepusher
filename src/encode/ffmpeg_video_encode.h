#pragma once

#include "video_encoder.h"

extern "C" {
#include<libavcodec/avcodec.h>
}

namespace livepusher {
	class FFmpegVideoEncoder :public VideoEncoder {
	public:
		FFmpegVideoEncoder();

		~FFmpegVideoEncoder() override;

		//打开编码器
		// width/height: 分辨率，fps: 帧率，bitrate: 目标码率（bps）
		bool open(int width,int height,int fps,int bitrate) override;

		//送一帧 YUV 进去编码
		void encode(std::shared_ptr<AVFrame> frame) override;

		// 设置码流回调，回调时需要执行的逻辑
		void set_callback(EncodedPacketCallback cb) override;

		void close()override;

		AVCodecContext* getCodecCtx() { return codec_ctx_; }

	private:
		//编码上下文
		AVCodecContext* codec_ctx_ = nullptr;
		//编码器
		const AVCodec* codec_ = nullptr;
		
		//回调
		EncodedPacketCallback callback_;

		//编码参数
		int width_ = 0;
		int height_ = 0;
		int fps_ = 0;
		int bitrate_ = 0;
	};

}