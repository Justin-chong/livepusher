#include "ffmpeg_muxer.h"
#include "common/logger.h"

extern "C" {
#include<libavformat/avformat.h>
}

namespace livepusher {

	FFmpegMuxer::FFmpegMuxer()
	{
		LOG_INFO << "FFMPEGMUXER CREATE!";
	}

	FFmpegMuxer::~FFmpegMuxer()
	{
		close();
		LOG_INFO << "FFMPEGMUXER DESTROY!";
	}
	
	//url="test.flv"
	//url="rtmp://server/live/stream"
	bool FFmpegMuxer::open(const std::string& url)
	{
		// 保存 url，断线重连时复用。
		url_ = url;

		// 根据 URL 自动推断格式：.flv 是 flv，rtmp:// 也是 flv
		/*
		- 第二个参数 nullptr：自动推断
	   - 第三个参数 "flv"：强制用 flv 格式
	   - 第四个参数 url：文件路径或 rtmp://
		*/
		int ret = avformat_alloc_output_context2(&ctx_, nullptr, "flv", url.c_str());
		if (ret < 0) {
			LOG_ERROR << "OPEN URL ERROR!";
			return false;
		}

		// 打开 IO（文件或网络）
		if (!(ctx_->oformat->flags & AVFMT_NOFILE)) {
			ret = avio_open(&ctx_->pb, url.c_str(), AVIO_FLAG_WRITE);
			if (ret < 0) {
				LOG_ERROR << "AVIO OPEN ERROR!";
				avformat_free_context(ctx_);
				ctx_ = nullptr;
				return false;
			}
		}

		LOG_INFO << "Muxer opened: " << url;
		return true;
	}

	int FFmpegMuxer::addVideoStream(const AVCodecContext* codec_ctx)
	{
		//创建流
		AVStream* stream = avformat_new_stream(ctx_, nullptr);
		if (!stream) {
			LOG_ERROR << "CREATE NEW VIDEO STREAM ERROR!";
			return -1;
		}
		//从编码器复制参数
		int ret = avcodec_parameters_from_context(stream->codecpar, codec_ctx);
		if (ret < 0) {
			LOG_ERROR << "copy video codec params failed";
			return -1;
		}

		//设置时间基
		stream->time_base = codec_ctx->time_base;
		video_stream_idx_ = stream->index;
		LOG_INFO << "VIDEO STREAM ADD, INDEX= " << video_stream_idx_;
		return video_stream_idx_;
	}

	int FFmpegMuxer::addAudioStream(const AVCodecContext* codec_ctx)
	{
		//创建流
		AVStream* stream = avformat_new_stream(ctx_, nullptr);
		if (!stream) {
			LOG_ERROR << "CREATE NEW AUDIO STREAM ERROR!";
			return -1;
		}
		// 诊断：看此时编码器的 extradata 是否已经生成（AAC 的 AudioSpecificConfig）
		LOG_INFO << "[AUDIO] extradata_size=" << codec_ctx->extradata_size
			<< " codec_id=" << codec_ctx->codec_id
			<< " profile=" << codec_ctx->profile
			<< " sample_fmt=" << av_get_sample_fmt_name(codec_ctx->sample_fmt);
		//从编码器复制参数
		int ret = avcodec_parameters_from_context(stream->codecpar, codec_ctx);
		if (ret < 0) {
			LOG_ERROR << "copy audio codec params failed";
			return -1;
		}

		//设置时间基
		stream->time_base = codec_ctx->time_base;
		audio_stream_idx_ = stream->index;
		LOG_INFO << "AUDIO STREAM ADD, INDEX= " << audio_stream_idx_;
		return audio_stream_idx_;
	}

	void FFmpegMuxer::writeHeader()
	{
		int ret = avformat_write_header(ctx_, nullptr);
		if (ret < 0) {
			LOG_ERROR << "WRITE HEADER ERROR!";
			return ;
		}
		LOG_INFO << "SUCCESSFUL WRITE HEADER!";
	}

	void FFmpegMuxer::writePacket(std::shared_ptr<AVPacket> pkt, int stream_idx)
	{
		if (!ctx_ || stream_idx < 0)return;

		// 加锁：视频/音频两个编码线程并发写同一个 muxer 时必须串行。
		// av_interleaved_write_frame 不是线程安全的，并发调用会破坏内部
		// 缓冲与时间戳排序状态，导致死锁（现象：video queue full 刷屏，
		// 编码线程卡死不再消费队列）。
		std::lock_guard<std::mutex> lock(write_mutex_);

		// 断线后直接丢弃：不再写已经断掉的 socket（否则每帧都失败还浪费 CPU）。
		// 放在拿锁之后检查，避免和 reconnect 里的 resetInternal 竞态。
		if (disconnected_.load()) {
			return;
		}

		//克隆 packet，避免改到调用方的 packet
		AVPacket* new_pkt = av_packet_clone(pkt.get());
		if (!new_pkt)return;
		//设置流索引
		new_pkt->stream_index = stream_idx;

		//pts/dts 转换到流的时间基
		AVStream* stream = ctx_->streams[stream_idx];
		av_packet_rescale_ts(new_pkt, pkt->time_base, stream->time_base);

		//写入Socket缓冲区或写入本地文件MP4中
		int ret = av_interleaved_write_frame(ctx_, new_pkt);
		av_packet_free(&new_pkt);

		// ===== 断线检测 =====
		// av_interleaved_write_frame 返回负值 = 写失败（连接断了 / 服务器挂了 / 网络错）。
		// 单次失败可能偶发，连续失败达到阈值才判定断线，避免误报。
		if (ret < 0) {
			++consecutive_errors_;
			LOG_ERROR << "write frame failed, ret=" << ret
				<< " consecutive=" << consecutive_errors_
				<< "/" << kMaxConsecutiveErrors;

			// 连续失败达阈值且还没触发过 → 判定断线，触发一次回调。
			if (consecutive_errors_ >= kMaxConsecutiveErrors && !disconnected_.load()) {
				disconnected_.store(true);
				if (disconnect_callback_) {
					// 注意：回调在锁内触发，回调里不能做耗时操作（比如再写 muxer），
					// 否则可能死锁。这里回调只负责通知上层，重连由上层另开逻辑。
					disconnect_callback_(ret);
				}
			}
		} else {
			// 写成功，连续失败计数清零。
			consecutive_errors_ = 0;
		}
	}

