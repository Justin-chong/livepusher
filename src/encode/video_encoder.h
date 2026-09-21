#pragma once

#include<functional>
#include<memory>

extern "C" {
#include <libavutil/frame.h>
#include <libavcodec/packet.h>
}

namespace livepusher {
	// 编码后的码流回调：每编码出一个包，就调用一次		//函数接收参数
	using EncodedPacketCallback = std::function<void(std::shared_ptr<AVPacket> pkt)>;

	//视频编码器,抽象接口，为了不同的视频编码协议
	//- 输入：YUV 帧（AVFrame）
	//-输出：H.264 码流包（AVPacket）
	class VideoEncoder {
	public:
		virtual ~VideoEncoder() = default;
		//打开编码器
		// width/height: 分辨率，fps: 帧率，bitrate: 目标码率（bps）
		virtual bool open(int width,int height,int fps,int bitrate) = 0;

		//送一帧 YUV 进去编码
		virtual void encode(std::shared_ptr<AVFrame> frame) = 0;

		// 设置码流回调，回调时需要执行的逻辑
		virtual void set_callback(EncodedPacketCallback cb) = 0;

		//关闭编码器
		virtual void close() = 0;


	};
}