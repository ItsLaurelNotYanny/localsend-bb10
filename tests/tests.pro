TARGET = core-check
TEMPLATE = app
CONFIG += console warn_on
CONFIG -= app_bundle
QT += core network
LIBS += -lcrypto

INCLUDEPATH += ../src
DEPENDPATH += ../src

SOURCES += \
    core-check.cpp \
    ../src/protocol/ProtocolJson.cpp \
    ../src/protocol/CryptoUtil.cpp \
    ../src/receive/HttpConnection.cpp \
    ../src/receive/HttpServer.cpp \
    ../src/receive/ReceiveServer.cpp \
    ../src/send/SendManager.cpp \
    ../src/app/Application.cpp \
    ../src/discovery/DiscoveryManager.cpp \
    ../src/ui/UiController.cpp

HEADERS += \
    ../src/protocol/ProtocolTypes.h \
    ../src/protocol/ProtocolJson.h \
    ../src/protocol/CryptoUtil.h \
    ../src/receive/HttpConnection.h \
    ../src/receive/HttpServer.h \
    ../src/receive/ReceiveServer.h \
    ../src/send/SendManager.h \
    ../src/app/Application.h \
    ../src/discovery/DiscoveryManager.h \
    ../src/ui/UiController.h
