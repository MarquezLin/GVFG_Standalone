#include "capture_controller.h"
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>
#include <QMetaObject>
#include <QStringList>
#include <QTimer>

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
    constexpr int kAudioSampleRate = 48000;
    constexpr int kAudioChannelCount = 2;

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
        const QString interfaceName = signal.video_interface == GVFG_INPUT_INTERFACE_SDI
                                          ? QStringLiteral("SDI")
                                      : signal.video_interface == GVFG_INPUT_INTERFACE_HDMI
                                          ? QStringLiteral("HDMI")
                                          : QStringLiteral("--");
        return QStringLiteral("%1 %2 %3-bit %4").arg(resolution, format, bit, interfaceName);
    }

    QString eventTypeText(gvfg_event_type_t type)
    {
        switch (type)
        {
        case GVFG_EVENT_VIDEO_FORMAT_CHANGED: return QStringLiteral("VIDEO_FORMAT_CHANGED");
        case GVFG_EVENT_VIDEO_INPUT_PLUGIN: return QStringLiteral("VIDEO_INPUT_PLUGIN");
        case GVFG_EVENT_VIDEO_INPUT_UNPLUG: return QStringLiteral("VIDEO_INPUT_UNPLUG");
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
        appendLog(QStringLiteral("[SDK] ERROR no GVFG device found"));
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
        appendLog(QStringLiteral("[APP] Open skipped | no device selected"));
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
    if (handle_ == nullptr && !openDevice())
        return false;

    const bool newlyOpened = !runtime.opened;
    const bool zeroCopyEnabled = runtime.zeroCopy;
    gvfg_status_t status = GVFG_OK;
    if (newlyOpened)
    {
        status = gvfg_set_channel_zero_copy_enabled(handle_, channel, zeroCopyEnabled ? 1 : 0);
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
    }

    status = gvfg_set_channel_audio_enabled(
        handle_, channel, runtime.requestedAudio ? 1 : 0);
    if (status != GVFG_OK)
    {
        reportError(QStringLiteral("gvfg_set_channel_audio_enabled"), status, channel);
        return false;
    }
    if (!applyOutputFormat(channel))
        return false;

    if (newlyOpened)
    {
        appendLog(QStringLiteral("Opened device index %1 CH%2 | mode=%3")
                      .arg(selectedDeviceIndex_)
                      .arg(channel)
                      .arg(zeroCopyEnabled ? QStringLiteral("zero-copy") : QStringLiteral("copy")));
    }
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
    appendLog(QStringLiteral("CH%1 Output format | %2")
                  .arg(channel)
                  .arg(format == GVFG_PIXFMT_Y210 ? QStringLiteral("Y210") : QStringLiteral("YUY2")));
    return true;
}

void CaptureController::closeDevice()
{
    closeDeviceSession(true);
}

void CaptureController::closeDeviceIfIdle()
{
    if (closingDevice_ || handle_ == nullptr)
        return;

    for (const ChannelRuntime &channel : channels_)
    {
        if (channel.running.load(std::memory_order_acquire))
            return;
    }

    closeDeviceSession(false);
}

void CaptureController::closeDeviceSession(bool clearSelection)
{
    if (closingDevice_)
        return;
    closingDevice_ = true;

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

    if (clearSelection)
    {
        emit statusChanged(QStringLiteral("Idle"));
        lastSignalStatusText_.clear();
        emit sdiInfoChanged(QStringLiteral("SDI Info: --"));
        selectedDeviceIndex_ = -1;
    }
    for (ChannelRuntime &channel : channels_)
    {
        channel.opened = false;
        if (clearSelection)
        {
            channel.cachedSignalStatus = {};
            channel.haveCachedSignalStatus = false;
            channel.cachedSdiInfoText.clear();
            channel.lastLoggedInputStatus.clear();
        }
    }
    closingDevice_ = false;
    emit stateChanged();
}

void CaptureController::startCapture(int channelIndex)
{
    ChannelRuntime &channel = channels_[channelIndex];
    if (channel.running.load(std::memory_order_acquire))
        return;

    if (!openChannel(channelIndex))
    {
        closeDeviceIfIdle();
        return;
    }

    // Consume queued events, then let gvfg_start_channel perform the single
    // fresh signal query used to accept or reject this explicit Start.
    processPendingEvents();

    const bool audioEnabled = channel.requestedAudio;
    channel.audioFormat = {};
    if (audioEnabled)
    {
        const gvfg_status_t audioStatus =
            gvfg_get_channel_audio_format(handle_, channelIndex, &channel.audioFormat);
        if (audioStatus != GVFG_OK)
        {
            reportError(QStringLiteral("gvfg_get_channel_audio_format"), audioStatus, channelIndex);
            closeDeviceIfIdle();
            return;
        }
        if (channel.audioFormat.sample_rate != kAudioSampleRate ||
            channel.audioFormat.channels != kAudioChannelCount ||
            channel.audioFormat.bits_per_sample != 16)
        {
            appendLog(QStringLiteral("CH%1 [APP] ERROR audio format unsupported | %2 Hz %3 ch %4-bit")
                          .arg(channelIndex)
                          .arg(channel.audioFormat.sample_rate)
                          .arg(channel.audioFormat.channels)
                          .arg(channel.audioFormat.bits_per_sample));
            closeDeviceIfIdle();
            return;
        }
    }

#if GVFG_INTERNAL_DIAGNOSTICS
    internalDiagnostics_.beginStart(channelIndex);
#endif
    const gvfg_status_t st = gvfg_start_channel(handle_, channelIndex);
#if GVFG_INTERNAL_DIAGNOSTICS
    internalDiagnostics_.finishStartCall(channelIndex);
#endif
    if (st != GVFG_OK)
    {
        reportError(QStringLiteral("gvfg_start_channel"), st, channelIndex);
        if (st == GVFG_ETIMEOUT)
        {
            updateSignalStatus();
            refreshSdiInfo(channelIndex);
        }
        emit previewCloseRequested(channelIndex);
        emit stateChanged();
        closeDeviceIfIdle();
        return;
    }

    // While the SDK channel is running this returns the signal state cached by
    // gvfg_start_channel, without another GigabyteLib/driver read.
    updateSignalStatus();
    refreshSdiInfo(channelIndex);
    if (!channel.haveCachedSignalStatus || !channel.cachedSignalStatus.connected)
    {
        appendLog(QStringLiteral("CH%1 [SDK] Started without cached signal state; stopping")
                      .arg(channelIndex));
        closeDeviceIfIdle();
        return;
    }

    channel.frameAvailable.store(false, std::memory_order_release);
    emit previewSourceSizeChanged(channelIndex, channel.cachedSignalStatus.width,
                                  channel.cachedSignalStatus.height);
    emit previewShowRequested(channelIndex);
    if (!applyPreview(channelIndex))
    {
        emit previewCloseRequested(channelIndex);
        closeDeviceIfIdle();
        return;
    }

    if (gvfg_preview_prepare(channel.previewHandle,
                             channel.cachedSignalStatus.width,
                             channel.cachedSignalStatus.height,
                             channel.cachedSignalStatus.bit_depth) != GVFG_PREVIEW_OK)
    {
        appendLog(QStringLiteral("CH%1 [APP] Preview prewarm failed").arg(channelIndex));
    }

#if GVFG_INTERNAL_DIAGNOSTICS
    appendLog(QStringLiteral("FPGA signal before stream start"));
#endif

    if (gvfg_preview_wait_idle(channel.previewHandle, 2000) != GVFG_PREVIEW_OK)
    {
        appendLog(QStringLiteral("CH%1 [APP] WARNING preview busy from previous run; start skipped").arg(channelIndex));
        emit previewCloseRequested(channelIndex);
        closeDeviceIfIdle();
        return;
    }
    channel.previewFailureCount = 0;
    channel.audioEnabled = audioEnabled;
    channel.videoFailed = 0;
    channel.lastLoggedPreviewFailures = 0;
    channel.lastLoggedAudioReleaseFailures = channel.lastLoggedAudioOutputFailures = 0;
    channel.lastDeliveryLogMs = 0;
    channel.previewBaseline = {};
    gvfg_preview_get_delivery_stats(channel.previewHandle, &channel.previewBaseline);
#if GVFG_INTERNAL_DIAGNOSTICS
    internalDiagnostics_.resetChannel(handle_, channelIndex);
#endif
    {
        std::lock_guard<std::mutex> lock(channel.audioQueueMutex);
        channel.audioQueue.clear();
        channel.audioReceivedFrames = 0;
        channel.audioReleaseFailedFrames = channel.audioOutputFailedFrames = 0;
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
            appendLog(QStringLiteral("CH%1 [APP] WARNING preview drain timeout | in-flight not classified as lost").arg(channelIndex));
        logDeliveryStatus(channelIndex, true);
        gvfg_stop_channel(handle_, channelIndex);
        channel.running.store(false, std::memory_order_release);
        channel.frameAvailable.store(false, std::memory_order_release);
        updateSignalStatus(false);
        channel.audioEnabled = false;
        emit previewCloseRequested(channelIndex);
        appendLog(QStringLiteral("CH%1 Stopped capture").arg(channelIndex));
    }

    channel.frameAvailable.store(false, std::memory_order_release);
    closeDeviceIfIdle();
    if (handle_ != nullptr)
    {
        updateSignalStatus();
        emit stateChanged();
    }
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
            appendLog(QStringLiteral("CH%1 [APP] Preview setup failed | %2")
                          .arg(channelIndex)
                          .arg(QString::fromUtf8(gvfg_preview_strerror(st))));
            channel.previewHandle = nullptr;
            return false;
        }
    }

    if (!channel.previewTarget)
    {
        appendLog(QStringLiteral("CH%1 [APP] Preview setup failed | no native window target").arg(channelIndex));
        return false;
    }
    const gvfg_preview_status_t st = gvfg_preview_attach_window(
        channel.previewHandle, channel.previewTarget);
    if (st != GVFG_PREVIEW_OK)
    {
        appendLog(QStringLiteral("CH%1 [APP] Preview setup failed | %2")
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
            appendLog(QStringLiteral("CH%1 [LIB] Event | %2").arg(channel).arg(eventTypeText(eventType)));

            if (eventType == GVFG_EVENT_VIDEO_FORMAT_CHANGED ||
                eventType == GVFG_EVENT_VIDEO_INPUT_PLUGIN ||
                eventType == GVFG_EVENT_VIDEO_INPUT_UNPLUG)
                updateSignalStatus();

            if (eventType == GVFG_EVENT_VIDEO_INPUT_UNPLUG)
            {
                channels_[channel].signalConnected.store(false, std::memory_order_release);
            }
            else if (eventType == GVFG_EVENT_VIDEO_INPUT_PLUGIN)
            {
                channels_[channel].signalConnected.store(true, std::memory_order_release);
                channels_[channel].signalReady.notify_all();
            }

            if (eventType == GVFG_EVENT_VIDEO_INPUT_PLUGIN ||
                eventType == GVFG_EVENT_VIDEO_INPUT_UNPLUG ||
                eventType == GVFG_EVENT_VIDEO_FORMAT_CHANGED)
                refreshSdiInfo(channel);

            if (eventType == GVFG_EVENT_VIDEO_INPUT_UNPLUG && channels_[channel].previewHandle)
                gvfg_preview_clear(channels_[channel].previewHandle);

            event = {};
            event.struct_size = sizeof(event);
        }
    }

}

