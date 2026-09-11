cmake_minimum_required(VERSION 4.0)

project(
  context_hmi
  VERSION 0.3.0
  DESCRIPTION "Local machine-context resolver and task-based HMI service"
  LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)
include(cmake/Dependencies.cmake)

find_package(Threads REQUIRED)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CONTEXT_HMI_BUILD_BENCHMARKS "Build native benchmark tooling" ON)
option(CONTEXT_HMI_USE_SYSTEM_DROGON "Use an exact system Drogon 1.9.13 package" OFF)
option(CONTEXT_HMI_ENABLE_HARDENING "Enable supported compiler and linker hardening" ON)
option(CONTEXT_HMI_ENABLE_TLS "Enable HTTPS inference providers through OpenSSL" OFF)
option(CONTEXT_HMI_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
option(CONTEXT_HMI_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" OFF)
option(CONTEXT_HMI_CONSTRAINED "Use constrained-display resource defaults" OFF)

if(CONTEXT_HMI_ENABLE_TLS)
  find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)
endif()

context_hmi_require_drogon()

add_library(context_hmi_project_options INTERFACE)

if(MSVC)
  target_compile_options(
    context_hmi_project_options
    INTERFACE /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8)
  if(CONTEXT_HMI_WARNINGS_AS_ERRORS)
    target_compile_options(context_hmi_project_options INTERFACE /WX)
  endif()
  if(CONTEXT_HMI_ENABLE_HARDENING)
    target_compile_options(context_hmi_project_options INTERFACE /guard:cf /sdl)
    target_link_options(context_hmi_project_options INTERFACE /DYNAMICBASE /NXCOMPAT /guard:cf)
  endif()
else()
  target_compile_options(
    context_hmi_project_options
    INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wformat=2 -Wshadow -Wundef)
  if(CONTEXT_HMI_WARNINGS_AS_ERRORS)
    target_compile_options(context_hmi_project_options INTERFACE -Werror)
  endif()
  if(CONTEXT_HMI_ENABLE_HARDENING)
    target_compile_options(
      context_hmi_project_options
      INTERFACE $<$<CONFIG:Release>:-fstack-protector-strong;-D_FORTIFY_SOURCE=3>)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      target_link_options(
        context_hmi_project_options
        INTERFACE $<$<CONFIG:Release>:-Wl,-z,relro,-z,now>)
    endif()
  endif()
endif()

if(CONTEXT_HMI_SANITIZERS)
  if(MSVC)
    message(FATAL_ERROR "CONTEXT_HMI_SANITIZERS requires GCC or Clang")
  endif()
  target_compile_options(
    context_hmi_project_options
    INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(context_hmi_project_options INTERFACE -fsanitize=address,undefined)
endif()

if(WIN32)
  target_compile_definitions(
    context_hmi_project_options
    INTERFACE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
endif()

if(CONTEXT_HMI_CONSTRAINED)
  target_compile_definitions(context_hmi_project_options INTERFACE CONTEXT_HMI_CONSTRAINED=1)
endif()

add_library(
  context_hmi_core STATIC
  src/context_hmi/bounded_cache.cpp
  src/context_hmi/context_import.cpp
  src/context_hmi/context_retrieval.cpp
  src/context_hmi/diagnostic_logger.cpp
  src/context_hmi/engine.cpp
  src/context_hmi/lifecycle.cpp
  src/context_hmi/model_validation.cpp
  src/context_hmi/operational_store.cpp
  src/context_hmi/reconciliation.cpp
  src/context_hmi/screen_composer.cpp
  src/context_hmi/telemetry_store.cpp
  src/context_hmi/worker_pool.cpp)
add_library(context_hmi::core ALIAS context_hmi_core)
target_include_directories(
  context_hmi_core
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(
  context_hmi_core
  SYSTEM PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party>)
target_link_libraries(context_hmi_core PUBLIC context_hmi_project_options)

add_executable(
  context-hmi
  src/context_hmi/http_common.cpp
  src/context_hmi/read_routes.cpp
  src/context_hmi/task_routes.cpp
  src/context_hmi/stream_routes.cpp
  src/context_hmi/service_records.cpp
  src/context_hmi/service_runtime.cpp
  src/context_hmi/runtime_context.cpp
  src/context_hmi/runtime_lifecycle.cpp
  src/context_hmi/runtime_surface.cpp
  src/context_hmi/server.cpp
  src/context_hmi/inference.cpp
  src/context_hmi/options.cpp)
target_link_libraries(
  context-hmi
  PRIVATE context_hmi::core context_hmi_project_options Drogon::Drogon Threads::Threads)
target_compile_definitions(
  context-hmi PRIVATE CONTEXT_HMI_VERSION="${PROJECT_VERSION}")

add_executable(context-hmi-context tools/context_import.cpp)
target_link_libraries(
  context-hmi-context PRIVATE context_hmi::core context_hmi_project_options)

if(CONTEXT_HMI_ENABLE_TLS)
  target_compile_definitions(context-hmi PRIVATE CONTEXT_HMI_ENABLE_TLS=1)
  target_link_libraries(context-hmi PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

if(WIN32)
  target_link_libraries(context-hmi PRIVATE ws2_32)
endif()

if(CONTEXT_HMI_BUILD_BENCHMARKS)
  add_executable(context-hmi-benchmark tools/benchmark.cpp)
  target_link_libraries(
    context-hmi-benchmark PRIVATE context_hmi::core context_hmi_project_options)
  target_compile_definitions(
    context-hmi-benchmark PRIVATE BENCHMARK_BUILD_TYPE="$<CONFIG>")
endif()

set(context_hmi_cpp_tests
    tests/test_adversarial.cpp
    tests/test_cache.cpp
    tests/test_context_import.cpp
    tests/test_context_retrieval.cpp
    tests/test_execution.cpp
    tests/test_journal.cpp
    tests/test_lifecycle.cpp
    tests/test_logger.cpp
    tests/test_reconciliation.cpp
    tests/test_telemetry_store.cpp
    tests/test_validation.cpp)
set(context_hmi_python_tests
    tests/test_current_api.py
    tests/test_http_surface.py
    tests/test_provider_contract.py)

file(GLOB context_hmi_discovered_cpp_tests CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.cpp")
file(GLOB context_hmi_discovered_python_tests CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.py")
list(TRANSFORM context_hmi_cpp_tests PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
list(TRANSFORM context_hmi_python_tests PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
list(SORT context_hmi_cpp_tests)
list(SORT context_hmi_python_tests)
list(SORT context_hmi_discovered_cpp_tests)
list(SORT context_hmi_discovered_python_tests)
if(NOT "${context_hmi_cpp_tests}" STREQUAL "${context_hmi_discovered_cpp_tests}")
  message(FATAL_ERROR
    "Every tests/test_*.cpp source must be explicitly registered. "
    "Declared: ${context_hmi_cpp_tests}; found: ${context_hmi_discovered_cpp_tests}")
endif()
if(NOT "${context_hmi_python_tests}" STREQUAL "${context_hmi_discovered_python_tests}")
  message(FATAL_ERROR
    "Every tests/test_*.py source must be explicitly registered. "
    "Declared: ${context_hmi_python_tests}; found: ${context_hmi_discovered_python_tests}")
endif()

if(BUILD_TESTING)
  find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)

  foreach(suite IN ITEMS adversarial cache context_import context_retrieval execution journal lifecycle logger
                         reconciliation telemetry_store validation)
    add_executable(test_${suite} tests/test_${suite}.cpp)
    target_link_libraries(test_${suite} PRIVATE context_hmi::core context_hmi_project_options)
    target_include_directories(test_${suite} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    add_test(NAME ${suite} COMMAND test_${suite})
    set_tests_properties(${suite} PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  endforeach()

  foreach(suite IN ITEMS current_api http_surface provider_contract)
    add_test(
      NAME ${suite}
      COMMAND
        ${CMAKE_COMMAND} -E env
        "CONTEXT_HMI_BINARY=$<TARGET_FILE:context-hmi>"
        ${Python3_EXECUTABLE} -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_${suite}.py" -v)
    set_tests_properties(
      ${suite}
      PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                 TIMEOUT 180)
  endforeach()
endif()

install(
  TARGETS context-hmi
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  COMPONENT runtime)
install(
  TARGETS context-hmi-context
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  COMPONENT tools)
if(CONTEXT_HMI_BUILD_BENCHMARKS)
  install(
    TARGETS context-hmi-benchmark
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT tools)
endif()
foreach(example_directory IN ITEMS machines telemetry artifacts)
  install(
    DIRECTORY "examples/${example_directory}/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/context-hmi/examples/${example_directory}"
    COMPONENT runtime
    OPTIONAL
    FILES_MATCHING PATTERN "*.json" PATTERN "*.csv")
endforeach()
install(
  DIRECTORY config/
  DESTINATION ${CMAKE_INSTALL_DATADIR}/context-hmi/config
  COMPONENT runtime
  OPTIONAL)
install(
  DIRECTORY schemas/
  DESTINATION ${CMAKE_INSTALL_DATADIR}/context-hmi/schemas
  COMPONENT runtime
  OPTIONAL)
install(
  DIRECTORY workbench/build/
  DESTINATION ${CMAKE_INSTALL_DATADIR}/context-hmi/workbench
  COMPONENT workbench
  OPTIONAL)
install(
  DIRECTORY docs/
  DESTINATION ${CMAKE_INSTALL_DOCDIR}
  COMPONENT documentation
  FILES_MATCHING PATTERN "*.adoc")
install(
  FILES README.adoc LICENSE
  DESTINATION ${CMAKE_INSTALL_DOCDIR}
  COMPONENT documentation)

set(CPACK_PACKAGE_VENDOR "HMISOL")
set(CPACK_PACKAGE_CONTACT "zxe@lowercase.io")
set(CPACK_PACKAGE_CHECKSUM SHA256)
set(CPACK_PACKAGE_FILE_NAME
    "context-hmi-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_ALL runtime workbench documentation tools)
set(CPACK_COMPONENT_RUNTIME_REQUIRED ON)
set(CPACK_COMPONENT_RUNTIME_DESCRIPTION
    "Context HMI service and runtime machine data")
set(CPACK_COMPONENT_TOOLS_DESCRIPTION
    "Context import and optional benchmark command-line tools")
set(CPACK_COMPONENT_WORKBENCH_DESCRIPTION "Static Context HMI operator workbench")
set(CPACK_COMPONENT_DOCUMENTATION_DESCRIPTION "Context HMI documentation")
set(CPACK_GENERATOR "TGZ;ZIP")
set(CPACK_SOURCE_GENERATOR "TGZ;ZIP")
set(CPACK_SOURCE_IGNORE_FILES
    "/\\.git/;/build/;/build-[^/]*/;/cmake-build-[^/]*/;/dist/;/out/;/Testing/;"
    "/workbench/node_modules/;/workbench/\\.svelte-kit/;/workbench/build/;"
    "/adr/;assignment\\.md;rule\\.adoc;project-state\\.yaml;AGENTS\\.md;"
    "CMakeUserPresets\\.json;context-hmi-handoff;/\\.env$;/models/;/runtime/")
include(CPack)
