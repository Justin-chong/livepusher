#include "ffmpeg_video_encode.h"
#include"common/logger.h"

namespace livepusher {


	FFmpegVideoEncoder::FFmpegVideoEncoder()
	{
		LOG_INFO << "FFmpegVideoEncoder created!";
	}

	FFmpegVideoEncoder::~FFmpegVideoEncoder()
	{
		//�ȹرձ�����
		close();
		LOG_INFO << "FFmpegVideoEncoder destroyed!";
	}

	bool FFmpegVideoEncoder::open(int width, int height, int fps, int bitrate)
	{
		width_ = width;
		height_ = height;
		fps_ = fps;
		bitrate_ = bitrate;

		//1.�ұ�����
		codec_ = avcodec_find_encoder_by_name("libx264");
		if (!codec_) {
			LOG_ERROR << "DONT' FIND libx264 ENCODER!";
			return false;
		}


		//2.��������������ģ������ڴ棬����һ���յ������Ľṹ�壨�ṹ���������г�Աȫ�����Ĭ��ֵ������ codec_ ��������
		codec_ctx_ = avcodec_alloc_context3(codec_);
		if (!codec_ctx_) {
			LOG_ERROR << "CAN'T CREATE AVCodecContext!";
			return false;
		}
		//3.���ò���
		codec_ctx_->width = width_;
		codec_ctx_->height = height_;
		codec_ctx_->bit_rate = bitrate_;
		codec_ctx_->time_base = { 1,fps_ };// ʱ���
		codec_ctx_->framerate = { fps_,1 };//֡��
		codec_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;//���ظ�ʽ
		codec_ctx_->gop_size = 30;//GOP,ÿ30֡һ���ؼ�֡

		//4.�򿪱�������������������Ĳ�����������ʼ���������������ڲ�����������ʼ���㷨״̬��
		int ret = avcodec_open2(codec_ctx_, codec_, nullptr);
		if (ret < 0) {
			LOG_ERROR << "OPEN ENCODER ERROR!";
			avcodec_free_context(&codec_ctx_);
			return false;
		}
		return true;
	}
	
	//����
	void FFmpegVideoEncoder::encode(std::shared_ptr<AVFrame> frame)
	{
		//1. �� YUV ֡��������
		AVFrame* raw = frame ? frame.get() : nullptr;// frame �� nullptr ��ʾ��ϴ
		int ret = avcodec_send_frame(codec_ctx_, raw);
		//LOG_INFO << "send_frame ret=" << ret;
		if (ret < 0) {
			LOG_ERROR << "SEND FRAME ERROR!";
			return;
		}

		//2.ѭ������������ȡ��
		while (true) {
			AVPacket* pkt = av_packet_alloc();
			ret = avcodec_receive_packet(codec_ctx_, pkt);

			if (ret == AVERROR(EAGAIN)) {
				//��ʱû������Ҫ���͸���֡
				av_packet_free(&pkt);
				break;
			}
			if (ret < 0) {
				LOG_ERROR << "receive packet failed, ret=" << ret;
				av_packet_free(&pkt);
				break;
			}

			// 关键修复：avcodec_receive_packet 不会设置 pkt->time_base，
			// 默认是 {0,1}。av_packet_rescale_ts 用这个 time_base 转 stream 的 time_base
			// 会算出 pts=0，导致所有帧堆在 0 时刻，播放器看着"只播一帧"
			pkt->time_base = codec_ctx_->time_base;

			// 3. �õ�����������ָ��������ص����ϲ�
			std::shared_ptr<AVPacket> packet(pkt, [](AVPacket* p) {
				av_packet_free(&p);
				});
			if (callback_) {
				callback_(packet);
			}
		}

	}

	void FFmpegVideoEncoder::set_callback(EncodedPacketCallback cb)
	{
		callback_ = std::move(cb);
	}

	//�رձ�����
	void FFmpegVideoEncoder::close()
	{
		if (codec_ctx_) {
			avcodec_free_context(&codec_ctx_);
			codec_ctx_ = nullptr;
		}

		LOG_INFO << "Video Encoder closed!";
	}

}