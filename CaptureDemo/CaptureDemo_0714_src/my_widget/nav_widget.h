#ifndef NAV_WIDGET_H
#define NAV_WIDGET_H

#include <QWidget>

class NavWidget : public QWidget
{
    Q_OBJECT
public:
    explicit NavWidget(QWidget *parent = nullptr);
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *m) override;
signals:
    void doubleClicked();
public slots:
};

#endif // NAV_WIDGET_H
