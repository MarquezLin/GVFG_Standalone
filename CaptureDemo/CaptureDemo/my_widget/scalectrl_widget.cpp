#include "scalectrl_widget.h"

ScaleCtrlWidget::ScaleCtrlWidget(E_SCALE_POS scale_pos, int width, int height, QWidget *parent):QWidget(parent)
{
    width_ = width;
    height_ = height;
    scale_pos_ = scale_pos;
    scale_cb_ = nullptr;
    setMouseTracking(true);
}

void ScaleCtrlWidget::mousePressEvent(QMouseEvent *event)
{

}

void ScaleCtrlWidget::mouseMoveEvent(QMouseEvent *event)
{

}

void ScaleCtrlWidget::mouseReleaseEvent(QMouseEvent *event)
{

}
