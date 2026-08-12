#include "mainwindow.h"
#include "previewwindow.h"
#include "ui_mainwindow.h"

#if GVFG_INTERNAL_DIAGNOSTICS
#include <gvfg_debug.h>
#endif

#include <QCloseEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QIODevice>
#include <QMetaObject>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QStringList>
#include <QTimer>

#include <chrono>

namespace
{
    QString logFilePrefix()
    {
#if GVFG_INTERNAL_DIAGNOSTICS
        return QStringLiteral("gvfg_qt_preview_debug");
#else
        return QStringLiteral("gvfg_qt_preview");
#endif
    }

    class LogHighlighter final : public QSyntaxHighlighter
    {
    public:
        explicit LogHighlighter(QTextDocument *document)
            : QSyntaxHighlighter(document)
        {
            errorFormat_.setForeground(QColor(200, 0, 0));
            errorFormat_.setFontWeight(QFont::Bold);
            recoveryFormat_.setForeground(QColor(0, 128, 0));
            recoveryFormat_.setFontWeight(QFont::Bold);
            warningFormat_.setForeground(QColor(190, 110, 0));
            warningFormat_.setFontWeight(QFont::Bold);
        }

    protected:
        void highlightBlock(const QString &text) override
        {
            if (text.contains(QStringLiteral("SIGNAL_DISCONNECTED")) ||
                text.contains(QStringLiteral("failed"), Qt::CaseInsensitive) ||
                text.contains(QStringLiteral("error"), Qt::CaseInsensitive))
            {
                setFormat(0, text.size(), errorFormat_);
                return;
            }

            if (text.contains(QStringLiteral("warning"), Qt::CaseInsensitive))
            {
                setFormat(0, text.size(), warningFormat_);
                return;
            }

            if (text.contains(QStringLiteral("SIGNAL_CONNECTED")) ||
                text.contains(QStringLiteral("recovered"), Qt::CaseInsensitive))
                setFormat(0, text.size(), recoveryFormat_);
        }

    private:
        QTextCharFormat errorFormat_;
        QTextCharFormat recoveryFormat_;
        QTextCharFormat warningFormat_;
    };

#if GVFG_INTERNAL_DIAGNOSTICS
    QString boolText(int value)
    {
        return value ? QStringLiteral("yes") : QStringLiteral("no");
    }

    QString valueOrDash(uint64_t value)
    {
        return QString::number(static_cast<qulonglong>(value));
    }

    QString captureStatusText(const gvfg_debug_backend_stats_t &stats, bool signalConnected)
    {
        if (!stats.backend_running)
            return QStringLiteral("stopped");
        if (!signalConnected)
            return QStringLiteral("waiting_signal");
        if (!stats.backend_capture_active)
            return QStringLiteral("paused");
        return QStringLiteral("streaming");
    }

    QString appHoldingText(uint64_t value)
    {
        return value == UINT64_MAX ? QStringLiteral("none") : QStringLiteral("slot %1").arg(valueOrDash(value));
    }
#endif

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
        if (!signal.connected)
            return QStringLiteral("No signal");

        const QString resolution = (signal.width > 0 && signal.height > 0)
                                       ? QStringLiteral("%1x%2").arg(signal.width).arg(signal.height)
                                       : QStringLiteral("--");
        const QString format = signal.pixel_format != GVFG_PIXFMT_UNKNOWN
                                   ? QString::fromLatin1(gvfg_pixel_format_name(signal.pixel_format))
                                   : QStringLiteral("--");
        const QString bit = signal.bit_depth > 0 ? QString::number(signal.bit_depth) : QStringLiteral("--");
        return QStringLiteral("%1 %2 %3-bit").arg(resolution, format, bit);
    }

#if GVFG_INTERNAL_DIAGNOSTICS
    QString backendLastError(gvfg_handle handle)
    {
        char message[512] = {};
        if (gvfg_debug_get_last_error_detail(handle, message, sizeof(message)) != GVFG_OK || message[0] == '\0')
            return {};
        return QString::fromUtf8(message);
    }
#endif

    QString eventTypeText(gvfg_event_type_t type)
    {
        switch (type)
        {
        case GVFG_EVENT_SIGNAL_CONNECTED:
            return QStringLiteral("SIGNAL_CONNECTED");
        case GVFG_EVENT_SIGNAL_DISCONNECTED:
            return QStringLiteral("SIGNAL_DISCONNECTED");
        case GVFG_EVENT_STREAM_READY:
            return QStringLiteral("STREAM_READY");
        case GVFG_EVENT_FORMAT_CHANGE_BEGIN:
            return QStringLiteral("FORMAT_CHANGE_BEGIN");
        case GVFG_EVENT_FRAME_LOSS:
            return QStringLiteral("FRAME_LOSS");
        default:
            return QStringLiteral("UNKNOWN");
        }
    }
}

