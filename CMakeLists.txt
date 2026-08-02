# RecordFS — mount business records as a native Windows drive.
# Copyright (C) 2026 Marc Micalizzi
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.24)
project(recordfs VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenSSL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
# Optional: without the WinFsp SDK (Developer feature of the WinFsp install)
# the build still produces the protocol client; only `recordfs mount` is out.
find_package(WinFsp)

add_executable(recordfs
  src/main.cpp
  src/config.cpp
  src/credstore.cpp
  src/wsclient.cpp
  src/fsdclient.cpp
  src/recordspace.cpp
  src/filecache.cpp
  src/probe.cpp
)
target_include_directories(recordfs PRIVATE src shared)
target_link_libraries(recordfs PRIVATE
  OpenSSL::SSL OpenSSL::Crypto
  nlohmann_json::nlohmann_json
  Boost::headers
  ws2_32 crypt32 advapi32)
target_compile_definitions(recordfs PRIVATE
  _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
  target_compile_options(recordfs PRIVATE /W4 /EHsc /bigobj /utf-8)
  # WINDOWS subsystem with a classic main(): the logon agent must never flash
  # a console; CLI runs re-attach to the caller's console (main.cpp).
  target_link_options(recordfs PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
endif()

if(WinFsp_FOUND)
  target_sources(recordfs PRIVATE src/winfsp_fs.cpp)
  target_compile_definitions(recordfs PRIVATE RECORDFS_HAVE_WINFSP=1)
  target_link_libraries(recordfs PRIVATE WinFsp::WinFsp delayimp)
  # WinFsp guidance: delay-load the DLL and resolve it via FspLoad() so the
  # exe starts (with mount disabled) on machines without WinFsp installed.
  target_link_options(recordfs PRIVATE /DELAYLOAD:winfsp-x64.dll)
  message(STATUS "WinFsp SDK found - mount support enabled")
else()
  message(STATUS "WinFsp SDK not found - building protocol client only")
endif()

# Runtime DLLs: builds through the vcpkg toolchain get app-local DLL
# deployment automatically (VCPKG_APPLOCAL_DEPS). Builds against an external
# prefix (CMAKE_PREFIX_PATH) must co-locate libssl/libcrypto DLLs themselves.
