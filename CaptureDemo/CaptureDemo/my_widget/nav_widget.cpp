#include "nav_widget.h"
#include <QStyleOption>
#include <QPainter>

NavWidget::NavWidget(QWidget *parent) : QWidget(parent)
{
    setProperty("type", QVariant("nav_bar"));
}

void NavWidget::mouseDoubleClickEvent(QMouseEvent *event) {
    emit doubleClicked();
}

void NavWidget::paintEvent(QPaintEvent *m)
{
    QStyleOption opt;
    opt.initFrom(this);
    QPainter painter(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &painter, this);
}
