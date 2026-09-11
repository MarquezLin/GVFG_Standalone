#include "mainwindow.h"
#include "capture_controller.h"
#include "previewwindow.h"
#include "ui_mainwindow.h"

#include <QCloseEvent>
#include <QStandardItemModel>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>

namespace
{
constexpr bool kChannel1UiVisible = false;

class LogHighlighter final : public QSyntaxHighlighter
{
public:
    explicit LogHighlighter(QTextDocument *document) : QSyntaxHighlighter(document)
    {
        error_.setForeground(QColor(200, 0, 0)); error_.setFontWeight(QFont::Bold);
        recovery_.setForeground(QColor(0, 128, 0)); recovery_.setFontWeight(QFont::Bold);
        warning_.setForeground(QColor(190, 110, 0)); warning_.setFontWeight(QFont::Bold);
    }
protected:
    void highlightBlock(const QString &text) override
    {
        if (text.contains(QStringLiteral("SIGNAL_DISCONNECTED")) ||
            text.contains(QStringLiteral("failed"), Qt::CaseInsensitive) ||
            text.contains(QStringLiteral("error"), Qt::CaseInsensitive))
            setFormat(0, text.size(), error_);
        else if (text.contains(QStringLiteral("warning"), Qt::CaseInsensitive))
            setFormat(0, text.size(), warning_);
        else if (text.contains(QStringLiteral("SIGNAL_CONNECTED")) ||
                 text.contains(QStringLiteral("recovered"), Qt::CaseInsensitive))
            setFormat(0, text.size(), recovery_);
    }
private:
    QTextCharFormat error_, recovery_, warning_;
};
}

