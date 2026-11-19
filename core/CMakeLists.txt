# resindeck_core: everything that is not user interface, tested on its own.
add_library(resindeck_core STATIC
  printers/status.h printers/status.cpp
  printers/des.h printers/des.cpp
  printers/halot.h printers/halot.cpp
)
target_include_directories(resindeck_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(resindeck_core
  PUBLIC Qt6::Core Qt6::Network Qt6::WebSockets
  PRIVATE resindeck_warnings)
