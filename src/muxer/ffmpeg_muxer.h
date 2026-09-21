#pragma once
#include  "muxer.h"

extern "C" {
#include<libavformat/avformat.h>
}

namespace livepusher {
	//ffmpeg 封装器
	class FFmpegMuxer :public Muxer {
	public:
		FFmpegMuxer();
		~FFmpegMuxer() override;
		//打开文件/url
		bool open(const std::string& url) override;

		//添加视频/音频流
		int addVideoStream(const AVCodecContext* codec_ctx) override;
		int addAudioStream(const AVCodecContext* codec_ctx) override;

		//写文件头
		void writeHeader() override;
		//写包
		void writePacket(std::shared_ptr<AVPacket> pkt, int stream_idx) override;
		//写尾并关闭
		void close() override;

	private:
		//封装上下文
		AVFormatContext* ctx_ = nullptr;
		int video_stream_idx_ = -1;
		int audio_stream_idx_ = -1;

	};

}
