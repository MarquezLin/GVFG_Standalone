#include "gl_widget.h"
#include <QtOpenGL/QOpenGLShaderProgram>
#include <QtOpenGL/QOpenGLTexture>
#include <QMouseEvent>
#include <QMenu>
#include <math.h>
#include <QDebug>

#define VERTEXIN 0
#define TEXTUREIN 1
#define POSITION 2

#define SHOW_DRAW 0
#define SHOW_RGB 1

GLfloat GL_DETEDT_COLOR[4] = {0, 1, 0, 1};
GLfloat GL_DRAWING_COLOR[4] = {1, 0, 0, 1};
GLfloat GL_SELECTED_COLOR[4] = {1, 1, 1, 1};
GLfloat GL_MAX_COLOR[4] = {1, 1, 0, 1};
GLfloat GL_MIN_COLOR[4] = {1, 0, 1, 1};
GLfloat GL_MASK_COLOR[4] = {0.5, 0.5, 0.5, 1};

GLWidget::GLWidget(QWidget *parent) : QOpenGLWidget(parent)
{
    init_ = false;
    img_width_ = 0;
    img_height_ = 0;
    widget_width_ = 0;
    widget_height_ = 0;

    curr_video_frame_ = nullptr;
    video_frame_free_queue_ = std::make_shared<std::queue<std::shared_ptr<T_VideoFrame>>>();
    video_frame_busy_queue_ = std::make_shared<std::queue<std::shared_ptr<T_VideoFrame>>>();

    default_video_frame_ = std::make_shared<T_VideoFrame>();
    default_video_frame_->width = 1920;
    default_video_frame_->height = 1080;
    memset(default_video_frame_->yuv422, 0, default_video_frame_->width*default_video_frame_->height*2);
    for(int i = 0; i < 40; i++) {
        std::shared_ptr<T_VideoFrame> video_frame = std::make_shared<T_VideoFrame>();
        video_frame_free_queue_->push(video_frame);
    }
}

GLWidget::~GLWidget()
{
    exit_display_thread_ = true;
    video_busy_queue_sem_->release();
    if(display_thread_ && display_thread_->joinable()) {
        display_thread_->join();
    }
}

void GLWidget::showImg(const uchar *yuv422, uint width, uint height)
{
    std::shared_ptr<T_VideoFrame> video_frame;
    std::lock_guard<std::mutex> lck(video_queue_mutex_);
    if(video_frame_free_queue_->empty()) {
        video_frame = std::make_shared<T_VideoFrame>();
    } else {
        video_frame = video_frame_free_queue_->front();
    }

    memcpy(video_frame->yuv422, yuv422, width * height * 2);
    video_frame->width = width;
    video_frame->height = height;

    video_frame_busy_queue_->push(video_frame);
    video_busy_queue_sem_->release(1);
    memcpy(yuv422_data_, yuv422, width * height * 2);
    img_width_ = width;
    img_height_ = height;
    update();
    // qDebug() << "show img";
    // static int count = 0;
    // if (count == 0) {
    //     FILE *fp = fopen("img.yuv", "wb");
    //     fwrite((char*)rgb, 1, width*height*2, fp);
    //     fclose(fp);
    // }
    // count++;
}

void GLWidget::internalShowImg(std::shared_ptr<T_VideoFrame> video_frame)
{
    curr_video_frame_ = video_frame;
//    memcpy(rgb_data_, video_frame->rgb, video_frame->width * video_frame->height * 3);
//    img_width_ = video_frame->width;
//    img_height_ = video_frame->height;
    update();
}

