#include "previewwindow.h"
#include "ui_previewwindow.h"

#include <QGuiApplication>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QScreen>
#include <QSize>
#include <algorithm>

PreviewWindow::PreviewWindow(QWidget *parent)
    : QWidget(parent), ui_(new Ui::PreviewWindow)
{
    ui_->setupUi(this);
    ui_->verticalLayout->setContentsMargins(0, 0, 0, 0);
    ui_->verticalLayout->setSpacing(0);
    ui_->previewHost->setAttribute(Qt::WA_NativeWindow, true);
    ui_->previewHost->setStyleSheet(QStringLiteral("background: black;"));
}

PreviewWindow::~PreviewWindow()
{
    delete ui_;
}

void *PreviewWindow::nativePreviewHandle() const
{
    return reinterpret_cast<void *>(ui_->previewHost->winId());
}

void PreviewWindow::setSourceSize(int sourceWidth, int sourceHeight)
{
    if (sourceWidth <= 0 || sourceHeight <= 0)
        return;

    if (sourceWidth_ == sourceWidth && sourceHeight_ == sourceHeight && isVisible())
        return;

    sourceWidth_ = sourceWidth;
    sourceHeight_ = sourceHeight;

    if (isFullScreen())
        return;

    QScreen *targetScreen = screen();
    if (!targetScreen)
        targetScreen = QGuiApplication::primaryScreen();

    ui_->previewHost->setMinimumSize(QSize(1, 1));
    ui_->previewHost->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);

    const QSize currentHostSize = ui_->previewHost->size();
    const QSize windowOverhead = size() - currentHostSize;

    QSize targetSize(sourceWidth, sourceHeight);
    if (targetScreen)
    {
        const QSize maxSize = targetScreen->availableGeometry().size() - windowOverhead - QSize(16, 48);
        const QSize boundedSize(std::max(320, maxSize.width()), std::max(180, maxSize.height()));
        if (targetSize.width() > boundedSize.width() || targetSize.height() > boundedSize.height())
            targetSize.scale(boundedSize, Qt::KeepAspectRatio);
    }

    resize(targetSize + windowOverhead);
}

void PreviewWindow::closePreview()
{
    closeAllowed_ = true;
    emit previewVisibilityChanged(false);
    close();
}

void PreviewWindow::showPreview(int sourceWidth, int sourceHeight)
{
    closeAllowed_ = false;
    setSourceSize(sourceWidth, sourceHeight);
    if (isFullScreen())
        showNormal();
    show();
    emit previewVisibilityChanged(true);
    raise();
    activateWindow();
}

void PreviewWindow::showFullscreenPreview(int sourceWidth, int sourceHeight)
{
    closeAllowed_ = false;
    setSourceSize(sourceWidth, sourceHeight);
    enterFullscreen();
    emit previewVisibilityChanged(true);
}

void PreviewWindow::enterFullscreen()
{
    showFullScreen();
    raise();
    activateWindow();
}

void PreviewWindow::exitFullscreen()
{
    if (!isFullScreen())
        return;

    showNormal();
    raise();
    activateWindow();
}

void PreviewWindow::closeEvent(QCloseEvent *event)
{
    if (closeAllowed_)
    {
        event->accept();
        return;
    }

    hide();
    emit previewVisibilityChanged(false);
    event->ignore();
}

void PreviewWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape && isFullScreen())
    {
        exitFullscreen();
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_F11)
    {
        isFullScreen() ? exitFullscreen() : enterFullscreen();
        event->accept();
        return;
    }

    QWidget::keyPressEvent(event);
}

void PreviewWindow::mouseDoubleClickEvent(QMouseEvent *event)
{
    isFullScreen() ? exitFullscreen() : enterFullscreen();
    event->accept();
}
