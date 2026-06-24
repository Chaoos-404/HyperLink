cmake_minimum_required(VERSION 3.24)

project(
  Hyperlink
  VERSION 0.1.6
  DESCRIPTION "C++ library for high-speed peer-to-peer data transfer over USB links"
  LANGUAGES CXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

option(HYPERLINK_BUILD_EXAMPLES "Build Hyperlink example programs" ON)
option(HYPERLINK_BUILD_TESTS "Build Hyperlink tests" ON)
option(HYPERLINK_ENABLE_LIBUSB "Enable the libusb transport backend when libusb is available" ON)
set(HYPERLINK_PACKAGE_TARGET "" CACHE STRING "Package target suffix used by CPack")
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
  if(WIN32)
    target_link_libraries(hyperlink_file PRIVATE iphlpapi)
  endif()

  add_executable(hyperlink_cli examples/cli.cpp)
  set_target_properties(hyperlink_cli PROPERTIES OUTPUT_NAME hyperlink)
  target_compile_features(hyperlink_cli PRIVATE cxx_std_20)
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

install(
  TARGETS hyperlink
  EXPORT HyperlinkTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(HYPERLINK_BUILD_EXAMPLES)
  install(
    TARGETS hyperlink_cli hyperlink_file hyperlink_bench hyperlink_probe
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

install(
  DIRECTORY include/
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

install(
  EXPORT HyperlinkTargets
  NAMESPACE Hyperlink::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Hyperlink")

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/HyperlinkConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/HyperlinkConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/HyperlinkConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Hyperlink")

install(
  FILES
    "${CMAKE_CURRENT_BINARY_DIR}/HyperlinkConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/HyperlinkConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Hyperlink")

set(CPACK_PACKAGE_NAME "Hyperlink")
set(CPACK_PACKAGE_VENDOR "Hyperlink contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Hyperlink")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")

set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/Chaoos-404/HyperLink")

if(WIN32)
  set(CPACK_GENERATOR "ZIP")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(CPACK_GENERATOR "TGZ;DEB")
else()
  set(CPACK_GENERATOR "TGZ")
endif()

set(CPACK_DEBIAN_PACKAGE_NAME "hyperlink")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Hyperlink contributors")
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")

if(HYPERLINK_PACKAGE_TARGET)
  set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${PROJECT_VERSION}-${HYPERLINK_PACKAGE_TARGET}")
else()
  set(CPACK_PACKAGE_FILE_NAME
      "${CPACK_PACKAGE_NAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
endif()

include(CPack)
