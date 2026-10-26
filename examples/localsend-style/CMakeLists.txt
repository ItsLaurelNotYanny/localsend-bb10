cmake_minimum_required(VERSION 3.16)
project(LocalSendStyleDemo LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 98)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Determine default SDK directory if not explicitly provided
if(NOT DEFINED POSIX_TLS_SDK_DIR)
    if(QNXNTO OR CMAKE_SYSTEM_NAME STREQUAL "QNX")
        get_filename_component(DEFAULT_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../posix-tls/build/sdk-qnx" ABSOLUTE)
    else()
        get_filename_component(DEFAULT_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../posix-tls/build/sdk-host" ABSOLUTE)
    endif()
    set(POSIX_TLS_SDK_DIR "${DEFAULT_SDK_DIR}")
endif()

message(STATUS "Using posix-tls SDK directory: ${POSIX_TLS_SDK_DIR}")

find_package(posix_tls REQUIRED PATHS "${POSIX_TLS_SDK_DIR}" NO_DEFAULT_PATH)

add_executable(localsend_style_demo
    src/localsend_style.cpp
)

target_include_directories(localsend_style_demo PRIVATE
    ${posix_tls_INCLUDE_DIRS}
)

target_link_libraries(localsend_style_demo PRIVATE
    posix_tls::posix_tls
)

# Enforce symbol isolation on GNU/Clang compilers
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(localsend_style_demo PRIVATE "-Wl,--exclude-libs,ALL")
endif()
