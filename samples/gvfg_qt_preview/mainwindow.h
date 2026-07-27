#pragma once

#include <gvfg_capture.h>
#include <gvfg_preview.h>

#include <QFile>
#include <QElapsedTimer>
#include <QString>
#include <QWidget>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

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
    void refreshDevices();
    void showPreviewWindow();
    void showFullscreenPreviewWindow();
    bool openDevice();
    void closeDevice();
    void startCapture();
    void stopCapture();
    bool applyPreview();
    void updatePreviewSourceSize(const gvfg_runtime_info_t &info,
                                 const gvfg_signal_status_t &signal);
    void updatePreviewSourceSize();
    void updateSignalStatus();
    void updateUiState();
    void showError(const QString &apiName, gvfg_status_t status);
    void openLogFile();
    bool openLogFilePart();
    void rotateLogFileIfNeeded();
    void writeLogFileLine(const QString &line);
    void writeDiagnosticSnapshot(const QString &statusText);
    void appendLog(const QString &message);
    void captureReadLoop();
    void joinCaptureThread();

    static constexpr qint64 kMaxLogFileBytes = 20ll * 1024ll * 1024ll;

    Ui::MainWindow *ui_ = nullptr;
    PreviewWindow *previewWindow_ = nullptr;
    std::array<gvfg_device_info_t, GVFG_MAX_DEVICES> devices_{};
    int deviceCount_ = 0;
    gvfg_handle handle_ = nullptr;
    gvfg_preview_handle previewHandle_ = nullptr;
    std::atomic<bool> captureRunning_{false};
    std::atomic<bool> captureStop_{false};
    std::thread captureThread_;
    uint64_t previewFailureCount_ = 0;
    QTimer *signalStatusTimer_ = nullptr;
    QString lastSignalStatusText_;
    QString lastLoggedBackendError_;
    QElapsedTimer diagnosticSnapshotTimer_;
    QFile logFile_;
    std::mutex logFileMutex_;
    QString logDirPath_;
    QString logFilePath_;
    QString logSessionStamp_;
    int logPartIndex_ = 1;
};

QWidget *createMainWindow();
