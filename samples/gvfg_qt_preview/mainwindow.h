#pragma once

#include <gvfg_capture.h>
#include <gvfg_preview.h>

#include <QFile>
#include <QString>
#include <QWidget>

#include <array>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

class QCloseEvent;
class PreviewWindow;
class QTimer;

QT_BEGIN_NAMESPACE
namespace Ui
{
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow final : public QWidget
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    struct ChannelRuntime
    {
        bool opened = false;
        gvfg_preview_handle previewHandle = nullptr;
        std::atomic<bool> running{false};
        std::atomic<bool> stopRequested{false};
        std::atomic<bool> signalConnected{false};
        std::mutex signalMutex;
        std::condition_variable signalReady;
        std::atomic<bool> frameAvailable{false};
        std::atomic<bool> previewVisible{false};
        std::thread captureThread;
        std::thread audioThread;
        std::thread audioPlaybackThread;
        std::mutex audioQueueMutex;
        std::condition_variable audioQueueReady;
        struct AudioPacket { std::vector<uint8_t> pcm; uint64_t id; };
        std::deque<AudioPacket> audioQueue;
        bool audioEnabled = false;
        gvfg_audio_format_t audioFormat{};
        std::atomic<uint64_t> audioFrames{0};
        std::atomic<uint64_t> audioBytes{0};
        std::atomic<uint64_t> audioQueueDrops{0};
        // Audio accounting is protected by audioQueueMutex. Accepted means
        // written to QAudioSink, not physically played by the device.
        uint64_t audioReceivedBytes = 0;
        uint64_t audioAcceptedBytes = 0;
        uint64_t audioQueuedBytes = 0;
        uint64_t audioDroppedBytes = 0;
        uint64_t audioCancelledBytes = 0;
        uint64_t audioFailedBytes = 0;
        uint64_t audioIdGaps = 0;
        uint64_t audioIdResets = 0;
        uint64_t audioLastId = 0;
        uint64_t audioLastDropId = 0;
        qint64 audioLastDropTimeMs = 0;
        double audioMaxWriteStallMs = 0;
        double audioMaxReadGapMs = 0;
        std::atomic<uint64_t> videoReceived{0}, videoSubmitted{0}, videoSkipped{0}, videoFailed{0};
        std::atomic<uint64_t> videoIdGaps{0}, videoIdResets{0};
        std::atomic<uint64_t> videoLastId{0};
        gvfg_preview_delivery_stats_t previewBaseline{};
        std::chrono::steady_clock::time_point startupStartTime{};
        double startupStartCallMs = 0.0;
        uint64_t lastLoggedVideoIssues = 0, lastLoggedAudioIssues = 0;
        qint64 lastDeliveryLogMs = 0;
        std::atomic<double> getFrameAverageMs{0.0};
        std::atomic<double> getFrameMaximumMs{0.0};
        std::atomic<double> getFrameWindowMaximumMs{0.0};
        std::atomic<uint64_t> getFrameSamples{0};
        uint64_t previewFailureCount = 0;
        std::atomic<double> previewCallAverageMs{0.0};
        std::atomic<double> previewCallMaximumMs{0.0};
        std::atomic<double> previewCallWindowMaximumMs{0.0};
        std::atomic<uint64_t> previewCallSamples{0};
        gvfg_signal_status_t cachedSignalStatus{};
        bool haveCachedSignalStatus = false;
        QString lastLoggedInputStatus;
#if GVFG_INTERNAL_DIAGNOSTICS
        uint64_t audioReceivedFrames = 0;
        uint64_t lastDebugDmaErrors = 0;
        bool haveDebugBaseline = false;
#endif
    };

    void refreshDevices();
    void showPreviewWindow(int channel);
    void showFullscreenPreviewWindow(int channel);
    bool openDevice();
    bool openChannel(int channel);
    bool applyOutputFormat(int channel);
    void updateOutputFormatOptions(int changedChannel = -1);
    void closeDevice();
    void startCapture(int channel);
    void stopCapture(int channel);
    void stopAllCaptures();
    bool applyPreview(int channel);
    void updatePreviewSourceSize(int channel, const gvfg_signal_status_t &signal);
    void processPendingEvents();
    void updateSignalStatus(bool queryHardware = true);
    void updateUiState();
    void showError(const QString &apiName, gvfg_status_t status, int channel = -1);
    void openLogFile();
    bool openLogFilePart();
    void rotateLogFileIfNeeded();
    void writeLogFileLine(const QString &line);
#if GVFG_INTERNAL_DIAGNOSTICS
    void writeDiagnosticSnapshot(const QString &statusText);
#endif
    void appendLog(const QString &message);
    void captureReadLoop(int channel);
    void audioReadLoop(int channel);
    void audioPlaybackLoop(int channel);
    void joinCaptureThread(int channel);
    void joinAudioThread(int channel);
    void logDeliveryStatus(int channel, bool finalSnapshot = false);

    static constexpr qint64 kMaxLogFileBytes = 20ll * 1024ll * 1024ll;

    Ui::MainWindow *ui_ = nullptr;
    std::array<PreviewWindow *, 2> previewWindows_{};
    std::array<gvfg_device_info_t, GVFG_MAX_DEVICES> devices_{};
    int deviceCount_ = 0;
    gvfg_handle handle_ = nullptr;
    int selectedDeviceIndex_ = -1;
    std::array<ChannelRuntime, 2> channels_{};
    QTimer *runtimeStatusTimer_ = nullptr;
    QString lastSignalStatusText_;
    QFile logFile_;
    std::mutex logFileMutex_;
    QString logDirPath_;
    QString logFilePath_;
    QString logSessionStamp_;
    int logPartIndex_ = 1;
};

QWidget *createMainWindow();
