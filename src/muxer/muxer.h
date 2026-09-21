#pragma once
#include<string>
#include<memory>

extern "C"{
#include<libavcodec/packet.h>
#include<libavcodec/avcodec.h>
}

namespace livepusher {

	//抽象封装接口
	// 输入：H.264/AAC 编码包
	// 输出：FLV 流（文件或 RTMP）
	class Muxer {
	public:
		virtual ~Muxer() = default;
		//打开文件//打开URL
		virtual bool open(const std::string& url) = 0;

		//添加视频流
		/*
		@prama  编码器上下文
		@retuan 视频流索引
		*/
		virtual int addVideoStream(const AVCodecContext* codec_ctx) = 0;

		//添加音频流
		/*
		@prama 编码器上下文
		@return 音频流索引
		*/
		virtual int addAudioStream(const AVCodecContext* codec_ctx) = 0;

		//写头
		virtual void writeHeader() = 0;
		//写包
		virtual void writePacket(std::shared_ptr<AVPacket> pkt, int stream_idx) = 0;
		//写尾并关闭
		virtual void close() = 0;
	};
}