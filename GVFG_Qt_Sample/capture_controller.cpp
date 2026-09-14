#include "capture_controller.h"
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QIODevice>
#include <QMediaDevices>
#include <QMetaObject>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace
{
    constexpr size_t kMaxQueuedAudioFrames = 10;

    QString logFilePrefix()
    {
#if GVFG_INTERNAL_DIAGNOSTICS
        return QStringLiteral("gvfg_qt_preview_debug");
#else
        return QStringLiteral("gvfg_qt_preview");
#endif
    }

#if GVFG_INTERNAL_DIAGNOSTICS
    QString boolText(int value) { return value ? QStringLiteral("yes") : QStringLiteral("no"); }

    QString captureStatusText(const gvfg_debug_backend_stats_t &stats, bool signalConnected)
    {
        if (!stats.running) return QStringLiteral("stopped");
        if (!signalConnected) return QStringLiteral("waiting_signal");
        return QStringLiteral("streaming");
    }
#endif

    QString frameText(bool valid, int width, int height, const char *pixelFormat, int bitDepth)
    {
        if (!valid || width <= 0 || height <= 0) return QStringLiteral("--");
        const QString format = pixelFormat && pixelFormat[0] != '\0'
                                   ? QString::fromUtf8(pixelFormat) : QStringLiteral("--");
        const QString bit = bitDepth > 0 ? QString::number(bitDepth) : QStringLiteral("--");
        return QStringLiteral("%1x%2 %3 %4-bit").arg(width).arg(height).arg(format, bit);
    }

    QString signalFrameText(const gvfg_signal_status_t &signal)
    {
        if (!signal.connected) return QStringLiteral("No signal");
        const QString resolution = signal.width > 0 && signal.height > 0
                                       ? QStringLiteral("%1x%2").arg(signal.width).arg(signal.height)
                                       : QStringLiteral("--");
        const QString format = signal.pixel_format != GVFG_PIXFMT_UNKNOWN
                                   ? QString::fromLatin1(gvfg_pixel_format_name(signal.pixel_format))
                                   : QStringLiteral("--");
        const QString bit = signal.bit_depth > 0 ? QString::number(signal.bit_depth) : QStringLiteral("--");
        return QStringLiteral("%1 %2 %3-bit").arg(resolution, format, bit);
    }

    QString eventTypeText(gvfg_event_type_t type)
    {
        switch (type)
        {
        case GVFG_EVENT_SIGNAL_CONNECTED: return QStringLiteral("SIGNAL_CONNECTED");
        case GVFG_EVENT_SIGNAL_DISCONNECTED: return QStringLiteral("SIGNAL_DISCONNECTED");
        case GVFG_EVENT_STREAM_READY: return QStringLiteral("STREAM_READY");
        case GVFG_EVENT_FORMAT_CHANGE_BEGIN: return QStringLiteral("FORMAT_CHANGE_BEGIN");
        default: return QStringLiteral("UNKNOWN");
        }
    }
}
CaptureController::CaptureController(QObject *parent) : QObject(parent)
{
    runtimeStatusTimer_ = new QTimer(this);
    runtimeStatusTimer_->setInterval(200);
    connect(runtimeStatusTimer_, &QTimer::timeout, this, [this] {
        processPendingEvents();
        updateSignalStatus(false);
    });
    openLogFile();
}

CaptureController::~CaptureController()
{
    closeDevice();
}

void CaptureController::setChannelOptions(int channel, bool zeroCopy,
                                          gvfg_pixel_format_t format, bool audioEnabled)
{
    if (channel < GVFG_CHANNEL_0 || channel > GVFG_CHANNEL_1)
        return;
    channels_[channel].zeroCopy = zeroCopy;
    channels_[channel].requestedFormat = format;
    channels_[channel].requestedAudio = channel == GVFG_CHANNEL_0 && audioEnabled;
}

void CaptureController::setChannelStatusVisible(int channel, bool visible)
{
    if (channel >= GVFG_CHANNEL_0 && channel <= GVFG_CHANNEL_1)
        channelStatusVisible_[channel] = visible;
}

void CaptureController::setPreviewTarget(int channel, void *nativeWindow)
{
    if (channel >= GVFG_CHANNEL_0 && channel <= GVFG_CHANNEL_1)
        channels_[channel].previewTarget = nativeWindow;
}

void CaptureController::setPreviewVisible(int channel, bool visible)
{
    if (channel >= GVFG_CHANNEL_0 && channel <= GVFG_CHANNEL_1)
        channels_[channel].previewVisible.store(visible, std::memory_order_release);
}

bool CaptureController::channelOpened(int channel) const
{
    return channel >= GVFG_CHANNEL_0 && channel <= GVFG_CHANNEL_1 && channels_[channel].opened;
}

bool CaptureController::channelRunning(int channel) const
{
    return channel >= GVFG_CHANNEL_0 && channel <= GVFG_CHANNEL_1 &&
           channels_[channel].running.load(std::memory_order_acquire);
}

bool CaptureController::frameAvailable(int channel) const
{
    return channel >= GVFG_CHANNEL_0 && channel <= GVFG_CHANNEL_1 &&
           channels_[channel].frameAvailable.load(std::memory_order_acquire);
}

bool CaptureController::cachedSignalStatus(int channel, gvfg_signal_status_t *status) const
{
    if (!status || channel < GVFG_CHANNEL_0 || channel > GVFG_CHANNEL_1 ||
        !channels_[channel].haveCachedSignalStatus)
        return false;
    *status = channels_[channel].cachedSignalStatus;
    return true;
}

QString CaptureController::sdkVersion() const
{
    return QString::fromLatin1(gvfg_get_version());
}

void CaptureController::logStartupInfo()
{
    appendLog(QStringLiteral("GVFG SDK version | %1").arg(sdkVersion()));
    appendLog(logFile_.isOpen()
                  ? QStringLiteral("Log file | %1").arg(logFilePath_)
                  : QStringLiteral("Log file unavailable | %1").arg(logFilePath_));
}

void CaptureController::logUiMessage(const QString &message)
{
    appendLog(message);
}

void CaptureController::refreshDevices()
{
    if (handle_ != nullptr)
        closeDevice();

    devices_ = {};

    const int count = gvfg_enumerate_devices(devices_.data(), GVFG_MAX_DEVICES);
    deviceCount_ = count > 0 ? count : 0;

    QStringList names;
    for (int i = 0; i < deviceCount_; ++i)
    {
        const QString name = QString::fromUtf8(devices_[i].name);
        const QString displayName = name.isEmpty() ? QStringLiteral("GVFG Capture") : name;
        names.push_back(displayName);
    }

    if (deviceCount_ <= 0)
    {
        appendLog(QStringLiteral("No GVFG device found"));
    }
    else
    {
        appendLog(QStringLiteral("Found %1 GVFG capture device(s)").arg(deviceCount_));
    }
    emit devicesChanged(names);
}

