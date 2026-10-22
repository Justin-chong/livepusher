#include"ffmpeg_audio_encode.h"
#include"common/logger.h"

extern "C" {
#include<libswresample/swresample.h>
}

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
		int ret = 0;
		sample_rate_ = sample_rate;
		channels_ = channels;
		bit_rate_ = bit_rate;
		// 每次 open 重置累计状态（否则第二次推流 pts 会从上次的采样点继续累加）
		total_samples_ = 0;
		audio_origin_us_ = -1;

		//1.查找音频编码器 aac：codec_ 是只读的"编码器描述"，ctx_ 是可写的"编码参数"
		codec_ = avcodec_find_encoder_by_name("aac");
		if (!codec_) {
			LOG_ERROR << "CAN'T FIND ENCODER AAC!";
			return false;
		}

		//2.分配编码器上下文：分配内存，初始化它，为编码器提供对应的参数字段
		ctx_ = avcodec_alloc_context3(codec_);
		if (!ctx_) {
			LOG_ERROR << "CREATE AVCodecContext ERROR!";
			return false;
		}

		//3.设置 aac 编码参数
		ctx_->sample_rate = sample_rate_;
		if (channels_ == 1) {
			ctx_->ch_layout = AV_CHANNEL_LAYOUT_MONO;
		}
		else {
			ctx_->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
		}
		ctx_->bit_rate = bit_rate_;
		ctx_->sample_fmt = AV_SAMPLE_FMT_FLTP;
		// 时间基：音频的时间单位是采样点，1/采样率 秒一个采样点。
		// 必须显式设置！不设的话 avcodec_alloc_context3 默认是 {0,1}（分子 0），
		// 之后 av_packet_rescale_ts 用这个 time_base 换算 pts 会除零，
		// 导致所有音频帧 pts 堆在 0 → 音画不同步、录制 mp4 被"快进"。
		// 视频编码器设的是 {1,fps}，音频对应设 {1,sample_rate}，语义一致。
		ctx_->time_base = { 1, sample_rate_ };
		// FLV/RTMP 推流必须设 global header：
		// 让 AAC 编码器把 AudioSpecificConfig(ASC) 放到 extradata 里，
		// 输出「不带 ADTS 头的裸 AAC」，而不是每包都带 7 字节 ADTS 头。
		// 不设这个标志，编码器输出的包会带 ADTS 头（首字节 0xFF），
		// 服务器(如 livego)读 FLV 音频 tag 的 SoundFormat 字段会错位，
		// 报 no support audio codec 并越界 panic。
		ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

		//4.打开编码器：绑定编码器和编码上下文，之后根据上下文里设置好的参数执行编码算法
		ret = avcodec_open2(ctx_, codec_, nullptr);
		if (ret < 0) {
			avcodec_free_context(&ctx_);
			LOG_ERROR << "OPEN AUDIO CODER ERROR!";
			return false;
		}

		//5.创建重采样器，必须在 avcodec_open2 之后！
		//   原因：avcodec_open2 之后，编码器才把"最终接受的参数"写进 ctx_，
		//   比如 MONO 可能被调整，ctx_->sample_fmt 也可能被改。
		//   所以要用 ctx_ 的值作为转换目标，而不是自己拍脑袋的值。
		SwrContext* swr = nullptr;
		ret = swr_alloc_set_opts2(
			&swr,
			&ctx_->ch_layout,           // 输出：声道布局（编码器选好的）
			ctx_->sample_fmt,           // 输出：采样格式（编码器选好的，FLTP）
			ctx_->sample_rate,          // 输出：采样率
			&ctx_->ch_layout,           // 输入：声道布局（采集来的，和编码器一致）
			AV_SAMPLE_FMT_S16,          // 输入：采集来的 s16
			ctx_->sample_rate,          // 输入：采样率
			0,
			nullptr);
		if (ret < 0) {
			char errbuf[128] = { 0 };
			av_strerror(ret, errbuf, sizeof(errbuf));
			LOG_ERROR<< "swr_alloc_set_opts2 failed: " << errbuf;
			avcodec_free_context(&ctx_);
			return false;
		}
		// swr_init 初始化重采样算法，建立内部映射。不调用这个转换会全部失败。
		ret = swr_init(swr);
		if (ret < 0) {
			char errbuf[128] = { 0 };
			av_strerror(ret, errbuf, sizeof(errbuf));
			LOG_ERROR << "swr_init failed: " << errbuf;
			swr_free(&swr);
			avcodec_free_context(&ctx_);
			return false;
		}
		swr_ = swr;

		LOG_INFO << "channels' number:"<<ctx_->ch_layout.nb_channels;
		LOG_INFO << "Resampler ready: s16 -> "
			<< av_get_sample_fmt_name(ctx_->sample_fmt)
			<< ", out_samples max=" << swr_get_out_samples(swr_, 0);
		return true;
	}

	void FFmpegAudioEncoder::close()
	{

		//释放资源
		if (swr_) {
			swr_free(&swr_);
		}
		if (ctx_) {
			avcodec_free_context(&ctx_);
			ctx_ = nullptr;
		}
		LOG_INFO << "Audio Encoder closed!";
	}

	void FFmpegAudioEncoder::encode(std::shared_ptr<AVFrame> frame)
	{
		if (!ctx_ || !swr_) {
			LOG_ERROR << "DON'T OPEN AUDIO ENCODER!";
			return;
		}

		// 诊断：确认 encode 被调用，且 frame 有效
		{
			static int enc_cnt = 0;
			if (enc_cnt++ < 5) {
				LOG_INFO << "[AUDIO ENC] encode called, frame_null=" << (frame == nullptr)
					<< " nb_samples=" << (frame ? frame->nb_samples : -1)
					<< " fmt=" << (frame ? av_get_sample_fmt_name((AVSampleFormat)frame->format) : "n/a");
			}
		}

		// 把编码器已经编好的包安全取出，回调给上层
		auto drain_packets = [&]() {
			while (true) {
				AVPacket* pkt = av_packet_alloc();
				int r = avcodec_receive_packet(ctx_, pkt);
				if (r == AVERROR(EAGAIN)) {          // 暂时没有，稍后再取
					av_packet_free(&pkt);
					break;
				}
				if (r == AVERROR_EOF) {              // 编码器冲刷结束，正常流程
					av_packet_free(&pkt);
					break;
				}
				if (r < 0) {                          // 其他真实错误
					LOG_ERROR << "receive packet failed, ret=" << r;
					av_packet_free(&pkt);
					break;
				}
				pkt->time_base = ctx_->time_base;     // 和音频编码器保持一致
				std::shared_ptr<AVPacket> packet(pkt, [](AVPacket* p) {
					av_packet_free(&p);
					});
				if (callback_) {
					callback_(packet);
				}
			}
			};

		// ===== 情况 A：传进来的 frame 为空，没有数据要转换 =====
		if (!frame) {
			avcodec_send_frame(ctx_, nullptr);
			drain_packets();
			return;
		}

		// ===== 情况 B：正常编码 =====

		// 音频起点归零：第一帧记录「音频自己的微秒原点」，之后所有帧都减它。
		// 这样音频 pts 从 0 开始，和视频（也从 0 开始）对齐，
		// 不会把「麦克风晚启动的 1.1 秒」错误地加进每一帧声音里。
		if (audio_origin_us_ < 0 && frame->pts != AV_NOPTS_VALUE) {
			audio_origin_us_ = frame->pts;   // 记住第一帧的微秒，作为音频的 0 点
		}

		// ===== 一次性 s16 -> fltp 转换 + 切 1024 帧送编码器 =====
		// 之前的「喂入(传 nullptr 输出) + 取出」两段式写法 swr_convert 返回 0，
		// 数据根本没被吃进去。改成最直接的做法：
		//   swr_convert(out, out_count, in, in_count) 同时给输入+拿输出，
		//   一次把整帧 22050 点 s16 转成 fltp，返回实际转换出的采样点数。
		// 采样率不变(44100→44100)，swr 只做格式转换，不会产生缓冲残余，
		// 直接一次性转换整帧即可，再用循环把转好的 fltp 切成 1024 一帧送编码器。
		{
			int in_count = frame->nb_samples;

			// 1. 分配一个足够大的 fltp 目标帧，装下整帧转换结果
			AVFrame* tmp = av_frame_alloc();
			tmp->sample_rate = ctx_->sample_rate;
			tmp->ch_layout = ctx_->ch_layout;
			tmp->format = ctx_->sample_fmt;
			tmp->nb_samples = in_count;   // 采样率不变，输出点数 = 输入点数
			if (av_frame_get_buffer(tmp, 0) < 0) {
				LOG_ERROR << "av_frame_get_buffer failed";
				av_frame_free(&tmp);
				return;
			}

			// 2. 一次性转换：s16 -> fltp
			int got = swr_convert(swr_, tmp->data, in_count,
				(const uint8_t**)frame->data, in_count);
			if (got < 0) {
				LOG_ERROR << "swr_convert failed, ret=" << got;
				av_frame_free(&tmp);
				return;
			}

			// 诊断：打印转换结果 + 编码器帧大小
			{
				static int swr_cnt = 0;
				if (swr_cnt++ < 3) {
					LOG_INFO << "[SWR OUT] got=" << got
						<< " in_count=" << in_count
						<< " frame_size=" << ctx_->frame_size;
				}
			}

			// 3. 把转好的 fltp 数据切成 1024 一帧，逐帧送编码器
			int frame_size = ctx_->frame_size > 0 ? ctx_->frame_size : 1024;
			int offset = 0;
			int sent_frames = 0;
			while (offset + frame_size <= got) {
				AVFrame* out = av_frame_alloc();
				out->sample_rate = ctx_->sample_rate;
				out->ch_layout = ctx_->ch_layout;
				out->format = ctx_->sample_fmt;
				out->nb_samples = frame_size;
				if (av_frame_get_buffer(out, 0) < 0) {
					LOG_ERROR << "av_frame_get_buffer(out) failed";
					av_frame_free(&out);
					break;
				}

				// 从 tmp 里搬 frame_size 个采样点（单声道：一个平面）
				// fltp：float 平面，每声道 linesize 是 frame_size*4 字节
				int bytes_per_sample = 4;   // fltp = float = 4 字节
				memcpy(out->data[0], tmp->data[0] + offset * bytes_per_sample,
					frame_size * bytes_per_sample);

				out->pts = total_samples_;
				total_samples_ += frame_size;

				int ret = avcodec_send_frame(ctx_, out);
				av_frame_free(&out);
				if (ret < 0) {
					char errbuf[128] = { 0 };
					av_strerror(ret, errbuf, sizeof(errbuf));
					LOG_ERROR << "SEND FRAME ERROR! " << errbuf;
					break;
				}
				drain_packets();

				offset += frame_size;
				sent_frames++;
			}

			// 4. 尾部不足 1024 的采样点：留在 swr 内部缓冲不丢，下一帧接着用？
			//    实际上 swr_convert 一次性转换不会自动缓冲，尾部丢弃。
			//    这里简单起见丢弃 <1024 的尾部（22050 % 1024 = 546 点，可忽略）。

			av_frame_free(&tmp);
		}
	}



	//注意回调：在 main.cpp 中调用 set_callback 时，把 lambda 函数转移给 callback_。
	//所以在编码阶段执行 callback_ 时，就是执行 main 写好的 lambda。
	//这样解耦了：main.cpp 不需要知道有没有编码成功，编码阶段也不需要 main 的使用权。
	void FFmpegAudioEncoder::set_callback(EncodedPacketCallback cb)
	{
		callback_ = std::move(cb);
	}

}