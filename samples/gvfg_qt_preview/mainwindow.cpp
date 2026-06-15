#include "mainwindow.h"
#include "previewwindow.h"
#include "ui_mainwindow.h"

#include <QCloseEvent>
#include <QDateTime>
#include <QMetaObject>
#include <QTimer>

namespace
{
    QString hex32(uint32_t value)
    {
        return QStringLiteral("0x") + QString::number(value, 16).rightJustified(8, QLatin1Char('0')).toUpper();
    }

    QString eventTypeText(gvfg_event_type_t type)
    {
        switch (type)
        {
        case GVFG_EVENT_VIDEO_IRQ:
            return QStringLiteral("VIDEO_IRQ");
        case GVFG_EVENT_PLUG_IN:
            return QStringLiteral("PLUG_IN");
        case GVFG_EVENT_PLUG_OUT:
            return QStringLiteral("PLUG_OUT");
        case GVFG_EVENT_CAPTURE_PAUSED:
            return QStringLiteral("CAPTURE_PAUSED");
        case GVFG_EVENT_CAPTURE_RESUMED:
            return QStringLiteral("CAPTURE_RESUMED");
        default:
            return QStringLiteral("UNKNOWN");
        }
    }
}

MainWindow::MainWindow(QWidget *parent)
    : QWidget(parent), ui_(new Ui::MainWindow)
{
    ui_->setupUi(this);

    previewWindow_ = new PreviewWindow();
    ui_->logEdit->setMaximumBlockCount(300);
    ui_->statusLabel->setWordWrap(true);
    signalStatusTimer_ = new QTimer(this);
    signalStatusTimer_->setInterval(1000);

    connect(ui_->refreshButton, &QPushButton::clicked, this, [this]() { refreshDevices(); });
    connect(ui_->openButton, &QPushButton::clicked, this, [this]()
            {
                if (handle_)
                    closeDevice();
                else
                    openDevice();
            });
    connect(ui_->showPreviewButton, &QPushButton::clicked, this, [this]() { showPreviewWindow(); });
    connect(ui_->fullscreenPreviewButton, &QPushButton::clicked, this, [this]() { showFullscreenPreviewWindow(); });
    connect(ui_->startButton, &QPushButton::clicked, this, [this]() { startCapture(); });
    connect(ui_->stopButton, &QPushButton::clicked, this, [this]() { stopCapture(); });
    connect(signalStatusTimer_, &QTimer::timeout, this, [this]() { updateSignalStatus(true); });

    updateUiState();
    refreshDevices();
}

MainWindow::~MainWindow()
{
    closeDevice();
    delete previewWindow_;
    delete ui_;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    closeDevice();
    if (previewWindow_)
        previewWindow_->closePreview();

    QWidget::closeEvent(event);
}

void MainWindow::refreshDevices()
{
    if (handle_)
        closeDevice();

    ui_->deviceCombo->clear();
    devices_ = {};

    const int count = gvfg_enumerate_devices(devices_.data(), GVFG_MAX_DEVICES);
    deviceCount_ = count > 0 ? count : 0;

    for (int i = 0; i < deviceCount_; ++i)
    {
        const QString name = QString::fromUtf8(devices_[i].name);
        ui_->deviceCombo->addItem(name.isEmpty() ? QStringLiteral("GVFG Capture") : name, devices_[i].index);
    }

    if (deviceCount_ <= 0)
    {
        ui_->deviceCombo->addItem(QStringLiteral("No GVFG device found"), -1);
        appendLog(QStringLiteral("No GVFG device found"));
    }
    else
    {
        appendLog(QStringLiteral("Found %1 XDMA device(s)").arg(deviceCount_));
    }
}

void MainWindow::showPreviewWindow()
{
    updatePreviewSourceSize();
    previewWindow_->showPreview();

    if (handle_ && !applyPreview())
        appendLog(QStringLiteral("Show Preview failed: unable to update preview window"));
}

void MainWindow::showFullscreenPreviewWindow()
{
    updatePreviewSourceSize();
    previewWindow_->showFullscreenPreview();

    if (handle_ && !applyPreview())
        appendLog(QStringLiteral("Fullscreen Preview failed: unable to update preview window"));
}

bool MainWindow::openDevice()
{
    if (handle_)
        return true;

    if (ui_->deviceCombo->currentData().toInt() < 0)
    {
        appendLog(QStringLiteral("Open skipped: no device selected"));
        return false;
    }

    gvfg_status_t st = gvfg_create(&handle_);
    if (st != GVFG_OK || !handle_)
    {
        showError(QStringLiteral("gvfg_create"), st);
        handle_ = nullptr;
        updateUiState();
        return false;
    }

    const int deviceIndex = ui_->deviceCombo->currentData().toInt();
    st = gvfg_open(handle_, deviceIndex);
    if (st != GVFG_OK)
    {
        showError(QStringLiteral("gvfg_open"), st);
        closeDevice();
        return false;
    }

    lastSignalStatusText_.clear();
    appendLog(QStringLiteral("Opened device index %1").arg(deviceIndex));
    appendLog(QStringLiteral("FPGA signal monitor active"));
    updateSignalStatus(true);
    signalStatusTimer_->start();
    updateUiState();
    return true;
}