bool CaptureController::openDevice()
{
    if (handle_ != nullptr)
        return true;

    if (selectedDeviceIndex_ < 0)
    {
        appendLog(QStringLiteral("Open skipped: no device selected"));
        return false;
    }

    gvfg_status_t st = gvfg_create(&handle_);
    if (st != GVFG_OK || handle_ == nullptr)
    {
        reportError(QStringLiteral("gvfg_create"), st);
        handle_ = nullptr;
        emit stateChanged();
        return false;
    }

    lastSignalStatusText_.clear();
    appendLog(QStringLiteral("Created device session for index %1").arg(selectedDeviceIndex_));
    runtimeStatusTimer_->start();
    updateSignalStatus(false);
    emit stateChanged();
    return true;
}

bool CaptureController::openChannel(int channel)
{
    ChannelRuntime &runtime = channels_[channel];
    if (runtime.opened)
        return applyOutputFormat(channel);
    if (handle_ == nullptr && !openDevice())
        return false;

    const bool zeroCopyEnabled = runtime.zeroCopy;
    gvfg_status_t status = gvfg_set_channel_zero_copy_enabled(handle_, channel, zeroCopyEnabled ? 1 : 0);
    if (status != GVFG_OK)
    {
        reportError(QStringLiteral("gvfg_set_channel_zero_copy_enabled"), status, channel);
        return false;
    }
    status = gvfg_open_channel(handle_, selectedDeviceIndex_, channel);
    if (status != GVFG_OK)
    {
        reportError(QStringLiteral("gvfg_open_channel"), status, channel);
        return false;
    }
    runtime.opened = true;
    if (!applyOutputFormat(channel))
        return false;

    appendLog(QStringLiteral("Opened device index %1 CH%2 | mode=%3")
                  .arg(selectedDeviceIndex_)
                  .arg(channel)
                  .arg(zeroCopyEnabled ? QStringLiteral("zero-copy") : QStringLiteral("copy")));
    updateSignalStatus();
    return true;
}

bool CaptureController::applyOutputFormat(int channel)
{
    if (handle_ == nullptr || !channels_[channel].opened ||
        channels_[channel].running.load(std::memory_order_acquire))
        return false;

    const gvfg_pixel_format_t format = channels_[channel].requestedFormat;
    const gvfg_status_t status = gvfg_set_channel_video_format(handle_, channel, format);
    if (status != GVFG_OK)
    {
        reportError(QStringLiteral("gvfg_set_channel_video_format"), status, channel);
        return false;
    }
    appendLog(QStringLiteral("CH%1 Output format | %2 (GigabyteLib extension)")
                  .arg(channel)
                  .arg(format == GVFG_PIXFMT_Y210 ? QStringLiteral("Y210") : QStringLiteral("YUY2")));
    return true;
}

void CaptureController::closeDevice()
{
    if (runtimeStatusTimer_)
        runtimeStatusTimer_->stop();

    stopAllCaptures();
    for (ChannelRuntime &channel : channels_)
    {
        if (channel.previewHandle)
        {
            gvfg_preview_destroy(channel.previewHandle);
            channel.previewHandle = nullptr;
        }
    }

    if (handle_ != nullptr)
    {
        gvfg_destroy(handle_);
        handle_ = nullptr;
        appendLog(QStringLiteral("Closed device"));
    }

    emit statusChanged(QStringLiteral("Idle"));
    lastSignalStatusText_.clear();
    selectedDeviceIndex_ = -1;
    for (ChannelRuntime &channel : channels_)
    {
        channel.opened = false;
        channel.cachedSignalStatus = {};
        channel.haveCachedSignalStatus = false;
        channel.lastLoggedInputStatus.clear();
    }
    emit stateChanged();
}