MainWindow::MainWindow(QWidget *parent)
    : QWidget(parent), ui_(new Ui::MainWindow)
{
    ui_->setupUi(this);

#if GVFG_INTERNAL_DIAGNOSTICS
    setWindowTitle(QStringLiteral("GVFG Internal Diagnostic"));
#else
    setWindowTitle(QStringLiteral("GVFG Preview Sample"));
#endif
    previewWindow_ = new PreviewWindow();
    ui_->logEdit->setMaximumBlockCount(300);
    new LogHighlighter(ui_->logEdit->document());
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
            { updateSignalStatus(); });

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
        const QString displayName = name.isEmpty() ? QStringLiteral("GVFG Capture") : name;
        for (int channel = GVFG_CHANNEL_0; channel <= GVFG_CHANNEL_1; ++channel)
        {
            const int selection = devices_[i].index * 2 + channel;
            ui_->deviceCombo->addItem(QStringLiteral("%1 - CH%2").arg(displayName).arg(channel), selection);
        }
    }

    if (deviceCount_ <= 0)
    {
        ui_->deviceCombo->addItem(QStringLiteral("No GVFG device found"), -1);
        appendLog(QStringLiteral("No GVFG device found"));
    }
    else
    {
        appendLog(QStringLiteral("Found %1 GVFG capture device(s)").arg(deviceCount_));
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

    const int selection = ui_->deviceCombo->currentData().toInt();
    const int deviceIndex = selection / 2;
    const int channelIndex = selection % 2;
    st = gvfg_open_channel(handle_, deviceIndex, channelIndex);
    if (st != GVFG_OK)
    {
        showError(QStringLiteral("gvfg_open_channel"), st);
        closeDevice();
        return false;
    }

    lastSignalStatusText_.clear();
    appendLog(QStringLiteral("Opened device index %1 CH%2").arg(deviceIndex).arg(channelIndex));
    appendLog(QStringLiteral("Signal monitoring active"));
    updateSignalStatus();
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
    lastLoggedInputStatus_.clear();
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

    gvfg_signal_status_t previewSignal{};
    if (gvfg_get_signal_status(handle_, &previewSignal) == GVFG_OK &&
        previewSignal.connected &&
        gvfg_preview_prepare(previewHandle_,
                             previewSignal.width,
                             previewSignal.height,
                             previewSignal.bit_depth) != GVFG_PREVIEW_OK)
    {
        appendLog(QStringLiteral("Preview prewarm failed; first frame may initialize GPU resources"));
    }

#if GVFG_INTERNAL_DIAGNOSTICS
    appendLog(QStringLiteral("FPGA signal before stream start"));
#endif
    updateSignalStatus();

    const gvfg_status_t st = gvfg_start(handle_);
    if (st != GVFG_OK)
    {
        showError(QStringLiteral("gvfg_start"), st);
        updateSignalStatus();
        updateUiState();
        return;
    }

    previewFailureCount_ = 0;
    previewCallAverageMs_.store(0.0, std::memory_order_relaxed);
    previewCallSamples_.store(0, std::memory_order_relaxed);
#if GVFG_INTERNAL_DIAGNOSTICS
    haveDebugBaseline_ = false;
    lastDebugDmaErrors_ = 0;
    lastDebugDroppedFrames_ = 0;
    pendingDroppedFrames_ = 0;
    lastDroppedWarningMs_ = 0;
#endif
    captureStop_.store(false, std::memory_order_release);
    captureRunning_ = true;
    captureThread_ = std::thread([this]()
                                 { captureReadLoop(); });
    updateUiState();
    appendLog(QStringLiteral("Started capture"));
    updateSignalStatus();
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
        updateSignalStatus();
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

void MainWindow::updatePreviewSourceSize(const gvfg_signal_status_t &signal)
{
    if (signal.width > 0 && signal.height > 0)
        previewWindow_->setSourceSize(signal.width, signal.height);
}

void MainWindow::updatePreviewSourceSize()
{
    if (!handle_)
        return;

    gvfg_signal_status_t signal{};
    if (gvfg_get_signal_status(handle_, &signal) == GVFG_OK)
        updatePreviewSourceSize(signal);
}

void MainWindow::updateSignalStatus()
{
    if (!handle_)
        return;

    gvfg_signal_status_t signal{};
    if (gvfg_get_signal_status(handle_, &signal) != GVFG_OK)
        return;

    const QString inputStatus = signal.connected
                                    ? QStringLiteral("CH%1 Connected | %2")
                                          .arg(signal.channel)
                                          .arg(signalFrameText(signal))
                                    : QStringLiteral("CH%1 No signal")
                                          .arg(signal.channel);
    if (inputStatus != lastLoggedInputStatus_)
    {
        appendLog(QStringLiteral("Input status | %1").arg(inputStatus));
        lastLoggedInputStatus_ = inputStatus;
    }

#if GVFG_INTERNAL_DIAGNOSTICS
    gvfg_debug_backend_stats_t backendStats{};
    const bool haveBackendStats = gvfg_debug_get_backend_stats(handle_, &backendStats) == GVFG_OK;
#endif
    if (previewWindow_->isVisible())
        updatePreviewSourceSize(signal);

    gvfg_preview_info_t previewInfo{};
    const bool previewInfoOk = previewHandle_ &&
                               gvfg_preview_get_info(previewHandle_, &previewInfo) == GVFG_PREVIEW_OK &&
                               previewInfo.active;
    gvfg_preview_stats_t previewStats{};
    const bool previewStatsOk = previewHandle_ &&
                                gvfg_preview_get_stats(previewHandle_, &previewStats) == GVFG_PREVIEW_OK;
    const QString previewFps = previewStatsOk && previewStats.present_fps > 0.0
                                   ? QString::number(previewStats.present_fps, 'f', 2)
                                   : QStringLiteral("--");
    const QString previewFrame = previewInfoOk
                                     ? frameText(true,
                                                 previewInfo.width,
                                                 previewInfo.height,
                                                 previewInfo.pixel_format,
                                                 previewInfo.bit_depth)
                                     : QStringLiteral("--");
    const QString previewState = !captureRunning_
                                     ? QStringLiteral("Stopped")
                                     : previewFps == QStringLiteral("--")
                                         ? QStringLiteral("Measuring")
                                          : QStringLiteral("%1 FPS").arg(previewFps);
    const double previewCallAverageMs = previewCallAverageMs_.load(std::memory_order_relaxed);
    const uint64_t previewCallSamples = previewCallSamples_.load(std::memory_order_relaxed);
    QStringList statusLines;
    statusLines << (signal.connected
                        ? QStringLiteral("Input   | CH%1 | Connected | %2")
                              .arg(signal.channel)
                              .arg(signalFrameText(signal))
                        : QStringLiteral("Input   | CH%1 | No signal")
                              .arg(signal.channel));
    statusLines << (previewInfoOk
                        ? previewCallSamples > 0
                              ? QStringLiteral("Preview | %1 | %2 | GPU call avg=%3 ms/frame samples=%4")
                                    .arg(previewState, previewFrame)
                                    .arg(previewCallAverageMs, 0, 'f', 3)
                                    .arg(static_cast<qulonglong>(previewCallSamples))
                              : QStringLiteral("Preview | %1 | %2 | GPU call measuring")
                                    .arg(previewState, previewFrame)
                        : QStringLiteral("Preview | Inactive"));

#if GVFG_INTERNAL_DIAGNOSTICS
    statusLines << (haveBackendStats
                        ? QStringLiteral("Capture| status=%1 pending_irqs=%2 dma_errors=%3 no_frame_waits=%4")
                              .arg(captureStatusText(backendStats, signal.connected != 0))
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

    bool diagnosticProblemDetected = false;
    if (haveBackendStats)
    {
        if (haveDebugBaseline_)
        {
            if (backendStats.backend_dma_errors > lastDebugDmaErrors_)
            {
                const uint64_t delta = backendStats.backend_dma_errors - lastDebugDmaErrors_;
                appendLog(QStringLiteral("ERROR DMA failures +%1, total=%2")
                              .arg(valueOrDash(delta),
                                   valueOrDash(backendStats.backend_dma_errors)));
                diagnosticProblemDetected = true;
            }
            if (backendStats.backend_frames_dropped > lastDebugDroppedFrames_)
            {
                const uint64_t delta = backendStats.backend_frames_dropped - lastDebugDroppedFrames_;
                pendingDroppedFrames_ += delta;
            }
        }

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (pendingDroppedFrames_ > 0 &&
            (lastDroppedWarningMs_ == 0 || nowMs - lastDroppedWarningMs_ >= 10000))
        {
            appendLog(QStringLiteral("WARNING dropped frames +%1, total=%2")
                          .arg(valueOrDash(pendingDroppedFrames_),
                               valueOrDash(backendStats.backend_frames_dropped)));
            pendingDroppedFrames_ = 0;
            lastDroppedWarningMs_ = nowMs;
            diagnosticProblemDetected = true;
        }

        lastDebugDmaErrors_ = backendStats.backend_dma_errors;
        lastDebugDroppedFrames_ = backendStats.backend_frames_dropped;
        haveDebugBaseline_ = true;
    }

    const QString lastError = backendLastError(handle_);
    if (!lastError.isEmpty())
        statusLines << QStringLiteral("Error   | %1").arg(lastError);

    if (!lastError.isEmpty() && lastError != lastLoggedBackendError_)
    {
        lastLoggedBackendError_ = lastError;
        appendLog(QStringLiteral("Backend error | %1").arg(lastError));
        diagnosticProblemDetected = true;
    }
    else if (lastError.isEmpty())
    {
        lastLoggedBackendError_.clear();
    }
#endif

    const QString statusText = statusLines.join(QLatin1Char('\n'));
    const bool changed = lastSignalStatusText_ != statusText;
    if (changed)
    {
        ui_->statusLabel->setText(statusText);
        lastSignalStatusText_ = statusText;
    }

#if GVFG_INTERNAL_DIAGNOSTICS
    if (diagnosticProblemDetected)
        writeDiagnosticSnapshot(statusText);
#endif
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

    logFilePath_ = QStringLiteral("%1/%2_%3_part%4.log")
                       .arg(logDirPath_, logFilePrefix(), logSessionStamp_)
                       .arg(logPartIndex_, 2, 10, QLatin1Char('0'));
    logFile_.setFileName(logFilePath_);
    if (!logFile_.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return false;

    const QString header = QStringLiteral("\n==== %1 session %2 part %3 ====\n")
                               .arg(logFilePrefix(),
                                    QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")))
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
        logFilePath_ = QStringLiteral("%1/%2_%3_part%4.log")
                           .arg(logDirPath_, logFilePrefix(), logSessionStamp_)
                           .arg(logPartIndex_, 2, 10, QLatin1Char('0'));
        logFile_.setFileName(logFilePath_);
        if (logFile_.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        {
            const QString header = QStringLiteral("\n==== %1 session %2 part %3 ====\n")
                                       .arg(logFilePrefix(),
                                            QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")))
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

#if GVFG_INTERNAL_DIAGNOSTICS
void MainWindow::writeDiagnosticSnapshot(const QString &statusText)
{
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    const QStringList lines = statusText.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i)
    {
        const QString line = QStringLiteral("[%1] %2%3")
                                 .arg(timestamp,
                                      i == 0 ? QStringLiteral("Diagnostic | ") : QStringLiteral("             "),
                                      lines.at(i));
        writeLogFileLine(line);
    }
}
#endif

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
    constexpr uint64_t kPreviewTimingWarmupFrames = 30;
    constexpr uint64_t kPreviewTimingSampleFrames = 300;

    uint32_t consecutiveTimeouts = 0;
    bool captureStalledLogged = false;
    uint64_t previewTimingWarmupCount = 0;
    uint64_t previewTimingSampleCount = 0;
    double previewTimingTotalMs = 0.0;

    while (!captureStop_.load(std::memory_order_acquire))
    {
        gvfg_event_t event{};
        event.struct_size = sizeof(event);
        while (gvfg_poll_event(handle_, &event, 0) == GVFG_OK)
        {
            const auto eventType = static_cast<gvfg_event_type_t>(event.type);
            const QString type = eventTypeText(eventType);
            const uint64_t count = event.count;
            QMetaObject::invokeMethod(this, [this, eventType, type, count]()
                                      {
                                          if (eventType == GVFG_EVENT_FRAME_LOSS)
                                              appendLog(QStringLiteral("WARNING dropped frames +%1").arg(count));
                                          else
                                              appendLog(QStringLiteral("EVENT %1").arg(type));
                                      },
                                      Qt::QueuedConnection);
            event = {};
            event.struct_size = sizeof(event);
        }

        gvfg_frame_t frame{};
        const gvfg_status_t st = gvfg_read_frame(handle_, &frame, 200);
        if (st == GVFG_OK)
        {
            consecutiveTimeouts = 0;
            if (captureStalledLogged)
            {
                captureStalledLogged = false;
                QMetaObject::invokeMethod(this, [this]()
                                          { appendLog(QStringLiteral("RECOVERED capture resumed")); },
                                          Qt::QueuedConnection);
            }

            if (previewHandle_)
            {
                gvfg_preview_frame_t previewFrame{};
                previewFrame.data = frame.data;
                previewFrame.data_size = frame.data_size;
                previewFrame.width = frame.width;
                previewFrame.height = frame.height;
                previewFrame.row_bytes = frame.row_stride_bytes;
                previewFrame.bit_depth = frame.bit_depth;
                previewFrame.frame_id = frame.frame_id;

                switch (frame.pixel_format)
                {
                case GVFG_PIXFMT_YVYU:
                    previewFrame.pixel_format = GVFG_PREVIEW_PIXFMT_YVYU;
                    break;
                case GVFG_PIXFMT_Y210:
                    previewFrame.pixel_format = GVFG_PREVIEW_PIXFMT_Y210;
                    break;
                default:
                    previewFrame.pixel_format = 0;
                    break;
                }

                const auto previewStart = std::chrono::steady_clock::now();
                const gvfg_preview_status_t previewStatus =
                    gvfg_preview_render_frame(previewHandle_, &previewFrame);
                const auto previewEnd = std::chrono::steady_clock::now();
                const double previewElapsedMs =
                    std::chrono::duration<double, std::milli>(previewEnd - previewStart).count();
                if (previewStatus != GVFG_PREVIEW_OK)
                {
                    const uint64_t failures = ++previewFailureCount_;
                    if (failures == 1)
                    {
                        QMetaObject::invokeMethod(this, [this, failures, previewStatus]()
                                                  { appendLog(QStringLiteral("preview render failed #%1: %2")
                                                                  .arg(static_cast<qulonglong>(failures))
                                                                  .arg(QString::fromUtf8(gvfg_preview_strerror(previewStatus)))); }, Qt::QueuedConnection);
                    }
                }
                else
                {
                    if (previewFailureCount_ != 0)
                    {
                        const uint64_t failures = previewFailureCount_;
                        previewFailureCount_ = 0;
                        QMetaObject::invokeMethod(this, [this, failures]()
                                                  { appendLog(QStringLiteral("preview render recovered after %1 failure(s)")
                                                                  .arg(static_cast<qulonglong>(failures))); }, Qt::QueuedConnection);
                    }

                    if (previewTimingWarmupCount < kPreviewTimingWarmupFrames)
                    {
                        ++previewTimingWarmupCount;
                    }
                    else
                    {
                        previewTimingTotalMs += previewElapsedMs;
                        ++previewTimingSampleCount;

                        if (previewTimingSampleCount >= kPreviewTimingSampleFrames)
                        {
                            const double averageMs =
                                previewTimingTotalMs / static_cast<double>(previewTimingSampleCount);
                            previewCallAverageMs_.store(averageMs, std::memory_order_relaxed);
                            previewCallSamples_.store(kPreviewTimingSampleFrames, std::memory_order_relaxed);

                            previewTimingSampleCount = 0;
                            previewTimingTotalMs = 0.0;
                        }
                    }
                }
            }
            gvfg_release_frame(handle_, &frame);

            continue;
        }

        if (st == GVFG_ETIMEOUT)
        {
            ++consecutiveTimeouts;
            const bool firstReport = !captureStalledLogged && consecutiveTimeouts >= 10;
            const bool periodicReport = captureStalledLogged && (consecutiveTimeouts % 50) == 0;
            if (firstReport || periodicReport)
            {
                gvfg_signal_status_t signal{};
                const gvfg_status_t signalStatus = gvfg_get_signal_status(handle_, &signal);
                const QString detail = signalStatus != GVFG_OK
                                           ? QStringLiteral("signal query failed: %1")
                                                 .arg(QString::fromUtf8(gvfg_strerror(signalStatus)))
                                           : signal.connected
                                               ? signalFrameText(signal)
                                               : QStringLiteral("No signal");
                captureStalledLogged = true;
                const uint32_t elapsedMs = consecutiveTimeouts * 200u;
                QMetaObject::invokeMethod(this, [this, elapsedMs, detail]()
                                          {
                                              appendLog(QStringLiteral("ERROR no capture frame for %1 ms | %2")
                                                            .arg(elapsedMs)
                                                            .arg(detail));
#if GVFG_INTERNAL_DIAGNOSTICS
                                              updateSignalStatus();
                                              writeDiagnosticSnapshot(lastSignalStatusText_);
#endif
                                          },
                                          Qt::QueuedConnection);
            }
            continue;
        }
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
