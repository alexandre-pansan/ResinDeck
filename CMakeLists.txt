# ResinDeck native application (C++ and Qt). See docs/ROADMAP.md.
#
#   cmake -B build/cpp -G Ninja -DCMAKE_PREFIX_PATH=~/Qt/6.11.3/gcc_64
#   cmake --build build/cpp
#   ctest --test-dir build/cpp --output-on-failure
cmake_minimum_required(VERSION 3.21)
project(ResinDeck VERSION 0.3.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RESINDECK_WERROR "Treat compiler warnings as errors" OFF)
option(RESINDECK_SANITIZE "Build with AddressSanitizer and UBSan" OFF)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Concurrent Gui HttpServer Network ShaderTools WebSockets Widgets Test)
qt_standard_project_setup()

add_library(resindeck_warnings INTERFACE)
if(MSVC)
  target_compile_options(resindeck_warnings INTERFACE /W4 /permissive- $<$<BOOL:${RESINDECK_WERROR}>:/WX>)
else()
  target_compile_options(resindeck_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion
                         $<$<BOOL:${RESINDECK_WERROR}>:-Werror>)
  if(RESINDECK_SANITIZE)
    target_compile_options(resindeck_warnings INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(resindeck_warnings INTERFACE -fsanitize=address,undefined)
  endif()
endif()

enable_testing()
add_subdirectory(core)
add_subdirectory(app/cli)
add_subdirectory(app/desktop)
add_subdirectory(core/tests)
