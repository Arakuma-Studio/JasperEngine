// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ANIMSVGPATH_H
#define ANIMSVGPATH_H

#include "Export.h"

#include <map>
#include <string>
#include <vector>

namespace jasperengine {

    struct AnimSvgPoint { double x = 0, y = 0; };

    // The SVG geometry used by Anime.js getTotalLength/getPointAtLength. Geometry is kept in
    // user units; the viewport transform is applied only for motion paths on HTML targets.
    class JASPERENGINE_API AnimSvgPath {
    public:
        static AnimSvgPath fromAttributes(const std::string& tag, const std::map<std::string, std::string>& attributes);
        bool isValid() const { return valid; }
        double length() const { return totalLength; }
        AnimSvgPoint pointAtLength(double distance) const;

    private:
        struct Segment { AnimSvgPoint from, to; double end = 0; };
        void moveTo(AnimSvgPoint point);
        void lineTo(AnimSvgPoint point);
        void cubicTo(AnimSvgPoint a, AnimSvgPoint b, AnimSvgPoint end, int depth = 0);
        std::vector<Segment> segments;
        AnimSvgPoint cursor, first;
        double totalLength = 0;
        double circleX = 0, circleY = 0, circleRadius = -1;
        bool valid = false;
    };

}

#endif
