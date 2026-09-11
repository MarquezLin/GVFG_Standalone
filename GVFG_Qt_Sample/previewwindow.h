#pragma once

#include <QWidget>

class QKeyEvent;
class QMouseEvent;

QT_BEGIN_NAMESPACE
namespace Ui
{
class PreviewWindow;
}
QT_END_NAMESPACE

class PreviewWindow final : public QWidget
{
    Q_OBJECT

public:
    explicit PreviewWindow(QWidget *parent = nullptr);
    ~PreviewWindow() override;

    void *nativePreviewHandle() const;
    void setSourceSize(int sourceWidth, int sourceHeight);
    void closePreview();

public slots:
    void showPreview(int sourceWidth = 0, int sourceHeight = 0);
    void showFullscreenPreview(int sourceWidth = 0, int sourceHeight = 0);

signals:
    void previewVisibilityChanged(bool visible);

protected:
    void closeEvent(QCloseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    void enterFullscreen();
    void exitFullscreen();

    Ui::PreviewWindow *ui_ = nullptr;
    bool closeAllowed_ = false;
    int sourceWidth_ = 0;
    int sourceHeight_ = 0;
};