void CaptureController::startCapture(int channelIndex)
{
    ChannelRuntime &channel = channels_[channelIndex];
    if (channel.running.load(std::memory_order_acquire))
        return;

    if (!openChannel(channelIndex))
        return;

    // Consume queued signal events first. gvfg_start_channel() performs the
    // single authoritative hardware revalidation; preview setup reuses the
    // most recent UI cache instead of issuing another register query here.
    processPendingEvents();
    if (!channel.haveCachedSignalStatus || !channel.cachedSignalStatus.connected)
    {
        appendLog(QStringLiteral("CH%1 Start skipped: no input signal").arg(channelIndex));
        return;
    }

    const bool audioEnabled = channel.requestedAudio;
    gvfg_status_t st = gvfg_set_channel_audio_enabled(handle_, channelIndex, audioEnabled ? 1 : 0);
    if (st != GVFG_OK)
    {
        reportError(QStringLiteral("gvfg_set_channel_audio_enabled"), st, channelIndex);
        return;
    }
    channel.audioFormat = {};
    if (audioEnabled)
    {
        st = gvfg_get_channel_audio_format(handle_, channelIndex, &channel.audioFormat);
        if (st != GVFG_OK)
        {
            reportError(QStringLiteral("gvfg_get_channel_audio_format"), st, channelIndex);
            return;
        }
        if (channel.audioFormat.channels == 0 || channel.audioFormat.sample_rate == 0 ||
            (channel.audioFormat.bits_per_sample != 8 &&
             channel.audioFormat.bits_per_sample != 16 &&
             channel.audioFormat.bits_per_sample != 32))
        {
            appendLog(QStringLiteral("CH%1 Audio format unsupported by Qt playback | %2 Hz %3 ch %4-bit")
                          .arg(channelIndex)
                          .arg(channel.audioFormat.sample_rate)
                          .arg(channel.audioFormat.channels)
                          .arg(channel.audioFormat.bits_per_sample));
            return;
        }
    }

    channel.frameAvailable.store(false, std::memory_order_release);
    emit previewSourceSizeChanged(channelIndex, channel.cachedSignalStatus.width,
                                  channel.cachedSignalStatus.height);
    emit previewShowRequested(channelIndex);
    if (!applyPreview(channelIndex))
    {
        emit previewCloseRequested(channelIndex);
        return;
    }

    if (gvfg_preview_prepare(channel.previewHandle,
                             channel.cachedSignalStatus.width,
                             channel.cachedSignalStatus.height,
                             channel.cachedSignalStatus.bit_depth) != GVFG_PREVIEW_OK)
    {
        appendLog(QStringLiteral("CH%1 Preview prewarm failed; first frame may initialize GPU resources").arg(channelIndex));
    }

#if GVFG_INTERNAL_DIAGNOSTICS
    appendLog(QStringLiteral("FPGA signal before stream start"));
#endif

    if (gvfg_preview_wait_idle(channel.previewHandle, 2000) != GVFG_PREVIEW_OK)
    {
        appendLog(QStringLiteral("CH%1 Preview still busy from previous run; retry Start after it completes.").arg(channelIndex));
        return;
    }
    channel.startupStartTime = std::chrono::steady_clock::now();
    st = gvfg_start_channel(handle_, channelIndex);
    const auto startupStartCallEnd = std::chrono::steady_clock::now();
    channel.startupStartCallMs =
        std::chrono::duration<double, std::milli>(
            startupStartCallEnd - channel.startupStartTime)
            .count();
    if (st != GVFG_OK)
    {
        reportError(QStringLiteral("gvfg_start_channel"), st, channelIndex);
        emit previewCloseRequested(channelIndex);
        updateSignalStatus();
        emit stateChanged();
        return;
    }

    channel.previewFailureCount = 0;
    channel.getFrameAverageMs.store(0.0, std::memory_order_relaxed);
    channel.getFrameMaximumMs.store(0.0, std::memory_order_relaxed);
    channel.getFrameWindowMaximumMs.store(0.0, std::memory_order_relaxed);
    channel.getFrameSamples.store(0, std::memory_order_relaxed);
    channel.audioEnabled = audioEnabled;
    channel.videoReceived = 0; channel.videoSubmitted = 0;
    channel.videoSkipped = 0; channel.videoFailed = 0;
    channel.videoIdGaps = 0; channel.videoIdResets = 0;
    channel.videoLastId = 0;
    channel.lastLoggedVideoIssues = channel.lastLoggedAudioIssues = 0;
    channel.lastDeliveryLogMs = 0;
    channel.previewBaseline = {};
    gvfg_preview_get_delivery_stats(channel.previewHandle, &channel.previewBaseline);
    {
        std::lock_guard<std::mutex> lock(channel.audioQueueMutex);
        channel.audioQueue.clear();
        channel.audioReceivedFrames = 0;
        channel.audioReceivedBytes = channel.audioAcceptedBytes = channel.audioQueuedBytes = 0;
        channel.audioDroppedBytes = channel.audioCancelledBytes = 0;
        channel.audioFailedBytes = channel.audioIdGaps = channel.audioIdResets = 0;
        channel.audioLastId = 0;
    }
    channel.signalConnected.store(channel.cachedSignalStatus.connected != 0,
                                  std::memory_order_release);
    channel.stopRequested.store(false, std::memory_order_release);
    channel.running.store(true, std::memory_order_release);
    channel.captureThreadExited.store(false, std::memory_order_release);
    channel.captureThread = std::thread([this, channelIndex]()
                                        { captureReadLoop(channelIndex); });
    if (audioEnabled)
    {
        channel.audioPlaybackThread = std::thread([this, channelIndex]()
                                                   { audioPlaybackLoop(channelIndex); });
        channel.audioThread = std::thread([this, channelIndex]()
                                          { audioReadLoop(channelIndex); });
    }
    emit stateChanged();
    appendLog(audioEnabled
                  ? QStringLiteral("CH%1 Started video + audio | %2 Hz %3 ch %4-bit")
                        .arg(channelIndex)
                        .arg(channel.audioFormat.sample_rate)
                        .arg(channel.audioFormat.channels)
                        .arg(channel.audioFormat.bits_per_sample)
                  : QStringLiteral("CH%1 Started video-only").arg(channelIndex));
    updateSignalStatus(false);
}

void CaptureController::stopCapture(int channelIndex)
{
    ChannelRuntime &channel = channels_[channelIndex];
    if (handle_ != nullptr && channel.running.load(std::memory_order_acquire))
    {
        channel.stopRequested.store(true, std::memory_order_release);
        channel.signalReady.notify_all();
        channel.audioQueueReady.notify_all();
        joinCaptureThread(channelIndex);
        joinAudioThread(channelIndex);
        if (gvfg_preview_wait_idle(channel.previewHandle, 2000) != GVFG_PREVIEW_OK)
            appendLog(QStringLiteral("CH%1 Preview drain timed out; remaining in_flight is not counted as lost.").arg(channelIndex));
        logDeliveryStatus(channelIndex, true);
        gvfg_stop_channel(handle_, channelIndex);
        channel.running.store(false, std::memory_order_release);
        channel.frameAvailable.store(false, std::memory_order_release);
        channel.audioEnabled = false;
        emit previewCloseRequested(channelIndex);
        appendLog(QStringLiteral("CH%1 Stopped capture").arg(channelIndex));
        updateSignalStatus();
    }

    channel.frameAvailable.store(false, std::memory_order_release);
    emit stateChanged();
}

void CaptureController::stopAllCaptures()
{
    stopCapture(GVFG_CHANNEL_0);
    stopCapture(GVFG_CHANNEL_1);
}

bool CaptureController::applyPreview(int channelIndex)
{
    ChannelRuntime &channel = channels_[channelIndex];
    if (!channel.previewHandle)
    {
        const gvfg_preview_status_t st = gvfg_preview_create(&channel.previewHandle);
        if (st != GVFG_PREVIEW_OK || !channel.previewHandle)
        {
            appendLog(QStringLiteral("CH%1 Preview setup failed: %2")
                          .arg(channelIndex)
                          .arg(QString::fromUtf8(gvfg_preview_strerror(st))));
            channel.previewHandle = nullptr;
            return false;
        }
    }

    if (!channel.previewTarget)
    {
        appendLog(QStringLiteral("CH%1 Preview setup failed: no native window target").arg(channelIndex));
        return false;
    }
    const gvfg_preview_status_t st = gvfg_preview_attach_window(
        channel.previewHandle, channel.previewTarget);
    if (st != GVFG_PREVIEW_OK)
    {
        appendLog(QStringLiteral("CH%1 Preview setup failed: %2")
                      .arg(channelIndex)
                      .arg(QString::fromUtf8(gvfg_preview_strerror(st))));
        return false;
    }
    return true;
}

