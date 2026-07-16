#include "mainwindow.h"
#include <QVariant>
#include <QVBoxLayout>
#include <QCoreApplication>
#include <QPushButton>
#include <QApplication>
#include <QString>
#include <QDebug>
#include <QButtonGroup>
#include <QPainter>
#include <QPainterPath>
#include <QGraphicsDropShadowEffect>
#include <QLabel>
#include <QScreen>
#include <conio.h>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QDir>
#include "my_widget/nav_widget.h"
#define FRAMESHAPE 10

constexpr ULONG FOURCC_V210 = 0x76323130;
constexpr int GVFG_PIXFMT_Y210 = 7;
constexpr int GVFG_PIXFMT_V210 = 9;

struct GvfgFrame
{
    const void *data;
    uint64_t data_size;
    int width;
    int height;
    int pixel_format;
    int bit_depth;
    uint64_t frame_id;
};

struct GvfgPreviewRuntime
{
    using CreateFn = int (*)(void **);
    using DestroyFn = int (*)(void *);
    using AttachWindowFn = int (*)(void *, void *);
    using RenderFrameFn = int (*)(void *, const GvfgFrame *);
    using ShutdownFn = int (*)(void *);
    using StrErrorFn = const char *(*)(int);

    HMODULE dll = nullptr;
    void *handle = nullptr;
    CreateFn create = nullptr;
    DestroyFn destroy = nullptr;
    AttachWindowFn attachWindow = nullptr;
    RenderFrameFn renderFrame = nullptr;
    ShutdownFn shutdownFn = nullptr;
    StrErrorFn strError = nullptr;
    HWND attachedHwnd = nullptr;

    ~GvfgPreviewRuntime()
    {
        shutdown();
        if (dll)
        {
            FreeLibrary(dll);
            dll = nullptr;
        }
    }

    static QString findPreviewDll()
    {
        QDir dir(QCoreApplication::applicationDirPath());
        for (int i = 0; i < 8; ++i)
        {
            const QString debugPath = dir.filePath("build/Desktop_Qt_6_10_2_MSVC2022_64bit-Debug/bin/gvfg_preview.dll");
            if (QFileInfo::exists(debugPath))
                return QDir::toNativeSeparators(debugPath);

            const QString releasePath = dir.filePath("build/Desktop_Qt_6_10_2_MSVC2022_64bit-Release/bin/gvfg_preview.dll");
            if (QFileInfo::exists(releasePath))
                return QDir::toNativeSeparators(releasePath);

            if (!dir.cdUp())
                break;
        }
        return QString();
    }

    bool load()
    {
        if (dll)
            return true;

        const QString dllPath = findPreviewDll();
        if (dllPath.isEmpty())
        {
            qWarning() << "gvfg_preview.dll not found";
            return false;
        }

        dll = LoadLibraryExW(reinterpret_cast<LPCWSTR>(dllPath.utf16()), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!dll)
        {
            qWarning() << "LoadLibraryEx failed for" << dllPath << "error:" << GetLastError();
            return false;
        }

        create = reinterpret_cast<CreateFn>(GetProcAddress(dll, "gvfg_preview_create"));
        destroy = reinterpret_cast<DestroyFn>(GetProcAddress(dll, "gvfg_preview_destroy"));
        attachWindow = reinterpret_cast<AttachWindowFn>(GetProcAddress(dll, "gvfg_preview_attach_window"));
        renderFrame = reinterpret_cast<RenderFrameFn>(GetProcAddress(dll, "gvfg_preview_render_frame"));
        shutdownFn = reinterpret_cast<ShutdownFn>(GetProcAddress(dll, "gvfg_preview_shutdown"));
        strError = reinterpret_cast<StrErrorFn>(GetProcAddress(dll, "gvfg_preview_strerror"));

        if (!create || !destroy || !attachWindow || !renderFrame || !shutdownFn)
        {
            qWarning() << "gvfg_preview.dll missing required exports";
            return false;
        }

        int st = create(&handle);
        if (st != 0 || !handle)
        {
            qWarning() << "gvfg_preview_create failed:" << statusText(st);
            return false;
        }

        qDebug() << "gvfg_preview loaded:" << dllPath;
        return true;
    }

