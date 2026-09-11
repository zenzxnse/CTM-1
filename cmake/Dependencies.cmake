include(FetchContent)

function(context_hmi_require_drogon)
  if(CONTEXT_HMI_USE_SYSTEM_DROGON)
    find_package(Drogon 1.9.13 EXACT CONFIG REQUIRED)
    return()
  endif()

  set(_context_hmi_build_testing "${BUILD_TESTING}")
  set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
  set(BUILD_TESTING OFF CACHE BOOL "Build dependency tests" FORCE)
  set(BUILD_CTL OFF CACHE BOOL "Build drogon_ctl" FORCE)
  set(BUILD_EXAMPLES OFF CACHE BOOL "Build Drogon examples" FORCE)
  set(BUILD_ORM OFF CACHE BOOL "Build Drogon ORM" FORCE)
  set(BUILD_POSTGRESQL OFF CACHE BOOL "Build Drogon PostgreSQL support" FORCE)
  set(BUILD_MYSQL OFF CACHE BOOL "Build Drogon MySQL support" FORCE)
  set(BUILD_SQLITE OFF CACHE BOOL "Build Drogon SQLite support" FORCE)
  set(BUILD_REDIS OFF CACHE BOOL "Build Drogon Redis support" FORCE)
  set(BUILD_BROTLI OFF CACHE BOOL "Build Drogon Brotli support" FORCE)
  set(BUILD_YAML_CONFIG OFF CACHE BOOL "Build Drogon YAML support" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static dependencies" FORCE)
  set(USE_SUBMODULE ON CACHE BOOL "Use pinned Drogon submodules" FORCE)
  set(USE_COROUTINE ON CACHE BOOL "Enable Drogon C++20 coroutines" FORCE)
  set(USE_OPENSSL ${CONTEXT_HMI_ENABLE_TLS} CACHE BOOL
      "Enable Drogon OpenSSL support for HTTPS inference" FORCE)

  FetchContent_Declare(
    drogon
    GIT_REPOSITORY https://github.com/drogonframework/drogon.git
    GIT_TAG 4c5430757ea5451a7c38fbbef4b4bef7dbb47f2f
    GIT_SUBMODULES trantor
    GIT_SUBMODULES_RECURSE TRUE
    SYSTEM
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(drogon)
  set(BUILD_TESTING "${_context_hmi_build_testing}" CACHE BOOL "Build project tests" FORCE)
endfunction()