void CaptureController::processPendingEvents()
{
    if (handle_ == nullptr)
        return;

    for (int channel = GVFG_CHANNEL_0; channel <= GVFG_CHANNEL_1; ++channel)
    {
        if (!channels_[channel].opened)
            continue;
        gvfg_event_t event{};
        event.struct_size = sizeof(event);
        while (gvfg_poll_channel_event(handle_, channel, &event, 0) == GVFG_OK)
        {
            const auto eventType = static_cast<gvfg_event_type_t>(event.type);
            if (eventType != GVFG_EVENT_STREAM_READY)
                appendLog(QStringLiteral("CH%1 EVENT %2").arg(channel).arg(eventTypeText(eventType)));

            if (eventType == GVFG_EVENT_SIGNAL_CONNECTED ||
                eventType == GVFG_EVENT_SIGNAL_DISCONNECTED ||
                eventType == GVFG_EVENT_FORMAT_CHANGE_BEGIN ||
                eventType == GVFG_EVENT_STREAM_READY)
                updateSignalStatus();

            if (eventType == GVFG_EVENT_SIGNAL_DISCONNECTED)
                channels_[channel].signalConnected.store(false, std::memory_order_release);
            else if (eventType == GVFG_EVENT_SIGNAL_CONNECTED ||
                     eventType == GVFG_EVENT_STREAM_READY)
            {
                channels_[channel].signalConnected.store(true, std::memory_order_release);
                channels_[channel].signalReady.notify_all();
            }

            if (eventType == GVFG_EVENT_SIGNAL_DISCONNECTED && channels_[channel].previewHandle)
                gvfg_preview_clear(channels_[channel].previewHandle);

            event = {};
            event.struct_size = sizeof(event);
        }
    }
}

void CaptureController::updateSignalStatus(bool queryHardware)
{
    if (handle_ == nullptr)
        return;

    QStringList statusLines;
    for (int channelIndex = GVFG_CHANNEL_0; channelIndex <= GVFG_CHANNEL_1; ++channelIndex)
    {
        if (!channelStatusVisible_[channelIndex])
            continue;

        ChannelRuntime &channel = channels_[channelIndex];
        if (!channel.opened)
        {
            const bool zeroCopy = channel.zeroCopy;
            statusLines << QStringLiteral("CH%1 | Not opened | %2")
                               .arg(channelIndex)
                               .arg(zeroCopy ? QStringLiteral("Zero-copy") : QStringLiteral("Copy"));
            continue;
        }
        if (queryHardware)
        {
            gvfg_signal_status_t signal{};
            if (gvfg_get_channel_signal_status(handle_, channelIndex, &signal) == GVFG_OK)
            {
                channel.cachedSignalStatus = signal;
                channel.haveCachedSignalStatus = true;
            }
        }
        if (!channel.haveCachedSignalStatus)
        {
            statusLines << QStringLiteral("CH%1 | Signal unavailable").arg(channelIndex);
            continue;
        }

        const gvfg_signal_status_t &signal = channel.cachedSignalStatus;
        const QString inputStatus = signal.connected
                                        ? QStringLiteral("CH%1 Connected | %2").arg(channelIndex).arg(signalFrameText(signal))
                                        : QStringLiteral("CH%1 No signal").arg(channelIndex);
        if (inputStatus != channel.lastLoggedInputStatus)
        {
            appendLog(QStringLiteral("Input status | %1").arg(inputStatus));
            channel.lastLoggedInputStatus = inputStatus;
        }
        if (channel.previewVisible.load(std::memory_order_acquire) && signal.width > 0 && signal.height > 0)
            emit previewSourceSizeChanged(channelIndex, signal.width, signal.height);

        gvfg_preview_info_t previewInfo{};
        const bool previewInfoOk = channel.previewHandle &&
                                   gvfg_preview_get_info(channel.previewHandle, &previewInfo) == GVFG_PREVIEW_OK &&
                                   previewInfo.active;
        gvfg_preview_stats_t previewStats{};
        const bool previewStatsOk = channel.previewHandle &&
                                    gvfg_preview_get_stats(channel.previewHandle, &previewStats) == GVFG_PREVIEW_OK;
        const QString previewFps = previewStatsOk && previewStats.present_fps > 0.0
                                       ? QString::number(previewStats.present_fps, 'f', 2)
                                       : QStringLiteral("--");
        const QString previewFrame = previewInfoOk
                                         ? frameText(true, previewInfo.width, previewInfo.height,
                                                     previewInfo.pixel_format, previewInfo.bit_depth)
                                         : QStringLiteral("--");
        const bool zeroCopy = channel.zeroCopy;
        statusLines << QStringLiteral("CH%1 | %2 | %3 | %4")
                           .arg(QString::number(channelIndex),
                                channel.running.load(std::memory_order_acquire)
                                    ? QStringLiteral("Running") : QStringLiteral("Stopped"),
                                signalFrameText(signal),
                                zeroCopy ? QStringLiteral("Zero-copy") : QStringLiteral("Copy"));
        if (channel.audioEnabled)
        {
            uint64_t receivedFrames = 0;
            {
                std::lock_guard<std::mutex> lock(channel.audioQueueMutex);
                receivedFrames = channel.audioReceivedFrames;
            }
            statusLines << QStringLiteral("CH%1 Audio | %2 Hz %3 ch %4-bit | received=%5 frames")
                               .arg(channelIndex)
                               .arg(channel.audioFormat.sample_rate)
                               .arg(channel.audioFormat.channels)
                               .arg(channel.audioFormat.bits_per_sample)
                               .arg(static_cast<qulonglong>(receivedFrames));
#if GVFG_INTERNAL_DIAGNOSTICS
            uint64_t sdkBytes = 0;
            uint64_t appOutputBytes = 0;
            {
                std::lock_guard<std::mutex> lock(channel.audioQueueMutex);
                sdkBytes = channel.audioReceivedBytes;
                appOutputBytes = channel.audioAcceptedBytes;
            }
            gvfg_debug_backend_stats_t audioStats{};
            if (gvfg_debug_get_channel_backend_stats(handle_, channelIndex, &audioStats) == GVFG_OK)
            {
                statusLines << QStringLiteral("CH%1 Audio Debug | Events DMA=%2 Extra=%3 | Driver->SDK %4 frames/%5 bytes | SDK->APP %6 frames/%7 bytes | APP->Output %8 bytes")
                                   .arg(channelIndex)
                                   .arg(static_cast<qulonglong>(audioStats.audio_dma_event_wakes))
                                   .arg(static_cast<qulonglong>(audioStats.extra_audio_event_wakes))
                                   .arg(static_cast<qulonglong>(audioStats.audio_frames_from_driver))
                                   .arg(static_cast<qulonglong>(audioStats.audio_bytes_from_driver))
                                   .arg(static_cast<qulonglong>(receivedFrames))
                                   .arg(static_cast<qulonglong>(sdkBytes))
                                   .arg(static_cast<qulonglong>(appOutputBytes));
            }
#endif
        }
        statusLines << QStringLiteral("CH%1 Preview | %2 FPS | %3")
                           .arg(channelIndex)
                           .arg(previewFps, previewFrame);
        logDeliveryStatus(channelIndex);
        const uint64_t readSamples = channel.getFrameSamples.load(std::memory_order_relaxed);
        statusLines << (readSamples >= 300
                            ? QStringLiteral("CH%1 Read | avg=%2 max300=%3 max=%4 ms samples=%5")
                                  .arg(channelIndex)
                                  .arg(channel.getFrameAverageMs.load(std::memory_order_relaxed), 0, 'f', 3)
                                  .arg(channel.getFrameWindowMaximumMs.load(std::memory_order_relaxed), 0, 'f', 3)
                                  .arg(channel.getFrameMaximumMs.load(std::memory_order_relaxed), 0, 'f', 3)
                                  .arg(static_cast<qulonglong>(readSamples))
                            : QStringLiteral("CH%1 Read | measuring").arg(channelIndex));

#if GVFG_INTERNAL_DIAGNOSTICS
        gvfg_debug_backend_stats_t channelStats{};
        if (gvfg_debug_get_channel_backend_stats(handle_, channelIndex, &channelStats) == GVFG_OK)
        {
            statusLines << (channelStats.get_frame_timing_samples >= 300
                                ? QStringLiteral("CH%1 %2 | avg=%3 max300=%4 max=%5 us samples=%6")
                                      .arg(channelIndex)
                                      .arg(channel.zeroCopy
                                               ? QStringLiteral("ZeroCopy Acquire")
                                               : QStringLiteral("Copy GetFrame"))
                                      .arg(channelStats.get_frame_timing_average_us, 0, 'f', 3)
                                      .arg(channelStats.get_frame_timing_max300_us, 0, 'f', 3)
                                      .arg(channelStats.get_frame_timing_max_us, 0, 'f', 3)
                                      .arg(static_cast<qulonglong>(channelStats.get_frame_timing_samples))
                                : QStringLiteral("CH%1 Driver GetFrame | measuring").arg(channelIndex));
            statusLines << QStringLiteral("CH%1 Capture | status=%2 no_frame_waits=%3 video_wakes=%4")
                               .arg(channelIndex)
                               .arg(captureStatusText(channelStats, signal.connected != 0))
                               .arg(static_cast<qulonglong>(channelStats.frame_wait_timeouts))
                               .arg(static_cast<qulonglong>(channelStats.video_event_wakes));
        }
#endif
    }

    const QString statusText = statusLines.join(QLatin1Char('\n'));
    const bool changed = lastSignalStatusText_ != statusText;
    if (changed)
    {
        emit statusChanged(statusText);
        lastSignalStatusText_ = statusText;
    }
    emit stateChanged();

}

