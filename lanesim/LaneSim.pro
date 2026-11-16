QT       += core network
QT       -= gui

TARGET   = LaneSim
TEMPLATE = app

CONFIG   += c++11 console

SOURCES += \
    main.cpp \
    console.cpp \
    simserver.cpp \
    protocol_dup.cpp

HEADERS += \
    console.h \
    simserver.h \
    protocol_dup.h

DESTDIR = $$PWD/../bin