void MainWindow::closeDevice()
{
    stopCapture();
    previewRenderer_.shutdown();

    if (signalStatusTimer_)
        signalStatusTimer_->stop();

    if (handle_)
    {
        gvfg_destroy(handle_);
        handle_ = nullptr;
        appendLog(QStringLiteral("Closed device"));
    }

    ui_->statusLabel->setText(QStringLiteral("Idle"));
    lastSignalStatusText_.clear();
    updateUiState();
}

void MainWindow::startCapture()
{
    if (captureRunning_)
        return;

    if (!handle_ && !openDevice())
        return;

    updatePreviewSourceSize();
    previewWindow_->showPreview();
    if (!applyPreview())
        return;

    appendLog(QStringLiteral("FPGA signal before stream start"));
    updateSignalStatus(true);

    const gvfg_status_t st = gvfg_start(handle_);
    if (st != GVFG_OK)
    {
        showError(QStringLiteral("gvfg_start"), st);
        updateSignalStatus(true);
        updateUiState();
        return;
    }

    frameCount_ = 0;
    captureStop_.store(false, std::memory_order_release);
    captureRunning_ = true;
    captureThread_ = std::thread([this]()
                                 { captureReadLoop(); });
    updateUiState();
    appendLog(QStringLiteral("Started capture"));
    updateSignalStatus(true);
}

void MainWindow::stopCapture()
{
    if (handle_ && captureRunning_)
    {
        captureStop_.store(true, std::memory_order_release);
        joinCaptureThread();
        gvfg_stop(handle_);
        captureRunning_ = false;
        appendLog(QStringLiteral("Stopped capture"));
        updateSignalStatus(true);
    }

    updateUiState();
}

bool MainWindow::applyPreview()
{
    if (!previewRenderer_.configure(previewWindow_->nativePreviewHandle()))
    {
        appendLog(QStringLiteral("Preview setup failed: invalid preview window"));
        return false;
    }
    return true;
}

void MainWindow::updatePreviewSourceSize(const gvfg_runtime_info_t &info)
{
    const auto &signal = info.input_signal;
    const auto &readFrame = info.callback_frame;

    if (signal.width > 0 && signal.height > 0)
    {
        previewWindow_->setSourceSize(signal.width, signal.height);
        return;
    }

    if (readFrame.valid && readFrame.width > 0 && readFrame.height > 0)
        previewWindow_->setSourceSize(readFrame.width, readFrame.height);
}

void MainWindow::updatePreviewSourceSize()
{
    if (!handle_)
        return;

    gvfg_runtime_info_t info{};
    if (gvfg_get_runtime_info(handle_, &info) == GVFG_OK)
        updatePreviewSourceSize(info);
}

void MainWindow::updateSignalStatus(bool writeLog)
{
    if (!handle_)
        return;

    gvfg_runtime_info_t info{};
    if (gvfg_get_runtime_info(handle_, &info) != GVFG_OK)
        return;

    const auto &signal = info.input_signal;
    const auto &readFrame = info.callback_frame;
    if (previewWindow_->isVisible())
        updatePreviewSourceSize(info);

    const QString fpgaResolution = (signal.width > 0 && signal.height > 0)
                                       ? QStringLiteral("%1x%2").arg(signal.width).arg(signal.height)
                                       : QStringLiteral("--");
    const QString frameRateText = signal.frame_rate_code >= 0
                                      ? QStringLiteral("%1 (%2)")
                                            .arg(QString::fromLatin1(signal.frame_rate_bits))
                                            .arg(QString::fromLatin1(signal.frame_rate_name))
                                      : QStringLiteral("--");
    const QString formatText = signal.video_format_code >= 0
                                   ? QStringLiteral("%1 (%2)")
                                         .arg(signal.video_format_code)
                                         .arg(QString::fromLatin1(signal.video_format))
                                   : QStringLiteral("--");
    const QString bitDepthText = signal.bit_depth > 0 ? QString::number(signal.bit_depth) : QStringLiteral("--");
    const QString line0 = QStringLiteral("FPGA reported | signal=%1 fps=%2 format=%3 bitdepth=%4")
                              .arg(fpgaResolution)
                              .arg(frameRateText)
                              .arg(formatText)
                              .arg(bitDepthText);
    const QString line2 = QStringLiteral("FPGA status | SDI lock=%1 SDI DDR=%2 HDMI lock=%3 HDMI DDR=%4")
                              .arg(signal.sdi_locked)
                              .arg(signal.sdi_ddr_ok)
                              .arg(signal.hdmi_locked)
                              .arg(signal.hdmi_ddr_ok);
    const QString lineRaw = QStringLiteral("FPGA raw | size=%1x%2 fmt=%3 fps=%4 bit=%5 status=%6")
                                .arg(signal.raw.width)
                                .arg(signal.raw.height)
                                .arg(hex32(signal.raw.video_format))
                                .arg(hex32(signal.raw.frame_rate))
                                .arg(signal.raw.bit_depth)
                                .arg(hex32(signal.raw.status));
    const QString line3 = previewRenderer_.active()
                              ? QStringLiteral("Preview output | frame=%1x%2 format=%3 bitdepth=%4")
                                    .arg(previewRenderer_.width())
                                    .arg(previewRenderer_.height())
                                    .arg(QString::fromUtf8(previewRenderer_.pixelFormat()))
                                    .arg(previewRenderer_.bitDepth())
                              : QStringLiteral("Preview output | app renderer inactive");
    const QString line4 = readFrame.valid
                              ? QStringLiteral("Read frame | frame=%1x%2 format=%3 bitdepth=%4")
                                    .arg(readFrame.width)
                                    .arg(readFrame.height)
                                    .arg(QString::fromUtf8(readFrame.pixel_format))
                                    .arg(readFrame.bit_depth)
                              : QStringLiteral("Read frame | --");
    const QString line5 = QStringLiteral("App runtime | capture=%1 fps delivered=%2")
                              .arg(info.capture_fps > 0.0 ? QString::number(info.capture_fps, 'f', 2)
                                                          : QStringLiteral("--"))
                              .arg(static_cast<qulonglong>(info.delivered_frames));

    const QString statusText = line0 + QLatin1Char('\n') + line2 + QLatin1Char('\n') + lineRaw + QLatin1Char('\n') + line3 + QLatin1Char('\n') + line4 + QLatin1Char('\n') + line5;
    const bool changed = lastSignalStatusText_ != statusText;
    if (changed)
    {
        ui_->statusLabel->setText(statusText);
        lastSignalStatusText_ = statusText;
    }

    if (writeLog && changed)
        appendLog(statusText);
}

