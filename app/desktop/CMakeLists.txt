# resindeck: the desktop application (Qt Widgets), which also serves the web interface. The window is a
# library of its own, so tests can create it.
qt_add_library(resindeck_desktop STATIC
  i18n.h i18n.cpp
  mainwindow.h mainwindow.cpp
  dialogs.h dialogs.cpp
  widgets.h widgets.cpp
  viewer3d.h viewer3d.cpp
)
target_compile_definitions(resindeck_desktop PUBLIC RESINDECK_VERSION="${PROJECT_VERSION}")
target_include_directories(resindeck_desktop PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(resindeck_desktop PUBLIC resindeck_core Qt6::Widgets PRIVATE resindeck_warnings)

qt_add_executable(resindeck WIN32 MACOSX_BUNDLE main.cpp)
if(WIN32)
  target_sources(resindeck PRIVATE resindeck.rc)
endif()
target_link_libraries(resindeck PRIVATE resindeck_desktop resindeck_warnings)

# QRhi, which draws the 3D view, is public API with fewer compatibility guarantees across Qt versions.
# Its headers (rhi/qrhi.h) sit in QtGui's versioned folder. Qt6::GuiPrivate would add that folder but
# needs more development packages on Linux (xkbcommon), so add it directly.
set(RESINDECK_RHI_INCLUDE "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_HEADERS}/QtGui/${Qt6Gui_VERSION}/QtGui")
if(NOT EXISTS "${RESINDECK_RHI_INCLUDE}/rhi/qrhi.h")
  message(FATAL_ERROR "rhi/qrhi.h not found in ${RESINDECK_RHI_INCLUDE}")
endif()
target_include_directories(resindeck_desktop SYSTEM PRIVATE "${RESINDECK_RHI_INCLUDE}")

qt_add_shaders(resindeck_desktop "shaders"
  PREFIX "/shaders"
  BASE shaders
  FILES shaders/voxel.vert shaders/voxel.frag
)
qt_add_resources(resindeck_desktop "ui"
  PREFIX "/"
  FILES style.qss
)
qt_add_resources(resindeck_desktop "icon"
  PREFIX "/"
  BASE ${PROJECT_SOURCE_DIR}/packaging
  FILES ${PROJECT_SOURCE_DIR}/packaging/icon.png
)

# Smoke test: the window starts with a fresh home folder, draws itself and quits.
add_test(NAME desktop_starts
  COMMAND ${CMAKE_COMMAND} -E env HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home USERPROFILE=${CMAKE_CURRENT_BINARY_DIR}/test-home
          RESINDECK_CONFIG_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-home/config RESINDECK_CACHE_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-home/cache
          QT_QPA_PLATFORM=offscreen ASAN_OPTIONS=detect_leaks=0 RESINDECK_LAN=0
          $<TARGET_FILE:resindeck> --port 0 --screenshot ${CMAKE_CURRENT_BINARY_DIR}/desktop_starts.png)
set_tests_properties(desktop_starts PROPERTIES TIMEOUT 60)

# The window with a fake printer: following prints.
add_executable(test_desktop test_desktop.cpp)
target_include_directories(test_desktop PRIVATE ${PROJECT_SOURCE_DIR}/core/tests)
target_link_libraries(test_desktop PRIVATE resindeck_desktop resindeck_warnings Qt6::Test)
add_test(NAME test_desktop COMMAND test_desktop)
set_tests_properties(test_desktop PROPERTIES TIMEOUT 120
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;ASAN_OPTIONS=detect_leaks=0;RESINDECK_LAN=0")
