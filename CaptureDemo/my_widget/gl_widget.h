#ifndef GLWIDGET_H
#define GLWIDGET_H

#include <QtOpenGLWidgets/QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QtOpenGL/QOpenGLBuffer>
#include <QTimer>
#include <QFile>
#include <QRect>
#include <QVector>
#include <QPushButton>
#include <QContextMenuEvent>
#include <iostream>
#include <thread>
#include <memory>
#include <fstream>
#include <queue>
#include <mutex>
#include <QSemaphore>

QT_FORWARD_DECLARE_CLASS(QOpenGLShaderProgram)
QT_FORWARD_DECLARE_CLASS(QOpenGLTexture)

#define MAX_IMG_WIDTH 1920
#define MAX_IMG_HEIGHT 1280

typedef struct T_VideoFrame {
    size_t width;
    size_t height;
    char *yuv422;
    size_t buffer_len;
public:
    T_VideoFrame() {
        yuv422 = new char[MAX_IMG_WIDTH*MAX_IMG_HEIGHT*2];
        width = MAX_IMG_WIDTH;
        height = MAX_IMG_HEIGHT;
    }
};

class GLWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT
  public:
    GLWidget(QWidget *parent = nullptr);
    virtual ~GLWidget();

  public:
    void showImg(const uchar *rgb, uint width, uint height);
  protected:
    void initializeGL() Q_DECL_OVERRIDE;
    void paintGL() Q_DECL_OVERRIDE;
    void drawYUV422();
    void internalShowImg(std::shared_ptr<T_VideoFrame> video_frame);
  private:
    std::shared_ptr<std::thread> display_thread_;
    bool exit_display_thread_;
    std::mutex video_queue_mutex_;
    std::shared_ptr<T_VideoFrame> curr_video_frame_;
    std::shared_ptr<T_VideoFrame> default_video_frame_;
    std::shared_ptr<std::queue<std::shared_ptr<T_VideoFrame>>> video_frame_free_queue_;
    std::shared_ptr<std::queue<std::shared_ptr<T_VideoFrame>>> video_frame_busy_queue_;
    std::shared_ptr<QSemaphore> video_busy_queue_sem_;
    uchar yuv422_data_[MAX_IMG_WIDTH * MAX_IMG_HEIGHT * 3];
    bool init_;
    int img_width_;
    int img_height_;
    int widget_width_;
    int widget_height_;

    QOpenGLShaderProgram *program_;
    QOpenGLTexture *texture_yuv_;
    GLuint texture_uniform_yuv_;
    GLuint id_yuv_;

    std::ofstream log_file_;
};

#endif
