#pragma once

#include <cstdint>
#include <array>
#include <vector>
#include <map>
#include <memory>
#include <string>
#include <random>

// ─── One rasterized glyph: grayscale alpha mask ───────────────────────────
struct GlyphRaster {
    uint32_t             codepoint  = 0;
    int                  width      = 0;   // == cellWidth
    int                  height     = 0;   // == cellHeight
    std::vector<uint8_t> alpha;            // row-major, one byte per pixel (0-255)
    int                  luminosity = 0;   // 0-100: percent of "on" pixels, rounded to int
};

// ─── Persistence-of-vision cache entry ────────────────────────────────────
// Tracks the most recently selected glyph for one luminosity bucket together
// with a frame-countdown.  While framesLeft > 0 the same raster is reused,
// giving each character a stable appearance for a natural-looking number of
// frames before a new glyph is randomly drawn.
struct LumGlyphPovTracker {
    const GlyphRaster* raster     = nullptr;  // non-owning; GlyphAtlas owns the unique_ptrs
    int                framesLeft = 0;
};

// ─── Loads & rasterizes all glyphs from one or more font files ────────────
// lumGlyphRasterMap is indexed by int luminosity (0-100), values are vectors
// of unique_ptrs to GlyphRaster instances.
class GlyphAtlas {
public:
    // Load all glyphs from each font file in fontFiles.
    // cellWidth/cellHeight: target raster size in pixels (== DRM cell size).
    bool load(const std::vector<std::string>& fontFiles, int cellWidth, int cellHeight);

    // Returns the GlyphRaster* whose luminosity key is closest to the query value.
    // If multiple glyphs share that key, one is chosen at random.
    // Both lower_bound and predecessor are checked for true nearest-neighbor.
    const GlyphRaster* findClosest(int luminosity) const;

    // Persistence-of-vision lookup.
    // Reuses the cached glyph for this luminosity bucket until its framesLeft
    // countdown reaches zero, then selects a new random glyph via findClosest()
    // and resets the countdown to a value uniformly drawn from [povMin, povMax).
    // povMin/povMax are the --glyph-pov-min / --glyph-pov-max CLI values.
    const GlyphRaster* findWithPov(int luminosity, int povMin, int povMax) const;

    bool empty() const { return lumGlyphRasterMap_.empty(); }
    size_t glyphCount() const;

    // Direct map access for tests.
    const std::map<int, std::vector<std::unique_ptr<GlyphRaster>>>& getLumMap() const {
        return lumGlyphRasterMap_;
    }

private:
    // Primary storage: lumGlyphRasterMap[lum] owns the GlyphRasters via unique_ptr.
    std::map<int, std::vector<std::unique_ptr<GlyphRaster>>> lumGlyphRasterMap_;

    // POV cache: one tracker per luminosity bucket, mutated on every render frame.
    mutable std::array<LumGlyphPovTracker, 101> lumGlyphPovMap_{};

    mutable std::mt19937 rng_{std::random_device{}()};
};
