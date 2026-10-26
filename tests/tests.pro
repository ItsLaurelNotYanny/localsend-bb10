TARGET = core-check
TEMPLATE = app
CONFIG += console warn_on
CONFIG -= app_bundle
QT += core network

isEmpty(POSIX_TLS_SDK_DIR) {
    POSIX_TLS_SDK_DIR = $$PWD/../../posix-tls/build/sdk-host
}

INCLUDEPATH += ../src $$POSIX_TLS_SDK_DIR/include
DEPENDPATH += ../src

QMAKE_LFLAGS += -Wl,--exclude-libs,ALL -Wl,--defsym=__isoc23_strtol=strtol -Wl,--defsym=__isoc23_strtoul=strtoul
LIBS += $$POSIX_TLS_SDK_DIR/lib/libposix_tls.a \
        $$POSIX_TLS_SDK_DIR/lib/libssl.a \
        $$POSIX_TLS_SDK_DIR/lib/libcrypto.a \
        -lpthread -ldl

SOURCES += \
    core-check.cpp \
    ../src/protocol/ProtocolJson.cpp \
    ../src/protocol/CryptoUtil.cpp \
    ../src/security/LocalIdentity.cpp \
    ../src/security/PosixTlsSocket.cpp \
    ../src/security/PosixTlsHttpClient.cpp \
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
    ../src/security/LocalIdentity.h \
    ../src/security/PosixTlsSocket.h \
    ../src/security/PosixTlsHttpClient.h \
    ../src/receive/HttpConnection.h \
    ../src/receive/HttpServer.h \
    ../src/receive/ReceiveServer.h \
    ../src/send/SendManager.h \
    ../src/app/Application.h \
    ../src/discovery/DiscoveryManager.h \
    ../src/ui/UiController.h
