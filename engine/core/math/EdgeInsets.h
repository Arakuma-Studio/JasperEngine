// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef EdgeInsets_h
#define EdgeInsets_h

#include "Export.h"

#include <cstdio>
#include <string>

namespace jasperengine {

    // Distances in from the four edges of a rectangle (safe-area insets, device cutouts)
    struct JASPERENGINE_API EdgeInsets {
        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;

        EdgeInsets() = default;
        EdgeInsets(float left, float top, float right, float bottom): left(left), top(top), right(right), bottom(bottom) {}

        bool operator==(const EdgeInsets& other) const {
            return left == other.left && top == other.top && right == other.right && bottom == other.bottom;
        }
        bool operator!=(const EdgeInsets& other) const { return !(*this == other); }

        bool isZero() const { return left == 0.0f && top == 0.0f && right == 0.0f && bottom == 0.0f; }

        std::string toString() const {
            char text[128];
            std::snprintf(text, sizeof(text), "EdgeInsets(%g, %g, %g, %g)", left, top, right, bottom);
            return text;
        }
    };

}

#endif /* EdgeInsets_h */
