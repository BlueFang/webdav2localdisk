cmake_minimum_required(VERSION 3.21)
project(webdav2localdisk VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ----------------------------------------------------------------------
# WinFsp  ---  supports custom INC/LIB dirs for CI where SDK may be
#              separated from the MSI install root
# ----------------------------------------------------------------------
set(WINFSP_ROOT "C:/Program Files (x86)/WinFsp" CACHE PATH "WinFsp install root")
set(WINFSP_INC_DIR "${WINFSP_ROOT}/inc" CACHE PATH "WinFsp include directory")
set(WINFSP_LIB_DIR "${WINFSP_ROOT}/lib" CACHE PATH "WinFsp library directory")

if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(WINFSP_LIB "${WINFSP_LIB_DIR}/winfsp-x64.lib")
else()
  set(WINFSP_LIB "${WINFSP_LIB_DIR}/winfsp-x86.lib")
endif()

add_library(winfsp INTERFACE)
target_include_directories(winfsp SYSTEM INTERFACE "${WINFSP_INC_DIR}")
target_link_libraries(winfsp INTERFACE "${WINFSP_LIB}")

# ----------------------------------------------------------------------
# Qt
# ----------------------------------------------------------------------
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets)

# ----------------------------------------------------------------------
# Core library (non-Qt): fs_driver + webdav_client
# ----------------------------------------------------------------------
add_library(wdlcore STATIC
  src/fs_driver.cpp
  src/webdav_client.cpp
)
target_include_directories(wdlcore PUBLIC src)
target_link_libraries(wdlcore PUBLIC winfsp)
target_compile_definitions(wdlcore PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
if(MSVC)
  target_compile_options(wdlcore PRIVATE /W4 /permissive-)
endif()

# ----------------------------------------------------------------------
# GUI executable
# ----------------------------------------------------------------------
add_executable(webdav2localdisk
  ui/main.cpp
  ui/mainwindow.cpp
)
target_include_directories(webdav2localdisk PRIVATE src)
target_link_libraries(webdav2localdisk PRIVATE wdlcore Qt6::Widgets)
target_compile_definitions(webdav2localdisk PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)

if(MSVC)
  set_property(TARGET wdlcore webdav2localdisk PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ----------------------------------------------------------------------
# Deploy Qt DLLs next to the exe
# ----------------------------------------------------------------------
if(WIN32)
  find_program(WINDEPLOYQT windeployqt HINTS "${Qt6_DIR}/../../../bin" REQUIRED)
  add_custom_command(TARGET webdav2localdisk POST_BUILD
    COMMAND "${WINDEPLOYQT}" --no-translations --no-system-d3d-compiler
            --no-opengl-sw "$<TARGET_FILE_DIR:webdav2localdisk>/webdav2localdisk.exe"
    COMMENT "Running windeployqt"
  )
endif()

install(TARGETS webdav2localdisk RUNTIME DESTINATION bin)
install(FILES LICENSE README.md README_zh.md docs/PRINCIPLE.md DESTINATION share)
