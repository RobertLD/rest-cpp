### Project Configuration
### ---------------------

cmake_minimum_required(VERSION 3.14)
project(rest_cpp VERSION 0.1.0 LANGUAGES CXX)

# Top-level detection
if (DEFINED PROJECT_IS_TOP_LEVEL)
  set(_rest_cpp_top_level ${PROJECT_IS_TOP_LEVEL})
elseif (CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(_rest_cpp_top_level ON)
else()
  set(_rest_cpp_top_level OFF)
endif()

option(REST_CPP_BUILD_TESTS "Build tests" ${_rest_cpp_top_level})
option(REST_CPP_INSTALL "Enable install/export packaging" ${_rest_cpp_top_level})
option(REST_CPP_WARNINGS_AS_ERRORS "Treat warnings as errors" ${_rest_cpp_top_level})

# Add GNUInstallDirs & set install path variable
include(GNUInstallDirs)
set(REST_CPP_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/rest_cpp")

### Library Target
### --------------
add_library(rest_cpp
  src/client.cpp
)
add_library(rest_cpp::rest_cpp ALIAS rest_cpp)
target_compile_features(rest_cpp PUBLIC cxx_std_20)

# Prefer target-level PIC instead of global
set_target_properties(rest_cpp PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Include public headers
target_include_directories(rest_cpp
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Add warnings if top level package
function(rest_cpp_set_warnings target)
  if (MSVC)
    target_compile_options(${target} PRIVATE /W4)
    if (REST_CPP_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if (REST_CPP_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()
if (_rest_cpp_top_level)
  rest_cpp_set_warnings(rest_cpp)
endif()

### Dependencies
### -------------

# ssl
find_package(OpenSSL REQUIRED)

# boost
find_package(Boost CONFIG QUIET COMPONENTS system)
if (NOT Boost_FOUND)
  find_package(Boost REQUIRED COMPONENTS system)
endif()

# nlohmann/json
option(REST_CPP_USE_SYSTEM_NLOHMANN "Use system nlohmann_json package" ${REST_CPP_INSTALL})
if (REST_CPP_USE_SYSTEM_NLOHMANN OR REST_CPP_INSTALL)
  find_package(nlohmann_json CONFIG REQUIRED)
else()
  include(FetchContent)
  FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
  )
  FetchContent_MakeAvailable(nlohmann_json)
endif()

target_link_libraries(rest_cpp
  PUBLIC
    Boost::system
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
)

### Tests
### -----
if (REST_CPP_BUILD_TESTS)

  include(FetchContent)

  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
  )

  if (MSVC)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  endif()
  enable_testing()
  FetchContent_MakeAvailable(googletest)

  file(GLOB_RECURSE REST_CPP_TEST_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cxx"
  )

  if (REST_CPP_TEST_SOURCES)
    add_executable(rest_cpp_tests
      ${REST_CPP_TEST_SOURCES}
    )
    target_link_libraries(rest_cpp_tests
      PRIVATE
        GTest::gtest_main
        rest_cpp

    )
    include(GoogleTest)
    gtest_discover_tests(rest_cpp_tests DISCOVERY_MODE PRE_TEST)

    if (_rest_cpp_top_level)
      rest_cpp_set_warnings(rest_cpp_tests)
    endif()

  else()
    message(STATUS "REST_CPP_BUILD_TESTS=ON but no tests found; skipping rest_cpp_tests.")
  endif()
endif()

### Install / Export (only when packaging)
if (REST_CPP_INSTALL)
  include(CMakePackageConfigHelpers)

  install(TARGETS rest_cpp
    EXPORT rest_cppTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  )

  # Install public headers
  install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  )
  install(EXPORT rest_cppTargets
    FILE rest_cppTargets.cmake
    NAMESPACE rest_cpp::
     DESTINATION ${REST_CPP_CMAKE_DIR}
  )

  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/rest_cppConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY AnyNewerVersion
  )
    configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/rest_cppConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/rest_cppConfig.cmake"
    INSTALL_DESTINATION ${REST_CPP_CMAKE_DIR}
  )

  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/rest_cppConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/rest_cppConfigVersion.cmake"
    DESTINATION ${REST_CPP_CMAKE_DIR}
  )
endif()