void CaptureController::refreshSdiInfo(int channel)
{
    gvfg_sdi_info_t info{};
    if (gvfg_get_channel_sdi_info(handle_, channel, &info) != GVFG_OK)
        return;

    const QString text = QStringLiteral("CH%1 SDI Info | %2 | %3 | %4 | %5 | %6 | %7 | %8 | %9 | %10 | ErrorCount=%11")
                             .arg(channel)
                             .arg(QString::fromUtf8(info.signal_lock_name))
                             .arg(QString::fromUtf8(info.mode_name))
                             .arg(QString::fromUtf8(info.resolution_name))
                             .arg(QString::fromUtf8(info.fps_name))
                             .arg(QString::fromUtf8(info.scan_name))
                             .arg(QString::fromUtf8(info.st352_format_name))
                             .arg(QString::fromUtf8(info.st352_fps_name))
                             .arg(QString::fromUtf8(info.st352_chroma_name))
                             .arg(QString::fromUtf8(info.st352_bit_depth_name))
                             .arg(info.error_count);

    ChannelRuntime &runtime = channels_[channel];
    if (runtime.cachedSdiInfoText == text)
        return;
    runtime.cachedSdiInfoText = text;
    if (channel == GVFG_CHANNEL_0)
        emit sdiInfoChanged(text);
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
                                        ? QStringLiteral("CH%1 [LIB] GVFG_VIDEO_INFO.VideoSignalLock=1 | Video Locked | %2")
                                              .arg(channelIndex)
                                              .arg(signalFrameText(signal))
                                        : QStringLiteral("CH%1 [LIB] GVFG_VIDEO_INFO.VideoSignalLock=0 | Video Undetected")
                                              .arg(channelIndex);
        if (inputStatus != channel.lastLoggedInputStatus)
        {
            appendLog(inputStatus);
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
            internalDiagnostics_.appendAudioStatusLine(
                statusLines, handle_, channelIndex, receivedFrames);
#endif
        }
        statusLines << QStringLiteral("CH%1 Preview | %2 FPS | %3")
                           .arg(channelIndex)
                           .arg(previewFps, previewFrame);
        logDeliveryStatus(channelIndex);
#if GVFG_INTERNAL_DIAGNOSTICS
        internalDiagnostics_.appendVideoStatusLines(
            statusLines, handle_, channelIndex, channel.zeroCopy);
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
    QString message = QStringLiteral("[SDK API] %1 failed | %2")
                          .arg(apiName, QString::fromUtf8(gvfg_strerror(status)));
    if (channel == GVFG_CHANNEL_0 || channel == GVFG_CHANNEL_1)
        message.prepend(QStringLiteral("CH%1 ").arg(channel));
    if (status < 0 && handle_ != nullptr &&
        (channel == GVFG_CHANNEL_0 || channel == GVFG_CHANNEL_1))
    {
        char detail[512] = {};
        if (gvfg_get_channel_last_sdk_error_detail(handle_, channel, detail, sizeof(detail)) == GVFG_OK &&
            detail[0] != '\0')
            message += QStringLiteral(" | %1").arg(QString::fromUtf8(detail));
    }
    appendLog(message);
}

