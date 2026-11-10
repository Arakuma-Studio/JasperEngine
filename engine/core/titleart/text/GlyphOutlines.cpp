// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GlyphOutlines.h"

#include "hb.h"

using namespace jasperengine::titleart;
using namespace jasperengine::titleart::text;

namespace {

    geometry::Path& pathOf(void* data) { return *static_cast<geometry::Path*>(data); }

    void moveTo(hb_draw_funcs_t*, void* data, hb_draw_state_t*, float x, float y, void*) { pathOf(data).moveTo(x, y); }

    void lineTo(hb_draw_funcs_t*, void* data, hb_draw_state_t*, float x, float y, void*) { pathOf(data).lineTo(x, y); }

    void quadTo(hb_draw_funcs_t*, void* data, hb_draw_state_t*, float cx, float cy, float x, float y, void*) {
        pathOf(data).quadTo(cx, cy, x, y);
    }

    void cubicTo(hb_draw_funcs_t*, void* data, hb_draw_state_t*, float c1x, float c1y, float c2x, float c2y, float x, float y, void*) {
        pathOf(data).cubicTo(c1x, c1y, c2x, c2y, x, y);
    }

    void closePath(hb_draw_funcs_t*, void* data, hb_draw_state_t*, void*) { pathOf(data).close(); }

    // immutable after creation (a function-local static: created once, on first use)
    hb_draw_funcs_t* drawFuncs() {
        static hb_draw_funcs_t* funcs = [] {
            hb_draw_funcs_t* f = hb_draw_funcs_create();
            hb_draw_funcs_set_move_to_func(f, moveTo, nullptr, nullptr);
            hb_draw_funcs_set_line_to_func(f, lineTo, nullptr, nullptr);
            hb_draw_funcs_set_quadratic_to_func(f, quadTo, nullptr, nullptr);
            hb_draw_funcs_set_cubic_to_func(f, cubicTo, nullptr, nullptr);
            hb_draw_funcs_set_close_path_func(f, closePath, nullptr, nullptr);
            hb_draw_funcs_make_immutable(f);
            return f;
        }();
        return funcs;
    }

}

const geometry::Path& GlyphOutlines::outline(hb_font_t* font, uint32_t glyph) {
    const auto key = std::make_pair(font, glyph);
    const auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    geometry::Path path;
    if (font) hb_font_draw_glyph_or_fail(font, glyph, drawFuncs(), &path);
    return cache.emplace(key, std::move(path)).first->second;
}

void GlyphOutlines::clear() {
    cache.clear();
}