    bool attach(HWND hwnd)
    {
        if (!load() || !hwnd)
            return false;
        if (attachedHwnd == hwnd)
            return true;

        int st = attachWindow(handle, hwnd);
        if (st != 0)
        {
            qWarning() << "gvfg_preview_attach_window failed:" << statusText(st);
            return false;
        }

        attachedHwnd = hwnd;
        return true;
    }

    bool renderFrameData(const void *data, uint64_t size, int width, int height, int pixelFormat, int bitDepth, uint64_t frameId)
    {
        if (!handle || !data)
            return false;

        GvfgFrame frame = {};
        frame.data = data;
        frame.data_size = size;
        frame.width = width;
        frame.height = height;
        frame.pixel_format = pixelFormat;
        frame.bit_depth = bitDepth;
        frame.frame_id = frameId;

        int st = renderFrame(handle, &frame);
        if (st != 0)
        {
            qWarning() << "gvfg_preview_render_frame failed:" << statusText(st);
            return false;
        }
        return true;
    }

    void shutdown()
    {
        if (handle)
        {
            if (shutdownFn)
                shutdownFn(handle);
            if (destroy)
                destroy(handle);
            handle = nullptr;
        }
        attachedHwnd = nullptr;
    }

    const char *statusText(int st) const
    {
        return strError ? strError(st) : "unknown";
    }
};

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setProperty("type", QVariant("mainwindow"));
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowSystemMenuHint | Qt::WindowMinMaxButtonsHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
    setMinimumSize(1200, 600);
    is_dragging_ = false;
    is_scaling = false;
}

MainWindow::~MainWindow()
{
    //    if(write_worker_) {
    //        write_worker_->stop();
    //        write_worker_.reset();
    //    }
}

