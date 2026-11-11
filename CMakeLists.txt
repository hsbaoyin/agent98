cmake_minimum_required(VERSION 3.15)
project(Agent98 C)

if(NOT WIN32)
    message(FATAL_ERROR "Agent98 targets Win32; use MinGW or MSVC on Windows.")
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

file(GLOB_RECURSE AGENT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
list(FILTER AGENT_SOURCES EXCLUDE REGEX "/(main|main_gui|tlsprobe)\\.c$")
file(GLOB_RECURSE BEARSSL_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bearssl/src/*.c")

add_library(agent98_core STATIC
    ${AGENT_SOURCES}
    third_party/cjson/cJSON.c
    ${BEARSSL_SOURCES}
)

target_include_directories(agent98_core PUBLIC
    src
    src/core
    src/agent
    src/net
    src/gui
    third_party/cjson
    third_party/bearssl/inc
    third_party/bearssl/src
)

target_compile_definitions(agent98_core PUBLIC CJSON_HIDE_SYMBOLS)

if(MSVC)
    target_compile_options(agent98_core PRIVATE /W3)
else()
    target_compile_options(agent98_core PRIVATE -w)
endif()

target_link_libraries(agent98_core PUBLIC ws2_32 advapi32 user32 gdi32 comdlg32)

add_executable(a98 src/main.c)
target_link_libraries(a98 PRIVATE agent98_core)

add_executable(agent98 WIN32 src/gui/main_gui.c src/gui/gui.rc)
target_link_libraries(agent98 PRIVATE agent98_core)

add_executable(tlsprobe src/tlsprobe.c)
target_link_libraries(tlsprobe PRIVATE agent98_core)

if(MINGW)
    # Keep the host build usable on modern Windows while the shipped target is Win9x.
    target_link_options(a98 PRIVATE -static)
    target_link_options(agent98 PRIVATE -static)
    target_link_options(tlsprobe PRIVATE -static)
endif()
