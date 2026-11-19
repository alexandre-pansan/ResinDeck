# resindeck-cli: the command line (version, status, send, print, pause, stop).
add_executable(resindeck-cli main.cpp)
target_compile_definitions(resindeck-cli PRIVATE RESINDECK_VERSION="${PROJECT_VERSION}")
target_link_libraries(resindeck-cli PRIVATE resindeck_core resindeck_warnings)
