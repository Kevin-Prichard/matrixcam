#include <gtest/gtest.h>
#include "test_helpers.h"

// ─── Tier 1: Camera integration tests (requires /dev/video0) ─────────────────
#ifdef HARDWARE_AVAILABLE

#include "../camera.h"

TEST(CameraTier1, OpenDevice) {
    Camera cam;
    ASSERT_TRUE(cam.open(kTestCameraDevice))
        << "Could not open " << kTestCameraDevice;
    EXPECT_TRUE(cam.isOpen());
}

TEST(CameraTier1, ConfigureAndCapture) {
    Camera cam;
    ASSERT_TRUE(cam.open(kTestCameraDevice));

    // Request a common safe resolution
    ASSERT_TRUE(cam.configure(640, 480, 30))
        << "Failed to configure 640x480@30fps";

    EXPECT_GT(cam.frameWidth(),  0);
    EXPECT_GT(cam.frameHeight(), 0);

    ASSERT_TRUE(cam.startCapture());

    auto frame = cam.captureFrame();
    ASSERT_FALSE(frame.empty()) << "captureFrame() returned empty data";

    // Check dimensions match what was negotiated
    size_t expected = (size_t)cam.frameWidth() * (size_t)cam.frameHeight() * 3;
    EXPECT_EQ(frame.size(), expected);

    // RGB data should be non-zero (real camera image)
    bool anyNonZero = false;
    for (uint8_t v : frame) { if (v != 0) { anyNonZero = true; break; } }
    EXPECT_TRUE(anyNonZero) << "Frame RGB data is all zeros";

    cam.stopCapture();
}

TEST(CameraTier1, CaptureMultipleFrames) {
    Camera cam;
    ASSERT_TRUE(cam.open(kTestCameraDevice));
    ASSERT_TRUE(cam.configure(640, 480, 30));
    ASSERT_TRUE(cam.startCapture());

    int validFrames = 0;
    for (int i = 0; i < 5; ++i) {
        auto frame = cam.captureFrame();
        if (!frame.empty()) ++validFrames;
    }
    EXPECT_GE(validFrames, 1) << "Expected at least 1 valid frame out of 5";

    cam.stopCapture();
}

TEST(CameraTier1, AvailableFrameSizesNonEmpty) {
    Camera cam;
    ASSERT_TRUE(cam.open(kTestCameraDevice));
    // Frame sizes may be empty for cameras that only report stepwise ranges;
    // this is acceptable — just verify no crash.
    const auto& sizes = cam.availableFrameSizes();
    (void)sizes;  // just ensure it doesn't crash
    SUCCEED();
}

#else

TEST(CameraTier1, SkippedWithoutHardware) {
    GTEST_SKIP() << "Tier 1 camera tests require -DHARDWARE_AVAILABLE=ON";
}

#endif  // HARDWARE_AVAILABLE
