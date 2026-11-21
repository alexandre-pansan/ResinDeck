# ResinDeck native application (C++ and Qt). See docs/ROADMAP.md.
#
#   cmake -B build/cpp -G Ninja -DCMAKE_PREFIX_PATH=~/Qt/6.11.3/gcc_64
#   cmake --build build/cpp
#   ctest --test-dir build/cpp --output-on-failure
cmake_minimum_required(VERSION 3.21)
project(ResinDeck VERSION 1.1.0 LANGUAGES CXX)

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

# --- install: the layout packages are made from (see packaging/) ---
include(GNUInstallDirs)
install(TARGETS resindeck resindeck-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} BUNDLE DESTINATION .)
if(WIN32)
  # Server::findWebDir() looks for web/ next to the executable
  install(DIRECTORY static/ DESTINATION ${CMAKE_INSTALL_BINDIR}/web)
else()
  install(DIRECTORY static/ DESTINATION ${CMAKE_INSTALL_DATADIR}/resindeck/web)
  install(FILES packaging/resindeck.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  install(FILES packaging/icon.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps RENAME resindeck.png)
endif()
