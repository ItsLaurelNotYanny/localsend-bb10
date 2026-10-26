cmake_minimum_required(VERSION 3.16)
project(LocalSendBB10 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 98)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DEFINED POSIX_TLS_SDK_DIR)
    if(QNXNTO OR CMAKE_SYSTEM_NAME STREQUAL "QNX" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "armv7")
        get_filename_component(DEFAULT_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../posix-tls/build/sdk-qnx" ABSOLUTE)
    else()
        get_filename_component(DEFAULT_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../posix-tls/build/sdk-host" ABSOLUTE)
    endif()
    set(POSIX_TLS_SDK_DIR "${DEFAULT_SDK_DIR}")
endif()

message(STATUS "Using posix-tls SDK directory: ${POSIX_TLS_SDK_DIR}")
find_package(posix_tls REQUIRED PATHS "${POSIX_TLS_SDK_DIR}" NO_DEFAULT_PATH)

set(LOCALSEND_SOURCES
    src/protocol/ProtocolJson.cpp
    src/protocol/CryptoUtil.cpp
    src/security/LocalIdentity.cpp
    src/security/PosixTlsSocket.cpp
    src/receive/HttpConnection.cpp
    src/receive/HttpServer.cpp
    src/receive/ReceiveServer.cpp
    src/send/SendManager.cpp
    src/app/Application.cpp
    src/discovery/DiscoveryManager.cpp
    src/ui/UiController.cpp
    src/main.cpp
)

set(LOCALSEND_MOC_HEADERS
    src/security/PosixTlsSocket.h
    src/receive/HttpConnection.h
    src/receive/HttpServer.h
    src/receive/ReceiveServer.h
    src/send/SendManager.h
    src/app/Application.h
    src/discovery/DiscoveryManager.h
    src/ui/UiController.h
)

if(CMAKE_SYSTEM_PROCESSOR STREQUAL "armv7" OR QNXNTO OR CMAKE_SYSTEM_NAME STREQUAL "QNX")
    include(BB10Qt4)
    qt4_wrap_cpp(LOCALSEND_MOC_SOURCES ${LOCALSEND_MOC_HEADERS})

    add_executable(localsend-bb10
        ${LOCALSEND_SOURCES}
        ${LOCALSEND_MOC_SOURCES}
    )

    target_compile_definitions(localsend-bb10 PRIVATE BB_CASCADES)
    target_include_directories(localsend-bb10 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${posix_tls_INCLUDE_DIRS}
    )
    target_include_directories(localsend-bb10 SYSTEM PRIVATE
        /opt/bb10-sdk/target/qnx6/usr/include/qt4/QtNetwork
        /opt/bb10-sdk/target/qnx6/usr/include/qt4/QtGui
    )
    target_link_libraries(localsend-bb10 PRIVATE
        posix_tls::posix_tls
        QtCore
        QtNetwork
        QtDeclarative
        bbcascades
        bbcascadespickers
        bbdata
        bbsystem
        socket
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_link_options(localsend-bb10 PRIVATE "-Wl,--exclude-libs,ALL")
    endif()
endif()
