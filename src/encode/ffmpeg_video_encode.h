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

		// ===== 弱网处理：动态码率 =====
		// 运行时调整目标码率（bps）。x264 支持运行中改码率，无需重建编码器。
		// 只能在编码线程调用（codec_ctx_ 是编码线程独占的）。
		void setBitrate(int bitrate);

	private:
		// 把新的目标码率真正应用进 x264 编码器，并强制下一帧 I 帧让新码率立即生效。
		void applyBitrate(int bitrate);

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