void CaptureController::appendLog(const QString &message)
{
    emit logMessage(message);
}

#if GVFG_INTERNAL_DIAGNOSTICS
void CaptureController::writeDiagnosticSnapshot(const QString &statusText)
{
    emit diagnosticMessage(statusText);
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
    uint64_t audioReleaseFailed, audioOutputFailed;
    {
        std::lock_guard<std::mutex> lock(c.audioQueueMutex);
        audioReleaseFailed = c.audioReleaseFailedFrames;
        audioOutputFailed = c.audioOutputFailedFrames;
    }
    // Also flush any changes since the last aggregate when stopping.
    if (failed != c.lastLoggedPreviewFailures)
    {
        QString message = QStringLiteral("CH%1 [APP] ERROR preview submission | failures=%2")
            .arg(QString::number(channelIndex), count(failed));
        appendLog(message);
    }
    if (audioReleaseFailed != c.lastLoggedAudioReleaseFailures)
    {
        QString message = QStringLiteral("CH%1 [SDK API] ERROR audio frame release | failed_frames=%2")
            .arg(QString::number(channelIndex), count(audioReleaseFailed));
        appendLog(message);
    }
    if (audioOutputFailed != c.lastLoggedAudioOutputFailures)
    {
        QString message = QStringLiteral("CH%1 [APP] ERROR audio output write | failed_frames=%2")
            .arg(QString::number(channelIndex), count(audioOutputFailed));
        appendLog(message);
    }
    if (finalSnapshot || failed != c.lastLoggedPreviewFailures ||
        audioReleaseFailed != c.lastLoggedAudioReleaseFailures ||
        audioOutputFailed != c.lastLoggedAudioOutputFailures)
        c.lastDeliveryLogMs = now;
    c.lastLoggedPreviewFailures = failed;
    c.lastLoggedAudioReleaseFailures = audioReleaseFailed;
    c.lastLoggedAudioOutputFailures = audioOutputFailed;
}

