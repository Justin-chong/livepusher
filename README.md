# Live Pusher

Windows 桌面端 RTMP 推流客户端，基于 FFmpeg 实现「采集 → 编码 → 封装 → 推流」全链路。

## 技术栈

- **语言**：C++17
- **IDE**：Visual Studio 2022
- **构建**：CMake
- **多媒体**：FFmpeg 9.0.1（gyan.dev MSVC 编译版）
- **编码器**：libx264（H.264）、FFmpeg 原生 AAC

## 项目结构

```
live_pusher/
├── CMakeLists.txt
└── src/
    ├── main.cpp                          # 入口，各模块测试
    │
    ├── common/                           # 公共组件
    │   ├── logger.h                      # 单例日志器（LogStream + LOG_* 宏）
    │   ├── log_level.h                   # 日志级别枚举（DEBUG/INFO/WARN/ERR）
    │   ├── clock.h                       # steady_clock 单调时钟封装
    │   └── thread_safe_queue.h          # 有界阻塞队列（线程安全）
    │
    ├── capture/                          # 采集模块
    │   ├── video_capturer.h              # 视频采集抽象接口
    │   ├── ffmpeg_video_capturer.h       # FFmpeg 视频采集实现（dshow）
    │   ├── ffmpeg_video_capturer.cpp
    │   ├── audio_capturer.h              # 音频采集抽象接口
    │   ├── ffmpeg_audio_capturer.h       # FFmpeg 音频采集实现（dshow）
    │   └── ffmpeg_audio_capturer.cpp
    │
    ├── encode/                           # 编码模块
    │   ├── video_encoder.h               # 视频编码抽象接口
    │   ├── ffmpeg_video_encode.h         # FFmpeg H.264 编码实现
    │   ├── ffmpeg_video_encode.cpp
    │   ├── audio_encoder.h               # 音频编码抽象接口
    │   ├── ffmpeg_audio_encode.h        # FFmpeg AAC 编码实现
    │   └── ffmpeg_audio_encode.cpp
    │
    └── muxer/                            # 封装模块
        ├── muxer.h                       # 封装抽象接口
        ├── ffmpeg_muxer.h                # FFmpeg FLV 封装实现
        └── ffmpeg_muxer.cpp
```

## 模块设计

每个模块都采用「抽象接口 + 具体实现」的两层设计：

```
上层代码依赖接口，不关心底层实现
     │
     ├── VideoCapturer（接口）
     │     └── FFmpegVideoCapturer（dshow 实现）
     │
     ├── VideoEncoder（接口）
     │     └── FFmpegVideoEncoder（libx264 实现）
     │
     ├── AudioEncoder（接口）
     │     └── FFmpegAudioEncoder（AAC 实现）
     │
     └── Muxer（接口）
           └── FFmpegMuxer（FLV 实现）
```

### 数据流

```
摄像头（dshow）
    │ YUV 帧
    ▼
VideoCapturer ──回调──▶ VideoEncoder ──回调──▶ Muxer ──▶ FLV/RTMP
麦克风（dshow）
    │ PCM 帧
    ▼
AudioCapturer ──回调──▶ AudioEncoder ──回调──▶ Muxer
```

## 核心组件说明

### Logger

单例日志器，支持分级输出（DEBUG/INFO/WARN/ERR），使用宏 + RAII 流式写入：

```cpp
LOG_INFO << "width=" << width << ", height=" << height;
```

### Clock

基于 `std::chrono::steady_clock` 的单调时钟，用于打时间戳，不受系统时间修改影响。

### ThreadSafeQueue

有界阻塞队列，生产者 push / 消费者 pop，线程安全，支持关闭后自动退出消费者。

## 构建说明

### 依赖

- FFmpeg 9.0.1（gyan.dev MSVC shared 版本），路径：`D:\library\ffmpeg-9.0.1-full_build-shared`
- CMake 3.20+
- Visual Studio 2022（MSVC v143）

### 编译选项

- C++ 标准：C++17
- 编译参数：`/utf-8`（解决 MSVC 中文编码问题）
- Post-build：自动复制 bin 目录下的 DLL 到 exe 目录

### 构建步骤

```bash
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

## 已完成功能

- [x] 基础设施（日志、时钟、线程安全队列）
- [x] 视频采集（dshow 打开摄像头，YUV 帧输出）
- [x] 视频编码（libx264 H.264，YUV420P 输入）
- [x] 音频采集（dshow 打开麦克风，PCM 帧输出）
- [x] 音频编码（FFmpeg 原生 AAC，fltp 输入）
- [x] FLV 封装（本地文件输出）

## 待完成功能

- [ ] 音视频格式转换（sws_scale / swresample）
- [ ] RTMP 推流
- [ ] 采集 → 编码 → 封装全链路串联
- [ ] 弱网自适应 / 重连机制
- [ ] GUI 界面

## 注意事项

1. **FFmpeg 版本**：必须使用 MSVC 编译的 FFmpeg（gyan.dev），MinGW 版本的 DLL 在 MSVC 程序中无法注册 dshow 设备。
2. **音频采样格式**：AAC 编码器要求 `fltp`（平面浮点），采集器输出 `s16`（交错 16bit），中间需要重采样转换。
3. **视频像素格式**：dshow 摄像头输出 `yuvj422p`，libx264 要求 `yuv420p`，中间需要 swscale 转换。
4. **编码冲洗**：编码完所有帧后需要 `send_frame(nullptr)` 冲洗编码器，输出内部缓冲的帧。
