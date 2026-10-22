#pragma once

#include <QMainWindow>
#include <QWidget>
#include <QImage>
#include <QPaintEvent>
#include <memory>

#include "pusher_controller.h"

class QLineEdit;
class QPushButton;
class QPlainTextEdit;
class QLabel;
class QComboBox;
class QGroupBox;
class QCheckBox;

// 本地预览控件：在 paintEvent 里把最新一帧「等比缩放 + 居中」绘制。
// 相比 QLabel::setScaledContents(true)（强行拉伸、比例失真），
// 自绘可以严格保持源画面的宽高比，窗口任意缩放都不变形。
class PreviewWidget : public QWidget {
    Q_OBJECT
public:
    explicit PreviewWidget(QWidget* parent = nullptr) : QWidget(parent) {}

    // 供主线程调用：更新最新一帧并触发重绘。
    void setFrame(const QImage& img) {
        frame_ = img;
        update();   // 触发 paintEvent（异步，事件循环里执行）
    }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QImage frame_;   // 最新一帧（RGB888）
};

// 推流器主窗口：地址输入框 + 开始/停止按钮 + 预览区 + 状态栏 + 日志区。
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    // 点击「开始推流」
    void onStartClicked();
    // 点击「停止推流」
    void onStopClicked();

private:
    // 追加一行日志到界面（线程安全，通过信号槽跨线程）。
    void appendLog(const QString& line);
    // 刷新状态栏（由 stats 回调跨线程触发）。
    void updateStats(const livepusher::PusherController::Stats& s);
    // 刷新预览画面（由 preview 回调跨线程触发，参数是 RGB24 数据拷贝）。
    void updatePreview(const QImage& img);

    QLineEdit* url_edit_ = nullptr;
    QPushButton* start_btn_ = nullptr;
    QPushButton* stop_btn_ = nullptr;
    QPlainTextEdit* log_view_ = nullptr;
    QLabel* status_label_ = nullptr;
    QLabel* bitrate_label_ = nullptr;   // 码率档位 + 目标码率
    QLabel* sendrate_label_ = nullptr;  // 发送速率
    QLabel* elapsed_label_ = nullptr;   // 推流时长
    QLabel* conn_label_ = nullptr;      // 连接状态
    PreviewWidget* preview_ = nullptr;  // 本地预览画面（自绘、等比缩放）
    QComboBox* cam_combo_ = nullptr;    // 摄像头选择
    QComboBox* mic_combo_ = nullptr;    // 麦克风选择
    QComboBox* src_combo_ = nullptr;    // 视频源（摄像头/屏幕）
    QCheckBox* record_check_ = nullptr; // 同时录制到本地 mp4

    livepusher::PusherController controller_;
};
