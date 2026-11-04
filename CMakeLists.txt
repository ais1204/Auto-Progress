cmake_minimum_required(VERSION 3.15)

project(AutoClicker VERSION 1.1.0 LANGUAGES CXX RC)

if(NOT WIN32)
  message(FATAL_ERROR "AutoClicker is a Windows application. Use MSVC, MinGW-w64, or cmake/mingw-w64-x86_64.cmake to cross-compile.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_executable(AutoClicker WIN32
  src/main.cpp
  src/app.cpp
  src/app.h
  src/clicker.cpp
  src/clicker.h
  src/input.cpp
  src/input.h
  src/macro.cpp
  src/macro.h
  src/recorder.cpp
  src/recorder.h
  src/settings.cpp
  src/settings.h
  src/timing.cpp
  src/timing.h
  src/worker.cpp
  src/worker.h
  src/resource.h
  res/app.rc
)

target_include_directories(AutoClicker PRIVATE src res)
target_compile_definitions(AutoClicker PRIVATE
  UNICODE _UNICODE
  WIN32_LEAN_AND_MEAN NOMINMAX
  _WIN32_WINNT=0x0A00 WINVER=0x0A00
)
target_link_libraries(AutoClicker PRIVATE user32 gdi32 comctl32 comdlg32 shell32 winmm uxtheme)

if(MSVC)
  # ソースは UTF-8（日本語の文字列リテラルを含む）
  target_compile_options(AutoClicker PRIVATE /utf-8 /W4 /permissive-)
  target_compile_definitions(AutoClicker PRIVATE _CRT_SECURE_NO_WARNINGS)
  # マニフェストは app.rc に埋め込むので、リンカーの自動生成は止める
  target_link_options(AutoClicker PRIVATE /MANIFEST:NO)
  # VC++ ランタイムを静的リンクし、exe 単体で動くようにする
  set_property(TARGET AutoClicker PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  target_compile_options(AutoClicker PRIVATE -Wall -Wextra -finput-charset=UTF-8)
  target_link_options(AutoClicker PRIVATE -static -static-libgcc -static-libstdc++ $<$<CONFIG:Release,MinSizeRel>:-s>)
endif()
