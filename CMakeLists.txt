cmake_minimum_required(VERSION 3.24)

project(
  Hyperlink
  VERSION 0.1.0
  DESCRIPTION "C++ library for high-speed peer-to-peer data transfer over USB links"
  LANGUAGES CXX)

option(HYPERLINK_BUILD_EXAMPLES "Build Hyperlink example programs" ON)
option(HYPERLINK_BUILD_TESTS "Build Hyperlink tests" ON)
option(HYPERLINK_ENABLE_LIBUSB "Enable the libusb transport backend when libusb is available" ON)
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

add_library(hyperlink
  src/auto_config.cpp
  src/network_transport.cpp
  src/session.cpp
)

add_library(Hyperlink::hyperlink ALIAS hyperlink)

target_compile_features(hyperlink PUBLIC cxx_std_20)

set_target_properties(hyperlink
  PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

target_include_directories(hyperlink
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_definitions(hyperlink
  PUBLIC
    $<$<BOOL:${BUILD_SHARED_LIBS}>:HYPERLINK_SHARED>
  PRIVATE
    HYPERLINK_BUILDING_LIBRARY
)

if(MSVC)
  target_compile_options(hyperlink PRIVATE /W4 /permissive-)
else()
  target_compile_options(hyperlink PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
  target_link_libraries(hyperlink PRIVATE ws2_32)
endif()

if(HYPERLINK_ENABLE_LIBUSB)
  find_package(Libusb QUIET)

  if(TARGET Libusb::Libusb)
    target_sources(hyperlink PRIVATE src/usb_libusb_transport.cpp)
    target_link_libraries(hyperlink PRIVATE Libusb::Libusb)
    target_compile_definitions(hyperlink PUBLIC HYPERLINK_HAS_LIBUSB=1)
  else()
    message(STATUS "libusb-1.0 was not found; building without the libusb transport backend")
    target_sources(hyperlink PRIVATE src/usb_unavailable_transport.cpp)
    target_compile_definitions(hyperlink PUBLIC HYPERLINK_HAS_LIBUSB=0)
  endif()
else()
  target_sources(hyperlink PRIVATE src/usb_unavailable_transport.cpp)
  target_compile_definitions(hyperlink PUBLIC HYPERLINK_HAS_LIBUSB=0)
endif()

if(HYPERLINK_BUILD_EXAMPLES)
  add_executable(hyperlink_probe examples/probe.cpp)
  target_link_libraries(hyperlink_probe PRIVATE Hyperlink::hyperlink)

  add_executable(hyperlink_bench examples/bench.cpp)
  target_link_libraries(hyperlink_bench PRIVATE Hyperlink::hyperlink)

  add_executable(hyperlink_file examples/file_transfer.cpp)
  target_link_libraries(hyperlink_file PRIVATE Hyperlink::hyperlink)
endif()

if(HYPERLINK_BUILD_TESTS)
  enable_testing()
  add_executable(hyperlink_tests
    tests/auto_config_tests.cpp
    tests/session_tests.cpp
    tests/test_main.cpp
  )
  target_link_libraries(hyperlink_tests PRIVATE Hyperlink::hyperlink)
  add_test(NAME hyperlink_tests COMMAND hyperlink_tests)
endif()
