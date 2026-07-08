#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QSemaphore>
#include <QMainWindow>
#include <memory>
#include <QLineEdit>
#include <QSpinBox>
#include "video_card.h"
#include "my_widget/gl_widget.h"
#include "yuv_widget.h"
#include "task.h"

enum
{
    TOPLEFT = 11,
    TOP = 12,
    TOPRIGHT = 13,
    LEFT = 21,
    CENTER = 22,
    RIGHT = 23,
    BUTTOMLEFT = 31,
    BUTTOM = 32,
    BUTTOMRIGHT = 33
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = 0);
    ~MainWindow();

    void init();

public:
    void mousePressEvent(QMouseEvent *);
    void mouseMoveEvent(QMouseEvent *);
    void mouseReleaseEvent(QMouseEvent *);
    void paintEvent(QPaintEvent *event);

private:
    void clickedStartBtn();
    void clickedStopBtn();
    void clickedCloseBtn();
    void clickedMaxOrNormalBtn();
    void clickedMinimizeBtn();

private:
    YUVWidget *gl_widget_[PCIE_S2MM_MAX_CHANNELS];

    QPushButton *btn_start_capture_[PCIE_S2MM_MAX_CHANNELS];
    QPushButton *btn_stop_capture_[PCIE_S2MM_MAX_CHANNELS];

    QPushButton *btn_init_;
    QPushButton *btn_uninit_;

    QLineEdit *edt_width_;
    QLineEdit *edt_height_;
    QLineEdit *edt_bar0_addr_;
    QLineEdit *edt_bar0_val_;
    QPushButton *btn_read_bar0_;
    QPushButton *btn_write_bar0_;

    QLineEdit *edt_bar1_addr_;
    QLineEdit *edt_bar1_val_;
    QPushButton *btn_read_bar1_;
    QPushButton *btn_write_bar1_;

    QSpinBox *spn_frame_index_;
    QPushButton *btn_read_frame_;

    QPushButton *btn_max_or_normal_;

    QPoint move_start_pos_;
    QPoint window_pos_;

    int CalCursorCol(QPoint pt);              // 计算鼠标X的位置
    int CalCursorPos(QPoint pt, int col_pos); // 计算鼠标的位置
    void setCursorShape(int cal_pos);         // 设置鼠标对应位置的形状

    int cal_cursor_pos_;
    bool is_dragging_;   // 是否在拖动窗口，不是缩放
    bool is_scaling;     // 是否在缩放状态
    bool prepare_scale_; // 是否准备缩放（鼠标在缩放区域)
    QRect pre_geometry_rect_;
    QPoint mouse_pos_;

    std::shared_ptr<VideoCard> video_card_;
    std::unique_ptr<char[]> screen_data_buffer_;
    std::mutex screen_data_buffer_mtx_;
    std::unique_ptr<std::thread> screen_capture_thread_ = nullptr;
    bool capture_ = false;
};

#endif // MAINWINDOW_H
