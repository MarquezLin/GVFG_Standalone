#pragma once

#include <gvfg_capture.h>

#include <QWidget>
#include <array>

class CaptureController;
class QCloseEvent;
class PreviewWindow;
class SampleLog;

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
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
    void syncControllerOptions(int channel);
    void showPreviewWindow(int channel, bool fullscreen);
    void updateOutputFormatOptions(int changedChannel = -1);
    void updateUiState();
    void appendLogLine(const QString &line);

    Ui::MainWindow *ui_ = nullptr;
    CaptureController *controller_ = nullptr;
    SampleLog *log_ = nullptr;
    std::array<PreviewWindow *, 2> previewWindows_{};
};

QWidget *createMainWindow();