MainWindow::MainWindow(QWidget *parent)
    : QWidget(parent), ui_(new Ui::MainWindow), controller_(new CaptureController(this))
{
    ui_->setupUi(this);
#if GVFG_INTERNAL_DIAGNOSTICS
    setWindowTitle(QStringLiteral("GVFG Internal Diagnostic - SDK v%1").arg(controller_->sdkVersion()));
#else
    setWindowTitle(QStringLiteral("GVFG Preview Sample - SDK v%1").arg(controller_->sdkVersion()));
#endif
    ui_->logEdit->setMaximumBlockCount(300);
    new LogHighlighter(ui_->logEdit->document());
    ui_->statusLabel->setWordWrap(true);

    for (int channel = GVFG_CHANNEL_0; channel <= GVFG_CHANNEL_1; ++channel)
    {
        previewWindows_[channel] = new PreviewWindow();
        previewWindows_[channel]->setWindowTitle(QStringLiteral("GVFG Preview - CH%1").arg(channel));
        controller_->setPreviewTarget(channel, previewWindows_[channel]->nativePreviewHandle());
        connect(previewWindows_[channel], &PreviewWindow::previewVisibilityChanged,
                this, [this, channel](bool visible) { controller_->setPreviewVisible(channel, visible); });
    }
    controller_->setChannelStatusVisible(GVFG_CHANNEL_1, kChannel1UiVisible);

    connect(controller_, &CaptureController::devicesChanged, this, [this](const QStringList &names) {
        ui_->deviceCombo->clear();
        if (names.isEmpty()) ui_->deviceCombo->addItem(QStringLiteral("No GVFG device found"), -1);
        else for (int i = 0; i < names.size(); ++i) ui_->deviceCombo->addItem(names.at(i), i);
        updateUiState();
    });
    connect(controller_, &CaptureController::stateChanged, this, &MainWindow::updateUiState);
    connect(controller_, &CaptureController::statusChanged, ui_->statusLabel, &QLabel::setText);
    connect(controller_, &CaptureController::logMessage, this, &MainWindow::appendLogLine);
    connect(controller_, &CaptureController::previewSourceSizeChanged, this,
            [this](int channel, int width, int height) {
                if (width > 0 && height > 0) previewWindows_[channel]->setSourceSize(width, height);
            });
    connect(controller_, &CaptureController::previewShowRequested, this,
            [this](int channel) { previewWindows_[channel]->showPreview(); });
    connect(controller_, &CaptureController::previewCloseRequested, this,
            [this](int channel) { previewWindows_[channel]->closePreview(); });

    connect(ui_->refreshButton, &QPushButton::clicked, controller_, &CaptureController::refreshDevices);
    connect(ui_->openButton, &QPushButton::clicked, this, [this] {
        if (controller_->deviceOpen()) controller_->closeDevice();
        else {
            syncControllerOptions(GVFG_CHANNEL_0);
            syncControllerOptions(GVFG_CHANNEL_1);
            controller_->setSelectedDeviceIndex(ui_->deviceCombo->currentData().toInt());
            controller_->openDevice();
        }
    });
    connect(ui_->ch0StartButton, &QPushButton::clicked, this, [this] { syncControllerOptions(0); controller_->startCapture(0); });
    connect(ui_->ch1StartButton, &QPushButton::clicked, this, [this] { syncControllerOptions(1); controller_->startCapture(1); });
    connect(ui_->ch0StopButton, &QPushButton::clicked, controller_, [this] { controller_->stopCapture(0); });
    connect(ui_->ch1StopButton, &QPushButton::clicked, controller_, [this] { controller_->stopCapture(1); });
    connect(ui_->ch0PreviewButton, &QPushButton::clicked, this, [this] { showPreviewWindow(0, false); });
    connect(ui_->ch1PreviewButton, &QPushButton::clicked, this, [this] { showPreviewWindow(1, false); });
    connect(ui_->ch0FullscreenButton, &QPushButton::clicked, this, [this] { showPreviewWindow(0, true); });
    connect(ui_->ch1FullscreenButton, &QPushButton::clicked, this, [this] { showPreviewWindow(1, true); });

    auto formatChanged = [this](int channel) {
        updateOutputFormatOptions(channel);
        syncControllerOptions(channel);
        if (controller_->channelOpened(channel) && !controller_->channelRunning(channel))
        { controller_->applyOutputFormat(channel); controller_->updateSignalStatus(); }
    };
    connect(ui_->ch0OutputFormatCombo, &QComboBox::currentIndexChanged, this, [formatChanged](int) { formatChanged(0); });
    connect(ui_->ch1OutputFormatCombo, &QComboBox::currentIndexChanged, this, [formatChanged](int) { formatChanged(1); });
    connect(ui_->ch0ZeroCopyCheckBox, &QCheckBox::toggled, this, [this](bool) {
        syncControllerOptions(GVFG_CHANNEL_0);
        if (controller_->deviceOpen()) controller_->updateSignalStatus(false);
    });
    connect(ui_->ch1ZeroCopyCheckBox, &QCheckBox::toggled, this, [this](bool) {
        syncControllerOptions(GVFG_CHANNEL_1);
        if (controller_->deviceOpen()) controller_->updateSignalStatus(false);
    });

    updateOutputFormatOptions();
    updateUiState();
    controller_->logStartupInfo();
    controller_->refreshDevices();
}

MainWindow::~MainWindow()
{
    controller_->closeDevice();
    QObject::disconnect(controller_, nullptr, nullptr, nullptr);
    delete controller_;
    controller_ = nullptr;
    delete previewWindows_[0];
    delete previewWindows_[1];
    delete ui_;
}

QWidget *createMainWindow() { return new MainWindow(); }

void MainWindow::closeEvent(QCloseEvent *event)
{
    controller_->closeDevice();
    for (PreviewWindow *window : previewWindows_) if (window) window->closePreview();
    QWidget::closeEvent(event);
}

void MainWindow::syncControllerOptions(int channel)
{
    const bool zeroCopy = channel == 0 ? ui_->ch0ZeroCopyCheckBox->isChecked() : ui_->ch1ZeroCopyCheckBox->isChecked();
    const int formatIndex = channel == 0 ? ui_->ch0OutputFormatCombo->currentIndex() : ui_->ch1OutputFormatCombo->currentIndex();
    const bool audio = channel == 0 && ui_->ch0AudioCheckBox->isChecked();
    controller_->setChannelOptions(channel, zeroCopy,
                                   formatIndex == 1 ? GVFG_PIXFMT_Y210 : GVFG_PIXFMT_YUY2, audio);
}

