#include <iostream>
#include <string>

#include <QApplication>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "common/logger.h"
#include "main_window.h"

using namespace livepusher;

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavdevice/avdevice.h>
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    // ===== FFmpeg 环境自检 =====
    std::cout << "====== FFmpeg Environment Check ======" << std::endl;
    std::cout << "avformat version: " << avformat_version() << std::endl;
    std::cout << "avcodec  version: " << avcodec_version() << std::endl;
    std::cout << "avutil   version: " << avutil_version() << std::endl;

    const AVCodec* h264_encoder = avcodec_find_encoder_by_name("libx264");
    if (h264_encoder) {
        std::cout << "[OK] libx264 encoder: " << h264_encoder->long_name << std::endl;
    }
    const AVCodec* aac_encoder = avcodec_find_encoder_by_name("aac");
    if (aac_encoder) {
        std::cout << "[OK] aac encoder: " << aac_encoder->long_name << std::endl;
    }

    // 注册 libavdevice 的所有输入/输出设备 demuxer/muxer
    avdevice_register_all();
    std::cout << "[OK] avdevice registered, version: "
              << avdevice_version() << std::endl;

    // ===== Qt 应用启动 =====
    QApplication app(argc, argv);

    MainWindow window;
    window.show();

#ifdef _WIN32
    // 确保窗口出现在最前面：从文件管理器双击启动时，
    // 有时窗口会留在后台不抢焦点，用户以为"没弹出来"。
    window.raise();
    window.activateWindow();
    SetForegroundWindow(reinterpret_cast<HWND>(window.winId()));
#endif

    return app.exec();
}