void MainWindow::init()
{
    QWidget *main_widget = new QWidget(this);
    QVBoxLayout *main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(8, 8, 8, 8);
    main_layout->setSpacing(0);
    // main_layout->setContentsMargins(0);
    main_layout->setSizeConstraint(QLayout::SetMaximumSize);

    NavWidget *nav_widget = new NavWidget();
    nav_widget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    nav_widget->setFixedHeight(40);
    nav_widget->setMouseTracking(true);

    QLabel *title = new QLabel("CaptureDemo");
    title->setProperty("type", QVariant("nav_title"));

    QPushButton *btn_about = new QPushButton("深圳市拓新高电子科技有限公司");
    btn_about->setProperty("type", QVariant("nav_btn"));
    btn_about->setCursor(Qt::PointingHandCursor);
    btn_about->setCheckable(true);

    QPushButton *btn_minimize = new QPushButton;
    btn_minimize->setProperty("type", QVariant("nav_min_btn"));
    btn_minimize->setCursor(Qt::PointingHandCursor);
    btn_minimize->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    btn_minimize->setFixedSize(20, 20);
    connect(btn_minimize, &QPushButton::clicked, this, &MainWindow::clickedMinimizeBtn);

    btn_max_or_normal_ = new QPushButton;
    btn_max_or_normal_->setProperty("type", QVariant("nav_max_btn"));
    btn_max_or_normal_->setCursor(Qt::PointingHandCursor);
    btn_max_or_normal_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    btn_max_or_normal_->setFixedSize(20, 20);
    btn_max_or_normal_->setCheckable(true);
    connect(btn_max_or_normal_, &QPushButton::clicked, this, &MainWindow::clickedMaxOrNormalBtn);

    QPushButton *btn_close = new QPushButton;
    btn_close->setProperty("type", QVariant("nav_close_btn"));
    btn_close->setCursor(Qt::PointingHandCursor);
    btn_close->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    btn_close->setFixedSize(20, 20);
    connect(btn_close, &QPushButton::clicked, this, &MainWindow::clickedCloseBtn);

    QHBoxLayout *nav_layout = new QHBoxLayout;
    // nav_layout->setContentsMargins(0);
    nav_layout->setSpacing(0);
    nav_layout->setContentsMargins(8, 0, 8, 0);
    nav_layout->addWidget(title, 0, Qt::AlignLeft | Qt::AlignVCenter);

    nav_layout->addStretch(9);
    nav_layout->addWidget(btn_about, 0, Qt::AlignRight | Qt::AlignBottom);
    nav_layout->addStretch(1);
    nav_layout->addWidget(btn_minimize, 0, Qt::AlignRight | Qt::AlignVCenter);
    nav_layout->addSpacing(10);
    nav_layout->addWidget(btn_max_or_normal_, 0, Qt::AlignRight | Qt::AlignVCenter);
    nav_layout->addSpacing(10);
    nav_layout->addWidget(btn_close, 0, Qt::AlignRight | Qt::AlignVCenter);
    nav_widget->setLayout(nav_layout);
    main_layout->addWidget(nav_widget);
    connect(nav_widget, &NavWidget::doubleClicked, this, &MainWindow::clickedMaxOrNormalBtn);

    QHBoxLayout *gl_layout = new QHBoxLayout;
    gl_layout->setSpacing(4);
    gl_layout->setContentsMargins(0, 0, 0, 0);

    for (int i = 0; i < PCIE_S2MM_MAX_CHANNELS; i++)
    {
        gl_widget_[i] = new YUVWidget(this);
        gl_widget_[i]->setAttribute(Qt::WA_NativeWindow);
        gl_widget_[i]->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        gl_widget_[i]->setMouseTracking(true);
        gl_layout->addWidget(gl_widget_[i]);
    }

    main_layout->addLayout(gl_layout);

    // QHBoxLayout *wh_layout = new QHBoxLayout;
    // // wh_layout->setContentsMargins(0);
    // wh_layout->setSpacing(4);
    // wh_layout->setContentsMargins(25, 4, 25, 4);
    // wh_layout->setAlignment(Qt::AlignLeft);
    // QLabel *tmp = new QLabel("长：");
    // tmp->setProperty("type", QVariant("nav_title"));
    // wh_layout->addWidget(tmp);

    // edt_width_ = new QLineEdit;
    // edt_width_->setFixedWidth(100);
    // edt_width_->setText("1920");
    // wh_layout->addWidget(edt_width_, 0, Qt::AlignLeft);

    // QLabel *tmp1 = new QLabel("宽：");
    // tmp1->setProperty("type", QVariant("nav_title"));
    // wh_layout->addWidget(tmp1);

    // edt_height_ = new QLineEdit;
    // edt_height_->setFixedWidth(100);
    // edt_height_->setText("1080");
    // wh_layout->addWidget(edt_height_, 0, Qt::AlignLeft);

    // main_layout->addLayout(wh_layout);

    QHBoxLayout *bar0_layout = new QHBoxLayout;
    bar0_layout->setSpacing(4);
    bar0_layout->setContentsMargins(25, 4, 25, 4);
    bar0_layout->setAlignment(Qt::AlignLeft);

    QLabel *bar0_addr_label = new QLabel("BAR0地址：");
    bar0_addr_label->setProperty("type", QVariant("nav_title"));
    bar0_layout->addWidget(bar0_addr_label);

    edt_bar0_addr_ = new QLineEdit;
    edt_bar0_addr_->setFixedWidth(100);
    edt_bar0_addr_->setPlaceholderText("0x");
    bar0_layout->addWidget(edt_bar0_addr_, 0, Qt::AlignLeft);

    QLabel *bar0_val_label = new QLabel("值：");
    bar0_val_label->setProperty("type", QVariant("nav_title"));
    bar0_layout->addWidget(bar0_val_label);

    edt_bar0_val_ = new QLineEdit;
    edt_bar0_val_->setFixedWidth(100);
    edt_bar0_val_->setPlaceholderText("0x");
    bar0_layout->addWidget(edt_bar0_val_, 0, Qt::AlignLeft);

    btn_read_bar0_ = new QPushButton("读BAR0");
    btn_read_bar0_->setProperty("type", QVariant("gl_btn"));
    btn_read_bar0_->setProperty("color", QVariant("white"));
    btn_read_bar0_->setCursor(Qt::PointingHandCursor);
    btn_read_bar0_->setFixedWidth(80);
    bar0_layout->addWidget(btn_read_bar0_);
    connect(btn_read_bar0_, &QPushButton::clicked, [=]()
            {
        if (!video_card_) return;
        bool ok;
        ULONG addr = edt_bar0_addr_->text().toULong(&ok, 0);
        if (!ok) {
            qWarning() << "BAR0 read: invalid address";
            return;
        }
        ULONG val = 0;
        if (video_card_->readReg(addr, val)) {
            edt_bar0_val_->setText(QString("0x%1").arg(val, 8, 16, QChar('0')));
            qDebug() << "BAR0 read: addr=0x" << Qt::hex << addr << " val=0x" << val;
        } });

    btn_write_bar0_ = new QPushButton("写BAR0");
    btn_write_bar0_->setProperty("type", QVariant("gl_btn"));
    btn_write_bar0_->setProperty("color", QVariant("white"));
    btn_write_bar0_->setCursor(Qt::PointingHandCursor);
    btn_write_bar0_->setFixedWidth(80);
    bar0_layout->addWidget(btn_write_bar0_);
    connect(btn_write_bar0_, &QPushButton::clicked, [=]()
            {
        if (!video_card_) return;
        bool ok1, ok2;
        ULONG addr = edt_bar0_addr_->text().toULong(&ok1, 0);
        ULONG val = edt_bar0_val_->text().toULong(&ok2, 0);
        if (!ok1 || !ok2) {
            qWarning() << "BAR0 write: invalid address or value";
            return;
        }
        if (video_card_->writeReg(addr, val)) {
            qDebug() << "BAR0 write: addr=0x" << Qt::hex << addr << " val=0x" << val;
        } });

    main_layout->addLayout(bar0_layout);

    QHBoxLayout *bar1_layout = new QHBoxLayout;
    bar1_layout->setSpacing(4);
    bar1_layout->setContentsMargins(25, 4, 25, 4);
    bar1_layout->setAlignment(Qt::AlignLeft);

    QLabel *bar1_addr_label = new QLabel("BAR1地址：");
    bar1_addr_label->setProperty("type", QVariant("nav_title"));
    bar1_layout->addWidget(bar1_addr_label);

    edt_bar1_addr_ = new QLineEdit;
    edt_bar1_addr_->setFixedWidth(100);
    edt_bar1_addr_->setPlaceholderText("0x");
    bar1_layout->addWidget(edt_bar1_addr_, 0, Qt::AlignLeft);

    QLabel *bar1_val_label = new QLabel("值：");
    bar1_val_label->setProperty("type", QVariant("nav_title"));
    bar1_layout->addWidget(bar1_val_label);

    edt_bar1_val_ = new QLineEdit;
    edt_bar1_val_->setFixedWidth(100);
    edt_bar1_val_->setPlaceholderText("0x");
    bar1_layout->addWidget(edt_bar1_val_, 0, Qt::AlignLeft);

    btn_read_bar1_ = new QPushButton("读BAR1");
    btn_read_bar1_->setProperty("type", QVariant("gl_btn"));
    btn_read_bar1_->setProperty("color", QVariant("white"));
    btn_read_bar1_->setCursor(Qt::PointingHandCursor);
    btn_read_bar1_->setFixedWidth(80);
    bar1_layout->addWidget(btn_read_bar1_);
    connect(btn_read_bar1_, &QPushButton::clicked, [=]()
            {
        if (!video_card_) return;
        bool ok;
        ULONG addr = edt_bar1_addr_->text().toULong(&ok, 0);
        if (!ok) {
            qWarning() << "BAR1 read: invalid address";
            return;
        }
        ULONG val = 0;
        if (video_card_->readRegBar1(addr, val)) {
            edt_bar1_val_->setText(QString("0x%1").arg(val, 8, 16, QChar('0')));
            qDebug() << "BAR1 read: addr=0x" << Qt::hex << addr << " val=0x" << val;
        } });

    btn_write_bar1_ = new QPushButton("写BAR1");
    btn_write_bar1_->setProperty("type", QVariant("gl_btn"));
    btn_write_bar1_->setProperty("color", QVariant("white"));
    btn_write_bar1_->setCursor(Qt::PointingHandCursor);
    btn_write_bar1_->setFixedWidth(80);
    bar1_layout->addWidget(btn_write_bar1_);
    connect(btn_write_bar1_, &QPushButton::clicked, [=]()
            {
        if (!video_card_) return;
        bool ok1, ok2;
        ULONG addr = edt_bar1_addr_->text().toULong(&ok1, 0);
        ULONG val = edt_bar1_val_->text().toULong(&ok2, 0);
        if (!ok1 || !ok2) {
            qWarning() << "BAR1 write: invalid address or value";
            return;
        }
        if (video_card_->writeRegBar1(addr, val)) {
            qDebug() << "BAR1 write: addr=0x" << Qt::hex << addr << " val=0x" << val;
        } });

    main_layout->addLayout(bar1_layout);

    QHBoxLayout *frame_read_layout = new QHBoxLayout;
    frame_read_layout->setSpacing(4);
    frame_read_layout->setContentsMargins(25, 4, 25, 4);
    frame_read_layout->setAlignment(Qt::AlignLeft);

    QLabel *frame_index_label = new QLabel("FrameIndex：");
    frame_index_label->setProperty("type", QVariant("nav_title"));
    frame_read_layout->addWidget(frame_index_label);

    spn_frame_index_ = new QSpinBox;
    spn_frame_index_->setRange(0, PCIE_S2MM_DMA_BUFFER_COUNT - 1);
    spn_frame_index_->setFixedWidth(100);
    frame_read_layout->addWidget(spn_frame_index_, 0, Qt::AlignLeft);

    // btn_read_frame_ = new QPushButton("读取");
    // btn_read_frame_->setProperty("type", QVariant("gl_btn"));
    // btn_read_frame_->setProperty("color", QVariant("white"));
    // btn_read_frame_->setCursor(Qt::PointingHandCursor);
    // btn_read_frame_->setFixedWidth(80);
    // frame_read_layout->addWidget(btn_read_frame_);
    // connect(btn_read_frame_, &QPushButton::clicked, [=]()
    //         {
    //     if (!video_card_ || !video_card_->is_initialized_) {
    //         qWarning() << "请先打开板卡";
    //         QMessageBox::warning(this, "提示", "请先打开板卡！");
    //         return;
    //     }

    //     bool ok1, ok2;
    //     int w = edt_width_->text().toInt(&ok1, 10);
    //     int h = edt_height_->text().toInt(&ok2, 10);
    //     if (!ok1 || !ok2 || w <= 0 || h <= 0) {
    //         qWarning() << "无效的宽高参数";
    //         QMessageBox::warning(this, "提示", "无效的宽高参数！");
    //         return;
    //     }

    //     DWORD frame_size = w * h * 2; // YUV422
    //     std::unique_ptr<BYTE[]> frame_data(new BYTE[frame_size]);
    //     ULONG frame_index = (ULONG)spn_frame_index_->value();

    //     int ret = video_card_->getFrame(frame_data.get(), frame_size, frame_index);
    //     if (ret < 0) {
    //         qWarning() << "读取帧失败, frameIndex:" << frame_index << " ret:" << ret;
    //         QMessageBox::warning(this, "提示", QString("读取帧失败！frameIndex=%1, ret=%2").arg(frame_index).arg(ret));
    //         return;
    //     }

    //     QString filename = QString("frame_%1_%2x%3.bin")
    //                            .arg(frame_index)
    //                            .arg(w)
    //                            .arg(h);
    //     QFile file(filename);
    //     if (!file.open(QIODevice::WriteOnly)) {
    //         qWarning() << "无法创建文件:" << filename;
    //         QMessageBox::warning(this, "提示", QString("无法创建文件：%1").arg(filename));
    //         return;
    //     }
    //     file.write((const char *)frame_data.get(), frame_size);
    //     file.close();

    //     qDebug() << "帧已保存:" << filename << "大小:" << frame_size << "字节";
    //     QMessageBox::information(this, "提示", QString("帧已保存到：%1\n大小：%2 字节").arg(filename).arg(frame_size)); });

    main_layout->addLayout(frame_read_layout);

    QWidget *btns_widget = new QWidget;
    QHBoxLayout *btns_layout = new QHBoxLayout;
    // btns_layout->setContentsMargins(0);
    btns_layout->setSpacing(4);
    btns_layout->setContentsMargins(0, 0, 0, 0);

    video_card_ = std::make_shared<VideoCard>();

    QButtonGroup *btn_group = new QButtonGroup;
    btn_init_ = new QPushButton("打开板卡");
    btn_init_->setProperty("type", QVariant("gl_btn"));
    btn_init_->setProperty("color", QVariant("white"));
    btn_init_->setCheckable(true);
    btn_init_->setCursor(Qt::PointingHandCursor);
    btn_init_->setFixedWidth(100);
    btn_init_->setEnabled(true);
    btns_layout->addWidget(btn_init_);
    connect(btn_init_, &QPushButton::clicked, [=]()
            {
        if(video_card_) {
            bool ok1;
            bool ok2;
            // int w = edt_width_->text().toInt(&ok1, 10);
            // int h = edt_height_->text().toInt(&ok2, 10);
            // if (!ok1 || !ok2) {
            //     qDebug() << "error width or height";
            //     return;
            // }

            if(0 != video_card_->init()) {
                qDebug() << "video card init failed.";
                return;
            }

            // edt_width_->setEnabled(false);
            // edt_height_->setEnabled(false);

                for (int i = 0; i < PCIE_S2MM_MAX_CHANNELS; i++)
                    btn_start_capture_[i]->setEnabled(true);
            btn_init_->setEnabled(false);
            btn_uninit_->setEnabled(true);
        } });

    for (int i = 0; i < PCIE_S2MM_MAX_CHANNELS; i++)
    {
        QString start_text = QString("开始采集 CH%1").arg(i);
        btn_start_capture_[i] = new QPushButton(start_text);
        btn_start_capture_[i]->setProperty("type", QVariant("gl_btn"));
        btn_start_capture_[i]->setProperty("color", QVariant("white"));
        btn_start_capture_[i]->setCheckable(true);
        btn_start_capture_[i]->setCursor(Qt::PointingHandCursor);
        btn_start_capture_[i]->setFixedWidth(120);
        btn_start_capture_[i]->setEnabled(false);
        btns_layout->addWidget(btn_start_capture_[i]);
        connect(btn_start_capture_[i], &QPushButton::clicked, [=]()
                {
            if(video_card_) {
                bool ok1, ok2;
                // int w = edt_width_->text().toInt(&ok1, 10);
                // int h = edt_height_->text().toInt(&ok2, 10);
                // if (!ok1 || !ok2) {
                //     qDebug() << "error width or height";
                //     return;
                // }
                int ch = i; // capture channel index
                ULONG width;
                ULONG height;
                ULONG fourcc;
                video_card_->readReg(CH_VIDEO_BASE(i) + VIDEO_HSIZE_OFFSET, width);
                video_card_->readReg(CH_VIDEO_BASE(i) + VIDEO_VSIZE_OFFSET, height);
                video_card_->readReg(CH_VIDEO_BASE(i) + VIDEO_FORMAT_OFFSET, fourcc);

                if (width == 0 || height == 0) {
                    qDebug() << "ch:" << ch << ", width:" << width << ", height:" << height;
                    return;
                }
                qDebug() << "ch:" << ch << ", width:" << width << ", height:" << height
                         << ", fourcc: 0x" << Qt::hex << fourcc;

                HWND previewHwnd = nullptr;
                if (fourcc == FOURCC_V210) {
                    if (!preview_runtime_[ch])
                        preview_runtime_[ch] = std::make_unique<GvfgPreviewRuntime>();
                    gl_widget_[ch]->setUpdatesEnabled(false);
                    previewHwnd = reinterpret_cast<HWND>(gl_widget_[ch]->winId());
                    if (!preview_runtime_[ch]->attach(previewHwnd)) {
                        qWarning() << "failed to attach gvfg preview for ch" << ch;
                    }
                }

                auto cb = [=](uchar *data, int video_width, int video_height) {
                    if (fourcc == FOURCC_V210 && preview_runtime_[ch]) {
                        const uint64_t frameSize = (uint64_t)(((video_width + 5) / 6) * 16) * (uint64_t)video_height;
                        preview_runtime_[ch]->renderFrameData(data,
                                                              frameSize,
                                                              video_width,
                                                              video_height,
                                                              GVFG_PIXFMT_V210,
                                                              10,
                                                              0);
                    } else {
                        gl_widget_[ch]->renderYUV422((const uchar *)data, video_width, video_height);
                    }
                };

                if(0 != video_card_->startCapture(ch, width, height, fourcc, cb)) {
                    qDebug() << "video card startCapture failed for ch" << ch;
                    return;
                }

                btn_stop_capture_[ch]->setEnabled(true);
                btn_start_capture_[ch]->setEnabled(false);
            } });

        QString stop_text = QString("停止采集 CH%1").arg(i);
        btn_stop_capture_[i] = new QPushButton(stop_text);
        btn_stop_capture_[i]->setProperty("type", QVariant("gl_btn"));
        btn_stop_capture_[i]->setProperty("color", QVariant("white"));
        btn_stop_capture_[i]->setCheckable(true);
        btn_stop_capture_[i]->setCursor(Qt::PointingHandCursor);
        btn_stop_capture_[i]->setFixedWidth(120);
        btn_stop_capture_[i]->setEnabled(false);
        btns_layout->addWidget(btn_stop_capture_[i]);
        connect(btn_stop_capture_[i], &QPushButton::clicked, [=]()
                {
                if(video_card_) {
                    int ch = i; // capture channel index
                    video_card_->stopCapture(ch);
                    if (preview_runtime_[ch])
                        preview_runtime_[ch]->shutdown();
                    gl_widget_[ch]->setUpdatesEnabled(true);
                    btn_start_capture_[ch]->setEnabled(true);
                    btn_stop_capture_[ch]->setEnabled(false);
                } });
    }

    btn_uninit_ = new QPushButton("关闭板卡");
    btn_uninit_->setProperty("type", QVariant("gl_btn"));
    btn_uninit_->setProperty("color", QVariant("white"));
    btn_uninit_->setCheckable(true);
    btn_uninit_->setCursor(Qt::PointingHandCursor);
    btn_uninit_->setFixedWidth(100);
    btn_uninit_->setEnabled(false);
    btns_layout->addWidget(btn_uninit_);
    connect(btn_uninit_, &QPushButton::clicked, [=]()
            {
        if(video_card_) {
            video_card_->uninit();
            for (int i = 0; i < PCIE_S2MM_MAX_CHANNELS; i++) {
                if (preview_runtime_[i])
                    preview_runtime_[i]->shutdown();
                gl_widget_[i]->setUpdatesEnabled(true);
            }
            btn_init_->setEnabled(true);
            btn_uninit_->setEnabled(false);
                for (int i = 0; i < PCIE_S2MM_MAX_CHANNELS; i++) {
                    btn_start_capture_[i]->setEnabled(false);
                    btn_stop_capture_[i]->setEnabled(false);
                }
            // edt_width_->setEnabled(true);
            // edt_height_->setEnabled(true);
        } });

    btns_widget->setLayout(btns_layout);
    btns_widget->setMouseTracking(true);

    main_layout->addSpacing(5);
    main_layout->addWidget(btns_widget);

    QWidget *bottom_widget = new QWidget;
    bottom_widget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    bottom_widget->setFixedHeight(5);
    bottom_widget->setProperty("type", QVariant("bottom"));
    main_layout->addWidget(bottom_widget);

    main_widget->setLayout(main_layout);
    main_widget->setProperty("type", QVariant("mainwindow"));
    setCentralWidget(main_widget);

    centralWidget()->setMouseTracking(true);
    setMouseTracking(true);
}