void CaptureController::reportError(const QString &apiName, gvfg_status_t status, int channel)
{
    QString message = QStringLiteral("%1 failed: %2")
                          .arg(apiName, QString::fromUtf8(gvfg_strerror(status)));
    if (channel == GVFG_CHANNEL_0 || channel == GVFG_CHANNEL_1)
        message.prepend(QStringLiteral("CH%1 ").arg(channel));
    if (handle_ != nullptr && (channel == GVFG_CHANNEL_0 || channel == GVFG_CHANNEL_1))
    {
        char detail[512] = {};
        if (gvfg_get_channel_last_error_detail(handle_, channel, detail, sizeof(detail)) == GVFG_OK &&
            detail[0] != '\0')
            message += QStringLiteral(" | %1").arg(QString::fromUtf8(detail));
    }
    appendLog(message);
}

void CaptureController::appendLog(const QString &message)
{
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    const QStringList lines = message.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i)
    {
        const QString line = QStringLiteral("[%1] %2%3")
                                 .arg(timestamp, i == 0 ? QString() : QStringLiteral("  "), lines.at(i));
        writeLogFileLine(line);
        emit logMessage(line);
    }
}

void CaptureController::openLogFile()
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

bool CaptureController::openLogFilePart()
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

void CaptureController::rotateLogFileIfNeeded()
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

void CaptureController::writeLogFileLine(const QString &line)
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
void CaptureController::writeDiagnosticSnapshot(const QString &statusText)
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

void CaptureController::logDeliveryStatus(int channelIndex, bool finalSnapshot)
{
    auto &c = channels_[channelIndex];
    if (!finalSnapshot && !c.running.load())
        return;
    const qint64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    // Keep normal operation quiet; aggregate repeated failures at most every 5 s.
    if (!finalSnapshot && now - c.lastDeliveryLogMs < 5000)
        return;

    const auto count = [](uint64_t value) { return QString::number(static_cast<qulonglong>(value)); };
    gvfg_preview_delivery_stats_t p{};
    const bool previewOk = c.previewHandle &&
        gvfg_preview_get_delivery_stats(c.previewHandle, &p) == GVFG_PREVIEW_OK;
    const auto &b = c.previewBaseline;
    const uint64_t failed = (previewOk ? p.failed - b.failed : 0) + c.videoFailed.load();
    const uint64_t videoIssues = failed + c.videoIdGaps.load() + c.videoIdResets.load();

    uint64_t audioIssues, audioGaps, audioResets, audioFailed;
    bool audioAccounted;
    {
        std::lock_guard<std::mutex> lock(c.audioQueueMutex);
        audioGaps = c.audioIdGaps;
        audioResets = c.audioIdResets;
        audioFailed = c.audioFailedBytes;
        audioIssues = audioGaps + audioResets + audioFailed;
        audioAccounted = c.audioQueuedBytes == 0 && c.audioReceivedBytes ==
            c.audioAcceptedBytes + c.audioDroppedBytes + c.audioCancelledBytes + c.audioFailedBytes;
    }
    if (finalSnapshot)
    {
        const bool videoAccounted = previewOk && c.videoReceived.load() ==
            c.videoSubmitted.load() + c.videoSkipped.load() + c.videoFailed.load() &&
            c.videoSubmitted.load() == p.submitted - b.submitted;
        if (!videoAccounted || !audioAccounted)
            appendLog(QStringLiteral("CH%1 ERROR delivery accounting mismatch | video=%2 audio=%3")
                .arg(QString::number(channelIndex),
                     videoAccounted ? QStringLiteral("OK") : QStringLiteral("MISMATCH"),
                     audioAccounted ? QStringLiteral("OK") : QStringLiteral("MISMATCH")));
    }
    // Also flush any changes since the last aggregate when stopping.
    if (videoIssues != c.lastLoggedVideoIssues)
    {
        QString message = QStringLiteral("CH%1 ERROR preview delivery | failed=%2 id_gaps=%3 reset_or_duplicate=%4")
            .arg(QString::number(channelIndex), count(failed), count(c.videoIdGaps.load()),
                 count(c.videoIdResets.load()));
        appendLog(message);
    }
    if (audioIssues != c.lastLoggedAudioIssues)
    {
        QString message = QStringLiteral("CH%1 ERROR audio delivery | id_gaps=%2 reset_or_duplicate=%3 failed_bytes=%4")
            .arg(QString::number(channelIndex), count(audioGaps), count(audioResets), count(audioFailed));
        appendLog(message);
    }
    if (finalSnapshot || videoIssues != c.lastLoggedVideoIssues || audioIssues != c.lastLoggedAudioIssues)
        c.lastDeliveryLogMs = now;
    c.lastLoggedVideoIssues = videoIssues;
    c.lastLoggedAudioIssues = audioIssues;
}

