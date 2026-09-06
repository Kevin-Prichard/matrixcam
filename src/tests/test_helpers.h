#pragma once

#include <string>
#include <cstdlib>

// ─── Shared test fixtures and paths ──────────────────────────────────────────

// Mock font path as specified in the SDD (prompt2.yaml).
// Relative path from the CMake build/test working directory (src2/build/):
//   ../../fonts/... resolves to matrixcam/fonts/...
inline const std::string kTestFontPath =
    "../../fonts/Noto_Serif_Hentaigana/static/NotoSerifHentaigana-Medium.ttf";

// Alternative: bold variant for multi-font tests
inline const std::string kTestFontPathBold =
    "../../fonts/Noto_Serif_Hentaigana/static/NotoSerifHentaigana-Bold.ttf";

// Default camera device used in Tier 1 hardware tests
inline const std::string kTestCameraDevice = "/dev/video0";

// Default DRM device used in Tier 1 hardware tests
inline const std::string kTestDrmDevice = "/dev/dri/card0";

// Cell size used in glyph atlas unit tests (small enough to be fast)
inline constexpr int kTestCellWidth  = 10;
inline constexpr int kTestCellHeight = 20;

// ─── Utility: check if a file exists ─────────────────────────────────────────
#include <sys/stat.h>
inline bool testFileExists(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0;
}
