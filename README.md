# Live Pusher

Windows 桌面端 RTMP 推流客户端，基于 FFmpeg + Qt 实现「采集 → 编码 → 封装 → RTMP 推流」全链路，支持断线重连和弱网自适应。

## 功能特性

- **图形界面**（Qt 6）：地址输入框 + 开始/停止按钮 + 实时状态栏 + 日志区
- **完整推流链路**：摄像头/麦克风采集 → H.264/AAC 编码 → FLV 封装 → RTMP 推流
- **断线检测 + 自动重连**：写包连续失败判定断线，muxer 重建 + 强制 I 帧恢复
- **弱网自适应**：三档码率动态升降，基于发送速率监控提前降码率
- **可重复启停**：停止→再开始干净复位，可多次推流

## 技术栈

- **语言**：C++17
- **GUI**：Qt 6.10.3（Widgets）
- **构建**：CMake + Ninja（MSVC 2022）
- **多媒体**：FFmpeg 9.0.1（gyan.dev MSVC 版）
- **编码器**：libx264（H.264）、FFmpeg 原生 AAC

## 界面预览

```
┌─────────────────────────────────────────────────────┐
│ [推流地址输入框.............] [开始推流] [停止推流]     │
│ 推流中  码率:高(1000kbps)  发送速率:980kbps  时长:00:37  连接:正常 │
│ ┌───────────────────────────────────────────────┐ │
│ │ 日志区（实时滚动，LOG_XXX 自动同步）             │ │
│ └───────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────┘
```

## 项目结构

```
live_pusher/
├── CMakeLists.txt
├── CMakePresets.json
├── INTERVIEW_NOTES.md                # 面试笔记（坑 1-19）
├── tools/
│   └── play_loop.bat                 # 循环拉流脚本（断流自动重连）
└── src/
    ├── main.cpp                      # Qt 入口
    ├── main_window.h/.cpp            # Qt 主窗口（地址框 + 按钮 + 状态栏 + 日志）
    ├── pusher_controller.h/.cpp      # 推流生命周期封装（start/stop）
    │
    ├── common/                       # 公共组件
    │   ├── logger.h                  # 单例日志器（LOG_* 宏 + sink 回调）
    │   ├── log_level.h               # 日志级别枚举
    │   ├── clock.h                   # steady_clock 单调时钟
    │   └── thread_safe_queue.h      # 有界阻塞队列（支持 close/reset）
    │
    ├── capture/                      # 采集模块（抽象接口 + dshow 实现）
    │   ├── video_capturer.h          # 视频采集接口
    │   ├── ffmpeg_video_capturer.*   # dshow 摄像头（MJPEG 解码 + swscale）
    │   ├── audio_capturer.h          # 音频采集接口
    │   └── ffmpeg_audio_capturer.*   # dshow 麦克风（PCM s16）
    │
    ├── encode/                       # 编码模块（抽象接口 + 实现）
    │   ├── video_encoder.h           # 视频编码接口
    │   ├── ffmpeg_video_encode.*     # libx264 H.264（支持动态码率）
    │   ├── audio_encoder.h           # 音频编码接口
    │   └── ffmpeg_audio_encode.*     # AAC（swr 重采样 s16→fltp）
    │
    └── muxer/                        # 封装模块
        ├── muxer.h                   # 封装接口
        └── ffmpeg_muxer.*            # FLV/RTMP（断线检测 + 重连 + 速率估算）
```

## 架构设计

### 分层架构

每个模块都采用「抽象接口 + 具体实现」两层设计，通过 `std::function` 回调串联：

```
GUI（Qt）
  └── PusherController（start/stop 生命周期管理）
        │
        ├── VideoCapturer（接口）── FFmpegVideoCapturer（dshow）
        ├── AudioCapturer（接口）── FFmpegAudioCapturer（dshow）
        ├── VideoEncoder（接口）── FFmpegVideoEncoder（libx264）
        ├── AudioEncoder（接口）── FFmpegAudioEncoder（AAC）
        └── Muxer（接口）───────── FFmpegMuxer（FLV/RTMP）
```

### 数据流

```
摄像头(dshow) ──YUV──▶ VideoCapturer ──回调──▶ ThreadSafeQueue
                                                  │
                                                  ▼
麦克风(dshow) ──PCM──▶ AudioCapturer ──回调──▶ ThreadSafeQueue
                                                  │
                                                  ▼
                        VideoEncoder / AudioEncoder（编码线程）
                                                  │
                                                  ▼
                                    Muxer.writePacket ──▶ RTMP
                                                  ▲
                                    MonitorLoop（断线重连 + 弱网自适应）
```