void MainWindow::mousePressEvent(QMouseEvent *event)
{
    cal_cursor_pos_ = CalCursorPos(event->pos(), CalCursorCol(event->pos()));
    if (event->button() == Qt::LeftButton /*&& Qt::WindowMaximized != windowState()*/)
    {
        if (cal_cursor_pos_ != CENTER)
        {
            is_dragging_ = false;
            is_scaling = true;
        }
        else
        {
            is_dragging_ = true;
            is_scaling = false;
        }
    }

    move_start_pos_ = event->globalPos();
    if (is_dragging_)
    {
        window_pos_ = this->frameGeometry().topLeft(); // 记录当前窗口位置
    }
    else if (is_scaling)
    {
        pre_geometry_rect_ = geometry();
    }
}

void MainWindow::clickedCloseBtn()
{
    if (screen_capture_thread_)
    {
        capture_ = false;
        screen_capture_thread_->join();
        screen_capture_thread_.reset();
        screen_capture_thread_ = nullptr;
    }

    if (video_card_)
    {
        video_card_->uninit();
        video_card_.reset();
        video_card_ = nullptr;
    }

    for (int i = 0; i < PCIE_S2MM_MAX_CHANNELS; i++)
    {
        if (preview_runtime_[i])
            preview_runtime_[i]->shutdown();
    }

    for (int i = 0; i < PCIE_S2MM_MAX_CHANNELS; i++)
    {
        if (gl_widget_[i])
        {
            delete gl_widget_[i];
            gl_widget_[i] = nullptr;
        }
    }

    QCoreApplication::instance()->quit();
}

