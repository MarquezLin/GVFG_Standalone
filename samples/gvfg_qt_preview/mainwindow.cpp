#include "mainwindow.h"
#include "previewwindow.h"
#include "ui_mainwindow.h"

#include <gvfg_debug.h>

#include <QCloseEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QIODevice>
#include <QMetaObject>
#include <QStringList>
#include <QTimer>

namespace
{
    QString hex32(uint32_t value)
    {
        return QStringLiteral("0x") + QString::number(value, 16).rightJustified(8, QLatin1Char('0')).toUpper();
    }

    QString boolText(int value)
    {
        return value ? QStringLiteral("yes") : QStringLiteral("no");
    }

    QString valueOrDash(uint64_t value)
    {
        return QString::number(static_cast<qulonglong>(value));
    }

    QString captureStatusText(const gvfg_debug_backend_stats_t &stats)
    {
        if (!stats.backend_running)
            return QStringLiteral("stopped");
        if (stats.backend_data_worker_stop)
            return QStringLiteral("stopping");
        if (!stats.backend_capture_active)
            return QStringLiteral("paused");
        return QStringLiteral("streaming");
    }

    QString workerText(const gvfg_debug_backend_stats_t &stats)
    {
        return stats.backend_data_worker_stop ? QStringLiteral("stopping") : QStringLiteral("running");
    }

    QString appHoldingText(uint64_t value)
    {
        return value == UINT64_MAX ? QStringLiteral("none") : QStringLiteral("slot %1").arg(valueOrDash(value));
    }

    QString frameText(bool valid, int width, int height, const char *pixelFormat, int bitDepth)
    {
        if (!valid || width <= 0 || height <= 0)
            return QStringLiteral("--");

        const QString format = pixelFormat && pixelFormat[0] != '\0'
                                   ? QString::fromUtf8(pixelFormat)
                                   : QStringLiteral("--");
        const QString bit = bitDepth > 0 ? QString::number(bitDepth) : QStringLiteral("--");
        return QStringLiteral("%1x%2 %3 %4-bit").arg(width).arg(height).arg(format, bit);
    }

    QString signalFrameText(const gvfg_signal_status_t &signal)
    {
        const QString resolution = (signal.width > 0 && signal.height > 0)
                                       ? QStringLiteral("%1x%2").arg(signal.width).arg(signal.height)
                                       : QStringLiteral("--");
        const QString fps = signal.frame_rate_name[0] != '\0'
                                ? QString::fromLatin1(signal.frame_rate_name)
                                : QStringLiteral("--");
        const QString format = signal.video_format[0] != '\0'
                                   ? QString::fromLatin1(signal.video_format)
                                   : QStringLiteral("--");
        const QString bit = signal.bit_depth > 0 ? QString::number(signal.bit_depth) : QStringLiteral("--");
        return QStringLiteral("%1 %2 %3 %4-bit").arg(resolution, fps, format, bit);
    }

    QString backendLastError(gvfg_handle handle)
    {
        char message[512] = {};
        if (gvfg_debug_get_last_error_detail(handle, message, sizeof(message)) != GVFG_OK || message[0] == '\0')
            return {};
        return QString::fromUtf8(message);
    }

