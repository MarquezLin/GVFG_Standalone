#-------------------------------------------------
#
# Project created by QtCreator 2018-12-06T23:31:36
#
#-------------------------------------------------

QT       += core gui openglwidgets

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET = CaptureDemo
TEMPLATE = app

# The following define makes your compiler emit warnings if you use
# any feature of Qt which has been marked as deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS QT_DEBUG

# You can also make your code fail to compile if you use deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

CONFIG += c++11
CONFIG += console

SOURCES += \
        main.cpp \
        mainwindow.cpp \
    my_widget/gl_widget.cpp \
    video_card.cpp \
    my_widget/nav_widget.cpp \
    task.cpp \
    common/utils.cpp \
    yuv_widget.cpp

HEADERS += \
        mainwindow.h \
    mainwindow.h \
    my_widget/gl_widget.h \
    video_card.h \
    my_widget/nav_widget.h \
    3rdparty/include/elog.h \
    3rdparty/include/elog_cfg.h \
    task.h \
    common/semaphore.h \
    common/utils.h \
    video_card.h \
    xdma_public.h \
    yuv_widget.h

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

RESOURCES += \
    res.qrc

INCLUDEPATH += $$PWD/3rdparty/include
DEPENDPATH += $$PWD/3rdparty/include

GVFG_ROOT = $$clean_path($$PWD/../..)
INCLUDEPATH += \
    $$GVFG_ROOT/helpers/gvfg_preview/include

CONFIG(debug, debug|release) {
    GVFG_CONFIG = Debug
    GVFG_DEMO_OUTPUT = $$OUT_PWD/debug
} else {
    GVFG_CONFIG = Release
    GVFG_DEMO_OUTPUT = $$OUT_PWD/release
}

msvc {
    GVFG_BUILD = $$GVFG_ROOT/build/Desktop_Qt_6_10_2_MSVC2022_64bit-$${GVFG_CONFIG}
    GVFG_PREVIEW_DLL = $$GVFG_BUILD/bin/gvfg_preview.dll
}

mingw {
    GVFG_BUILD = $$GVFG_ROOT/build/Desktop_Qt_6_10_2_MinGW_64_bit-$${GVFG_CONFIG}
    GVFG_PREVIEW_DLL = $$GVFG_BUILD/bin/libgvfg_preview.dll
}

LIBS += -L$$GVFG_BUILD/lib -lgvfg_preview

QMAKE_POST_LINK += $$QMAKE_COPY $$shell_path($$GVFG_PREVIEW_DLL) $$shell_path($$GVFG_DEMO_OUTPUT)

msvc {
    QMAKE_CFLAGS += /utf-8
    QMAKE_CXXFLAGS += /utf-8
}

win32: LIBS += -lSetupAPI
