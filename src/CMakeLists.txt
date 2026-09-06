cmake_minimum_required(VERSION 3.16)
project(matrixcam LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─── Dependencies ─────────────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(FREETYPE   REQUIRED freetype2)
pkg_check_modules(LIBDRM     REQUIRED libdrm)

# turbojpeg removed
# pkg_check_modules(TURBOJPEG  REQUIRED libturbojpeg)
# turbojpeg replaced with
find_path(TURBOJPEG_INCLUDE_DIR turbojpeg.h REQUIRED)
find_library(TURBOJPEG_LIBRARY turbojpeg REQUIRED)

find_package(Threads REQUIRED)

# ─── Main executable ──────────────────────────────────────────────────────────
add_executable(matrixcam
    main.cpp
    camera.cpp
    drm_display.cpp
    glyph.cpp
)

target_include_directories(matrixcam PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${FREETYPE_INCLUDE_DIRS}
    ${LIBDRM_INCLUDE_DIRS}
    ${TURBOJPEG_INCLUDE_DIRS}
)

target_link_libraries(matrixcam
    ${FREETYPE_LIBRARIES}
    ${LIBDRM_LIBRARIES}
    ${TURBOJPEG_LIBRARY}
    Threads::Threads
)

target_compile_options(matrixcam PRIVATE
    ${FREETYPE_CFLAGS_OTHER}
    ${LIBDRM_CFLAGS_OTHER}
#    ${TURBOJPEG_CFLAGS_OTHER}
    -Wall -Wextra -Wpedantic
)

# ─── Tests ────────────────────────────────────────────────────────────────────
option(BUILD_TESTS          "Build the test suite"              ON)
option(HARDWARE_AVAILABLE   "Enable Tier 1 hardware-dependent tests" OFF)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ─── Install ──────────────────────────────────────────────────────────────────
install(TARGETS matrixcam RUNTIME DESTINATION bin)
