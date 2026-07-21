#ifndef YUV_WIDGET_H
#define YUV_WIDGET_H

#include <QtOpenGLWidgets/QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLBuffer>
#include <QByteArray>

QT_FORWARD_DECLARE_CLASS(QOpenGLShaderProgram)
QT_FORWARD_DECLARE_CLASS(QOpenGLTexture)

class YUVWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT
public:
    explicit YUVWidget(QWidget *parent = nullptr);
    ~YUVWidget();

    void renderYUV422(const uint8_t *yuyvData, int width, int height);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private slots:
    void onRenderYUV422(const QByteArray &data, int width, int height);

private:
    QOpenGLShaderProgram *m_program = nullptr;
    GLuint m_textureY = 0;
    int m_imgW = 0, m_imgH = 0;
    GLuint m_vbo = 0;
};

#endif
