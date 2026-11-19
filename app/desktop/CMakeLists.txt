# resindeck: the desktop application (Qt Widgets), which also serves the web interface.
qt_add_executable(resindeck WIN32 MACOSX_BUNDLE
  main.cpp
  i18n.h i18n.cpp
  mainwindow.h mainwindow.cpp
  dialogs.h dialogs.cpp
  widgets.h widgets.cpp
  viewer3d.h viewer3d.cpp
)
target_compile_definitions(resindeck PRIVATE RESINDECK_VERSION="${PROJECT_VERSION}")
if(WIN32)
  target_sources(resindeck PRIVATE resindeck.rc)
endif()
target_link_libraries(resindeck PRIVATE resindeck_core resindeck_warnings Qt6::Widgets)

# QRhi, which draws the 3D view, is public API with fewer compatibility guarantees across Qt versions.
# Its headers (rhi/qrhi.h) sit in QtGui's versioned folder. Qt6::GuiPrivate would add that folder but
# needs more development packages on Linux (xkbcommon), so add it directly.
set(RESINDECK_RHI_INCLUDE "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_HEADERS}/QtGui/${Qt6Gui_VERSION}/QtGui")
if(NOT EXISTS "${RESINDECK_RHI_INCLUDE}/rhi/qrhi.h")
  message(FATAL_ERROR "rhi/qrhi.h not found in ${RESINDECK_RHI_INCLUDE}")
endif()
target_include_directories(resindeck SYSTEM PRIVATE "${RESINDECK_RHI_INCLUDE}")

qt_add_shaders(resindeck "shaders"
  PREFIX "/shaders"
  BASE shaders
  FILES shaders/voxel.vert shaders/voxel.frag
)
qt_add_resources(resindeck "ui"
  PREFIX "/"
  FILES style.qss
)
qt_add_resources(resindeck "icon"
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
