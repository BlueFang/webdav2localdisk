cmake_minimum_required(VERSION 3.20)
project(webdav2localdisk VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- WinFsp lookup -------------------------------------------------------
# Set WINFSP_ROOT to your installed WinFsp (default: C:\Program Files\WinFsp).
if(NOT DEFINED WINFSP_ROOT)
  set(WINFSP_ROOT "C:/Program Files (x86)/WinFsp")
endif()

if(NOT EXISTS "${WINFSP_ROOT}/inc/winfsp")
  message(FATAL_ERROR
    "WinFsp headers not found under '${WINFSP_ROOT}'.\n"
    "Install WinFsp from https://winfsp.dev/ and either:\n"
    "  - set WINFSP_ROOT=<path> on the cmake command line, or\n"
    "  - drop a WinFsp distribution into third_party/winfsp.")
endif()

add_library(winfsp INTERFACE)
target_include_directories(winfsp INTERFACE "${WINFSP_ROOT}/inc")
target_link_libraries(winfsp INTERFACE
  "${WINFSP_ROOT}/lib/winfsp_x64.lib"
  "${WINFSP_ROOT}/lib/winfsp-$<CONFIG>.lib")

# ---- OpenSSL (for the SHA-1 used by the local cache) --------------------
find_package(OpenSSL REQUIRED)

# ---- main binary --------------------------------------------------------
add_executable(webdav2localdisk
  src/main.cpp
  src/fs_driver.cpp
  src/webdav_client.cpp
  src/cache.cpp)

target_link_libraries(webdav2localdisk
  PRIVATE winfsp OpenSSL::Crypto)

target_compile_definitions(webdav2localdisk PRIVATE
  UNICODE _UNICODE
  WIN32_LEAN_AND_MEAN)

if(MSVC)
  target_compile_options(webdav2localdisk PRIVATE /W4 /permissive-)
else()
  target_compile_options(webdav2localdisk PRIVATE -Wall -Wextra -Werror)
endif()

# ---- installer ----------------------------------------------------------
install(TARGETS webdav2localdisk RUNTIME DESTINATION bin)
install(FILES LICENSE README.md docs/PRINCIPLE.md DESTINATION share)
