# Live Pusher

基于 **FFmpeg + Qt 6** 的 Windows 桌面端 RTMP 推流客户端，从零实现「采集 → 编码 → 封装 → 推流 / 本地录制」完整链路。

支持**摄像头 / 屏幕双视频源**、**本地实时预览**、**边推流边录制 MP4**、**弱网自适应**与**断线自动重连**，是一份用于学习与面试的完整音视频流媒体工程实践。

---

## 功能特性

- **双视频源采集**
  - 摄像头：dshow 采集（MJPEG 需解码 → 转 YUV420P）
  - 屏幕：gdigrab 采集（BMP 需解码 → 转 YUV420P），支持全屏录制
  - 麦克风：dshow 采集（PCM s16）
- **本地实时预览**：采集画面实时显示在界面左侧，YUV→RGB 转换 + 跨线程投递 + 等比缩放
- **音视频编码**：libx264（H.264）+ FFmpeg 原生 AAC（swresample 重采样 s16→fltp）
- **FLV 封装 + RTMP 推流**：`av_interleaved_write_frame` 加锁保证音视频两编码线程安全写入
- **边推流边录制**：同一编码包同时写入推流 muxer（RTMP）与录制 muxer（MP4），不重复编码
- **弱网自适应**：三档动态码率 + 发送速率监控提前降档 + 极端弱网丢 P 帧策略
- **断线检测 + 自动重连**：写包连续失败判定断线，muxer 重建 + 强制 I 帧，重连无黑屏
- **音画同步**：音视频统一锚定真实时间轴，视频 pts 用真实时间而非帧序号
- **Qt 图形界面**：左预览 + 右控制面板（OBS 风格分栏），设备/视频源下拉选择，实时状态栏

---

## 界面预览

```
┌──────────────────────────────┬──────────────────────┐
│                              │  推流设置            │
│                              │  [推流地址输入框...]   │
│         本地预览              │  [✓] 同时录制到本地    │
│     （实时画面，等比缩放）      │  [开始推流] [停止推流] │
│                              ├──────────────────────┤
│                              │  采集设置            │
│                              │  视频源: [摄像头 ▾]    │
│                              │  摄像头: [设备 ▾]      │
│                              │  麦克风: [设备 ▾]      │
│                              ├──────────────────────┤
│                              │  状态                │
│                              │  码率档: 高(1000kbps) │
│                              │  发送速率: 980kbps    │
│                              │  时长: 00:37 连接:正常 │
└──────────────────────────────┴──────────────────────┘
日志区（实时滚动，LOG_XXX 自动同步）
```

---

## 技术栈

| 类别  | 技术                                                                    |
| --- | --------------------------------------------------------------------- |
| 语言  | C++17                                                                 |
| GUI | Qt 6.10.3（Widgets，MSVC 2022 版）                                        |
| 构建  | CMake + Ninja                                                         |
| 多媒体 | FFmpeg 9.0.1（libavcodec / avformat / avdevice / swscale / swresample） |
| 编码  | libx264（H.264）、FFmpeg 原生 AAC                                          |
| 封装  | FLV（推流）、MP4（录制）                                                       |

---

## 项目结构

```
live_pusher/
├── CMakeLists.txt
├── CMakePresets.json
├── INTERVIEW_NOTES.md            # 面试笔记（20+ 踩坑记录 + 概念 + 问答）
├── src/
│   ├── main.cpp                  # Qt 入口
│   ├── main_window.h/.cpp        # 主窗口（预览 + 控制面板 + 状态栏 + 日志）
│   ├── pusher_controller.h/.cpp  # 推流生命周期封装（start/stop + 回调调度）
│   │
│   ├── common/                   # 公共组件
│   │   ├── logger.h              # 单例日志器（LOG_* 宏 + sink 回调）
│   │   ├── log_level.h           # 日志级别枚举
│   │   ├── clock.h               # steady_clock 单调时钟
│   │   └── thread_safe_queue.h   # 有界阻塞队列（try_push / pop / close / reset）
│   │
│   ├── capture/                  # 采集模块（抽象接口 + 实现）
│   │   ├── video_capturer.h      # 视频采集接口
│   │   ├── ffmpeg_video_capturer.*   # dshow 摄像头（MJPEG 解码）
│   │   ├── ffmpeg_screen_capturer.*  # gdigrab 屏幕（BMP 解码）
│   │   ├── audio_capturer.h      # 音频采集接口
│   │   └── ffmpeg_audio_capturer.*   # dshow 麦克风（PCM s16）
│   │
│   ├── encode/                   # 编码模块（抽象接口 + 实现）
│   │   ├── video_encoder.h       # 视频编码接口
│   │   ├── ffmpeg_video_encode.* # libx264 H.264（动态码率）
│   │   ├── audio_encoder.h       # 音频编码接口
│   │   └── ffmpeg_audio_encode.* # AAC（swr 重采样 s16→fltp）
│   │
│   └── muxer/                    # 封装模块
│       ├── muxer.h               # 封装接口
│       ├── ffmpeg_muxer.*        # FLV/RTMP（断线检测 + 重连 + 速率估算）
│       └── ffmpeg_recorder.*     # MP4 录制（写 moov 尾）
│
└── tools/
    └── livego/                   # 本地 RTMP 服务器（用于推流验证）
```

