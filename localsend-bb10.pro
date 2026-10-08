APP_NAME = localsend-bb10
TARGET = localsend-bb10
TEMPLATE = app

# Source & Header layout
INCLUDEPATH += src
DEPENDPATH += src

HEADERS += \
    src/protocol/ProtocolTypes.h \
    src/protocol/ProtocolJson.h \
    src/protocol/CryptoUtil.h \
    src/receive/HttpConnection.h \
    src/receive/HttpServer.h \
    src/receive/ReceiveServer.h \
    src/send/SendManager.h \
    src/app/Application.h \
    src/discovery/DiscoveryManager.h \
    src/ui/UiController.h

SOURCES += \
    src/protocol/ProtocolJson.cpp \
    src/protocol/CryptoUtil.cpp \
    src/receive/HttpConnection.cpp \
    src/receive/HttpServer.cpp \
    src/receive/ReceiveServer.cpp \
    src/send/SendManager.cpp \
    src/app/Application.cpp \
    src/discovery/DiscoveryManager.cpp \
    src/ui/UiController.cpp \
    src/main.cpp

# BlackBerry 10 Cascades configuration
contains(CONFIG, cascades10) | qnx {
    CONFIG += qt warn_on cascades10
    QT += core network declarative
    LIBS += -lbbdata -lcrypto
} else {
    # Host testing / verification configuration
    CONFIG += qt warn_on console
    QT += core network
    LIBS += -lcrypto
}
