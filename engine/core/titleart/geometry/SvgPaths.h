// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R25, §6.6 "Vector (SVG)")
// - Purpose: the geometry of an SVG for Vector layers in `shape` mode (fills become outlines,
//   strokes stay strokes, the SVG's colours are ignored). LunaSVG renders but exposes no path
//   data, so this reads the file with tinyxml2: path data (the whole `d` grammar, arcs as
//   cubics), rect, circle, ellipse, line, polyline, polygon, nested g, transform, local use
//   (and symbol through it), fill-rule, stroke, stroke-width, stroke-linejoin, stroke-linecap,
//   stroke-miterlimit, display and visibility, from attributes, `style` and simple class rules
//   (`.name { ... }` in a <style>). Anything else that draws (text, images, filters, clip
//   paths, masks, markers, patterns, other CSS selectors, relative units) is ignored and named
//   in `ignored` (problem svg_feature_ignored).
// - Space: SVG user space of the root (y down), every transform applied; stroke widths scale
//   with the transform. TitleGeometry fits and flips it into layer space.
// - Thread-safety: pure.

#pragma once

#include "titleart/geometry/Offset.h"
#include "titleart/geometry/Path.h"

#include <string>
#include <vector>

namespace jasperengine::titleart::geometry {

    struct SvgStroke {
        Path path;                  // contours open unless closed in the SVG
        double width = 1.0;
        OffsetJoin join = OffsetJoin::Miter;
        OffsetCap cap = OffsetCap::Butt;
        double miterLimit = 4.0;
    };

    struct SvgGeometry {
        bool ok = false;
        std::string error;                  // not XML, or no <svg> root
        std::vector<Path> fills;            // evenOdd from fill-rule
        std::vector<SvgStroke> strokes;
        std::vector<std::string> ignored;   // sorted, unique: "text", "filter", "css selector 'rect'" ...
        double width = 0.0, height = 0.0;   // the root's viewport (viewBox, else width / height), 0 when absent
    };

    SvgGeometry parseSvgGeometry(const std::string& text);
    // the path of an SVG path-data string (`d`), user units; false on a syntax error (what parsed
    // before it is kept, as SVG renderers do)
    bool parseSvgPathData(const std::string& d, Path& out);

}
