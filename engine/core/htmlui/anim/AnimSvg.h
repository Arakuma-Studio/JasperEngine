// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ANIMSVG_H
#define ANIMSVG_H

#include "Export.h"
#include "AnimParams.h"

namespace jasperengine {

    class UIElement;

    class JASPERENGINE_API AnimSvg {
    public:
        static AnimTargetList createDrawable(const AnimTargetList& targets, double start = 0, double end = 0);
        static AnimParam morphTo(const AnimTargetList& targets, double precision = 0.33);
        static AnimParam createMotionPath(const AnimTargetList& targets, double offset = 0);
        // Attribute writes by the existing anime element target (draw is a normalized range).
        static void setDraw(const UIElement& element, const std::string& value);
    };

}

#endif
