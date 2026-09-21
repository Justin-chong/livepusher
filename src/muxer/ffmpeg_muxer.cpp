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
		// 根据 URL 自动推断格式（.flv → flv，rtmp:// → flv）
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
		//创建新流
		AVStream* stream = avformat_new_stream(ctx_, nullptr);
		if (!stream) {
			LOG_ERROR << "CREATE NEW VIDEO STREAM ERROR!";
			return -1;
		}
		//从编码器上下文复制流参数
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
		//创建新流
		AVStream* stream = avformat_new_stream(ctx_, nullptr);
		if (!stream) {
			LOG_ERROR << "CREATE NEW AUDIO STREAM ERROR!";
			return -1;
		}
		//从编码器上下文复制流参数
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

		//复制packet,设置流索引和时间基
		AVPacket* new_pkt = av_packet_clone(pkt.get());
		if (!new_pkt)return;
		//属于哪条流
		new_pkt->stream_index = stream_idx;

		//pts/dts转换到流的时间基
		AVStream* stream = ctx_->streams[stream_idx];
		av_packet_rescale_ts(new_pkt, pkt->time_base, stream->time_base);

		//写包
		int ret = av_interleaved_write_frame(ctx_, new_pkt);
		av_packet_free(&new_pkt);

		if (ret < 0) {
			LOG_ERROR << "write frame failed, ret=" << ret;
		}
	}

	void FFmpegMuxer::close()
	{
		if (ctx_) {
			// 写尾
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

}