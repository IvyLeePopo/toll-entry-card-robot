QT       += core gui network
greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET   = LaneCtl
TEMPLATE = app

# W1: 并发基础件已抽到 lane_core (不依赖 Qt), W4 会把它做成独立静态库
CONFIG += c++14

CORE = $$PWD/../lane_core
INCLUDEPATH += $$CORE

SOURCES += \
    main.cpp \
    lanewindow.cpp \
    protocol.cpp \
    vehicle.cpp \
    logger.cpp \
    $$CORE/ThreadPool.cpp \
    $$CORE/CommandTracker.cpp

HEADERS += \
    lanewindow.h \
    protocol.h \
    vehicle.h \
    logger.h \
    $$CORE/ThreadPool.h \
    $$CORE/CommandTracker.h

win32 {
    # 脏点: 抄来的 lib, 一个符号都没用到
    LIBS += -lws2_32
}

DESTDIR = $$PWD/../bin
