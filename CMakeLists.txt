cmake_minimum_required(VERSION 3.20)
project(RustDeskHdrService VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W3 /utf-8)
    # Release: optimize for size
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O1 /GL")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /LTCG")
endif()

set(SOURCES
    main.cpp
    service_mode.cpp
    install_mode.cpp
    cm_log_watcher.cpp
    hdr_control.cpp
    session_bridge.cpp
    debug.cpp
    resources.rc
)

set(HEADERS
    service_mode.h
    install_mode.h
    cm_log_watcher.h
    hdr_control.h
    session_bridge.h
    debug.h
)

add_executable(RustDeskHdrService ${SOURCES} ${HEADERS})

target_link_libraries(RustDeskHdrService
    user32
    advapi32
    wtsapi32
    userenv
)