void CaptureController::captureReadLoop(int channelIndex)
{
    std::chrono::steady_clock::time_point noFrameSince{};
    std::chrono::steady_clock::time_point nextNoFrameReport{};
    bool captureStalledLogged = false;
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
#if GVFG_INTERNAL_DIAGNOSTICS
        const auto getFrameStart = std::chrono::steady_clock::now();
#endif
        const gvfg_status_t st = gvfg_read_channel_frame(handle_, channelIndex, &frame, 200);
#if GVFG_INTERNAL_DIAGNOSTICS
        const double getFrameElapsedMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - getFrameStart)
                .count();
#endif
        if (st == GVFG_OK)
        {
#if GVFG_INTERNAL_DIAGNOSTICS
            internalDiagnostics_.recordVideoRead(channelIndex, getFrameElapsedMs);
#endif
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
                                          { appendLog(QStringLiteral("CH%1 [SDK] Video frame delivery resumed").arg(channelIndex)); }, Qt::QueuedConnection);
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

#if GVFG_INTERNAL_DIAGNOSTICS
                const auto previewStart = std::chrono::steady_clock::now();
#endif
                const gvfg_preview_status_t previewStatus =
                    gvfg_preview_render_frame(channel.previewHandle, &previewFrame);
#if GVFG_INTERNAL_DIAGNOSTICS
                const double previewElapsedMs =
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - previewStart)
                        .count();
