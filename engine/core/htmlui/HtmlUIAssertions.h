// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUIASSERTIONS_H
#define HTMLUIASSERTIONS_H

#include "Export.h"
#include "htmlui/HtmlUIInspector.h"

#include <cstddef>
#include <string>
#include <vector>

namespace jasperengine::htmlui {

// Layout assertions over a live HTML UI document (MCP assert_ui), on top of HtmlUIInspector's
// view of it (same refs, boxes and coordinates). Zero matches fail every kind except Count
// with max or count 0 ("selector matched nothing"). Main thread only.
enum class AssertKind { Exists, Count, Visible, TextFits, NoOverlap, WithinParent, WithinViewport, Contrast, MinSize };

struct AssertSpec {
    AssertKind kind = AssertKind::Exists;
    std::string selector;
    std::string with;          // NoOverlap: second set (empty: the selector's matches pairwise)
    std::string container;     // WithinParent: closest ancestor matching this instead of the parent
    int count = -1, min = -1, max = -1;   // Count (Exists = min 1)
    float tolerance = 0.5f;    // px
    float minRatio = 0;        // Contrast; 0 = WCAG AA: 4.5, or 3 for large text (>= 24px, or >= 18.66px and weight >= 700)
    bool hasBackground = false;
    float background[3] = {0, 0, 0};   // Contrast: sRGB 0-1 backdrop assumed where the document is transparent
    bool unobstructed = false; // Visible: also the topmost element at the center of its visible part
    bool allowScroll = false;  // TextFits: overflow inside an overflow:auto/scroll container passes
    bool singleLine = false;   // TextFits: wrapping to a second line fails
    float minWidth = 44, minHeight = 44;   // MinSize
};

struct AssertFailure {
    std::string ref;
    std::string message;       // one sentence with the numbers ("text is 166px wide in a 120px content box")
    InspectRect rect;
    std::string otherRef;      // NoOverlap partner / WithinParent container
    InspectRect otherRect;
};

struct AssertResult {
    bool passed = true;
    size_t matched = 0;
    std::vector<AssertFailure> failures;
    std::vector<std::string> inconclusive;  // Contrast: refs whose backdrop could not be resolved
    std::string error;                      // invalid selector etc. (passed = false)
};

class JASPERENGINE_API HtmlUIAssertions {
public:
    static AssertResult run(Scene* scene, Entity entity, const AssertSpec& spec);
};

}

#endif //HTMLUIASSERTIONS_H