void MainWindow::updateUiState()
{
    const bool deviceOpen = handle_ != nullptr;
    ui_->openButton->setText(deviceOpen ? QStringLiteral("Close Device") : QStringLiteral("Open Device"));
    ui_->openButton->setEnabled(!captureRunning_);
    ui_->startButton->setEnabled(deviceOpen && !captureRunning_);
    ui_->stopButton->setEnabled(captureRunning_);
    ui_->fullscreenPreviewButton->setEnabled(true);
    ui_->refreshButton->setEnabled(!deviceOpen && !captureRunning_);
    ui_->deviceCombo->setEnabled(!deviceOpen && !captureRunning_);
}

void MainWindow::showError(const QString &apiName, gvfg_status_t status)
{
    appendLog(QStringLiteral("%1 failed: %2").arg(apiName, QString::fromUtf8(gvfg_strerror(status))));
}

void MainWindow::appendLog(const QString &message)
{
    const QString line = QStringLiteral("[%1] %2")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")))
                             .arg(message);
    ui_->logEdit->appendPlainText(line);
}

void MainWindow::captureReadLoop()
{
    while (!captureStop_.load(std::memory_order_acquire))
    {
        gvfg_event_t event{};
        while (gvfg_poll_event(handle_, &event, 0) == GVFG_OK)
        {
            const QString type = eventTypeText(event.type);
            const uint32_t irqBit = event.irq_bit;
            const uint32_t irqMask = event.irq_mask;
            const uint64_t timestampNs = event.timestamp_ns;
            QMetaObject::invokeMethod(this,
                                      [this, type, irqBit, irqMask, timestampNs]()
                                      {
                                          appendLog(QStringLiteral("event %1 irq=%2 mask=%3 ts=%4")
                                                        .arg(type)
                                                        .arg(irqBit)
                                                        .arg(hex32(irqMask))
                                                        .arg(static_cast<qulonglong>(timestampNs)));
                                      },
                                      Qt::QueuedConnection);
        }

        gvfg_frame_t frame{};
        const gvfg_status_t st = gvfg_read_frame(handle_, &frame, 200);
        if (st == GVFG_OK)
        {
            const uint64_t count = ++frameCount_;
            const int width = frame.width;
            const int height = frame.height;
            const uint64_t frameId = frame.frame_id;
            previewRenderer_.render(frame);
            gvfg_release_frame(handle_, &frame);

            if (count <= 5 || (count % 60) == 0)
            {
                QMetaObject::invokeMethod(this,
                                          [this, count, frameId, width, height]()
                                          {
                                              appendLog(QStringLiteral("read_frame #%1 source id=%2 %3x%4")
                                                            .arg(static_cast<qulonglong>(count))
                                                            .arg(static_cast<qulonglong>(frameId))
                                                            .arg(width)
                                                            .arg(height));
                                          },
                                          Qt::QueuedConnection);
            }
            continue;
        }

        if (st == GVFG_ETIMEOUT)
            continue;
        if (captureStop_.load(std::memory_order_acquire) || st == GVFG_ESTATE)
            break;

        QMetaObject::invokeMethod(this,
                                  [this, st]()
                                  {
                                      appendLog(QStringLiteral("gvfg_read_frame failed: %1")
                                                    .arg(QString::fromUtf8(gvfg_strerror(st))));
                                  },
                                  Qt::QueuedConnection);
        break;
    }
}

void MainWindow::joinCaptureThread()
{
    if (captureThread_.joinable())
        captureThread_.join();
}
