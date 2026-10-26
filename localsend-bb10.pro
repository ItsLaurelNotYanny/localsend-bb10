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
    src/security/LocalIdentity.h \
    src/security/PosixTlsSocket.h \
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
    src/security/LocalIdentity.cpp \
    src/security/PosixTlsSocket.cpp \
    src/receive/HttpConnection.cpp \
    src/receive/HttpServer.cpp \
    src/receive/ReceiveServer.cpp \
    src/send/SendManager.cpp \
    src/app/Application.cpp \
    src/discovery/DiscoveryManager.cpp \
    src/ui/UiController.cpp \
    src/main.cpp

isEmpty(POSIX_TLS_SDK_DIR) {
    contains(CONFIG, cascades10) | qnx {
        POSIX_TLS_SDK_DIR = $$PWD/../posix-tls/build/sdk-qnx
    } else {
        POSIX_TLS_SDK_DIR = $$PWD/../posix-tls/build/sdk-host
    }
}

INCLUDEPATH += $$POSIX_TLS_SDK_DIR/include

# BlackBerry 10 Cascades configuration
contains(CONFIG, cascades10) | qnx {
    CONFIG += qt warn_on cascades10
    QT += core network declarative
    DEFINES += BB_CASCADES
    QMAKE_LFLAGS += -Wl,--exclude-libs,ALL
    LIBS += $$POSIX_TLS_SDK_DIR/lib/libposix_tls.a \
            $$POSIX_TLS_SDK_DIR/lib/libssl.a \
            $$POSIX_TLS_SDK_DIR/lib/libcrypto.a \
            -lbbdata -lbbcascadespickers -lbbsystem -lsocket
} else {
    # Host testing / verification configuration
    CONFIG += qt warn_on console
    QT += core network
    QMAKE_LFLAGS += -Wl,--exclude-libs,ALL -Wl,--defsym=__isoc23_strtol=strtol -Wl,--defsym=__isoc23_strtoul=strtoul
    LIBS += $$POSIX_TLS_SDK_DIR/lib/libposix_tls.a \
            $$POSIX_TLS_SDK_DIR/lib/libssl.a \
            $$POSIX_TLS_SDK_DIR/lib/libcrypto.a \
            -lpthread -ldl
}
