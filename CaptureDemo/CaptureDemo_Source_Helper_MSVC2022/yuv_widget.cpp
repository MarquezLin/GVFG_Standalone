#include "yuv_widget.h"
#include <QOpenGLShaderProgram>
#include <QDebug>

// 顶点着色器
const char *vertexShader = R"(
    attribute vec4 aPos;
    attribute vec2 aTexCoord;
    varying vec2 vTexCoord;
    void main() {
        gl_Position = aPos;
        vTexCoord = aTexCoord;
    }
)";

// YUYV422 正确片元着色器
const char *fragmentShader = R"(
    varying vec2 vTexCoord;
    uniform sampler2D yuyvTexture;
    uniform vec2 imgSize;

    void main() {
        float x = vTexCoord.x * imgSize.x;
        float texU = floor(x * 0.5) / (imgSize.x * 0.5);
        vec2 uv = vec2(texU, vTexCoord.y);
        vec4 yuyv = texture2D(yuyvTexture, uv);

        float y0 = yuyv.r;
        float v  = yuyv.g;
        float y1 = yuyv.b;
        float u  = yuyv.a;

        vec3 rgb;
        if(mod(x, 2.0) < 1.0){
            rgb.r = y0 + 1.403 * (v - 0.5);
            rgb.g = y0 - 0.344 * (u - 0.5) - 0.714 * (v - 0.5);
            rgb.b = y0 + 1.773 * (u - 0.5);
        }else{
            rgb.r = y1 + 1.403 * (v - 0.5);
            rgb.g = y1 - 0.344 * (u - 0.5) - 0.714 * (v - 0.5);
            rgb.b = y1 + 1.773 * (u - 0.5);
        }
        gl_FragColor = vec4(rgb, 1.0);
    }
)";

YUVWidget::YUVWidget(QWidget *parent) : QOpenGLWidget(parent)
{
    // 必须在构造函数最早位置设置format
    // QSurfaceFormat fmt;
    // fmt.setRenderableType(QSurfaceFormat::OpenGLES);
    // fmt.setVersion(2, 0);
    // fmt.setDepthBufferSize(0);
    // setFormat(fmt);
}

YUVWidget::~YUVWidget()
{
    makeCurrent();
    if(m_textureY) glDeleteTextures(1, &m_textureY);
    if(m_vbo) glDeleteBuffers(1, &m_vbo);
    delete m_program;
    doneCurrent();
}

void YUVWidget::initializeGL()
{
    initializeOpenGLFunctions();
    qDebug() << "✅ initializeGL 已执行";

    // 编译着色器
    m_program = new QOpenGLShaderProgram(this);
    m_program->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShader);
    m_program->addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShader);
    m_program->link();

    // 创建纹理
    glGenTextures(1, &m_textureY);
    glBindTexture(GL_TEXTURE_2D, m_textureY);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // 顶点
    GLfloat vertices[] = {
        -1,-1,0, 0,1,
        1,-1,0, 1,1,
        -1,1,0,  0,0,
        1,1,0,   1,0
    };
    glGenBuffers(1, &m_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    // 属性
    int aPos = m_program->attributeLocation("aPos");
    glVertexAttribPointer(aPos,3,GL_FLOAT,GL_FALSE,5*sizeof(GLfloat),0);
    glEnableVertexAttribArray(aPos);

    int aTex = m_program->attributeLocation("aTexCoord");
    glVertexAttribPointer(aTex,2,GL_FLOAT,GL_FALSE,5*sizeof(GLfloat),(void*)(3*sizeof(GLfloat)));
    glEnableVertexAttribArray(aTex);
}

void YUVWidget::resizeGL(int w, int h)
{
    glViewport(0,0,w,h);
}

// ✅ 外部接口：任意线程可调用
void YUVWidget::renderYUV422(const uint8_t *yuyvData, int width, int height)
{
    if(!yuyvData || width<=0 || height<=0) return;
    QByteArray buf((const char*)yuyvData, width * height * 2);

    // 强制主线程执行（绝对安全）
    QMetaObject::invokeMethod(this, [=](){
        onRenderYUV422(buf, width, height);
    }, Qt::QueuedConnection);
}

// ✅ 主线程执行：纹理上传（修复所有崩溃）
void YUVWidget::onRenderYUV422(const QByteArray &data, int width, int height)
{
    m_imgW = width;
    m_imgH = height;

    makeCurrent(); // 必须先绑定上下文

    glBindTexture(GL_TEXTURE_2D, m_textureY);

    // ==============================
    // ✅ 正确格式：YUYV422 必须用 RGBA + width/2
    // ==============================
    glTexImage2D(GL_TEXTURE_2D, 0,
                 GL_RGBA,
                 width/2, height, 0,
                 GL_RGBA,
                 GL_UNSIGNED_BYTE,
                 data.constData());

    doneCurrent();
    update();
}

void YUVWidget::paintGL()
{
    if(m_imgW <=0 || m_imgH <=0) return;

    glClear(GL_COLOR_BUFFER_BIT);
    m_program->bind();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_textureY);
    m_program->setUniformValue("yuyvTexture", 0);
    m_program->setUniformValue("imgSize", QVector2D(m_imgW, m_imgH));

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_program->release();
}
