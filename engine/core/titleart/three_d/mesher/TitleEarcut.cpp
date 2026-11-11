// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleEarcut.h"

#include "mapbox/earcut.hpp"

#include <array>

namespace mapbox::util {

    template <>
    struct nth<0, jasperengine::titleart::TitleVec2> {
        static double get(const jasperengine::titleart::TitleVec2& p) { return p.x; }
    };
    template <>
    struct nth<1, jasperengine::titleart::TitleVec2> {
        static double get(const jasperengine::titleart::TitleVec2& p) { return p.y; }
    };

}

std::vector<uint32_t> jasperengine::titleart::three_d::triangulatePolygon(const Contour& outer, const std::vector<Contour>& holes) {
    std::vector<Contour> polygon;
    polygon.reserve(1 + holes.size());
    polygon.push_back(outer);
    for (const Contour& h : holes) polygon.push_back(h);
    return mapbox::earcut<uint32_t>(polygon);
}