void CaptureController::captureReadLoop(int channelIndex)
{
    constexpr uint64_t kTimingWarmupFrames = 30;
    constexpr uint64_t kGetFrameTimingSampleFrames = 300;

    std::chrono::steady_clock::time_point noFrameSince{};
    std::chrono::steady_clock::time_point nextNoFrameReport{};
    bool captureStalledLogged = false;
    bool startupLatencyLogged = false;
    uint64_t successfulFrameCount = 0;
    uint64_t getFrameTimingSampleCount = 0;
    double getFrameTimingMaximumMs = 0.0;
    ChannelRuntime &channel = channels_[channelIndex];
    while (!channel.stopRequested.load(std::memory_order_acquire))
    {
        {
            std::unique_lock<std::mutex> lock(channel.signalMutex);
            channel.signalReady.wait(lock, [&channel]() {
                return channel.stopRequested.load(std::memory_order_acquire) ||
                       channel.signalConnected.load(std::memory_order_acquire);
            });
        }
        if (channel.stopRequested.load(std::memory_order_acquire))
            break;

        gvfg_frame_t frame{};
        const auto getFrameStart = std::chrono::steady_clock::now();
        const gvfg_status_t st = gvfg_read_channel_frame(handle_, channelIndex, &frame, 200);
        const double getFrameElapsedMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - getFrameStart)
                .count();
        if (st == GVFG_OK)
        {
            const uint64_t previousId = channel.videoLastId.exchange(frame.frame_id);
            if (channel.videoReceived.fetch_add(1) != 0)
            {
                if (frame.frame_id > previousId && frame.frame_id - previousId > 1)
                    channel.videoIdGaps += frame.frame_id - previousId - 1;
                else if (frame.frame_id <= previousId)
                    ++channel.videoIdResets;
            }
            const bool timingWarmupComplete = ++successfulFrameCount > kTimingWarmupFrames;
            if (timingWarmupComplete)
            {
                ++getFrameTimingSampleCount;
                getFrameTimingMaximumMs = (std::max)(getFrameTimingMaximumMs, getFrameElapsedMs);
                const uint64_t totalSamples = channel.getFrameSamples.fetch_add(1, std::memory_order_relaxed) + 1;
                const double previousAverage = channel.getFrameAverageMs.load(std::memory_order_relaxed);
                channel.getFrameAverageMs.store(
                    previousAverage + (getFrameElapsedMs - previousAverage) / static_cast<double>(totalSamples),
                    std::memory_order_relaxed);
                double observedMaximum = channel.getFrameMaximumMs.load(std::memory_order_relaxed);
                while (getFrameElapsedMs > observedMaximum &&
                       !channel.getFrameMaximumMs.compare_exchange_weak(
                           observedMaximum, getFrameElapsedMs, std::memory_order_relaxed))
                {
                }
                if (totalSamples <= kGetFrameTimingSampleFrames)
                    channel.getFrameWindowMaximumMs.store(getFrameTimingMaximumMs, std::memory_order_relaxed);
                if (getFrameTimingSampleCount >= kGetFrameTimingSampleFrames)
                {
                    channel.getFrameWindowMaximumMs.store(getFrameTimingMaximumMs, std::memory_order_relaxed);
                    getFrameTimingSampleCount = 0;
                    getFrameTimingMaximumMs = 0.0;
                }
            }
            noFrameSince = {};
            nextNoFrameReport = {};
            if (!channel.frameAvailable.exchange(true, std::memory_order_acq_rel))
            {
                QMetaObject::invokeMethod(this, [this]()
                                          { emit stateChanged(); }, Qt::QueuedConnection);
            }
            if (captureStalledLogged)
            {
                captureStalledLogged = false;
                QMetaObject::invokeMethod(this, [this, channelIndex]()
                                          { appendLog(QStringLiteral("CH%1 RECOVERED capture resumed").arg(channelIndex)); }, Qt::QueuedConnection);
            }

            const bool attemptedPreview = channel.previewHandle &&
                channel.previewVisible.load(std::memory_order_acquire);
            if (attemptedPreview)
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
                case GVFG_PIXFMT_YUY2:
                    previewFrame.pixel_format = GVFG_PREVIEW_PIXFMT_YUY2;
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
                    gvfg_preview_render_frame(channel.previewHandle, &previewFrame);
                const auto previewEnd = std::chrono::steady_clock::now();
                const double previewElapsedMs =
                    std::chrono::duration<double, std::milli>(previewEnd - previewStart).count();
                if (previewStatus != GVFG_PREVIEW_OK)
                {
                    ++channel.videoFailed;
                    const uint64_t failures = ++channel.previewFailureCount;
                    if (failures == 1)
                    {
                        QMetaObject::invokeMethod(this, [this, channelIndex, failures, previewStatus]()
                                                  { appendLog(QStringLiteral("CH%1 preview render failed #%2: %3")
                                                                  .arg(channelIndex)
                                                                  .arg(static_cast<qulonglong>(failures))
                                                                  .arg(QString::fromUtf8(gvfg_preview_strerror(previewStatus)))); }, Qt::QueuedConnection);
                    }
                }
                else
                {
                    ++channel.videoSubmitted;
                    if (!startupLatencyLogged)
                    {
                        startupLatencyLogged = true;
                        const double startupLatencyMs =
                            std::chrono::duration<double, std::milli>(
                                previewEnd - channel.startupStartTime)
                                .count();
                        const double startCallMs = channel.startupStartCallMs;
                        const double firstReadMs = getFrameElapsedMs;
                        const double firstRenderFrameMs = previewElapsedMs;
                        const uint64_t startupFrameId = frame.frame_id;
                        QMetaObject::invokeMethod(
                            this,
                            [this, channelIndex, startCallMs, firstReadMs,
                             firstRenderFrameMs, startupLatencyMs, startupFrameId]()
                            {
                                appendLog(QStringLiteral("CH%1 Startup latency | Start -> start_channel return=%2 ms | "
                                                         "first read_frame call -> return=%3 ms | "
                                                         "first render_frame call -> return=%4 ms | "
                                                         "Start -> first render_frame return=%5 ms | frame_id=%6")
                                              .arg(channelIndex)
                                              .arg(startCallMs, 0, 'f', 3)
                                              .arg(firstReadMs, 0, 'f', 3)
                                              .arg(firstRenderFrameMs, 0, 'f', 3)
                                              .arg(startupLatencyMs, 0, 'f', 3)
                                              .arg(static_cast<qulonglong>(startupFrameId)));
                            },
                            Qt::QueuedConnection);
                    }
                    if (channel.previewFailureCount != 0)
                    {
                        const uint64_t failures = channel.previewFailureCount;
                        channel.previewFailureCount = 0;
                        QMetaObject::invokeMethod(this, [this, channelIndex, failures]()
                                                  { appendLog(QStringLiteral("CH%1 preview render recovered after %2 failure(s)")
                                                                  .arg(channelIndex)
                                                                  .arg(static_cast<qulonglong>(failures))); }, Qt::QueuedConnection);
                    }

                }
            }
            if (!channel.previewHandle)
                ++channel.videoFailed;
            else if (!attemptedPreview)
                ++channel.videoSkipped;
            const gvfg_status_t releaseStatus = gvfg_release_channel_frame(handle_, channelIndex, &frame);
            if (releaseStatus != GVFG_OK)
            {
                QMetaObject::invokeMethod(this, [this, channelIndex, releaseStatus]()
                                          { reportError(QStringLiteral("gvfg_release_channel_frame"), releaseStatus, channelIndex); }, Qt::QueuedConnection);
                break;
            }

            continue;
        }

        if (st == GVFG_ETIMEOUT)
        {
            const auto now = std::chrono::steady_clock::now();
            if (noFrameSince == std::chrono::steady_clock::time_point{})
            {
                noFrameSince = now;
                nextNoFrameReport = now + std::chrono::seconds(2);
            }
            if (now >= nextNoFrameReport)
            {
                captureStalledLogged = true;
                const uint64_t elapsedMs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - noFrameSince).count());
                nextNoFrameReport = now + std::chrono::seconds(10);
                QMetaObject::invokeMethod(this, [this, channelIndex, elapsedMs]()
                                          {
                                              appendLog(QStringLiteral("CH%1 ERROR no capture frame for %2 ms")
                                                            .arg(channelIndex)
                                                            .arg(elapsedMs));
#if GVFG_INTERNAL_DIAGNOSTICS
                                              updateSignalStatus(false);
                                              writeDiagnosticSnapshot(lastSignalStatusText_);
#endif
                                          },
                                          Qt::QueuedConnection);
            }
            continue;
        }
        if (channel.stopRequested.load(std::memory_order_acquire))
            break;

        QMetaObject::invokeMethod(this, [this, channelIndex, st]()
                                  { reportError(QStringLiteral("gvfg_read_channel_frame"), st, channelIndex); }, Qt::QueuedConnection);
        break;
    }
    channel.captureThreadExited.store(true, std::memory_order_release);
}

