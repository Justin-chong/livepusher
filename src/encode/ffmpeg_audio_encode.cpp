#include"ffmpeg_audio_encode.h"
#include"common/logger.h"

namespace livepusher {

	FFmpegAudioEncoder::FFmpegAudioEncoder()
	{
		LOG_INFO << "FFmpegAudioEncoder created!";
	}

	FFmpegAudioEncoder::~FFmpegAudioEncoder()
	{
		close();
		LOG_INFO << "FFmpegAudioEncoder destroyed!";
	}

	bool FFmpegAudioEncoder::open(int sample_rate, int channels, int bit_rate)
	{
		sample_rate_ = sample_rate;
		channels_ = channels;
		bit_rate_ = bit_rate;

		//1.找音频编码器aac，，codec_ 是只读的 "编码器类型"，ctx_ 是可写的 "这次编码的参数"
		codec_ = avcodec_find_encoder_by_name("aac");
		if (!codec_) {
			LOG_ERROR << "CAN'T FIND ENCODER AAC!";
			return false;
		}

		//2.创建编码器上下文，初始化编码器上下文中对应的参数字段
		ctx_ = avcodec_alloc_context3(codec_);
		if (!ctx_) {
			LOG_ERROR << "CREATE AVCodecContext ERROR!";
			return false;
		}

		//3，设置aac编码器参数
		ctx_->sample_rate = sample_rate_;
		if (channels_ == 1) {
			ctx_->ch_layout = AV_CHANNEL_LAYOUT_MONO;
		}
		else {
			ctx_->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
		}
		ctx_->bit_rate = bit_rate_;
		ctx_->sample_fmt = AV_SAMPLE_FMT_FLTP;

		//4.打开编码器（绑定编码器和编码器上下文），编码器根据编码器上下文里设置好的参数来执行编码算法
		int ret = avcodec_open2(ctx_, codec_, nullptr);
		if (ret < 0) {
			avcodec_free_context(&ctx_);
			LOG_ERROR << "OPEN AUDIO CODER ERROR!";
			return false;
		}
		return true;
	}

	void FFmpegAudioEncoder::close()
	{
		//销毁资源
		if (ctx_) {
			avcodec_free_context(&ctx_);
			ctx_ = nullptr;
		}
		LOG_INFO << "Audio Encoder closed!";
	}

	void FFmpegAudioEncoder::encode(std::shared_ptr<AVFrame> frame)
	{
		//avcodec_secnd_frame将原始数据AVFrame送入aac编码器
		if (!ctx_) {
			LOG_ERROR << "DON'T OPEN AUDIO ENCODER!";
			return;
		}

		// 送 PCM 帧进去（nullptr 表示冲洗）
		AVFrame* raw = frame ? frame.get() : nullptr;
		int ret = avcodec_send_frame(ctx_, raw);
		if (ret < 0) {
			LOG_ERROR << "SEND FRAME ERROR!";
			return;
		}
		
		//循环取包，取出AVPacket，再通过回调函数，返回给上层
		while (true) {
			AVPacket* pkt = av_packet_alloc();
			ret = avcodec_receive_packet(ctx_, pkt);
			if (ret == AVERROR(EAGAIN)) {
				//暂时没包，需要再送更多帧
				av_packet_free(&pkt);
				break;
			}
			if (ret < 0) {
				av_packet_free(&pkt);
				break;
			}

			//用智能指针管理，通过回调，返回给上层
			std::shared_ptr<AVPacket> packet(pkt,[](AVPacket* p){
				av_packet_free(&p);
				});

			if (callback_) {
				callback_(packet);
			}
		}
	}


	//注册回调,当在mian.cpp中调用set_callback时，将匿名lambda函数转移给callback_
	//后续在编码阶段执行callback_时，也就是在执行事先写好的lambda函数
	//这么做是因为，外层（main.cpp）不知道有没有编码出来包，内层（编码阶段）也没有外层容器的使用权或者说解耦性差
	void FFmpegAudioEncoder::set_callback(EncodedPacketCallback cb)
	{
		callback_ = std::move(cb);
	}

}