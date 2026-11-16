QT       += core gui network
greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET   = LaneCtl
TEMPLATE = app

# 脏点: qmake 的 .pro 工程, 没有 CMake, 没有单元测试, 没有 CI
CONFIG += c++11

SOURCES += \
    main.cpp \
    lanewindow.cpp \
    protocol.cpp \
    vehicle.cpp \
    logger.cpp

HEADERS += \
    lanewindow.h \
    protocol.h \
    vehicle.h \
    logger.h

win32 {
    # 脏点: 抄来的 lib, 一个符号都没用到
    LIBS += -lws2_32
}

DESTDIR = $$PWD/../bin
