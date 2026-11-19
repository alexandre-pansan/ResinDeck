# Vendored QR code library (third_party/qrcodegen), built without our warning flags.
add_library(qrcodegen STATIC ${PROJECT_SOURCE_DIR}/third_party/qrcodegen/qrcodegen.cpp)
target_include_directories(qrcodegen PUBLIC ${PROJECT_SOURCE_DIR}/third_party/qrcodegen)

# resindeck_core: everything that is not user interface, tested on its own.
add_library(resindeck_core STATIC
  config.h config.cpp
  deck.h deck.cpp
  net/mdns.h net/mdns.cpp
  remote/qr.h remote/qr.cpp
  remote/server.h remote/server.cpp
  formats/sliced.h formats/sliced.cpp
  formats/cxdlp.h formats/cxdlp.cpp
  mesh/mesh.h mesh/mesh.cpp
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
  PUBLIC Qt6::Core Qt6::Concurrent Qt6::Gui Qt6::HttpServer Qt6::Network Qt6::WebSockets
  PRIVATE resindeck_warnings qrcodegen)
# where the web interface (static/) is found when running from the build directory
target_compile_definitions(resindeck_core PRIVATE RESINDECK_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

