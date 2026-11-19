# resindeck_core: everything that is not user interface, tested on its own.
add_library(resindeck_core STATIC
  config.h config.cpp
  formats/sliced.h formats/sliced.cpp
  formats/cxdlp.h formats/cxdlp.cpp
  printers/status.h printers/status.cpp
  printers/driver.h printers/driver.cpp
  printers/drivers.h printers/drivers.cpp
  printers/printer.h printers/printer.cpp
  net/lan.h net/lan.cpp
  printers/des.h printers/des.cpp
  printers/halot.h printers/halot.cpp
)
target_include_directories(resindeck_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(resindeck_core
  PUBLIC Qt6::Core Qt6::Concurrent Qt6::Gui Qt6::Network Qt6::WebSockets
  PRIVATE resindeck_warnings)