	void FFmpegMuxer::close()
	{
		if (ctx_) {
			// дβ
			av_write_trailer(ctx_);

			// 关闭 IO
			if (!(ctx_->oformat->flags & AVFMT_NOFILE) && ctx_->pb) {
				avio_closep(&ctx_->pb);
			}

			avformat_free_context(ctx_);
			ctx_ = nullptr;
		}
		video_stream_idx_ = -1;
		audio_stream_idx_ = -1;
		LOG_INFO << "Muxer closed";
	}

	// ===== 断线检测 =====
	void FFmpegMuxer::setDisconnectCallback(std::function<void(int)> cb)
	{
		disconnect_callback_ = std::move(cb);
	}

	bool FFmpegMuxer::isDisconnected() const
	{
		return disconnected_.load();
	}

	// ===== 断线重连 =====
	// 强制释放当前 muxer，不做 write_trailer（连接已断，写尾没意义还可能阻塞）。
	void FFmpegMuxer::resetInternal()
	{
		if (ctx_) {
			if (!(ctx_->oformat->flags & AVFMT_NOFILE) && ctx_->pb) {
				avio_closep(&ctx_->pb);
			}
			avformat_free_context(ctx_);
			ctx_ = nullptr;
		}
		video_stream_idx_ = -1;
		audio_stream_idx_ = -1;
	}

	bool FFmpegMuxer::reconnect(const AVCodecContext* video_ctx, const AVCodecContext* audio_ctx)
	{
		// 拿锁：和 writePacket 互斥，避免「编码线程还在写」时我们关掉 ctx_。
		std::lock_guard<std::mutex> lock(write_mutex_);

		LOG_INFO << "[RECONNECT] 关闭旧连接，重新推流...";

		// 1. 关掉断掉的旧连接（跳过 write_trailer）
		resetInternal();

		// 2. 用保存的 url 重新打开
		if (!open(url_)) {
			LOG_ERROR << "[RECONNECT] reopen failed";
			return false;
		}

		// 3. 重新加流（FLV 流索引从 0 重新编号，视频=0 音频=1，和首次一致）
		video_stream_idx_ = addVideoStream(video_ctx);
		audio_stream_idx_ = addAudioStream(audio_ctx);
		if (video_stream_idx_ < 0 || audio_stream_idx_ < 0) {
			LOG_ERROR << "[RECONNECT] add stream failed";
			resetInternal();
			return false;
		}

		// 4. 写头
		writeHeader();

		// 5. 复位断线状态，让 writePacket 恢复写
		disconnected_.store(false);
		consecutive_errors_ = 0;

		LOG_INFO << "[RECONNECT] 重连成功";
		return true;
	}

	// ===== 弱网处理（进阶）：发送速率估算 =====
	// 基于 AVIOContext::bytes_written 的增量，算「两次调用之间」的平均发送速率（bps）。
	// 这个速率是「真正发进 socket 的字节数/时间」，比目标码率更真实地反映上行带宽。
	// 当它明显低于目标码率时，说明数据在积压（阶段1），要提前降码率。
	int64_t FFmpegMuxer::estimateSendRate()
	{
		// 拿锁：ctx_ 和 pb 可能在 reconnect 里被关掉/重建，避免 use-after-free。
		std::lock_guard<std::mutex> lock(write_mutex_);

		if (!ctx_ || !ctx_->pb) {
			// 没连接（比如断线重连间隙），速率视为 0。
			has_last_sample_ = false;
			return 0;
		}

		int64_t now_bytes = ctx_->pb->bytes_written;
		auto now_time = std::chrono::steady_clock::now();

		// 首次调用没有历史基准，只记录，返回 0。
		if (!has_last_sample_) {
			last_bytes_written_ = now_bytes;
			last_sample_time_ = now_time;
			has_last_sample_ = true;
			return 0;
		}

		// 算两次采样之间的平均速率（字节/秒 → 比特/秒）。
		int64_t delta_bytes = now_bytes - last_bytes_written_;
		auto delta_us = std::chrono::duration_cast<std::chrono::microseconds>(
			now_time - last_sample_time_).count();

		// 更新基准，供下次调用。
		last_bytes_written_ = now_bytes;
		last_sample_time_ = now_time;

		if (delta_us <= 0) {
			return 0;  // 时间没推进（极端情况），避免除零。
		}

		// 平均发送速率（bps）= 字节增量 * 8 / 秒数。
		int64_t rate_bps = delta_bytes * 8LL * 1000000LL / delta_us;
		return rate_bps;
	}

}