void MainWindow::showPreviewWindow(int channel, bool fullscreen)
{
    if (!controller_->channelRunning(channel) || !controller_->frameAvailable(channel)) return;
    gvfg_signal_status_t signal{};
    if (controller_->cachedSignalStatus(channel, &signal) && signal.width > 0 && signal.height > 0)
        previewWindows_[channel]->setSourceSize(signal.width, signal.height);
    if (fullscreen) previewWindows_[channel]->showFullscreenPreview();
    else previewWindows_[channel]->showPreview();
    if (!controller_->applyPreview(channel))
        controller_->logUiMessage(
            fullscreen
                ? QStringLiteral("CH%1 Fullscreen Preview failed: unable to update preview window").arg(channel)
                : QStringLiteral("CH%1 Show Preview failed: unable to update preview window").arg(channel));
}

void MainWindow::updateOutputFormatOptions(int changedChannel)
{
    QComboBox *combos[] = {ui_->ch0OutputFormatCombo, ui_->ch1OutputFormatCombo};
    constexpr int y210 = 1;
    if ((changedChannel == 0 || changedChannel == 1) && combos[changedChannel]->currentIndex() == y210)
    {
        const int other = changedChannel == 0 ? 1 : 0;
        if (combos[other]->currentIndex() == y210) combos[other]->setCurrentIndex(0);
    }
    const bool enabled[] = {combos[1]->currentIndex() != y210, combos[0]->currentIndex() != y210};
    for (int channel = 0; channel < 2; ++channel)
    {
        auto *model = qobject_cast<QStandardItemModel *>(combos[channel]->model());
        if (model && model->item(y210)) model->item(y210)->setEnabled(enabled[channel]);
        combos[channel]->setToolTip(
            enabled[channel]
                ? QStringLiteral("Requested CH%1 capture output format").arg(channel)
                : QStringLiteral("Y210 is already selected by the other channel"));
    }
}

void MainWindow::updateUiState()
{
    const bool open = controller_->deviceOpen();
    const bool running[] = {controller_->channelRunning(0), controller_->channelRunning(1)};
    const bool anyRunning = running[0] || running[1];
    const bool selected = ui_->deviceCombo->currentData().toInt() >= 0;
    ui_->openButton->setText(open ? QStringLiteral("Close Device") : QStringLiteral("Open Device"));
    ui_->openButton->setEnabled(!anyRunning);
    ui_->refreshButton->setEnabled(!open); ui_->deviceCombo->setEnabled(!open);
    ui_->ch0StartButton->setEnabled(selected && !running[0]); ui_->ch1StartButton->setEnabled(selected && !running[1]);
    ui_->ch0StopButton->setEnabled(running[0]); ui_->ch1StopButton->setEnabled(running[1]);
    ui_->ch0PreviewButton->setEnabled(running[0] && controller_->frameAvailable(0));
    ui_->ch1PreviewButton->setEnabled(running[1] && controller_->frameAvailable(1));
    ui_->ch0FullscreenButton->setEnabled(ui_->ch0PreviewButton->isEnabled());
    ui_->ch1FullscreenButton->setEnabled(ui_->ch1PreviewButton->isEnabled());
    ui_->ch0OutputFormatCombo->setEnabled(!running[0]); ui_->ch1OutputFormatCombo->setEnabled(!running[1]);
    ui_->ch0ZeroCopyCheckBox->setEnabled(!controller_->channelOpened(0));
    ui_->ch1ZeroCopyCheckBox->setEnabled(!controller_->channelOpened(1));
    ui_->ch0AudioCheckBox->setEnabled(!running[0]);
    ui_->channel1GroupBox->setVisible(kChannel1UiVisible);
}

void MainWindow::appendLogLine(const QString &line) { ui_->logEdit->appendPlainText(line); }