void GLWidget::initializeGL()
{
    widget_width_ = this->geometry().width();
    widget_height_ = this->geometry().height();

    initializeOpenGLFunctions();
    glEnable(GL_DEPTH_TEST);

    static const GLfloat vertices[]{
        -1.0f,
        -1.0f,
        -1.0f,
        +1.0f,
        +1.0f,
        +1.0f,
        +1.0f,
        -1.0f,
        0.0f,
        1.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        1.0f,
        1.0f,
        0.0f,
        0.5f,
        -0.8f,
        -0.5f,
        0.5f,
        -0.5f,
        1.0f,
        1.0f,
    };

    QOpenGLShader *vshader = new QOpenGLShader(QOpenGLShader::Vertex, this);
    const char *vsrc =
        "attribute vec4 vertexIn;           \
    attribute vec2 textureIn;           \
    attribute vec4 vPosition;           \
    varying vec2 textureOut;            \
    void main(void)                     \
    {                                   \                           \
        gl_Position = vPosition;    \                          \
        textureOut = textureIn;         \
    }";

    if(!vshader->compileSourceCode(vsrc)) {
        return;
    }

    QOpenGLShader *fshader = new QOpenGLShader(QOpenGLShader::Fragment, this);
    const char *fsrc =
        "\
        varying vec2 textureOut;\
        uniform sampler2D texYUYV;\
        void main()\
        {\
            vec2 yuyv = texture2D(texYUYV, textureOut).rg;\
            float y = yuyv.r;\
            float u = yuyv.g - 0.5;\
            float v = yuyv.g - 0.5;\
            float r = y + 1.402 * v;\
            float g = y - 0.344 * u - 0.714 * v;\
            float b = y + 1.772 * u;\
            gl_FragColor = vec4(r, g, b, 1.0);\
        }";

    if(!fshader->compileSourceCode(fsrc)) {
        qDebug() << "Fragment shader compilation error:" << fshader->log();
        return;
    }
    program_ = new QOpenGLShaderProgram(this);

    if(!program_->addShader(vshader)) {
        return;
    }

    if(!program_->addShader(fshader)) {
        return;
    }

    if(!program_->link()) {
        return;
    }

    if(!program_->bind()) {
        return;
    }

    program_->enableAttributeArray(VERTEXIN);
    program_->enableAttributeArray(TEXTUREIN);
    program_->enableAttributeArray(POSITION);
    program_->setAttributeArray("vertexIn", GL_FLOAT, vertices, 2, 2 * sizeof(GLfloat));
    program_->enableAttributeArray("textureIn");
    program_->setAttributeArray("textureIn", GL_FLOAT, vertices + 8, 2, 2 * sizeof(GLfloat));
    program_->setAttributeArray("vPosition", GL_FLOAT, vertices + 16, 2, 2 * sizeof(GLfloat));

    texture_uniform_yuv_ = program_->uniformLocation("texYUYV");
    texture_yuv_ = new QOpenGLTexture(QOpenGLTexture::Target2D);
    if(!texture_yuv_->create()) {
        return;
    }
    id_yuv_ = texture_yuv_->textureId();

    glClearColor(0.0, 0.0, 1.0, 0.0);

    video_busy_queue_sem_ = std::make_shared<QSemaphore>(0);

    exit_display_thread_ = false;
    display_thread_ = std::make_shared<std::thread>([&](){
        while(1) {
            if(!video_busy_queue_sem_->tryAcquire(1, 10)) {
                continue;
            }
            if(exit_display_thread_) {
                break;
            }
            std::lock_guard<std::mutex> lck(video_queue_mutex_);
            curr_video_frame_ = video_frame_busy_queue_->front();
            video_frame_busy_queue_->pop();
            update();
            //internalShowImg(video_frame);
        }
    });
}

void GLWidget::drawYUV422()
{
    if(!curr_video_frame_) {
        // glClearColor(0.0f,0.0f,0.0f,0.0f);
        // glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        // glActiveTexture(GL_TEXTURE0);
        // glBindTexture(GL_TEXTURE_2D, id_yuv_);
        // glTexImage2D(GL_TEXTURE_2D, 0, GL_RG, default_video_frame_->width, default_video_frame_->height, 0, GL_RG, GL_UNSIGNED_BYTE, default_video_frame_->yuv422);
        // glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        // glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        // glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        // glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // glUniform1i(texture_uniform_yuv_, 0);
        // glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        return;
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, id_yuv_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG, curr_video_frame_->width, curr_video_frame_->height, 0, GL_RG, GL_UNSIGNED_BYTE, curr_video_frame_->yuv422);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glUniform1i(texture_uniform_yuv_, 0);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    qDebug() << "draw triangle";
    //显示完后，归还到free队列
    std::lock_guard<std::mutex> lck(video_queue_mutex_);
    video_frame_free_queue_->push(curr_video_frame_);
}

void GLWidget::paintGL()
{
    drawYUV422();
}