void CaptureController::joinCaptureThread(int channel)
{
    if (channels_[channel].captureThread.joinable())
    {
        // The capture worker can wait behind preview resource updates. Keep
        // DXGI's synchronous HWND messages flowing while joining it.
        while (!channels_[channel].captureThreadExited.load(std::memory_order_acquire))
        {
            MsgWaitForMultipleObjectsEx(0, nullptr, 1, QS_SENDMESSAGE, MWMO_INPUTAVAILABLE);
            MSG message{};
            PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
        }
        channels_[channel].captureThread.join();
    }
}

void CaptureController::audioReadLoop(int channelIndex)
{
    ChannelRuntime &channel = channels_[channelIndex];
    while (!channel.stopRequested.load(std::memory_order_acquire))
    {
        {
            std::unique_lock<std::mutex> lock(channel.signalMutex);
            channel.signalReady.wait(lock, [&channel]() {
                return channel.stopRequested.load(std::memory_order_acquire) ||
                       channel.signalConnected.load(std::memory_order_acquire);
            });
        }
        if (channel.stopRequested.load(std::memory_order_acquire))
            break;

        gvfg_audio_frame_t frame{};
        const gvfg_status_t status = gvfg_read_channel_audio_frame(handle_, channelIndex, &frame, 200);
        if (status == GVFG_ETIMEOUT)
            continue;
        if (status != GVFG_OK)
        {
            if (!channel.stopRequested.load(std::memory_order_acquire))
            {
                QMetaObject::invokeMethod(this, [this, channelIndex, status]()
                                          { reportError(QStringLiteral("gvfg_read_channel_audio_frame"), status, channelIndex); }, Qt::QueuedConnection);
            }
            break;
        }

        const uint64_t audioId = frame.frame_id;
        {
            std::lock_guard<std::mutex> lock(channel.audioQueueMutex);
            if (channel.audioReceivedBytes != 0)
            {
                if (audioId > channel.audioLastId && audioId - channel.audioLastId > 1)
                    channel.audioIdGaps += audioId - channel.audioLastId - 1;
                else if (audioId <= channel.audioLastId)
                    ++channel.audioIdResets;
            }
            channel.audioLastId = audioId;
            channel.audioReceivedBytes += frame.data_size;
            ++channel.audioReceivedFrames;
        }
        std::vector<uint8_t> pcm(frame.data_size);
        std::memcpy(pcm.data(), frame.data, frame.data_size);
        const gvfg_status_t releaseStatus =
            gvfg_release_channel_audio_frame(handle_, channelIndex, &frame);
        if (releaseStatus != GVFG_OK)
        {
            std::lock_guard<std::mutex> lock(channel.audioQueueMutex);
            channel.audioFailedBytes += pcm.size();
        }
        if (releaseStatus != GVFG_OK && !channel.stopRequested.load(std::memory_order_acquire))
        {
            QMetaObject::invokeMethod(this, [this, channelIndex, releaseStatus]()
                                          { reportError(QStringLiteral("gvfg_release_channel_audio_frame"), releaseStatus, channelIndex); }, Qt::QueuedConnection);
            break;
        }

        if (releaseStatus == GVFG_OK)
        {
            {
                std::lock_guard<std::mutex> lock(channel.audioQueueMutex);
                if (channel.stopRequested.load(std::memory_order_acquire))
                {
                    channel.audioCancelledBytes += pcm.size();
                    break;
                }
                if (channel.audioQueue.size() >= kMaxQueuedAudioFrames)
                {
                    const auto &oldest = channel.audioQueue.front();
                    channel.audioDroppedBytes += oldest.pcm.size();
                    channel.audioQueuedBytes -= oldest.pcm.size();
                    channel.audioQueue.pop_front();
                }
                channel.audioQueuedBytes += pcm.size();
                channel.audioQueue.push_back({std::move(pcm)});
            }
            channel.audioQueueReady.notify_one();
        }
    }
    channel.audioQueueReady.notify_all();
}