void MainWindow::mouseMoveEvent(QMouseEvent *event)
{
    // 窗体不是最大的话就改变鼠标的形状
    if (Qt::WindowMaximized != windowState())
    {
        setCursorShape(CalCursorPos(event->pos(), CalCursorCol(event->pos())));
    }
    // 获取当前的点，这个点是全局的
    QPoint current_pos = QCursor::pos();
    // 计算出移动的位置，当前点 - 鼠标左键按下的点
    QPoint move_size = current_pos - move_start_pos_;
    QRect temp_geometry = pre_geometry_rect_;
    if (is_scaling)
    {
        switch (cal_cursor_pos_)
        {
        case TOPLEFT:
            temp_geometry.setTopLeft(pre_geometry_rect_.topLeft() + move_size);
            break;
        case TOP:
            temp_geometry.setTop(pre_geometry_rect_.top() + move_size.y());
            break;
        case TOPRIGHT:
            temp_geometry.setTopRight(pre_geometry_rect_.topRight() + move_size);
            break;
        case LEFT:
            temp_geometry.setLeft(pre_geometry_rect_.left() + move_size.x());
            break;
        case RIGHT:
            temp_geometry.setRight(pre_geometry_rect_.right() + move_size.x());
            break;
        case BUTTOMLEFT:
            temp_geometry.setBottomLeft(pre_geometry_rect_.bottomLeft() + move_size);
            break;
        case BUTTOM:
            temp_geometry.setBottom(pre_geometry_rect_.bottom() + move_size.y());
            break;
        case BUTTOMRIGHT:
            temp_geometry.setBottomRight(pre_geometry_rect_.bottomRight() + move_size);
            break;
        default:
            break;
        }
        setGeometry(temp_geometry);
    }
    else if (is_dragging_)
    {
        QPoint relative_pos = event->globalPos() - move_start_pos_;
        move(window_pos_ + relative_pos);
    }
}

