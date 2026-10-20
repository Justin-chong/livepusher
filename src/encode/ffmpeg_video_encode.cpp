#include "ffmpeg_video_encode.h"
#include"common/logger.h"

extern "C" {
#include<libavutil/pixdesc.h>
#include<libavutil/opt.h>
}

namespace livepusher {


	FFmpegVideoEncoder::FFmpegVideoEncoder()
	{
		LOG_INFO << "FFmpegVideoEncoder created!";
	}

	FFmpegVideoEncoder::~FFmpegVideoEncoder()
	{
		//先关闭编码器
		close();
		LOG_INFO << "FFmpegVideoEncoder destroyed!";
	}

	bool FFmpegVideoEncoder::open(int width, int height, int fps, int bitrate)
	{
		width_ = width;
		height_ = height;
		fps_ = fps;
		bitrate_ = bitrate;

		//1.找编码器
		codec_ = avcodec_find_encoder_by_name("libx264");
		if (!codec_) {
			LOG_ERROR << "DONT' FIND libx264 ENCODER!";
			return false;
		}


		//2.分配编码器上下文：分配内存，创建一个空的编码器结构体（结构体里所有成员都置默认值），它和 codec_ 关联
		codec_ctx_ = avcodec_alloc_context3(codec_);
		if (!codec_ctx_) {
			LOG_ERROR << "CAN'T CREATE AVCodecContext!";
			return false;
		}
		//3.设置参数
		codec_ctx_->width = width_;
		codec_ctx_->height = height_;
		codec_ctx_->bit_rate = bitrate_;
		codec_ctx_->time_base = { 1,fps_ };// 时间基
		codec_ctx_->framerate = { fps_,1 };//帧率
		codec_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;//像素格式
		codec_ctx_->gop_size = 30;//GOP,每30帧一个关键帧
		codec_ctx_->max_b_frames = 0;//关 B 帧，让 dts==pts，帧间参考才能建立


		//4.打开编码器：根据设置的参数初始化编码器，为编码器内部分配空间，初始化算法状态
		int ret = avcodec_open2(codec_ctx_, codec_, nullptr);
		if (ret < 0) {
			LOG_ERROR << "OPEN ENCODER ERROR!";
			avcodec_free_context(&codec_ctx_);
			return false;
		}

		LOG_INFO << "gop_size=" << codec_ctx_->gop_size
			<< " max_b_frames=" << codec_ctx_->max_b_frames
			<< " fps=" << codec_ctx_->framerate.num << "/" << codec_ctx_->framerate.den
			<< " pix_fmt=" << av_get_pix_fmt_name(codec_ctx_->pix_fmt);

		return true;
	}
	
	//编码
	void FFmpegVideoEncoder::encode(std::shared_ptr<AVFrame> frame)
	{
		//1. 把 YUV 帧送给编码器
		AVFrame* raw = frame ? frame.get() : nullptr;// frame 为 nullptr 表示冲刷
		static int cnt = 0;
		if (frame && cnt++ < 3) {
			LOG_INFO << "encode frame: pts=" << frame->pts
				<< " fmt=" << av_get_pix_fmt_name((AVPixelFormat)frame->format);
		}

		int ret = avcodec_send_frame(codec_ctx_, raw);
		//LOG_INFO << "send_frame ret=" << ret;
		if (ret < 0) {
			LOG_ERROR << "SEND FRAME ERROR!";
			return;
		}

		//2.循环取出编码好的包
		while (true) {
			AVPacket* pkt = av_packet_alloc();
			ret = avcodec_receive_packet(codec_ctx_, pkt);

			if (ret == AVERROR(EAGAIN)) {
				//暂时没有，需要多送几帧
				av_packet_free(&pkt);
				break;
			}
			if (ret == AVERROR_EOF) {
				// 编码器冲刷结束（send_frame 送 nullptr 时），正常流程，不是错误
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

			// 3. 得到包之后，通过回调传给上层
			std::shared_ptr<AVPacket> packet(pkt, [](AVPacket* p) {
				av_packet_free(&p);
				});
			//写包，将Packet写进封装器
			if (callback_) {
				callback_(packet);
			}
		}

	}

	void FFmpegVideoEncoder::set_callback(EncodedPacketCallback cb)
	{
		callback_ = std::move(cb);
	}

	// ===== 弱网处理：动态码率 =====
	void FFmpegVideoEncoder::setBitrate(int bitrate)
	{
		applyBitrate(bitrate);
	}

	// 把新的目标码率真正应用进 x264 编码器，并强制下一帧 I 帧让新码率立即生效。
	void FFmpegVideoEncoder::applyBitrate(int bitrate)
	{
		if (!codec_ctx_) return;
		if (bitrate <= 0) return;

		// x264 运行中改码率：通过 priv_data 的 "bit_rate" 选项设置。
		// 不能直接改 codec_ctx_->bit_rate —— 那个字段只在 avcodec_open2 时被读一次，
		// 运行中改了 x264 内部不认。
		char buf[64] = { 0 };
		snprintf(buf, sizeof(buf), "%d", bitrate);
		int ret = av_opt_set(codec_ctx_->priv_data, "bit_rate", buf, 0);
		if (ret < 0) {
			LOG_ERROR << "set dynamic bitrate failed, ret=" << ret;
			return;
		}

		// 同步更新上下文字段，保持元数据一致（muxer 等处可能读它）。
		codec_ctx_->bit_rate = bitrate;

		// 强制下一帧 I 帧：x264 的码率控制在帧间预测里生效慢，
		// 临时把 gop_size 设成 1 再设回，等于告诉编码器"下一帧必须是关键帧"，
		// 让新码率立刻落地。
		int old_gop = codec_ctx_->gop_size;
		codec_ctx_->gop_size = 1;
		avcodec_send_frame(codec_ctx_, nullptr);  // 逼 x264 提前结束当前 GOP
		codec_ctx_->gop_size = old_gop;

		LOG_INFO << "[BITRATE] dynamic bitrate -> " << bitrate
			<< " bps (" << (bitrate / 1000) << " kbps), forced next I-frame";
	}

	//关闭编码器
	void FFmpegVideoEncoder::close()
	{
		if (codec_ctx_) {
			avcodec_free_context(&codec_ctx_);
			codec_ctx_ = nullptr;
		}

		LOG_INFO << "Video Encoder closed!";
	}

}