---

## 架构设计

### 分层架构

每个模块都采用「抽象接口 + FFmpeg 实现」两层设计，通过 `std::function` 回调串联：

```
GUI（Qt）
  └── PusherController（start/stop 生命周期管理 + 回调调度）
        │
        ├── VideoCapturer（接口）── FFmpegVideoCapturer（dshow 摄像头）
        │                        └─ FFmpegScreenCapturer（gdigrab 屏幕）
        ├── AudioCapturer（接口）── FFmpegAudioCapturer（dshow 麦克风）
        ├── VideoEncoder（接口）── FFmpegVideoEncoder（libx264）
        ├── AudioEncoder（接口）── FFmpegAudioEncoder（AAC）
        ├── Muxer（接口）───────── FFmpegMuxer（FLV/RTMP）
        └── Recorder（录制）────── FFmpegRecorder（MP4）
```

### 线程模型（6 个线程）

```
┌─ 视频采集线程（摄像头/屏幕二选一）── try_push ──▶ video_queue_（容量 8）
├─ 音频采集线程（麦克风）──────── try_push ──▶ audio_queue_（容量 64）
│
├─ 视频编码线程 ◀── pop ── 编码 ──▶ 写双 muxer（RTMP + MP4）
├─ 音频编码线程 ◀── pop ── 编码 ──▶ 写双 muxer（RTMP + MP4）
│
├─ 监控线程：断线检测 + 重连 + 弱网自适应（每 500ms 轮询）
└─ Qt 主线程：GUI 事件循环（预览渲染 + 状态刷新）
```

关键点：音视频从采集到编码**全程并行**，只在 muxer 写包时用一把锁（`write_mutex_`）汇合——因为 `av_interleaved_write_frame` 非线程安全。

### 数据流

```
摄像头(dshow) ──MJPEG解码──▶ YUV ──┐
                                   ├─▶ 转 YUV420P ──▶ video_queue_ ──▶ x264 ─┐
屏幕(gdigrab) ──BMP解码──▶ YUV ────┘                                          │
                                   └─▶ 转 RGB24 ──▶ 本地预览（跨线程投递 GUI）  │
                                                                            ▼
麦克风(dshow) ──PCM s16──▶ audio_queue_ ──▶ AAC ──────────────────▶ 写双 muxer
                                                                            │
                                                    ┌───────────────────────┤
                                                    ▼                       ▼
                                            推流 muxer（RTMP）      录制 muxer（MP4）
```

### 关键机制

- **队列解耦**：采集与编码之间用有界队列解耦，采集回调用 `try_push`（非阻塞，满了丢新帧），绝不阻塞采集线程。
- **断线检测**：`av_interleaved_write_frame` 返回负值，连续 3 次失败判定断线，回调通知。
- **断线重连**：关旧 muxer → 重新 open → 加流 → 写头，编码器不动；重连后强制 I 帧。重连在监控线程做（不在断线回调里，避免锁内重入死锁）。
- **弱网自适应**：`AVIOContext::bytes_written` 两次采样做差估算发送速率；速率低于目标码率 80% 持续 2 秒降档；降到底仍拥堵则丢 P 帧（每 2 帧丢 1 帧）；稳定 3 秒升档。
- **音画同步**：视频 pts 用采集帧真实微秒换算（而非帧序号），避免屏幕采集帧率波动时画面快进；音频以采样点序号计时，两者锚定真实时间轴。
- **跨线程 UI**：预览/日志/状态在工作线程产生，用 `Qt::QueuedConnection` 投递回主线程刷新。

---

## 构建说明

### 依赖

| 依赖            | 版本 / 路径                           | 说明                                             |
| ------------- | --------------------------------- | ---------------------------------------------- |
| FFmpeg        | 9.0.1（gyan.dev **MSVC shared** 版） | 路径 `D:\library\ffmpeg-9.0.1-full_build-shared` |
| Qt            | 6.10.3（**msvc2022_64** 版）         | 路径 `F:\QTdevelopment\6.10.3\msvc2022_64`       |
| CMake         | 3.15+                             |                                                |
| Visual Studio | 2022（MSVC v143）                   |                                                |