void MainWindow::mouseReleaseEvent(QMouseEvent *event)
{
    is_scaling = false;
    is_dragging_ = false;
    QApplication::restoreOverrideCursor();
}

int MainWindow::CalCursorCol(QPoint pt)
{
    return (pt.x() < FRAMESHAPE ? 1 : ((pt.x() > this->width() - FRAMESHAPE) ? 3 : 2));
}

int MainWindow::CalCursorPos(QPoint pt, int col_pos)
{
    return ((pt.y() < FRAMESHAPE ? 10 : ((pt.y() > this->height() - FRAMESHAPE) ? 30 : 20)) + col_pos);
}

void MainWindow::setCursorShape(int cal_pos)
{
    Qt::CursorShape cursor;
    switch (cal_pos)
    {
    case TOPLEFT:
    case BUTTOMRIGHT:
        cursor = Qt::SizeFDiagCursor;
        break;
    case TOPRIGHT:
    case BUTTOMLEFT:
        cursor = Qt::SizeBDiagCursor;
        break;
    case TOP:
    case BUTTOM:
        cursor = Qt::SizeVerCursor;
        break;
    case LEFT:
    case RIGHT:
        cursor = Qt::SizeHorCursor;
        break;
    default:
        cursor = Qt::ArrowCursor;
        break;
    }
    setCursor(cursor);
}

