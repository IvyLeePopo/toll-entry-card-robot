QT       =
TEMPLATE = app
TARGET   = lane_core_test

# 纯 C++ 验收程序, 不链接 Qt
CONFIG += console c++14
CONFIG -= qt

CORE = $$PWD/../../lane_core

INCLUDEPATH += $$CORE
SOURCES += main.cpp \
    $$CORE/ThreadPool.cpp \
    $$CORE/CommandTracker.cpp

HEADERS += \
    $$CORE/ThreadPool.h \
    $$CORE/CommandTracker.h

DESTDIR = $$PWD/../../bin
