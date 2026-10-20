#include "main_window.h"

#include <QLineEdit>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWidget>
#include <QMetaObject>

#include "common/logger.h"

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("live_pusher 推流器"));

    // ===== 控件 =====
    url_edit_ = new QLineEdit(this);
    url_edit_->setPlaceholderText(
        QStringLiteral("输入推流地址，如 rtmp://127.0.0.1:1935/live/你的key"));
    url_edit_->setText(QStringLiteral("rtmp://127.0.0.1:1935/live/movie"));

    start_btn_ = new QPushButton(QStringLiteral("开始推流"), this);
    stop_btn_ = new QPushButton(QStringLiteral("停止推流"), this);
    stop_btn_->setEnabled(false);   // 初始不可点

    status_label_ = new QLabel(QStringLiteral("未推流"), this);
    status_label_->setStyleSheet(QStringLiteral("font-weight:bold;"));

    bitrate_label_ = new QLabel(QStringLiteral("码率: --"), this);
    sendrate_label_ = new QLabel(QStringLiteral("发送速率: --"), this);
    elapsed_label_ = new QLabel(QStringLiteral("时长: 00:00"), this);
    conn_label_ = new QLabel(QStringLiteral("连接: --"), this);

    log_view_ = new QPlainTextEdit(this);
    log_view_->setReadOnly(true);
    log_view_->setMaximumBlockCount(1000);   // 最多保留 1000 行，避免内存无限涨

    // ===== 布局 =====
    auto* url_row = new QHBoxLayout();
    url_row->addWidget(url_edit_, 1);
    url_row->addWidget(start_btn_);
    url_row->addWidget(stop_btn_);

    // 状态栏：一行放多个实时指标
    auto* stats_row = new QHBoxLayout();
    stats_row->addWidget(status_label_);
    stats_row->addStretch(1);
    stats_row->addWidget(bitrate_label_);
    stats_row->addWidget(sendrate_label_);
    stats_row->addWidget(elapsed_label_);
    stats_row->addWidget(conn_label_);

    auto* root = new QVBoxLayout();
    root->addLayout(url_row);
    root->addLayout(stats_row);
    root->addWidget(log_view_, 1);

    auto* central = new QWidget(this);
    central->setLayout(root);
    setCentralWidget(central);

    resize(720, 480);

    // ===== 信号槽 =====
    connect(start_btn_, &QPushButton::clicked, this, &MainWindow::onStartClicked);
    connect(stop_btn_, &QPushButton::clicked, this, &MainWindow::onStopClicked);

    // ===== 日志 sink：把 LOG_XXX 重定向到界面 =====
    // 注意：Logger 的 sink 回调会在编码/采集等【工作线程】里被调用，
    // Qt 控件只能在【主线程】操作。所以这里用 Qt::QueuedConnection 的
    // lambda 信号槽（通过 QObject::connect + context this），Qt 会自动把
    // 调用投递回主线程，保证线程安全。
    livepusher::Logger::instance().set_sink(
        [this](const std::string& line) {
            // 跨线程投递到主线程
            QMetaObject::invokeMethod(this, [this, line]() {
                appendLog(QString::fromUtf8(line.c_str()));
                }, Qt::QueuedConnection);
        });

    // ===== 状态回调：更新状态标签 =====
    controller_.setStatusCallback([this](const std::string& s) {
        QMetaObject::invokeMethod(this, [this, s]() {
            status_label_->setText(QString::fromUtf8(s.c_str()));
            }, Qt::QueuedConnection);
        });

    // ===== 实时统计回调：刷新状态栏 =====
    controller_.setStatsCallback([this](const livepusher::PusherController::Stats& s) {
        QMetaObject::invokeMethod(this, [this, s]() {
            updateStats(s);
            }, Qt::QueuedConnection);
        });

    appendLog(QStringLiteral("就绪。输入地址后点击「开始推流」。"));
}

MainWindow::~MainWindow() {
    // 停止推流 + 解除日志回调，避免析构后仍有线程往 this 投递。
    controller_.stop();
    livepusher::Logger::instance().set_sink(nullptr);
}

void MainWindow::appendLog(const QString& line) {
    log_view_->appendPlainText(line);
}

void MainWindow::updateStats(const livepusher::PusherController::Stats& s) {
    // 码率档位名
    const char* level_name = "高";
    if (s.bitrate_level == 1) level_name = "中";
    else if (s.bitrate_level >= 2) level_name = "低";
    bitrate_label_->setText(
        QStringLiteral("码率:%1(%2kbps)")
            .arg(level_name)
            .arg(s.target_bitrate / 1000));

    // 发送速率
    if (s.send_rate > 0) {
        sendrate_label_->setText(
            QStringLiteral("发送速率:%1kbps").arg(s.send_rate / 1000));
    } else {
        sendrate_label_->setText(QStringLiteral("发送速率:--"));
    }

    // 时长 mm:ss
    int total_sec = static_cast<int>(s.elapsed_ms / 1000);
    elapsed_label_->setText(
        QStringLiteral("时长:%1:%2")
            .arg(total_sec / 60, 2, 10, QLatin1Char('0'))
            .arg(total_sec % 60, 2, 10, QLatin1Char('0')));

    // 连接状态
    if (s.disconnected) {
        conn_label_->setText(QStringLiteral("连接:断线"));
        conn_label_->setStyleSheet(QStringLiteral("color:red;"));
    } else {
        conn_label_->setText(QStringLiteral("连接:正常"));
        conn_label_->setStyleSheet(QStringLiteral("color:green;"));
    }
}

void MainWindow::onStartClicked() {
    QString url = url_edit_->text().trimmed();
    if (url.isEmpty()) {
        appendLog(QStringLiteral("[错误] 推流地址不能为空"));
        return;
    }

    start_btn_->setEnabled(false);
    stop_btn_->setEnabled(true);

    bool ok = controller_.start(url.toStdString());
    if (!ok) {
        // 启动失败，恢复按钮
        start_btn_->setEnabled(true);
        stop_btn_->setEnabled(false);
    }
}

void MainWindow::onStopClicked() {
    controller_.stop();
    start_btn_->setEnabled(true);
    stop_btn_->setEnabled(false);
}
