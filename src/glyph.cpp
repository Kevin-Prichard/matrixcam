#include "glyph.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <algorithm>
#include <cmath>
#include <cstdio>

bool GlyphAtlas::load(const std::vector<std::string>& fontFiles,
                      int cellWidth, int cellHeight)
{
    FT_Library library;
    if (FT_Init_FreeType(&library)) {
        fprintf(stderr, "GlyphAtlas: failed to init FreeType\n");
        return false;
    }

    for (const auto& fontPath : fontFiles) {
        FT_Face face;
        if (FT_New_Face(library, fontPath.c_str(), 0, &face)) {
            fprintf(stderr, "GlyphAtlas: failed to load font: %s\n", fontPath.c_str());
            FT_Done_FreeType(library);
            return false;
        }
        if (FT_Set_Pixel_Sizes(face, (FT_UInt)cellWidth, (FT_UInt)cellHeight)) {
            fprintf(stderr, "GlyphAtlas: failed to set pixel sizes for font: %s\n", fontPath.c_str());
            FT_Done_Face(face);
            FT_Done_FreeType(library);
            return false;
        }

        // Iterate all glyphs available in this font via charcode enumeration
        FT_UInt  glyphIndex = 0;
        FT_ULong charcode   = FT_Get_First_Char(face, &glyphIndex);
        size_t   loaded     = 0;

        while (glyphIndex != 0) {
            if (FT_Load_Glyph(face, glyphIndex, FT_LOAD_DEFAULT)) {
                charcode = FT_Get_Next_Char(face, charcode, &glyphIndex);
                continue;
            }
            if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL)) {
                charcode = FT_Get_Next_Char(face, charcode, &glyphIndex);
                continue;
            }
            const FT_Bitmap& bmp = face->glyph->bitmap;
            if (bmp.width == 0 || bmp.rows == 0) {
                charcode = FT_Get_Next_Char(face, charcode, &glyphIndex);
                continue;
            }

            auto gr        = std::make_unique<GlyphRaster>();
            gr->codepoint  = (uint32_t)charcode;
            gr->width      = cellWidth;
            gr->height     = cellHeight;
            gr->alpha.assign((size_t)(cellWidth * cellHeight), 0u);

            // Center the glyph bitmap within the cell
            int offX = (cellWidth  - (int)bmp.width) / 2;
            int offY = (cellHeight - (int)bmp.rows)  / 2;

            uint64_t coveredPixels = 0;  // pixels with alpha > 0
            for (int y = 0; y < (int)bmp.rows; ++y) {
                for (int x = 0; x < (int)bmp.width; ++x) {
                    int dx = offX + x, dy = offY + y;
                    if (dx < 0 || dx >= cellWidth || dy < 0 || dy >= cellHeight) continue;
                    uint8_t a = bmp.buffer[y * bmp.pitch + x];
                    gr->alpha[(size_t)(dy * cellWidth + dx)] = a;
                    if (a > 0) ++coveredPixels;
                }
            }

            // Luminosity: percent of "on" pixels, as float, rounded to int (0-100)
            float lumF = (float)coveredPixels / (float)(cellWidth * cellHeight) * 100.0f;
            gr->luminosity = (int)std::round(lumF);

            int lum = gr->luminosity;
            lumGlyphRasterMap_[lum].push_back(std::move(gr));
            ++loaded;

            charcode = FT_Get_Next_Char(face, charcode, &glyphIndex);
        }

        printf("GlyphAtlas: loaded %zu glyphs from %s\n", loaded, fontPath.c_str());
        FT_Done_Face(face);
    }

    FT_Done_FreeType(library);

    if (lumGlyphRasterMap_.empty()) {
        fprintf(stderr, "GlyphAtlas: no glyphs loaded from any font file\n");
        return false;
    }
    printf("GlyphAtlas: %zu luminosity buckets, %zu total glyphs\n",
           lumGlyphRasterMap_.size(), glyphCount());
    return true;
}

const GlyphRaster* GlyphAtlas::findClosest(int luminosity) const {
    if (lumGlyphRasterMap_.empty()) return nullptr;

    // Both-neighbor check using lower_bound/upper_bound for true nearest
    auto it = lumGlyphRasterMap_.lower_bound(luminosity);

    if (it == lumGlyphRasterMap_.end()) {
        --it;  // luminosity is beyond last key; use last bucket
    } else if (it != lumGlyphRasterMap_.begin()) {
        auto prev = std::prev(it);
        // Choose closer of prev vs it
        if (luminosity - prev->first <= it->first - luminosity) {
            it = prev;
        }
    }

    const auto& vec = it->second;
    if (vec.empty()) return nullptr;

    if (vec.size() == 1) {
        return vec[0].get();
    } else {
        std::uniform_int_distribution<size_t> dist(0, vec.size() - 1);
        return vec[dist(rng_)].get();
    }
}

const GlyphRaster* GlyphAtlas::findWithPov(int luminosity, int povMin, int povMax) const {
    // Clamp inputs so povMax > povMin is always true.
    if (povMax <= povMin) povMax = povMin + 1;
    luminosity = std::clamp(luminosity, 0, 100);

    LumGlyphPovTracker& tracker = lumGlyphPovMap_[(size_t)luminosity];

    // Reuse cached glyph while the countdown is still positive.
    if (tracker.raster != nullptr && tracker.framesLeft-- > 0) {
        return tracker.raster;
    }

    // Countdown expired (or no entry yet): pick a new glyph and reset countdown.
    const GlyphRaster* r = findClosest(luminosity);
    if (!r) return nullptr;

    int range  = povMax - povMin;
    int frames = povMin + std::uniform_int_distribution<int>(0, range - 1)(rng_);

    tracker.raster     = r;
    tracker.framesLeft = frames;
    return r;
}

size_t GlyphAtlas::glyphCount() const {
    size_t total = 0;
    for (const auto& kv : lumGlyphRasterMap_) total += kv.second.size();
    return total;
}
