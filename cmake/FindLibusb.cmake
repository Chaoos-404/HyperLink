find_package(PkgConfig QUIET)

if(PkgConfig_FOUND)
  pkg_check_modules(PC_LIBUSB QUIET libusb-1.0)
endif()

find_path(LIBUSB_INCLUDE_DIR
  NAMES libusb.h
  HINTS ${PC_LIBUSB_INCLUDE_DIRS}
  PATH_SUFFIXES libusb-1.0
)

find_library(LIBUSB_LIBRARY
  NAMES usb-1.0 libusb-1.0 libusb
  HINTS ${PC_LIBUSB_LIBRARY_DIRS}
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Libusb
  REQUIRED_VARS LIBUSB_LIBRARY LIBUSB_INCLUDE_DIR
  VERSION_VAR PC_LIBUSB_VERSION
)

if(Libusb_FOUND AND NOT TARGET Libusb::Libusb)
  add_library(Libusb::Libusb UNKNOWN IMPORTED)
  set_target_properties(Libusb::Libusb
    PROPERTIES
      IMPORTED_LOCATION "${LIBUSB_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${LIBUSB_INCLUDE_DIR}"
  )
endif()

mark_as_advanced(LIBUSB_INCLUDE_DIR LIBUSB_LIBRARY)