    QString eventTypeText(gvfg_event_type_t type)
    {
        switch (type)
        {
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
    openLogFile();

    connect(ui_->refreshButton, &QPushButton::clicked, this, [this]()
            { refreshDevices(); });
    connect(ui_->openButton, &QPushButton::clicked, this, [this]()
            {
                if (handle_)
                    closeDevice();
                else
                    openDevice(); });
    connect(ui_->showPreviewButton, &QPushButton::clicked, this, [this]()
            { showPreviewWindow(); });
    connect(ui_->fullscreenPreviewButton, &QPushButton::clicked, this, [this]()
            { showFullscreenPreviewWindow(); });
    connect(ui_->startButton, &QPushButton::clicked, this, [this]()
            { startCapture(); });
    connect(ui_->stopButton, &QPushButton::clicked, this, [this]()
            { stopCapture(); });
    connect(signalStatusTimer_, &QTimer::timeout, this, [this]()
            { updateSignalStatus(true); });

    updateUiState();
    appendLog(logFile_.isOpen()
                  ? QStringLiteral("Log file | %1").arg(logFilePath_)
                  : QStringLiteral("Log file unavailable | %1").arg(logFilePath_));
    refreshDevices();
}

MainWindow::~MainWindow()
{
    closeDevice();
    delete previewWindow_;
    delete ui_;
}

QWidget *createMainWindow()
{
    return new MainWindow();
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
        appendLog(QStringLiteral("Found %1 PCIES2MM device(s)").arg(deviceCount_));
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
    if (previewHandle_)
    {
        gvfg_preview_destroy(previewHandle_);
        previewHandle_ = nullptr;
    }

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
    previewFailureCount_ = 0;
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
    if (!previewHandle_)
    {
        const gvfg_preview_status_t st = gvfg_preview_create(&previewHandle_);
        if (st != GVFG_PREVIEW_OK || !previewHandle_)
        {
            appendLog(QStringLiteral("Preview setup failed: %1")
                          .arg(QString::fromUtf8(gvfg_preview_strerror(st))));
            previewHandle_ = nullptr;
            return false;
        }
    }

    const gvfg_preview_status_t st = gvfg_preview_attach_window(previewHandle_, previewWindow_->nativePreviewHandle());
    if (st != GVFG_PREVIEW_OK)
    {
        appendLog(QStringLiteral("Preview setup failed: %1")
                      .arg(QString::fromUtf8(gvfg_preview_strerror(st))));
        return false;
    }
    return true;
}

void MainWindow::updatePreviewSourceSize(const gvfg_runtime_info_t &info)
{
    const auto &signal = info.input_signal;
    const auto &readFrame = info.last_frame;

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
    const auto &readFrame = info.last_frame;
    gvfg_debug_fpga_signal_raw_t fpgaRaw{};
    const bool haveRaw = gvfg_debug_get_fpga_signal_raw(handle_, &fpgaRaw) == GVFG_OK;
    gvfg_debug_backend_stats_t backendStats{};
    backendStats.struct_size = sizeof(backendStats);
    const bool haveBackendStats = gvfg_debug_get_backend_stats(handle_, &backendStats) == GVFG_OK;
    if (previewWindow_->isVisible())
        updatePreviewSourceSize(info);

    gvfg_preview_info_t previewInfo{};
    const bool previewInfoOk = previewHandle_ &&
                               gvfg_preview_get_info(previewHandle_, &previewInfo) == GVFG_PREVIEW_OK &&
                               previewInfo.active;
    const QString previewFrame = previewInfoOk
                                     ? frameText(true,
                                                 previewInfo.width,
                                                 previewInfo.height,
                                                 previewInfo.pixel_format,
                                                 previewInfo.bit_depth)
                                     : QStringLiteral("--");
    const QString lastFrame = frameText(readFrame.valid != 0,
                                        readFrame.width,
                                        readFrame.height,
                                        readFrame.pixel_format,
                                        readFrame.bit_depth);

    QStringList statusLines;
    statusLines << QStringLiteral("Input   | SDI lock=%1 HDMI lock=%2 signal=%3")
                       .arg(boolText(signal.sdi_locked), boolText(signal.hdmi_locked), signalFrameText(signal));
    statusLines << (haveRaw
                        ? QStringLiteral("FPGA Raw| valid=%1 size_reg=%2x%3 format=%4 fps=%5 bit_depth=%6 status=%7")
                              .arg(hex32(fpgaRaw.valid_mask))
                              .arg(fpgaRaw.width_raw)
                              .arg(fpgaRaw.height_raw)
                              .arg(hex32(fpgaRaw.video_format_raw))
                              .arg(hex32(fpgaRaw.frame_rate_raw))
                              .arg(fpgaRaw.bit_depth_raw)
                              .arg(hex32(fpgaRaw.status_raw))
                        : QStringLiteral("FPGA Raw| unavailable"));
    statusLines << QStringLiteral("App     | reader_fps=%1 last_frame=%2 preview_active=%3 preview_output=%4")
                       .arg(info.capture_fps > 0.0 ? QString::number(info.capture_fps, 'f', 2) : QStringLiteral("--"))
                       .arg(lastFrame)
                       .arg(boolText(previewInfoOk ? 1 : 0))
                       .arg(previewFrame);
    statusLines << (haveBackendStats
                        ? QStringLiteral("Capture| status=%1 worker=%2 pending_irqs=%3 dma_errors=%4 no_frame_waits=%5")
                              .arg(captureStatusText(backendStats))
                              .arg(workerText(backendStats))
                              .arg(backendStats.backend_pending_events)
                              .arg(static_cast<qulonglong>(backendStats.backend_dma_errors))
                              .arg(static_cast<qulonglong>(backendStats.backend_wait_timeouts))
                        : QStringLiteral("Capture| unavailable"));
    if (haveBackendStats)
    {
        statusLines << QStringLiteral("Flow    | irqs=%1 captured=%2 app_read=%3 dropped=%4")
                           .arg(static_cast<qulonglong>(backendStats.backend_interrupt_count))
                           .arg(valueOrDash(backendStats.backend_frames_captured))
                           .arg(valueOrDash(backendStats.backend_frames_delivered))
                           .arg(valueOrDash(backendStats.backend_frames_dropped));
        statusLines << QStringLiteral("Buffer  | app_holding=%1 slots=%2 next_write=%3 frame_id newest/read=%4/%5")
                           .arg(appHoldingText(backendStats.backend_active_delivery_slot),
                                valueOrDash(backendStats.backend_ring_size),
                                valueOrDash(backendStats.backend_next_write_slot),
                                valueOrDash(backendStats.backend_latest_sequence),
                                valueOrDash(backendStats.backend_delivered_sequence));
    }
    const QString lastError = backendLastError(handle_);
    if (!lastError.isEmpty())
        statusLines << QStringLiteral("Error   | %1").arg(lastError);

    const QString statusText = statusLines.join(QLatin1Char('\n'));
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

void MainWindow::openLogFile()
{
    logSessionStamp_ = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    logPartIndex_ = 1;
    logDirPath_ = QCoreApplication::applicationDirPath() + QStringLiteral("/logs");
    if (!QDir().mkpath(logDirPath_))
    {
        logFilePath_ = logDirPath_;
        return;
    }

    openLogFilePart();
}

bool MainWindow::openLogFilePart()
{
    std::lock_guard<std::mutex> lock(logFileMutex_);
    if (logFile_.isOpen())
        logFile_.close();

    logFilePath_ = QStringLiteral("%1/gvfg_qt_preview_%2_part%3.log")
                       .arg(logDirPath_, logSessionStamp_)
                       .arg(logPartIndex_, 2, 10, QLatin1Char('0'));
    logFile_.setFileName(logFilePath_);
    if (!logFile_.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return false;

    const QString header = QStringLiteral("\n==== gvfg_qt_preview session %1 part %2 ====\n")
                               .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")))
                               .arg(logPartIndex_);
    logFile_.write(header.toUtf8());
    logFile_.flush();
    return true;
}

void MainWindow::rotateLogFileIfNeeded()
{
    if (!logFile_.isOpen() || logFile_.size() < kMaxLogFileBytes)
        return;

    const QString footer = QStringLiteral("==== log rotated at %1 ====\n")
                               .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")));
    logFile_.write(footer.toUtf8());
    logFile_.flush();
    ++logPartIndex_;
    openLogFilePart();
}

void MainWindow::writeLogFileLine(const QString &line)
{
    std::lock_guard<std::mutex> lock(logFileMutex_);
    if (!logFile_.isOpen())
        return;

    if (logFile_.size() >= kMaxLogFileBytes)
    {
        const QString footer = QStringLiteral("==== log rotated at %1 ====\n")
                                   .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")));
        logFile_.write(footer.toUtf8());
        logFile_.flush();
        ++logPartIndex_;
        if (logFile_.isOpen())
            logFile_.close();
        logFilePath_ = QStringLiteral("%1/gvfg_qt_preview_%2_part%3.log")
                           .arg(logDirPath_, logSessionStamp_)
                           .arg(logPartIndex_, 2, 10, QLatin1Char('0'));
        logFile_.setFileName(logFilePath_);
        if (logFile_.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        {
            const QString header = QStringLiteral("\n==== gvfg_qt_preview session %1 part %2 ====\n")
                                       .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")))
                                       .arg(logPartIndex_);
            logFile_.write(header.toUtf8());
        }
    }

    if (!logFile_.isOpen())
        return;

    logFile_.write(line.toUtf8());
    logFile_.write("\n");
    logFile_.flush();
}

void MainWindow::appendLog(const QString &message)
{
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    const QStringList lines = message.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i)
    {
        const QString line = QStringLiteral("[%1] %2%3")
                                 .arg(timestamp,
                                      i == 0 ? QString() : QStringLiteral("  "),
                                      lines.at(i));
        ui_->logEdit->appendPlainText(line);
        writeLogFileLine(line);
    }
}

void MainWindow::captureReadLoop()
{
    while (!captureStop_.load(std::memory_order_acquire))
    {
        gvfg_event_t event{};
        while (gvfg_poll_event(handle_, &event, 0) == GVFG_OK)
        {
            const QString type = eventTypeText(event.type);
            const uint64_t timestampNs = event.timestamp_ns;
            QMetaObject::invokeMethod(this, [this, type, timestampNs]()
                                      { appendLog(QStringLiteral("event %1 ts=%2")
                                                      .arg(type)
                                                      .arg(static_cast<qulonglong>(timestampNs))); }, Qt::QueuedConnection);
        }

        gvfg_frame_t frame{};
        const gvfg_status_t st = gvfg_read_frame(handle_, &frame, 200);
        if (st == GVFG_OK)
        {
            const uint64_t count = ++frameCount_;
            const int width = frame.width;
            const int height = frame.height;
            const uint64_t frameId = frame.frame_id;
            if (previewHandle_)
            {
                gvfg_preview_frame_t previewFrame{};
                previewFrame.struct_size = sizeof(previewFrame);
                previewFrame.data = frame.data;
                previewFrame.data_size = frame.data_size;
                previewFrame.width = frame.width;
                previewFrame.height = frame.height;
                previewFrame.bit_depth = frame.bit_depth;
                previewFrame.frame_id = frame.frame_id;

                switch (frame.pixel_format)
                {
                case GVFG_PIXFMT_YUY2:
                    previewFrame.pixel_format = GVFG_PREVIEW_PIXFMT_YUY2;
                    previewFrame.row_bytes = frame.width * 2;
                    break;
                case GVFG_PIXFMT_Y210:
                    previewFrame.pixel_format = GVFG_PREVIEW_PIXFMT_Y210;
                    previewFrame.row_bytes = frame.width * 4;
                    break;
                case GVFG_PIXFMT_V210:
                    previewFrame.pixel_format = GVFG_PREVIEW_PIXFMT_V210;
                    previewFrame.row_bytes = ((frame.width + 5) / 6) * 16;
                    break;
                default:
                    previewFrame.pixel_format = 0;
                    break;
                }

                gvfg_frame_layout_t layout{};
                layout.struct_size = sizeof(layout);
                if (gvfg_get_frame_layout(&frame, &layout) == GVFG_OK &&
                    layout.plane_count > 0 &&
                    layout.plane_data[0] &&
                    layout.plane_stride[0] > 0)
                {
                    previewFrame.data = layout.plane_data[0];
                    previewFrame.row_bytes = layout.plane_stride[0];
                }

                const gvfg_preview_status_t previewStatus =
                    gvfg_preview_render_frame(previewHandle_, &previewFrame);
                if (previewStatus != GVFG_PREVIEW_OK)
                {
                    const uint64_t failures = ++previewFailureCount_;
                    if (failures <= 5 || (failures % 60) == 0)
                    {
                        QMetaObject::invokeMethod(this, [this, failures, previewStatus]()
                                                  { appendLog(QStringLiteral("preview render failed #%1: %2")
                                                                  .arg(static_cast<qulonglong>(failures))
                                                                  .arg(QString::fromUtf8(gvfg_preview_strerror(previewStatus)))); }, Qt::QueuedConnection);
                    }
                }
                else if (previewFailureCount_ != 0)
                {
                    const uint64_t failures = previewFailureCount_;
                    previewFailureCount_ = 0;
                    QMetaObject::invokeMethod(this, [this, failures]()
                                              { appendLog(QStringLiteral("preview render recovered after %1 failure(s)")
                                                              .arg(static_cast<qulonglong>(failures))); }, Qt::QueuedConnection);
                }
            }
            gvfg_release_frame(handle_, &frame);

            if (count <= 5 || (count % 60) == 0)
            {
                QMetaObject::invokeMethod(this, [this, count, frameId, width, height]()
                                          { appendLog(QStringLiteral("read_frame #%1 source id=%2 %3x%4")
                                                          .arg(static_cast<qulonglong>(count))
                                                          .arg(static_cast<qulonglong>(frameId))
                                                          .arg(width)
                                                          .arg(height)); }, Qt::QueuedConnection);
            }
            continue;
        }

        if (st == GVFG_ETIMEOUT)
            continue;
        if (captureStop_.load(std::memory_order_acquire) || st == GVFG_ESTATE)
            break;

        QMetaObject::invokeMethod(this, [this, st]()
                                  { appendLog(QStringLiteral("gvfg_read_frame failed: %1")
                                                  .arg(QString::fromUtf8(gvfg_strerror(st)))); }, Qt::QueuedConnection);
        break;
    }
}

void MainWindow::joinCaptureThread()
{
    if (captureThread_.joinable())
        captureThread_.join();
}
