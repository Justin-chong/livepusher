#pragma once
#include  "muxer.h"
#include <mutex>
#include <functional>
#include <atomic>
#include <string>
#include <chrono>

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

		// ===== 断线检测 =====
		// 注册断线回调：连续 write 失败达到阈值时触发一次，参数是最后一次错误码。
		void setDisconnectCallback(std::function<void(int)> cb);
		// 查询当前是否已判定断线（重连成功后由重连逻辑复位）。
		bool isDisconnected() const;

		// ===== 断线重连 =====
		// 关闭断掉的旧连接，用保存的 url 重新 open、加流、写头，并复位断线状态。
		// 内部拿 write_mutex_ 锁，和 writePacket 互斥，避免并发 close/open 与写冲突。
		// 必须在锁外（非 writePacket 回调内）调用，否则非递归锁重入会死锁。
		bool reconnect(const AVCodecContext* video_ctx, const AVCodecContext* audio_ctx);

		// ===== 弱网处理（进阶）：发送速率估算 =====
		// 返回「自上次调用以来」的平均发送速率（bps），基于 AVIOContext::bytes_written 的增量。
		// 主循环周期性调用，和当前目标码率对比，判断是否在积压（阶段1，比断线更早）。
		// 首次调用返回 0（没有历史采样）。线程安全（拿 write_mutex_ 锁）。
		int64_t estimateSendRate();

	private:
		// 强制释放当前 muxer（不做 av_write_trailer，因为连接已断，写尾没意义还可能阻塞）。
		// 供 reconnect 使用；close() 正常结束时才走 write_trailer。
		void resetInternal();

		// 封装上下文
		AVFormatContext* ctx_ = nullptr;
		int video_stream_idx_ = -1;
		int audio_stream_idx_ = -1;
		// 推流地址，open 时保存，reconnect 时复用。
		std::string url_;
		// 互斥锁：av_interleaved_write_frame 不是线程安全的，
		// 视频编码线程和音频编码线程会同时调 writePacket 写同一个 ctx_，
		// 必须串行化，否则 muxer 内部状态被并发破坏 → 死锁/卡死。
		std::mutex write_mutex_;

		// ===== 断线检测状态 =====
		// 断线回调（由上层 main.cpp 注册，触发后做重连等处理）。
		std::function<void(int)> disconnect_callback_;
		// 连续写失败次数（成功写一帧就清零）。在 write_mutex_ 保护下读写。
		int consecutive_errors_ = 0;
		// 连续失败多少次才判定「断线」。单次失败可能是偶发，连续多次才可信。
		static constexpr int kMaxConsecutiveErrors = 3;
		// 是否已判定断线（用 atomic，允许其他线程无锁查询）。
		std::atomic<bool> disconnected_{ false };

		// ===== 弱网处理（进阶）：发送速率估算状态 =====
		// 上次采样的累计字节数和时间点，用于算两次采样之间的平均速率。
		int64_t last_bytes_written_ = 0;
		std::chrono::steady_clock::time_point last_sample_time_;
		bool has_last_sample_ = false;

	};

}
