#pragma once

#include <QMainWindow>
#include <memory>

#include "pusher_controller.h"

class QLineEdit;
class QPushButton;
class QPlainTextEdit;
class QLabel;

// 推流器主窗口：地址输入框 + 开始/停止按钮 + 日志区。
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

    QLineEdit* url_edit_ = nullptr;
    QPushButton* start_btn_ = nullptr;
    QPushButton* stop_btn_ = nullptr;
    QPlainTextEdit* log_view_ = nullptr;
    QLabel* status_label_ = nullptr;
    QLabel* bitrate_label_ = nullptr;   // 码率档位 + 目标码率
    QLabel* sendrate_label_ = nullptr;  // 发送速率
    QLabel* elapsed_label_ = nullptr;   // 推流时长
    QLabel* conn_label_ = nullptr;      // 连接状态

    livepusher::PusherController controller_;
};