### 关键机制

- **队列解耦**：采集与编码之间用有界队列解耦，采集回调用 `try_push`（非阻塞，满了丢帧），绝不阻塞采集线程。
- **断线检测**：`av_interleaved_write_frame` 返回负值 + 连续 3 次失败判定断线，回调通知。
- **断线重连**：关旧 muxer → 重新 open → 加流 → 写头，编码器不动；重连后强制 I 帧。
- **弱网自适应**：`AVIOContext::bytes_written` 估算发送速率，速率低于码率 80% 持续 2 秒降档；断线直接降档；稳定 3 秒升档。
- **跨线程 UI**：日志/状态在工作线程产生，用 `Qt::QueuedConnection` 投递回主线程刷新界面。

## 构建说明

### 依赖

- **FFmpeg 9.0.1**（gyan.dev MSVC shared 版），路径 `D:\library\ffmpeg-9.0.1-full_build-shared`
- **Qt 6.10.3**（MSVC 2022 64 位版），路径 `F:\QTdevelopment\6.10.3\msvc2022_64`
- **CMake 3.15+**、**Visual Studio 2022**（MSVC v143）

> ⚠️ 注意：Qt 必须用 **msvc2022_64** 版（匹配项目编译器）；MinGW 版无法链接。

### 构建步骤（Release）

```powershell
# 配置
cmake --preset x64-release

# 编译（需先设置 MSVC 环境，或用「x64 Native Tools Command Prompt」）
cmake --build out/build/x64-release

# 部署 Qt 运行时 DLL
windeployqt --release out/build/x64-release/live_pusher.exe
```

产物：`out/build/x64-release/live_pusher.exe`

> ⚠️ **必须用 Release 构建**：Debug 版依赖 `MSVCP140D.dll` 等 Debug CRT，未安装 Debug 组件时程序启动即崩溃（退出码 `-1073741515`）。

## 使用方法

### 1. 启动本地推流服务器（livego）

```bash
tools/livego/livego.exe
```

### 2. 获取推流密钥

```bash
curl http://127.0.0.1:8090/control/get?room=movie
# 返回 {"status":200,"data":"<channelKey>"}
```

### 3. 启动推流器

双击 `live_pusher.exe`，地址框填入：

```
rtmp://127.0.0.1:1935/live/<channelKey>
```

点「开始推流」。

### 4. 拉流验证

```bash
ffplay -fflags nobuffer -flags low_delay -probesize 32 -sync ext rtmp://127.0.0.1:1935/live/movie
```

> 注意：推流 URL 用 **channelKey**，拉流 URL 用 **room 名（movie）**，两者不同。

## 已完成功能

- [x] 基础设施（日志、时钟、线程安全队列）
- [x] 视频采集（dshow 摄像头，MJPEG 解码 + swscale 转 YUV420P）
- [x] 视频编码（libx264 H.264，支持动态码率）
- [x] 音频采集（dshow 麦克风，PCM s16）
- [x] 音频编码（AAC，swr 重采样 s16→fltp）
- [x] FLV 封装 + RTMP 推流
- [x] 断线检测 + 自动重连
- [x] 弱网码率自适应（最简版 + 发送速率监控进阶版）
- [x] Qt 图形界面 + 实时状态栏
- [x] 可重复启停

## 待完成功能

- [ ] 公网推流（真实 RTMP 服务器）
- [ ] 弱网丢帧策略（码率降到底还不够时）
- [ ] 音画同步精确对齐（当前粗对齐，23ms 残差）
- [ ] 打包发布（一键绿色版/安装包）

## 注意事项

1. **FFmpeg 版本**：必须用 MSVC 编译的 FFmpeg（gyan.dev），MinGW 版无法注册 dshow 设备。
2. **音频采样格式**：AAC 编码器要求 `fltp`（平面浮点），采集器输出 `s16`（交错 16bit），需 swr 重采样。
3. **视频像素格式**：dshow 摄像头输出 `yuvj422p`（full range），libx264 要求 `yuv420p`，需 swscale 转换。
4. **编码冲洗**：编码完需 `send_frame(nullptr)` 冲洗编码器，输出内部缓冲帧。
5. **channelKey 动态变化**：livego 重启后 key 会变（存内存），需重新 curl 获取。
6. **FLV 结束警告**：`Failed to update header` 是 RTMP 单向推流的固有提示，非致命。

## 面试笔记

项目中的踩坑、解法、面试概念记录在 [INTERVIEW_NOTES.md](./INTERVIEW_NOTES.md)，涵盖 19 个典型问题（队列解耦、muxer 死锁、断线重连、弱网自适应、Qt 跨线程等）。
