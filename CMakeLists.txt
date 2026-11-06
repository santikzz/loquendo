# Native Jorge voice: the Loquendo TTS 6 engine recompiled from x86 to portable C.
#   cmake -B build && cmake --build build -j
#   ./build/jorge --data voice Hola, soy Jorge.
cmake_minimum_required(VERSION 3.16)
project(jorge C)

if(NOT EXISTS ${CMAKE_SOURCE_DIR}/gen/funcs.h)
    message(FATAL_ERROR "gen/ is missing. Check out the full repository.")
endif()

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

file(GLOB GEN_SOURCES CONFIGURE_DEPENDS gen/*.c)
add_executable(jorge runtime/main.c runtime/runtime.c runtime/win32.c runtime/platform.c ${GEN_SOURCES})
target_include_directories(jorge PRIVATE runtime gen)

# Generated code spells out x86 semantics; its warnings are noise, and -O1 keeps compile time low.
if(WIN32)
    # no console window; winmm plays the audio
    set_target_properties(jorge PROPERTIES WIN32_EXECUTABLE ON)
    target_link_libraries(jorge PRIVATE winmm)
endif()
if(MSVC)
    set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "/w;/O1")
    target_link_options(jorge PRIVATE /ENTRY:mainCRTStartup)
else()
    set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-w;-O1")
    find_package(Threads REQUIRED)
    target_link_libraries(jorge PRIVATE Threads::Threads m)
endif()
