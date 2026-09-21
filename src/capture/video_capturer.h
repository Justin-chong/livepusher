#pragma once

#include <string>
#include <functional>
#include <memory>

extern "C" {
#include <libavutil/frame.h>
}

namespace livepusher {

// 视频帧回调：每采集到一帧，就调用这个函数
using VideoFrameCallback = std::function<void(std::shared_ptr<AVFrame> frame)>;

// 视频采集器抽象接口
// 上层只依赖这个接口，不关心底层是摄像头还是文件
class VideoCapturer {
public:
    virtual ~VideoCapturer() = default;

    // 打开设备，返回是否成功
    virtual bool open(const std::string& device_name,
                      int width, int height, int fps) = 0;

    // 启动采集线程
    virtual bool start() = 0;

    // 停止采集，等待线程退出
    virtual void stop() = 0;

    // 设置帧回调
    virtual void set_callback(VideoFrameCallback cb) = 0;

    // 关闭设备，释放资源
    virtual void close() = 0;
};

}  // namespace livepusher
