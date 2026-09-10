cmake_minimum_required(VERSION 3.25)
project(context_hmi VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
include(GNUInstallDirs)
find_package(Threads REQUIRED)

option(CONTEXT_HMI_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)

add_library(context_hmi_core src/context_hmi/engine.cpp)
target_compile_features(context_hmi_core PUBLIC cxx_std_20)
target_include_directories(context_hmi_core PUBLIC src)
target_include_directories(context_hmi_core SYSTEM PUBLIC third_party)
if(MSVC)
  target_compile_options(context_hmi_core PRIVATE /W4 /permissive-)
else()
  target_compile_options(context_hmi_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

add_executable(context-hmi src/context_hmi/server.cpp src/context_hmi/inference.cpp)
target_link_libraries(context-hmi PRIVATE context_hmi_core Threads::Threads)
add_executable(context-hmi-benchmark tools/benchmark.cpp)
target_link_libraries(context-hmi-benchmark PRIVATE context_hmi_core)
target_compile_definitions(context-hmi-benchmark PRIVATE BENCHMARK_BUILD_TYPE="$<CONFIG>")
if(NOT MSVC)
  target_compile_options(context-hmi-benchmark PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(WIN32)
  target_link_libraries(context-hmi PRIVATE ws2_32)
endif()
if(NOT MSVC)
  target_compile_options(context-hmi PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(CONTEXT_HMI_SANITIZERS AND NOT MSVC)
  target_compile_options(context_hmi_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(context_hmi_core PUBLIC -fsanitize=address,undefined)
endif()

if(BUILD_TESTING)
  find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
  add_executable(test_engine tests/test_engine.cpp)
  target_link_libraries(test_engine PRIVATE context_hmi_core)
  target_compile_definitions(test_engine PRIVATE EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples")
  add_test(NAME engine COMMAND test_engine)
  set_tests_properties(engine PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  add_executable(test_adversarial tests/test_adversarial.cpp)
  target_link_libraries(test_adversarial PRIVATE context_hmi_core)
  add_test(NAME adversarial COMMAND test_adversarial)
  set_tests_properties(adversarial PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  foreach(suite api inference)
    add_test(NAME ${suite}
      COMMAND ${CMAKE_COMMAND} -E env
        "CONTEXT_HMI_BINARY=$<TARGET_FILE:context-hmi>"
        ${Python3_EXECUTABLE} -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_${suite}.py" -v)
    set_tests_properties(${suite} PROPERTIES
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 45)
  endforeach()
endif()

install(TARGETS context-hmi RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY examples/ DESTINATION ${CMAKE_INSTALL_DATADIR}/context-hmi/examples
        FILES_MATCHING PATTERN "*.json")
install(DIRECTORY workbench/build/ DESTINATION ${CMAKE_INSTALL_DATADIR}/context-hmi/workbench
        OPTIONAL)
install(FILES README.adoc LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/context-hmi)
