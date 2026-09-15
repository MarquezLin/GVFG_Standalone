#pragma once

#include <gvfg_capture.h>
#include <gvfg_debug.h>
#include <gvfg_preview.h>

#include <QObject>
#include <QStringList>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

class QTimer;

class CaptureController final : public QObject
{
    Q_OBJECT

public:
    explicit CaptureController(QObject *parent = nullptr);
    ~CaptureController() override;

    void setSelectedDeviceIndex(int index) { selectedDeviceIndex_ = index; }
    void setChannelOptions(int channel, bool zeroCopy, gvfg_pixel_format_t format, bool audioEnabled);
    void setChannelStatusVisible(int channel, bool visible);
    void setPreviewTarget(int channel, void *nativeWindow);
    void setPreviewVisible(int channel, bool visible);

    bool deviceOpen() const { return handle_ != nullptr; }
    bool channelOpened(int channel) const;
    bool channelRunning(int channel) const;
    bool frameAvailable(int channel) const;
    bool cachedSignalStatus(int channel, gvfg_signal_status_t *status) const;
    QString sdkVersion() const;

public slots:
    void refreshDevices();
    void closeDevice();
    void startCapture(int channel);
    void stopCapture(int channel);
    void stopAllCaptures();
    bool applyPreview(int channel);
    void processPendingEvents();
    void updateSignalStatus(bool queryHardware = true);

signals:
    void devicesChanged(const QStringList &names);
    void stateChanged();
    void statusChanged(const QString &text);
    void sdiInfoChanged(const QString &text);
    void logMessage(const QString &message);
    void diagnosticMessage(const QString &message);
    void previewSourceSizeChanged(int channel, int width, int height);
    void previewShowRequested(int channel);
    void previewCloseRequested(int channel);

private:
    bool openDevice();
    bool applyOutputFormat(int channel);

    struct ChannelRuntime
    {
        struct AudioPacket { std::vector<uint8_t> pcm; };
        bool opened = false;
        bool zeroCopy = false;
        bool requestedAudio = false;
        gvfg_pixel_format_t requestedFormat = GVFG_PIXFMT_YUY2;
        void *previewTarget = nullptr;
        gvfg_preview_handle previewHandle = nullptr;
        std::atomic<bool> running{false}, stopRequested{false}, signalConnected{false};
        std::mutex signalMutex;
        std::condition_variable signalReady;
        std::atomic<bool> frameAvailable{false}, previewVisible{false}, captureThreadExited{true};
        std::thread captureThread, audioThread, audioPlaybackThread;
        std::mutex audioQueueMutex;
        std::condition_variable audioQueueReady;
        std::deque<AudioPacket> audioQueue;
        bool audioEnabled = false;
        gvfg_audio_format_t audioFormat{};
        uint64_t audioReceivedFrames = 0;
        uint64_t audioReleaseFailedFrames = 0, audioOutputFailedFrames = 0;
        std::atomic<uint64_t> videoReceived{0}, videoFailed{0};
        gvfg_preview_delivery_stats_t previewBaseline{};
        gvfg_debug_backend_stats_t backendBaseline{};
        std::chrono::steady_clock::time_point startupStartTime{};
        double startupStartCallMs = 0.0;
        uint64_t lastLoggedPreviewFailures = 0;
        uint64_t lastLoggedAudioReleaseFailures = 0, lastLoggedAudioOutputFailures = 0;
        qint64 lastDeliveryLogMs = 0;
        std::atomic<double> getFrameAverageMs{0.0}, getFrameMaximumMs{0.0}, getFrameWindowMaximumMs{0.0};
        std::atomic<uint64_t> getFrameSamples{0};
        uint64_t previewFailureCount = 0;
        gvfg_signal_status_t cachedSignalStatus{};
        bool haveCachedSignalStatus = false;
        QString cachedSdiInfoText;
        QString lastLoggedInputStatus;
    };

    void reportError(const QString &apiName, gvfg_status_t status, int channel = -1);
    void appendLog(const QString &message);
    bool openChannel(int channel);
    void refreshSdiInfo(int channel);
#if GVFG_INTERNAL_DIAGNOSTICS
    void writeDiagnosticSnapshot(const QString &statusText);
#endif
    void captureReadLoop(int channel);
    void audioReadLoop(int channel);
    void audioPlaybackLoop(int channel);
    void joinCaptureThread(int channel);
    void joinAudioThread(int channel);
    void logDeliveryStatus(int channel, bool finalSnapshot = false);

    std::array<gvfg_device_info_t, GVFG_MAX_DEVICES> devices_{};
    int deviceCount_ = 0;
    gvfg_handle handle_ = nullptr;
    int selectedDeviceIndex_ = -1;
    std::array<ChannelRuntime, 2> channels_{};
    std::array<bool, 2> channelStatusVisible_{{true, true}};
    QTimer *runtimeStatusTimer_ = nullptr;
    QString lastSignalStatusText_;
};
