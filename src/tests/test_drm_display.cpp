#include <gtest/gtest.h>
#include "test_helpers.h"

// ─── Tier 1: DRM/KMS display integration tests (requires /dev/dri/card0) ─────
#ifdef HARDWARE_AVAILABLE

#include "../drm_display.h"
#include <cstring>

TEST(DrmDisplayTier1, OpenDevice) {
    DrmDisplay drm;
    ASSERT_TRUE(drm.open(kTestDrmDevice.c_str()))
        << "Could not open DRM device " << kTestDrmDevice;
}

TEST(DrmDisplayTier1, ListModes) {
    DrmDisplay drm;
    ASSERT_TRUE(drm.open(kTestDrmDevice.c_str()));

    auto modes = drm.getAvailableModes();
    ASSERT_FALSE(modes.empty()) << "No display modes found";

    for (const auto& m : modes) {
        EXPECT_GT(m.width,     0);
        EXPECT_GT(m.height,    0);
        EXPECT_GT(m.refreshHz, 0);
    }
}

TEST(DrmDisplayTier1, SetRequestedResolution) {
    DrmDisplay drm;
    ASSERT_TRUE(drm.open(kTestDrmDevice.c_str()));

    auto modes = drm.getAvailableModes();
    ASSERT_FALSE(modes.empty());

    // Use the first available mode
    const auto& mode = modes[0];
    ASSERT_TRUE(drm.setMode(mode.width, mode.height))
        << "setMode(" << mode.width << "x" << mode.height << ") failed";

    EXPECT_EQ(drm.width(),  mode.width);
    EXPECT_EQ(drm.height(), mode.height);
}

TEST(DrmDisplayTier1, AllocateBothBuffers) {
    DrmDisplay drm;
    ASSERT_TRUE(drm.open(kTestDrmDevice.c_str()));

    auto modes = drm.getAvailableModes();
    ASSERT_FALSE(modes.empty());
    ASSERT_TRUE(drm.setMode(modes[0].width, modes[0].height));
    ASSERT_TRUE(drm.allocateBuffers());

    EXPECT_GT(drm.stride(), 0u);
    EXPECT_NE(drm.getBackBuffer(), nullptr);
}

TEST(DrmDisplayTier1, WritePixelsAndFlip) {
    DrmDisplay drm;
    ASSERT_TRUE(drm.open(kTestDrmDevice.c_str()));

    auto modes = drm.getAvailableModes();
    ASSERT_FALSE(modes.empty());
    ASSERT_TRUE(drm.setMode(modes[0].width, modes[0].height));
    ASSERT_TRUE(drm.allocateBuffers());

    // Write a distinctive pattern to the back buffer
    uint8_t* buf = drm.getBackBuffer();
    ASSERT_NE(buf, nullptr);
    size_t size = (size_t)drm.stride() * (size_t)drm.height();
    memset(buf, 0x42, size);  // arbitrary non-zero pattern

    // Flip should not crash
    ASSERT_NO_THROW(drm.flip());

    // After flip the back buffer pointer moves to the other buffer
    uint8_t* newBack = drm.getBackBuffer();
    EXPECT_NE(newBack, buf) << "Back buffer should have swapped after flip";
}

#else

TEST(DrmDisplayTier1, SkippedWithoutHardware) {
    GTEST_SKIP() << "Tier 1 DRM tests require -DHARDWARE_AVAILABLE=ON";
}

#endif  // HARDWARE_AVAILABLE
