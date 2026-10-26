// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUIPRIMITIVE_H
#define HTMLUIPRIMITIVE_H

#include <cstdint>

namespace jasperengine {

    // One instance of the HTMLUI_PRIM pipeline (S16 R8, htmlui_prim.vert): a glyph or image quad
    // (QUAD), a box with its border (BOX) or a box shadow (SHADOW_OUTER, SHADOW_INSET), positions
    // in document px with the translation baked in. Seven FLOAT4 and five UBYTE4N attributes.
    struct HtmlUIPrimitive {
        enum Mode : uint8_t { QUAD = 0, BOX = 1, SHADOW_OUTER = 2, SHADOW_INSET = 3 };
        float a[4];             // QUAD: corners 0 and 1; BOX: the border box; shadows: their shape
        float b[4];             // QUAD: corners 2 and 3; else the radii of a (x)
        float c[4];             // the radii of a (y)
        float d[4];             // QUAD: uv rect; BOX: border widths (top, right, bottom, left); shadows: the clip box
        float e[4];             // BOX: the fill's insets (top, right, bottom, left); shadows: the clip box's radii (x)
        float f[4];             // shadows: the clip box's radii (y)
        float params[4];        // x: mode, y: sigma (shadows), z: QUAD: the texture slot + 1 (0: untextured), w: 0
        uint8_t color[4];       // premultiplied: the quad's colour, the fill, the shadow's colour
        uint8_t border[4][4];   // BOX: top, right, bottom, left
    };
    static_assert(sizeof(HtmlUIPrimitive) == 132 && sizeof(HtmlUIPrimitive) <= 144, "an HTMLUI_PRIM instance is 132 bytes");
    static_assert(alignof(HtmlUIPrimitive) == 4, "HTMLUI_PRIM instances are tightly packed");

}

#endif //HTMLUIPRIMITIVE_H
