#include <gtest/gtest.h>
#include "test_helpers.h"

// ─── Tier 1: Full pipeline integration test (camera → queue → render → DRM) ──
#ifdef HARDWARE_AVAILABLE

#include "../camera.h"
#include "../drm_display.h"
#include "../glyph.h"
#include "../frame_queue.h"

#include <thread>
#include <atomic>
#include <chrono>

using namespace std::chrono_literals;

TEST(PipelineTier1, CameraToQueueToRenderFlip) {
    constexpr int kTargetFrames = 10;
    constexpr int conCharWidth  = 80;
    constexpr int conCharHeight = 30;

    // ── Open DRM and find a usable mode ────────────────────────────────────
    DrmDisplay drm;
    ASSERT_TRUE(drm.open(kTestDrmDevice.c_str()));
    auto modes = drm.getAvailableModes();
    ASSERT_FALSE(modes.empty()) << "No DRM modes available";

    const auto& mode = modes[0];
    ASSERT_TRUE(drm.setMode(mode.width, mode.height));
    ASSERT_TRUE(drm.allocateBuffers());

    // ── Compute cell dimensions ────────────────────────────────────────────
    int cellW = mode.width  / conCharWidth;
    int cellH = mode.height / conCharHeight;
    ASSERT_GT(cellW, 0);
    ASSERT_GT(cellH, 0);

    // ── Load glyph atlas ───────────────────────────────────────────────────
    ASSERT_TRUE(testFileExists(kTestFontPath)) << "Test font not found";
    GlyphAtlas atlas;
    ASSERT_TRUE(atlas.load({kTestFontPath}, cellW, cellH));
    ASSERT_FALSE(atlas.empty());

    // ── Open and configure camera ──────────────────────────────────────────
    Camera cam;
    ASSERT_TRUE(cam.open(kTestCameraDevice));
    ASSERT_TRUE(cam.configure(640, 480, 30));
    ASSERT_TRUE(cam.startCapture());

    // ── Create queue ───────────────────────────────────────────────────────
    FrameQueue queue(4);
    std::atomic<bool> running{true};
    std::atomic<int>  renderedFrames{0};

    // ── Camera thread ──────────────────────────────────────────────────────
    std::thread tCam([&] {
        while (running) {
            auto rgb = cam.captureFrame();
            if (rgb.empty()) continue;
            VideoFrame f;
            f.rgbData   = std::move(rgb);
            f.width     = cam.frameWidth();
            f.height    = cam.frameHeight();
            f.timestamp = std::chrono::steady_clock::now();
            queue.push(std::move(f));
        }
        queue.stop();
    });

    // ── Render thread ──────────────────────────────────────────────────────
    std::thread tRend([&] {
        while (true) {
            if (!queue.poll()) break;
            auto frameOpt = queue.pop();
            if (!frameOpt) continue;

            drm.clearBackBuffer();
            uint8_t* fb = drm.getBackBuffer();
            (void)fb;
            // Simplified render: just flip (no full glyph blit to keep test fast)
            drm.flip();

            ++renderedFrames;
            if (renderedFrames >= kTargetFrames) {
                running = false;
                break;
            }
        }
    });

    // ── Wait for completion with timeout ───────────────────────────────────
    auto deadline = std::chrono::steady_clock::now() + 30s;
    while (renderedFrames < kTargetFrames &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(100ms);
    }

    running = false;
    queue.stop();
    tCam.join();
    tRend.join();
    cam.stopCapture();

    EXPECT_GE(renderedFrames.load(), kTargetFrames)
        << "Pipeline did not render " << kTargetFrames << " frames within timeout";
}

#else

TEST(PipelineTier1, SkippedWithoutHardware) {
    GTEST_SKIP() << "Tier 1 pipeline test requires -DHARDWARE_AVAILABLE=ON";
}

#endif  // HARDWARE_AVAILABLE