> ⚠️ 两个坑：
>
> 1. Qt 必须用 **msvc2022_64** 版（匹配 MSVC 编译器），MinGW 版无法链接。
> 2. FFmpeg 必须用 **MSVC 编译版**（gyan.dev），MinGW 版无法注册 dshow 设备。

> 依赖路径在 `CMakeLists.txt` 里硬编码（`FFMPEG_ROOT`、`CMAKE_PREFIX_PATH`），  
> 换机器时需改成自己的路径。

### 构建步骤（Release）

在「x64 Native Tools Command Prompt」中：

```bat
cmake --preset x64-release
cmake --build out/build/x64-release
```

产物：`out/build/x64-release/live_pusher.exe`（构建时已自动复制 FFmpeg DLL，可直接双击运行）。

> ⚠️ **必须用 Release 构建**：Debug 版依赖 `MSVCP140D.dll` 等 Debug CRT，未安装 Debug 组件时启动即崩溃。

---

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

双击 `live_pusher.exe`，选择视频源（摄像头/屏幕）、设备，地址框填入：

```
rtmp://127.0.0.1:1935/live/<channelKey>
```

如需边推流边录制，勾选「同时录制到本地」，点开始后弹框选择 MP4 保存路径。

### 4. 拉流验证

```bash
ffplay rtmp://127.0.0.1:1935/live/movie
```

> ⚠️ 注意：推流 URL 用 **channelKey**，拉流 URL 用 **room 名（movie）**，两者不同。  
> channelKey 存在 livego 内存里，**重启后变化**，需重新 curl 获取。

---

## 已完成功能

- [x] 摄像头采集（dshow，MJPEG 解码 + swscale 转 YUV420P）
- [x] 屏幕采集（gdigrab，BMP 解码，1280×800 编码）
- [x] 麦克风采集（dshow，PCM s16）
- [x] 本地实时预览（YUV→RGB + 跨线程 + 等比缩放）
- [x] 视频编码（libx264 H.264，动态码率）
- [x] 音频编码（AAC，swr 重采样 s16→fltp）
- [x] FLV 封装 + RTMP 推流
- [x] 边推流边录制 MP4（双 muxer，不重复编码）
- [x] 断线检测 + 自动重连（muxer 重建 + 强制 I 帧）
- [x] 弱网自适应（三档码率 + 发送速率监控 + 丢 P 帧策略）
- [x] 音画同步（真实时间轴 pts）
- [x] Qt 图形界面（左右分栏 + 状态栏 + 设备选择）
- [x] 可重复启停

## 待完成功能

- [ ] 公网推流（真实 RTMP 服务器）
- [ ] 硬件编码 NVENC（屏幕高分辨率软编 CPU 吃紧）
- [ ] 录制与推流独立编码（隔离弱网对录制画质的影响，OBS 做法）
- [ ] 系统声音采集（WASAPI loopback，当前只录麦克风）
- [ ] 设置持久化（QSettings 记住上次配置）
- [ ] 打包发布（一键绿色版/安装包）

---

## 注意事项

1. **FFmpeg 版本**：必须用 MSVC 编译的 FFmpeg（gyan.dev），MinGW 版无法注册 dshow 设备。
2. **音频采样格式**：AAC 编码器要求 `fltp`（平面浮点），采集器输出 `s16`（交错 16bit），需 swr 重采样。
3. **视频像素格式**：dshow 摄像头输出 `yuvj422p`，gdigrab 屏幕解码后是 `bgra`，libx264 要求 `yuv420p`，均需 swscale 转换。
4. **采集需解码**：摄像头输出 MJPEG、屏幕输出 BMP，都是压缩格式，需先解码成裸像素再转 YUV。
5. **编码冲洗**：编码完需 `send_frame(nullptr)` 冲洗编码器，输出内部缓冲帧。
6. **MP4 必须写尾**：录制停止时 `av_write_trailer` 写 moov 索引，否则文件打不开。
7. **channelKey 动态变化**：livego 重启后 key 会变（存内存），需重新 curl 获取。

---

## 面试笔记

项目中的踩坑、解法、面试概念、源码通读问答，完整记录在 [INTERVIEW\_NOTES.md](./INTERVIEW_NOTES.md)，涵盖 20+ 个典型问题（队列解耦、muxer 死锁、断线重连、弱网自适应、音画同步、Qt 跨线程等）。