void MainWindow::clickedMaxOrNormalBtn()
{
    if (Qt::WindowMaximized != windowState())
    {
        btn_max_or_normal_->setChecked(true);
        setWindowState(Qt::WindowMaximized);
        update();
    }
    else
    {
        btn_max_or_normal_->setChecked(false);
        setWindowState(Qt::WindowNoState);
        update();
    }
}

void MainWindow::clickedMinimizeBtn()
{
    showMinimized();
}

void MainWindow::paintEvent(QPaintEvent *event)
{
    //    if(Qt::WindowMaximized == windowState())
    //    {
    //        qDebug() << "in here";
    //        setContentsMargins(0,0,0,0);
    //        QWidget::paintEvent(event);
    //        return;
    //    }
    //    setContentsMargins(20,20,20,20);
    //    QPainterPath path;
    //    QRect rect = this->rect();
    //    rect.adjust(22,22,-22,-22);

    //    path.addRoundedRect(rect, 10, 10);
    //    QPainter painter(this);
    //    painter.setRenderHint(QPainter::Antialiasing, true);
    //    QColor color(40, 40, 40, 50);
    //    for(int i = 0; i < 20; i++)
    //    {
    //        int alpha = 130 - sqrt(i)*1.1*38;
    //        if(alpha <= 0) {
    //            break;
    //        }

    //        color.setAlpha(alpha);
    //        QPainterPath path;
    //        rect.adjust(-1, -1, 1, 1);
    //        path.addRoundedRect(rect, 10, 10);
    //        painter.fillPath(path, color);
    //    }

    QWidget::paintEvent(event);
}
