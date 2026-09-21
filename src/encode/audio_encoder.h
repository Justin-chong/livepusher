#pragma once
#include<memory>
#include<functional>

extern "C" {
#include<libavutil/frame.h>
#include<libavcodec/packet.h>
}

namespace livepusher {
	using EncodedPacketCallback = std::function<void(std::shared_ptr<AVPacket> pkt)>;
	class AudioEncoder {
	public:
		virtual ~AudioEncoder() = default;//default是让编译器自动生成一个空的析构函数，纯虚析构函数必须在类外定义，写起来麻烦
		//打开音频编码器,需要音频参数
		virtual bool open(int sample_rate,int channels,int bit_rate)=0;
		//关闭音频编码器
		virtual void close() = 0;

		//送音频PCM帧编码
		virtual void encode(std::shared_ptr<AVFrame> frame) = 0;

		//设置码流回调
		virtual void set_callback(EncodedPacketCallback cb) = 0;
	};

}