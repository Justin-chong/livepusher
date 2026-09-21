#pragma once
#include <string>
#include<functional>
#include <memory>

extern "C" {
#include<libavutil/frame.h>
}

namespace livepusher {
	using AudioFrameCallback = std::function<void(std::shared_ptr<AVFrame> frame)>;

	class AudioCapturer {
	public:
		virtual ~AudioCapturer() = default;

		// 打开设备，返回是否成功,音频参数
		virtual bool open(const std::string& device_name,
			int bit_depth,int channels) = 0;
		// 启动采集线程
		virtual bool start() = 0;

		// 停止采集，等待线程退出
		virtual void stop() = 0;

		// 设置帧回调
		virtual void set_callback(AudioFrameCallback cb) = 0;

		// 关闭设备，释放资源
		virtual void close() = 0;
	};

}