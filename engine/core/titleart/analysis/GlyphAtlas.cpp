// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/generators/rings/magicCircle.ts (glyphAtlas) by SafEight, used with
// the author's permission.

#include "GlyphAtlas.h"

#include "titleart/geometry/TitleGeometry.h"
#include "titleart/text/GlyphOutlines.h"

#include "util/DefaultFont.h"
#include "util/FontFace.h"

#include "hb.h"

#include <algorithm>
#include <cmath>
#include <set>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::analysis;

namespace {

    std::shared_ptr<FontFace> engineBuiltinFace() {
        static std::shared_ptr<FontFace> face =
            FontFace::loadMemory(roboto_regular_ttf, sizeof(roboto_regular_ttf), 0, false, "built-in (Roboto)");
        return face;
    }

    GlyphFont builtinGlyphFont(const AssetResolver* assets) {
        GlyphFont out;
        out.face = assets ? assets->builtinFont() : nullptr;
        if (!out.face) out.face = engineBuiltinFace();
        out.key = "builtin:" + (out.face ? out.face->getLabel() : std::string());
        out.name = "the built-in face";
        return out;
    }

    // the face of a resolved font asset, or null with `error`
    std::shared_ptr<FontFace> loadResolved(const AssetResolver* assets, const TitleAssetRef& ref, ResolvedTitleAsset& resolved, std::string& error) {
        if (!assets) {
            error = "no asset resolver for this render";
            return nullptr;
        }
        resolved = assets->resolve(ref, TitleAssetKind::Font);
        if (!resolved.found) {
            error = "not found";
            return nullptr;
        }
        std::shared_ptr<FontFace> face = assets->loadFont(resolved, 0, &error);
        if (!face && error.empty()) error = "unreadable";
        return face;
    }

}

std::vector<uint32_t> analysis::utf8Codepoints(std::string_view text) {
    std::vector<uint32_t> out;
    size_t i = 0;
    while (i < text.size()) {
        const uint8_t c = (uint8_t)text[i];
        const int extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (extra < 0 || i + (size_t)extra >= text.size()) {
            out.push_back(0xFFFD);
            i++;
            continue;
        }
        uint32_t cp = extra == 0 ? c : extra == 1 ? (c & 0x1Fu) : extra == 2 ? (c & 0x0Fu) : (c & 0x07u);
        bool valid = true;
        for (int k = 1; k <= extra; k++) {
            const uint8_t b = (uint8_t)text[i + (size_t)k];
            if ((b & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            cp = (cp << 6) | (b & 0x3Fu);
        }
        if (!valid) {
            out.push_back(0xFFFD);
            i++;
            continue;
        }
        out.push_back(cp);
        i += (size_t)extra + 1;
    }
    return out;
}

std::string analysis::utf8Char(uint32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
    return out;
}

GlyphFont analysis::resolveGlyphFont(const AssetResolver* assets, const TitleAssetRef& font, const TitleAssetRef& library) {
    std::string error;
    ResolvedTitleAsset resolved;
    if (!font.empty()) {
        if (std::shared_ptr<FontFace> face = loadResolved(assets, font, resolved, error)) {
            return {face, resolved.path + "#" + std::to_string(resolved.revision), resolved.assetPath.empty() ? font.path : resolved.assetPath, ""};
        }
        GlyphFont out = builtinGlyphFont(assets);
        out.error = "font '" + font.path + "': " + error;
        return out;
    }
    // the library font is a project asset once S84 has copied it in; until then the built-in
    // face draws what it covers (risk 8) and the caller reports the rest as glyph_missing
    if (!library.empty()) {
        if (std::shared_ptr<FontFace> face = loadResolved(assets, library, resolved, error)) {
            return {face, resolved.path + "#" + std::to_string(resolved.revision), resolved.assetPath.empty() ? library.path : resolved.assetPath, ""};
        }
    }
    return builtinGlyphFont(assets);
}

GlyphAtlas analysis::buildGlyphAtlas(const FontFace& face, const std::vector<uint32_t>& codepoints, int cell) {
    GlyphAtlas atlas;
    const int count = std::min((int)codepoints.size(), GLYPH_ATLAS_MAX_GLYPHS);
    hb_font_t* font = face.getHbFont();
    if (count <= 0 || cell <= 0 || !font) return atlas;
    atlas.count = count;
    atlas.cell = cell;
    atlas.cols = std::min(GLYPH_ATLAS_COLUMNS, count);
    atlas.rows = (count + atlas.cols - 1) / atlas.cols;
    const int width = atlas.cols * cell, height = atlas.rows * cell;

    // em at 84 % of the cell: room for the outline's anti-aliasing and the linear filter's reach
    const double upem = std::max(1, face.getUnitsPerEm());
    const double em = 0.84 * cell;
    const double k = em / upem;
    const double ascender = face.getAscender(), descender = face.getDescender();
    const double emH = (ascender - descender) * k;

    text::GlyphOutlines outlines;
    geometry::TitleElement element;
    std::set<uint32_t> missing;
    for (int i = 0; i < count; i++) {
        const uint32_t cp = codepoints[(size_t)i];
        uint32_t glyph = face.getGlyphIndex(cp);
        // Pachislop's charToGlyph: a character the font lacks draws its .notdef
        if (glyph == 0 && missing.insert(cp).second) atlas.missing.push_back(cp);
        const geometry::Path& outline = outlines.outline(font, glyph);
        if (outline.empty()) continue;
        const int hbAdvance = hb_font_get_glyph_h_advance(font, glyph);
        const double adv = (hbAdvance != 0 ? hbAdvance : upem) * k;
        // the cell's top-left corner and the baseline in canvas rows (y down), as Pachislop's
        // getPath(x + (cell - adv) / 2, y + (cell - emH) / 2 + ascender k, em)
        const double x0 = (i % atlas.cols) * cell + 0.5 * (cell - adv);
        const double baseline = (double)(i / atlas.cols) * cell + 0.5 * (cell - emH) + ascender * k;
        // font units (y up) -> the mask frame's layer space (y up, the atlas' top at `height`)
        geometry::Affine toAtlas;
        toAtlas.a = k;
        toAtlas.d = k;
        toAtlas.e = x0;
        toAtlas.f = height - baseline;
        geometry::Path placed = outline;
        placed.transform(toAtlas);
        element.outlines.push_back(std::move(placed));
    }
    geometry::MaskFrame frame;
    frame.width = width;
    frame.height = height;
    frame.scale = 1.0;
    frame.origin = {0.0, (double)height};
    geometry::CoverageMask mask = geometry::rasterizeElement(element, frame);
    atlas.coverage = std::move(mask.pixels);
    atlas.coverage.resize((size_t)width * (size_t)height, 0);
    return atlas;
}
