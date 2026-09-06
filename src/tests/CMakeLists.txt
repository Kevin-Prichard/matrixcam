cmake_minimum_required(VERSION 3.16)

# ─── Google Test / Mock ───────────────────────────────────────────────────────
find_package(GTest REQUIRED)
include(GoogleTest)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FREETYPE   REQUIRED freetype2)
pkg_check_modules(LIBDRM     REQUIRED libdrm)
pkg_check_modules(TURBOJPEG  REQUIRED libturbojpeg)
find_package(Threads REQUIRED)

# Common include path (source root = parent of tests/)
set(SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

# ─── Shared compile options ───────────────────────────────────────────────────
set(COMMON_INCLUDES
    ${SRC_ROOT}
    ${FREETYPE_INCLUDE_DIRS}
    ${LIBDRM_INCLUDE_DIRS}
    ${TURBOJPEG_INCLUDE_DIRS}
)
set(COMMON_LIBS
    GTest::gtest
    GTest::gmock
    GTest::gtest_main
    ${FREETYPE_LIBRARIES}
    ${LIBDRM_LIBRARIES}
    ${TURBOJPEG_LIBRARIES}
    Threads::Threads
)

# ─── test_frame_queue (Tier 2 — no hardware needed) ──────────────────────────
add_executable(test_frame_queue
    test_frame_queue.cpp
)
target_include_directories(test_frame_queue PRIVATE ${COMMON_INCLUDES})
target_link_libraries(test_frame_queue PRIVATE ${COMMON_LIBS})
target_compile_options(test_frame_queue PRIVATE -Wall -Wextra)
gtest_discover_tests(test_frame_queue WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# ─── test_glyph_atlas (Tier 2 + optional Tier 1) ─────────────────────────────
add_executable(test_glyph_atlas
    test_glyph_atlas.cpp
    ${SRC_ROOT}/glyph.cpp
)
target_include_directories(test_glyph_atlas PRIVATE ${COMMON_INCLUDES})
target_link_libraries(test_glyph_atlas PRIVATE ${COMMON_LIBS})
target_compile_options(test_glyph_atlas PRIVATE
    -Wall -Wextra
    ${FREETYPE_CFLAGS_OTHER}
)
if(HARDWARE_AVAILABLE)
    target_compile_definitions(test_glyph_atlas PRIVATE HARDWARE_AVAILABLE)
endif()
gtest_discover_tests(test_glyph_atlas WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# ─── test_camera (Tier 1) ─────────────────────────────────────────────────────
add_executable(test_camera
    test_camera.cpp
    ${SRC_ROOT}/camera.cpp
)
target_include_directories(test_camera PRIVATE ${COMMON_INCLUDES})
target_link_libraries(test_camera PRIVATE ${COMMON_LIBS})
target_compile_options(test_camera PRIVATE -Wall -Wextra)
if(HARDWARE_AVAILABLE)
    target_compile_definitions(test_camera PRIVATE HARDWARE_AVAILABLE)
endif()
gtest_discover_tests(test_camera WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# ─── test_drm_display (Tier 1) ────────────────────────────────────────────────
add_executable(test_drm_display
    test_drm_display.cpp
    ${SRC_ROOT}/drm_display.cpp
)
target_include_directories(test_drm_display PRIVATE ${COMMON_INCLUDES})
target_link_libraries(test_drm_display PRIVATE ${COMMON_LIBS})
target_compile_options(test_drm_display PRIVATE
    -Wall -Wextra
    ${LIBDRM_CFLAGS_OTHER}
)
if(HARDWARE_AVAILABLE)
    target_compile_definitions(test_drm_display PRIVATE HARDWARE_AVAILABLE)
endif()
gtest_discover_tests(test_drm_display WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# ─── test_pipeline (Tier 1 — end-to-end N frames) ────────────────────────────
add_executable(test_pipeline
    test_pipeline.cpp
    ${SRC_ROOT}/camera.cpp
    ${SRC_ROOT}/drm_display.cpp
    ${SRC_ROOT}/glyph.cpp
)
target_include_directories(test_pipeline PRIVATE ${COMMON_INCLUDES})
target_link_libraries(test_pipeline PRIVATE ${COMMON_LIBS})
target_compile_options(test_pipeline PRIVATE
    -Wall -Wextra
    ${FREETYPE_CFLAGS_OTHER}
    ${LIBDRM_CFLAGS_OTHER}
)
if(HARDWARE_AVAILABLE)
    target_compile_definitions(test_pipeline PRIVATE HARDWARE_AVAILABLE)
endif()
gtest_discover_tests(test_pipeline WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
