# SPDX-License-Identifier: GPL-3.0-only
# Locates an installed WinFsp SDK (the "Developer" feature of the WinFsp MSI).
# Defines WinFsp::WinFsp when found.

set(_winfsp_hints "C:/Program Files (x86)/WinFsp")
if(DEFINED ENV{WINFSP_ROOT})
  list(PREPEND _winfsp_hints "$ENV{WINFSP_ROOT}")
endif()

find_path(WinFsp_INCLUDE_DIR winfsp/winfsp.h
  HINTS ${_winfsp_hints} PATH_SUFFIXES inc)
find_library(WinFsp_LIBRARY winfsp-x64
  HINTS ${_winfsp_hints} PATH_SUFFIXES lib)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(WinFsp DEFAULT_MSG
  WinFsp_INCLUDE_DIR WinFsp_LIBRARY)

if(WinFsp_FOUND AND NOT TARGET WinFsp::WinFsp)
  add_library(WinFsp::WinFsp UNKNOWN IMPORTED)
  set_target_properties(WinFsp::WinFsp PROPERTIES
    IMPORTED_LOCATION "${WinFsp_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${WinFsp_INCLUDE_DIR}")
endif()
mark_as_advanced(WinFsp_INCLUDE_DIR WinFsp_LIBRARY)
