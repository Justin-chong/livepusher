#include "ffmpeg_audio_capturer.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavutil/imgutils.h>
}

namespace livepusher {

	static const int kDefaultBitDepth = 16;

	FFmpegAudioCapturer::FFmpegAudioCapturer()
	{
		LOG_INFO << "FFmpegAudioCapturer created!";
	}

	FFmpegAudioCapturer::~FFmpegAudioCapturer()
	{
		LOG_INFO << "FFmpegAudioCapturer destroyed!";
	}

	bool FFmpegAudioCapturer::open(const std::string& device_name, int sample_rate, int channels)
	{
		//设置参数
		sample_rate_ = sample_rate;
		
		channels_ = channels;

		// 注册所有设备（dshow 等），幂等可多次调用
		avdevice_register_all();

		// 设置期望的分辨率和帧率，通过 AVDictionary 传给 dshow
		AVDictionary* opt = nullptr;
		av_dict_set(&opt, "sample_rate", std::to_string(sample_rate_).c_str(), 0);
		av_dict_set(&opt, "channels", std::to_string(channels_).c_str(), 0);
		av_dict_set(&opt, "bit_depth", std::to_string(kDefaultBitDepth).c_str(), 0);


		// 显式指定用 dshow 打开，不传的话 FFmpeg 不认识 "video=xxx"
		const AVInputFormat* ifmt = av_find_input_format("dshow");
		if (!ifmt) {
			LOG_ERROR << "Cannot find dshow demuxer";
			av_dict_free(&opt);
			return false;
		}

		// 打开设备：分配 ctx_、建立与摄像头的连接
		int ret = avformat_open_input(&ctx_, device_name.c_str(), ifmt, &opt);
		av_dict_free(&opt);
		if (ret < 0) {
			char errbuf[128] = { 0 };
			av_strerror(ret, errbuf, sizeof(errbuf));
			LOG_ERROR << "Cannot open device: " << errbuf;
			return false;
		}

		//探测流信息
		ret = avformat_find_stream_info(ctx_, nullptr);
		if (ret < 0) {
			LOG_ERROR << "Cannot find stream info";
			close();
			return false;
		}

		//找到音频流索引
		audio_stream_idx_ = av_find_best_stream(ctx_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
		if (audio_stream_idx_ < 0) {
			LOG_ERROR << "No video stream";
			close();
			return false;
		}
		// 注册中断回调
		ctx_->interrupt_callback.callback = &FFmpegAudioCapturer::interrupt_callback_fn;
		ctx_->interrupt_callback.opaque = this;

		LOG_INFO << "Device opened: " << device_name;
		return true;
	}

	bool FFmpegAudioCapturer::start()
	{
		if (running_.load()) {
			LOG_WARN << "Capturer already running";
			return false;
		}
		if (!ctx_) {
			LOG_ERROR << "Device not opened, call open() first";
			return false;
		}

		// 时钟归零，开始计时
		clock_.start();
		running_.store(true);

		// 启动采集线程
		capture_thread_ = std::thread(&FFmpegAudioCapturer::capture_loop, this);
		LOG_INFO << "Capture thread started";
		return true;
	}

	void FFmpegAudioCapturer::stop()
	{
		if (!running_.load()) {
			return;
		}

		// 先置标志，让 capture_loop 在下一帧处理完后退出
		running_.store(false);

		// 等待采集线程退出
		// dshow 持续送帧时，每帧处理完都会检查 running_，所以 join 不会卡住
		if (capture_thread_.joinable()) {
			capture_thread_.join();
		}

		// 关闭设备
		if (ctx_) {
			avformat_close_input(&ctx_);
			ctx_ = nullptr;
		}
		audio_stream_idx_ = -1;
		LOG_INFO << "Capture thread stopped";
	}

	void FFmpegAudioCapturer::set_callback(AudioFrameCallback cb)
	{
		callback_ = std::move(cb);
	}

	void FFmpegAudioCapturer::close()
	{
		stop();
	}

	// 音频采集线程主循环
// 职责：从麦克风读 PCM 数据 → 包装成 AVFrame → 打时间戳 → 回调给上层
	void FFmpegAudioCapturer::capture_loop() {
		LOG_INFO << "Audio capture_loop started";

		// 分配 AVPacket（FFmpeg 读取的原始数据包）
		AVPacket* pkt = av_packet_alloc();
		if (!pkt) return;

		// 取出音频流实际协商出来的参数
		AVStream* stream = ctx_->streams[audio_stream_idx_];
		int src_sample_rate = stream->codecpar->sample_rate;        // 采样率
		int src_channels = stream->codecpar->ch_layout.nb_channels; // 声道数
		AVSampleFormat src_fmt = (AVSampleFormat)stream->codecpar->format; // 采样格式

		LOG_INFO << "Audio params: " << src_sample_rate
			<< "Hz, " << src_channels << "ch"
			<< ", fmt=" << av_get_sample_fmt_name(src_fmt);

		int frame_count = 0;

		while (running_.load()) {
			// 从麦克风读一包 PCM 数据
			int ret = av_read_frame(ctx_, pkt);
			if (ret < 0) {
				if (ret != AVERROR_EXIT) {
					LOG_ERROR << "av_read_frame failed, ret=" << ret;
				}
				break;
			}

			// 只处理音频流，其他流（如视频包）直接丢
			if (pkt->stream_index != audio_stream_idx_) {
				av_packet_unref(pkt);
				continue;
			}

			// 分配 AVFrame，用智能指针管理生命周期
			std::shared_ptr<AVFrame> frame(
				av_frame_alloc(),
				[](AVFrame* f) { av_frame_free(&f); }
			);

			// 设置音频帧参数
			frame->sample_rate = src_sample_rate;           // 采样率
			frame->ch_layout = stream->codecpar->ch_layout; // 声道布局
			frame->format = src_fmt;                        // 采样格式（S16）
			// 计算采样点数：总字节数 / (声道数 * 每采样点字节数)
			// 16bit = 2 字节，所以每个采样点占 channels * 2 字节
			frame->nb_samples = pkt->size / (src_channels * 2);

			// 为帧分配底层缓冲区
			ret = av_frame_get_buffer(frame.get(), 0);
			if (ret < 0) {
				LOG_ERROR << "Failed to allocate audio frame buffer";
				av_packet_unref(pkt);
				continue;
			}

			// 音频数据是连续的一块内存，直接 memcpy 拷贝
			memcpy(frame->data[0], pkt->data, pkt->size);

			//// dshow 不提供 PTS，用自己的单调时钟打时间戳（微秒）
			//frame->pts = clock_.elapsed_us();
			//2026.10.9修改
			frame->pts = Clock::now_us();

			// 前5帧和每50帧打一次日志
			if (frame_count < 5 || frame_count % 50 == 0) {
				LOG_INFO << "Audio frame " << frame_count
					<< ": " << frame->nb_samples << " samples"
					<< ", pts=" << frame->pts;
			}
			frame_count++;

			// 把帧交给上层（AAC 编码器），调用回调函数
			if (callback_) callback_(frame);

			// 释放 packet 的引用，复用下一次
			av_packet_unref(pkt);

			// 每帧处理完检查运行标志
			if (!running_.load()) break;
		}

		av_packet_free(&pkt);
		LOG_INFO << "Audio capture loop exited, total: " << frame_count;
	}

	int FFmpegAudioCapturer::interrupt_callback_fn(void* opaque)
	{
		auto* self = static_cast<FFmpegAudioCapturer*>(opaque);
		return self->running_.load() ? 0 : 1;
	}


}
