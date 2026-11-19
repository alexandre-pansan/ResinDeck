# One executable per test file, each registered with CTest.
function(resindeck_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE resindeck_core resindeck_warnings Qt6::Test)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

resindeck_test(test_des)
resindeck_test(test_status)
resindeck_test(test_halot)
