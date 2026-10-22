#include "main_window.h"

#include <cstring>

#include <QLineEdit>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWidget>
#include <QMetaObject>
#include <QImage>
#include <QPixmap>
#include <QPainter>
#include <QSplitter>
#include <QComboBox>
#include <QGroupBox>
#include <QFormLayout>
#include <QCheckBox>
#include <QFileDialog>
#include <QStandardPaths>
#include <QDateTime>

#include "common/logger.h"

// ===== PreviewWidget：等比缩放绘制最新一帧 =====
void PreviewWidget::paintEvent(QPaintEvent* event) {
    QPainter p(this);
    // 背景填深色（画面未到/比例不匹配时的留边）
    p.fillRect(rect(), QColor(0x22, 0x22, 0x22));
    if (frame_.isNull()) {
        return;
    }
    // 关键：KeepAspectRatio 等比缩放，源画面 4:3 就始终显示 4:3，不变形。
    // 缩放后不足的部分（上下或左右）留背景色边。
    QSize scaled = frame_.size().scaled(size(), Qt::KeepAspectRatio);
    QRect target((width() - scaled.width()) / 2,
                 (height() - scaled.height()) / 2,
                 scaled.width(), scaled.height());
    p.drawImage(target, frame_);
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("live_pusher 推流器"));

    // ===== 控件 =====
    url_edit_ = new QLineEdit(this);
    url_edit_->setPlaceholderText(
        QStringLiteral("输入推流地址，如 rtmp://127.0.0.1:1935/live/你的key"));
    url_edit_->setText(QStringLiteral("rtmp://127.0.0.1:1935/live/movie"));

    start_btn_ = new QPushButton(QStringLiteral("开始"), this);
    stop_btn_ = new QPushButton(QStringLiteral("停止"), this);
    stop_btn_->setEnabled(false);   // 初始不可点

    status_label_ = new QLabel(QStringLiteral("未推流"), this);
    status_label_->setStyleSheet(QStringLiteral("font-weight:bold;"));

    bitrate_label_ = new QLabel(QStringLiteral("--"), this);
    sendrate_label_ = new QLabel(QStringLiteral("--"), this);
    elapsed_label_ = new QLabel(QStringLiteral("00:00"), this);
    conn_label_ = new QLabel(QStringLiteral("--"), this);

    // 本地预览区：自绘控件，等比缩放 + 居中，窗口怎么变画面都不变形。
    preview_ = new PreviewWidget(this);
    preview_->setMinimumSize(320, 240);

    log_view_ = new QPlainTextEdit(this);
    log_view_->setReadOnly(true);
    log_view_->setMaximumBlockCount(1000);   // 最多保留 1000 行，避免内存无限涨

    // 采集设备下拉框：枚举本机 dshow 设备填充
    src_combo_ = new QComboBox(this);
    src_combo_->addItem(QStringLiteral("摄像头"));
    src_combo_->addItem(QStringLiteral("屏幕"));
    cam_combo_ = new QComboBox(this);
    mic_combo_ = new QComboBox(this);
    {
        std::vector<std::string> cams, mics;
        if (livepusher::PusherController::listCaptureDevices(cams, mics)) {
            for (const auto& c : cams)
                cam_combo_->addItem(QString::fromUtf8(c.c_str()));
            for (const auto& m : mics)
                mic_combo_->addItem(QString::fromUtf8(m.c_str()));
        }
    }

    // ===== 右侧控制面板（分组框） =====

    // 组1：推流设置 —— 地址 + 开始/停止 + 同时录制
    auto* push_group = new QGroupBox(QStringLiteral("推流设置"), this);
    auto* push_layout = new QVBoxLayout(push_group);
    push_layout->addWidget(url_edit_);
    auto* btn_row = new QHBoxLayout();
    btn_row->addWidget(start_btn_);
    btn_row->addWidget(stop_btn_);
    push_layout->addLayout(btn_row);
    // 同时录制：勾选后点击「开始」会弹框选保存位置，推流同时写一份 mp4。
    record_check_ = new QCheckBox(QStringLiteral("同时录制到本地(MP4)"), this);
    push_layout->addWidget(record_check_);

    // 组2：采集设置 —— 视频源/摄像头/麦克风下拉框（后续分辨率、码率也加这）
    auto* cap_group = new QGroupBox(QStringLiteral("采集设置"), this);
    auto* cap_form = new QFormLayout(cap_group);
    cap_form->addRow(QStringLiteral("视频源:"), src_combo_);
    cap_form->addRow(QStringLiteral("摄像头:"), cam_combo_);
    cap_form->addRow(QStringLiteral("麦克风:"), mic_combo_);

    // 组3：状态信息
    auto* stat_group = new QGroupBox(QStringLiteral("状态"), this);
    auto* stat_form = new QFormLayout(stat_group);
    stat_form->addRow(QStringLiteral("状态:"), status_label_);
    stat_form->addRow(QStringLiteral("码率:"), bitrate_label_);
    stat_form->addRow(QStringLiteral("发送速率:"), sendrate_label_);
    stat_form->addRow(QStringLiteral("时长:"), elapsed_label_);
    stat_form->addRow(QStringLiteral("连接:"), conn_label_);

    // 右侧面板：三个分组框 + 日志（竖排，往下加新分组即可扩展）
    auto* right_panel = new QWidget(this);
    auto* right_layout = new QVBoxLayout(right_panel);
    right_layout->setContentsMargins(4, 4, 4, 4);
    right_layout->addWidget(push_group);
    right_layout->addWidget(cap_group);
    right_layout->addWidget(stat_group);
    right_layout->addWidget(new QLabel(QStringLiteral("日志:"), this));
    right_layout->addWidget(log_view_, 1);

    // 左右分割器：可拖拽调整左右宽度比例
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(preview_);        // 左：预览
    splitter->addWidget(right_panel);     // 右：控制面板
    splitter->setStretchFactor(0, 3);     // 预览占 3/4 宽
    splitter->setStretchFactor(1, 1);     // 面板占 1/4 宽
    splitter->setChildrenCollapsible(false);

    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(splitter);
    setCentralWidget(central);

    resize(1100, 650);

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

    // ===== 预览帧回调：刷新预览画面 =====
    // 采集线程回调，RGB24 数据需立即拷贝成 QImage（原缓冲会复用），
    // 再通过 QueuedConnection 投递回主线程显示。
    controller_.setPreviewCallback(
        [this](const uint8_t* rgb, int width, int height) {
            // 深拷贝一帧 RGB24 数据，脱离采集线程的缓冲
            QImage img(width, height, QImage::Format_RGB888);
            memcpy(img.bits(), rgb, static_cast<size_t>(width) * height * 3);
            QMetaObject::invokeMethod(this, [this, img]() {
                updatePreview(img);
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

void MainWindow::updatePreview(const QImage& img) {
    if (img.isNull()) {
        return;
    }
    preview_->setFrame(img);
}

void MainWindow::onStartClicked() {
    QString url = url_edit_->text().trimmed();
    if (url.isEmpty()) {
        appendLog(QStringLiteral("[错误] 推流地址不能为空"));
        return;
    }

    // 根据「视频源」下拉框选择摄像头还是屏幕
    auto source = (src_combo_->currentIndex() == 1)
        ? livepusher::PusherController::VideoSource::Screen
        : livepusher::PusherController::VideoSource::Camera;

    // 同时录制：勾选后弹框选保存位置（默认桌面 + 时间戳文件名），得到 mp4 路径传给 start。
    QString record_path;
    if (record_check_->isChecked()) {
        // 默认保存到桌面，文件名带时间戳，避免覆盖。
        QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
        QString default_name = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".mp4";
        QString path = QFileDialog::getSaveFileName(
            this,
            QStringLiteral("选择录制文件保存位置"),
            desktop + "/" + default_name,
            QStringLiteral("MP4 视频 (*.mp4)"));
        if (path.isEmpty()) {
            // 用户取消了选择，视为不录制。
            appendLog(QStringLiteral("[提示] 已取消录制，仅推流"));
        } else {
            // 强制补 .mp4 扩展名
            if (!path.endsWith(QStringLiteral(".mp4"), Qt::CaseInsensitive)) {
                path += QStringLiteral(".mp4");
            }
            record_path = path;
        }
    }

    start_btn_->setEnabled(false);
    stop_btn_->setEnabled(true);
    // 推流过程中不允许变更录制开关（录制和推流生命周期绑定）。
    record_check_->setEnabled(false);

    bool ok = controller_.start(url.toStdString(), source, record_path.toStdString());
    if (!ok) {
        // 启动失败，恢复按钮和录制开关
        start_btn_->setEnabled(true);
        stop_btn_->setEnabled(false);
        record_check_->setEnabled(true);
    } else if (!record_path.isEmpty()) {
        appendLog(QStringLiteral("正在录制 -> %1").arg(record_path));
    }
}

void MainWindow::onStopClicked() {
    controller_.stop();
    start_btn_->setEnabled(true);
    stop_btn_->setEnabled(false);
    // 恢复录制开关
    record_check_->setEnabled(true);
}