void CaptureController::audioPlaybackLoop(int channelIndex)
{
    ChannelRuntime &channel = channels_[channelIndex];
    const gvfg_audio_format_t format = channel.audioFormat;

    QAudioFormat outputFormat;
    outputFormat.setSampleRate(static_cast<int>(format.sample_rate));
    outputFormat.setChannelCount(static_cast<int>(format.channels));
    switch (format.bits_per_sample)
    {
    case 8:
        outputFormat.setSampleFormat(QAudioFormat::UInt8);
        break;
    case 16:
        outputFormat.setSampleFormat(QAudioFormat::Int16);
        break;
    case 32:
        outputFormat.setSampleFormat(QAudioFormat::Int32);
        break;
    default:
        return;
    }

    bool recovering = false;
    while (!channel.stopRequested.load(std::memory_order_acquire))
    {
        const QAudioDevice outputDevice = QMediaDevices::defaultAudioOutput();
        if (outputDevice.isNull() || !outputDevice.isFormatSupported(outputFormat))
        {
            if (!recovering)
                QMetaObject::invokeMethod(this, [this, channelIndex]() {
                    appendLog(QStringLiteral("CH%1 Audio output unavailable; monitoring playback will retry").arg(channelIndex));
                }, Qt::QueuedConnection);
            recovering = true;
            std::unique_lock<std::mutex> lock(channel.audioQueueMutex);
            while (!channel.audioQueue.empty())
            {
                channel.audioDroppedBytes += channel.audioQueue.front().pcm.size();
                channel.audioQueuedBytes -= channel.audioQueue.front().pcm.size();
                channel.audioQueue.pop_front();
            }
            channel.audioQueueReady.wait_for(lock, std::chrono::milliseconds(500), [&channel]() {
                return channel.stopRequested.load(std::memory_order_acquire);
            });
            continue;
        }

        QAudioSink audioSink(outputDevice, outputFormat);
        QIODevice *audioOutput = audioSink.start();
        if (!audioOutput)
        {
            if (!recovering)
                QMetaObject::invokeMethod(this, [this, channelIndex]() {
                    appendLog(QStringLiteral("CH%1 Audio output failed to start; monitoring playback will retry").arg(channelIndex));
                }, Qt::QueuedConnection);
            recovering = true;
            std::unique_lock<std::mutex> lock(channel.audioQueueMutex);
            while (!channel.audioQueue.empty())
            {
                channel.audioDroppedBytes += channel.audioQueue.front().pcm.size();
                channel.audioQueuedBytes -= channel.audioQueue.front().pcm.size();
                channel.audioQueue.pop_front();
            }
            channel.audioQueueReady.wait_for(lock, std::chrono::milliseconds(500), [&channel]() {
                return channel.stopRequested.load(std::memory_order_acquire);
            });
            continue;
        }

        if (recovering)
            QMetaObject::invokeMethod(this, [this, channelIndex]() {
                appendLog(QStringLiteral("CH%1 Audio monitoring playback recovered").arg(channelIndex));
            }, Qt::QueuedConnection);
        recovering = false;
        bool restartPlayback = false;
        while (!channel.stopRequested.load(std::memory_order_acquire) && !restartPlayback)
        {
            std::vector<uint8_t> pcm;
            {
                std::unique_lock<std::mutex> lock(channel.audioQueueMutex);
                channel.audioQueueReady.wait(lock, [&channel]() {
                    return channel.stopRequested.load(std::memory_order_acquire) ||
                           !channel.audioQueue.empty();
                });
                if (channel.stopRequested.load(std::memory_order_acquire))
                    break;
                pcm = std::move(channel.audioQueue.front().pcm);
                channel.audioQueue.pop_front();
                channel.audioQueuedBytes -= pcm.size();
            }

            qint64 written = 0;
            auto lastProgress = std::chrono::steady_clock::now();
            while (written < static_cast<qint64>(pcm.size()) &&
                   !channel.stopRequested.load(std::memory_order_acquire))
            {
                const qint64 result = audioOutput->write(
                    reinterpret_cast<const char *>(pcm.data()) + written,
                    static_cast<qint64>(pcm.size()) - written);
                const auto now = std::chrono::steady_clock::now();
                const bool stalled = result == 0 && now - lastProgress >= std::chrono::seconds(2);
                {
                    std::lock_guard<std::mutex> lock(channel.audioQueueMutex);
                    if (result > 0)
                        channel.audioAcceptedBytes += static_cast<uint64_t>(result);
                    else if (result < 0 || stalled)
                        channel.audioFailedBytes += pcm.size() - static_cast<uint64_t>(written);
                }
                if (result > 0)
                {
                    written += result;
                    lastProgress = now;
                    continue;
                }
                if (result < 0 || stalled)
                {
                    QMetaObject::invokeMethod(this, [this, channelIndex, stalled]() {
                        appendLog(stalled
                            ? QStringLiteral("CH%1 Audio output made no progress for 2 seconds; rebuilding monitoring playback").arg(channelIndex)
                            : QStringLiteral("CH%1 Audio output write failed; rebuilding monitoring playback").arg(channelIndex));
                    }, Qt::QueuedConnection);
                    restartPlayback = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (written != static_cast<qint64>(pcm.size()) && !restartPlayback)
            {
                std::lock_guard<std::mutex> lock(channel.audioQueueMutex);
                channel.audioCancelledBytes += pcm.size() - static_cast<uint64_t>(written);
            }
        }
        audioSink.stop();
        if (restartPlayback)
        {
            recovering = true;
            std::unique_lock<std::mutex> lock(channel.audioQueueMutex);
            while (!channel.audioQueue.empty())
            {
                channel.audioDroppedBytes += channel.audioQueue.front().pcm.size();
                channel.audioQueuedBytes -= channel.audioQueue.front().pcm.size();
                channel.audioQueue.pop_front();
            }
            channel.audioQueueReady.wait_for(lock, std::chrono::milliseconds(500), [&channel]() {
                return channel.stopRequested.load(std::memory_order_acquire);
            });
        }
    }
}

void CaptureController::joinAudioThread(int channel)
{
    if (channels_[channel].audioThread.joinable())
        channels_[channel].audioThread.join();
    channels_[channel].audioQueueReady.notify_all();
    if (channels_[channel].audioPlaybackThread.joinable())
        channels_[channel].audioPlaybackThread.join();
    std::lock_guard<std::mutex> lock(channels_[channel].audioQueueMutex);
    channels_[channel].audioCancelledBytes += channels_[channel].audioQueuedBytes;
    channels_[channel].audioQueuedBytes = 0;
    channels_[channel].audioQueue.clear();
}
