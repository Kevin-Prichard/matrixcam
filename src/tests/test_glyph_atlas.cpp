#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "../glyph.h"
#include "test_helpers.h"

// ─── Tier 2: GlyphAtlas unit tests (no hardware) ─────────────────────────────

// Helper: build a minimal GlyphAtlas by loading a real font (skips if unavailable)
class GlyphAtlasTest : public ::testing::Test {
protected:
    GlyphAtlas atlas;
    bool loaded = false;

    void SetUp() override {
        std::string fontPath = kTestFontPath;
        if (!testFileExists(fontPath)) fontPath = kTestFontPathBold;
        if (!testFileExists(fontPath)) {
            GTEST_SKIP() << "Test font not found; skipping atlas unit tests";
        }
        loaded = atlas.load({fontPath}, kTestCellWidth, kTestCellHeight);
    }
};

TEST_F(GlyphAtlasTest, LoadSucceeds) {
    EXPECT_TRUE(loaded);
    EXPECT_FALSE(atlas.empty());
}

TEST_F(GlyphAtlasTest, GlyphCountPositive) {
    EXPECT_GT(atlas.glyphCount(), 0u);
}

TEST_F(GlyphAtlasTest, LumMapNonEmpty) {
    const auto& m = atlas.getLumMap();
    EXPECT_FALSE(m.empty());
}

TEST_F(GlyphAtlasTest, LumMapKeysInRange0to100) {
    for (const auto& kv : atlas.getLumMap()) {
        EXPECT_GE(kv.first, 0);
        EXPECT_LE(kv.first, 100);
    }
}

TEST_F(GlyphAtlasTest, FindClosestReturnsValidPointer) {
    for (int lum : {0, 10, 50, 90, 100}) {
        const GlyphRaster* gr = atlas.findClosest(lum);
        ASSERT_NE(gr, nullptr) << "findClosest(" << lum << ") returned nullptr";
        EXPECT_GE(gr->luminosity, 0);
        EXPECT_LE(gr->luminosity, 100);
        EXPECT_GT(gr->alpha.size(), 0u);
        EXPECT_EQ(gr->width,  kTestCellWidth);
        EXPECT_EQ(gr->height, kTestCellHeight);
    }
}

TEST_F(GlyphAtlasTest, FindClosestNearestNeighbor) {
    // The returned key must be the closest available key to the query
    const auto& m = atlas.getLumMap();
    for (int query = 0; query <= 100; query += 5) {
        const GlyphRaster* gr = atlas.findClosest(query);
        if (!gr) continue;
        int returnedKey = gr->luminosity;
        // Check there's no key closer to query than returnedKey
        int dist_ret = std::abs(returnedKey - query);
        for (const auto& kv : m) {
            int dist_cand = std::abs(kv.first - query);
            // The returned key's distance must be ≤ every other key's distance
            EXPECT_LE(dist_ret, dist_cand)
                << "query=" << query << " returned key=" << returnedKey
                << " (dist=" << dist_ret << ") but key=" << kv.first
                << " (dist=" << dist_cand << ") is closer";
        }
    }
}

// ─── Free Tier 2 test (no font needed) ───────────────────────────────────────

TEST(GlyphAtlasUnit, FindClosestOnEmptyReturnsNull) {
    GlyphAtlas empty;
    EXPECT_EQ(empty.findClosest(50), nullptr);
}

// ─── Tier 1: hardware tests (real font, real cell size) ───────────────────────
#ifdef HARDWARE_AVAILABLE

TEST(GlyphAtlasTier1, LoadRealFontWithDisplayCellSize) {
    // Use a realistic display cell size (e.g. 1920x1080 / 80x30 = 24x36)
    constexpr int cellW = 24, cellH = 36;
    std::string fontPath = kTestFontPath;
    ASSERT_TRUE(testFileExists(fontPath)) << "Font not found: " << fontPath;

    GlyphAtlas atlas;
    ASSERT_TRUE(atlas.load({fontPath}, cellW, cellH));
    EXPECT_FALSE(atlas.empty());

    const auto& m = atlas.getLumMap();
    EXPECT_FALSE(m.empty());

    const GlyphRaster* gr = atlas.findClosest(50);
    ASSERT_NE(gr, nullptr);
    EXPECT_EQ(gr->width,  cellW);
    EXPECT_EQ(gr->height, cellH);
}

TEST(GlyphAtlasTier1, MultipleFontFiles) {
    std::vector<std::string> fonts = {kTestFontPath, kTestFontPathBold};
    bool anyExists = false;
    for (const auto& f : fonts) anyExists |= testFileExists(f);
    if (!anyExists) GTEST_SKIP() << "No test fonts found";

    std::vector<std::string> existingFonts;
    for (const auto& f : fonts)
        if (testFileExists(f)) existingFonts.push_back(f);

    GlyphAtlas atlas;
    ASSERT_TRUE(atlas.load(existingFonts, kTestCellWidth, kTestCellHeight));
    EXPECT_GT(atlas.glyphCount(), 0u);
}

#endif  // HARDWARE_AVAILABLE