#endif
                if (previewStatus != GVFG_PREVIEW_OK)
                {
                    ++channel.videoFailed;
                    const uint64_t failures = ++channel.previewFailureCount;
                    if (failures == 1)
                    {
                        QMetaObject::invokeMethod(this, [this, channelIndex, failures, previewStatus]()
                                                  { appendLog(QStringLiteral("CH%1 [APP] Preview render failed | consecutive=%2 error=%3")
                                                                  .arg(channelIndex)
                                                                  .arg(static_cast<qulonglong>(failures))
                                                                  .arg(QString::fromUtf8(gvfg_preview_strerror(previewStatus)))); }, Qt::QueuedConnection);
                    }
                }
                else
                {
#if GVFG_INTERNAL_DIAGNOSTICS
                    const QString startupLine = internalDiagnostics_.takeStartupLatencyLine(
                        channelIndex, getFrameElapsedMs, previewElapsedMs, frame.frame_id);
                    if (!startupLine.isEmpty())
                    {
                        QMetaObject::invokeMethod(
                            this,
                            [this, startupLine]() { appendLog(startupLine); },
                            Qt::QueuedConnection);
                    }
#endif
                    if (channel.previewFailureCount != 0)
                    {
                        const uint64_t failures = channel.previewFailureCount;
                        channel.previewFailureCount = 0;
                        QMetaObject::invokeMethod(this, [this, channelIndex, failures]()
                                                  { appendLog(QStringLiteral("CH%1 [APP] Preview render recovered | previous_failures=%2")
                                                                  .arg(channelIndex)
                                                                  .arg(static_cast<qulonglong>(failures))); }, Qt::QueuedConnection);
                    }

                }
            }
            if (!channel.previewHandle)
                ++channel.videoFailed;
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
                                              appendLog(QStringLiteral("CH%1 [SDK] ERROR no video frame returned | duration_ms=%2")
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

        {
            std::lock_guard<std::mutex> lock(channel.audioQueueMutex);
            ++channel.audioReceivedFrames;
        }
        std::vector<uint8_t> pcm(frame.data_size);
        std::memcpy(pcm.data(), frame.data, frame.data_size);
        const gvfg_status_t releaseStatus =
            gvfg_release_channel_audio_frame(handle_, channelIndex, &frame);
        if (releaseStatus != GVFG_OK)
        {
            std::lock_guard<std::mutex> lock(channel.audioQueueMutex);
            ++channel.audioReleaseFailedFrames;
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
                    break;
                if (channel.audioQueue.size() >= kMaxQueuedAudioFrames)
                    channel.audioQueue.pop_front();
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
                    appendLog(QStringLiteral("CH%1 [APP] WARNING audio output unavailable | retrying").arg(channelIndex));
                }, Qt::QueuedConnection);
            recovering = true;
            std::unique_lock<std::mutex> lock(channel.audioQueueMutex);
            while (!channel.audioQueue.empty())
                channel.audioQueue.pop_front();
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
                    appendLog(QStringLiteral("CH%1 [APP] Audio output start failed | retrying").arg(channelIndex));
                }, Qt::QueuedConnection);
            recovering = true;
            std::unique_lock<std::mutex> lock(channel.audioQueueMutex);
            while (!channel.audioQueue.empty())
                channel.audioQueue.pop_front();
            channel.audioQueueReady.wait_for(lock, std::chrono::milliseconds(500), [&channel]() {
                return channel.stopRequested.load(std::memory_order_acquire);
            });
            continue;
        }

        if (recovering)
            QMetaObject::invokeMethod(this, [this, channelIndex]() {
                appendLog(QStringLiteral("CH%1 [APP] Audio output recovered").arg(channelIndex));
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
                if (result > 0)
                {
                    written += result;
                    lastProgress = now;
                    continue;
                }
                if (result < 0 || stalled)
                {
                    {
                        std::lock_guard<std::mutex> lock(channel.audioQueueMutex);
                        ++channel.audioOutputFailedFrames;
                    }
                    QMetaObject::invokeMethod(this, [this, channelIndex, stalled]() {
                        appendLog(stalled
                            ? QStringLiteral("CH%1 [APP] Audio output stalled | duration_ms=2000").arg(channelIndex)
                            : QStringLiteral("CH%1 [APP] Audio output write failed").arg(channelIndex));
                    }, Qt::QueuedConnection);
                    restartPlayback = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        audioSink.stop();
        if (restartPlayback)
        {
            recovering = true;
            std::unique_lock<std::mutex> lock(channel.audioQueueMutex);
            while (!channel.audioQueue.empty())
                channel.audioQueue.pop_front();
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
    channels_[channel].audioQueue.clear();
}
