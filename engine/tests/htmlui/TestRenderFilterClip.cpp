// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A filtered element inside a rounded overflow clip. The clip is a stencil clip mask in the
// depth-stencil every layer shares (layer 0's); the filtered element draws into a pushed layer
// and is composited into layer 0 through that mask. When the filtered element was the clip's
// only content, layer 1 was the frame's first pass: the mask went into the shared stencil there,
// then layer 0's first pass (the composite) cleared its colour and the shared depth-stencil,
// mask and all, and the composite tested an empty stencil: nothing was drawn. A sibling drawn
// before it bound layer 0 first, and an empty mask was cleared instead. The shared depth-stencil
// is now cleared by the frame's first layer pass, whichever layer it is.

#include "TestRenderCommon.h"

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    constexpr int kWidth = 96;
    constexpr int kHeight = 64;

    enum class Sibling { NONE, BEFORE, AFTER };

    const char* siblingName(Sibling sibling){
        switch (sibling){
        case Sibling::NONE: return "only child";
        case Sibling::BEFORE: return "sibling before";
        case Sibling::AFTER: return "sibling after";
        }
        return "";
    }

    // #clip covers (8, 8)-(67, 55) with 16 px corners. The filtered element overflows it by
    // 8 px on every side (its blurred edges and its shadow are cut by the clip, the box is
    // filled to the clip's edges). The sibling, a green square at (34, 28)-(43, 37), is drawn
    // before the filtered element (and hidden by it) or after it (on top).
    std::string document(const std::string& filter, Sibling sibling){
        const std::string siblingDiv = "<div class=\"sibling\"/>";
        return R"(<rml><head><style>
body { width: 96px; height: 64px; margin: 0; padding: 0; }
div { display: block; }
#clip { position: absolute; left: 8px; top: 8px; width: 60px; height: 48px; border-radius: 16px; overflow: hidden; }
.filtered { position: absolute; left: -8px; top: -8px; width: 76px; height: 64px; background-color: #e04020; filter: )" +
               filter + R"(; }
.sibling { position: absolute; left: 26px; top: 20px; width: 10px; height: 10px; background-color: #20e040; }
</style></head><body><div id="clip">)" +
               (sibling == Sibling::BEFORE ? siblingDiv : std::string()) + "<div class=\"filtered\"/>" +
               (sibling == Sibling::AFTER ? siblingDiv : std::string()) + "</div></body></rml>";
    }

    struct FilterCase {
        const char* filter;
        // the filtered element's colour inside the clip (premultiplied, opaque)
        int r, g, b;
    };

    // a blur and a drop shadow (post-process passes reading outside the element) and colour
    // filters (one pass over the layer)
    const FilterCase kFilters[] = {
        {"blur(2px)", 0xe0, 0x40, 0x20},
        {"drop-shadow(#000000 3px 3px 2px)", 0xe0, 0x40, 0x20},
        {"brightness(0.5)", 0x70, 0x20, 0x10},
        {"grayscale(1)", 0x60, 0x60, 0x60},
    };

    // pixels with any coverage
    size_t drawnPixels(const std::vector<uint8_t>& rgba){
        size_t count = 0;
        for (size_t i = 0; i + 3 < rgba.size(); i += 4)
            count += rgba[i + 3] > 0;
        return count;
    }

    size_t differingPixels(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b){
        if (a.size() != b.size())
            return a.size() > b.size() ? a.size() : b.size();
        size_t count = 0;
        for (size_t i = 0; i + 3 < a.size(); i += 4){
            if (a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2] || a[i + 3] != b[i + 3])
                count++;
        }
        return count;
    }

    // layer 0 of the second drawn frame (the first creates the layer and the post-process targets)
    bool renderLayer(const std::string& rml, int samples, std::vector<uint8_t>& rgba){
        RenderTestContext render(kWidth, kHeight, kWidth, kHeight, samples);
        if (!render.isValid() || !render.loadFromMemory(rml, "filter-clip"))
            return false;
        if (!render.renderUntilDrawn())
            return false;
        return render.readLayer(rgba);
    }

    void expectClipped(const std::vector<uint8_t>& rgba, const FilterCase& filter, Sibling sibling){
        const PixelProbe probe{rgba, kWidth};
        // inside the rounded box: the filtered element (layer 0 is premultiplied, the colours are opaque)
        EXPECT_TRUE(probe.is(20, 16, filter.r, filter.g, filter.b, 255, 4)) << "inside the top-left corner's arc";
        EXPECT_TRUE(probe.is(16, 47, filter.r, filter.g, filter.b, 255, 4)) << "inside the bottom-left corner's arc";
        EXPECT_TRUE(probe.is(56, 20, filter.r, filter.g, filter.b, 255, 4)) << "inside the top-right corner's arc";
        EXPECT_TRUE(probe.is(52, 46, filter.r, filter.g, filter.b, 255, 4)) << "inside the bottom-right corner's arc";
        EXPECT_TRUE(probe.is(9, 31, filter.r, filter.g, filter.b, 255, 4)) << "at the clip's left edge";
        EXPECT_TRUE(probe.is(38, 54, filter.r, filter.g, filter.b, 255, 4)) << "at the clip's bottom edge";
        if (sibling == Sibling::AFTER)
            EXPECT_TRUE(probe.is(38, 32, 0x20, 0xe0, 0x40, 255, 4)) << "the sibling drawn after the filtered element";
        else
            EXPECT_TRUE(probe.is(38, 32, filter.r, filter.g, filter.b, 255, 4)) << "the middle of the clip";

        // the clipped corners, and outside the clip where the element, its blur and its shadow overflow
        const int outside[][2] = {{8, 8}, {10, 10}, {67, 8}, {65, 10}, {8, 55}, {10, 53}, {67, 55}, {65, 53},
                                  {4, 31}, {70, 31}, {38, 4}, {38, 58}, {0, 0}, {95, 63}};
        for (const auto& p : outside)
            EXPECT_EQ(int(probe.at(p[0], p[1])[3]), 0) << "drawn at (" << p[0] << ", " << p[1] << "), outside the rounded clip";

        // the rounded box and nothing else: 60 x 48 less the four corners (4 - pi) * 16^2 ~ 220
        const size_t drawn = drawnPixels(rgba);
        EXPECT_GT(drawn, size_t(60 * 48 - 220 - 120)) << "part of the rounded box is not drawn";
        EXPECT_LT(drawn, size_t(60 * 48 - 220 + 120)) << "drawn beyond the rounded box";
    }

}

// The filtered element as the clip's only content, with a sibling drawn before it and with one
// drawn after it: drawn in the rounded box, nothing in its corners, with and without MSAA
TEST(RenderFilterClip, FilteredChildOfRoundedOverflowClip){
    JASPER_REQUIRE_GPU();
    for (int samples : {1, 4}){
        for (const FilterCase& filter : kFilters){
            std::vector<uint8_t> onlyChild;
            for (Sibling sibling : {Sibling::NONE, Sibling::BEFORE, Sibling::AFTER}){
                SCOPED_TRACE(std::string(filter.filter) + ", " + siblingName(sibling) + ", " + std::to_string(samples) + " samples");
                std::vector<uint8_t> rgba;
                ASSERT_TRUE(renderLayer(document(filter.filter, sibling), samples, rgba));
                expectClipped(rgba, filter, sibling);
                if (sibling == Sibling::NONE)
                    onlyChild = rgba;
                else if (sibling == Sibling::BEFORE)
                    EXPECT_EQ(differingPixels(onlyChild, rgba), 0u) << "the sibling hidden under the filtered element changed the image";
            }
        }
    }
}

// The same clip as the only content of a filtered element nested in a second rounded clip
// (an intersected clip mask, stencil reference 2), its filter's layer the frame's first pass
TEST(RenderFilterClip, FilteredChildOfNestedRoundedClips){
    JASPER_REQUIRE_GPU();
    const char* const rml = R"(<rml><head><style>
body { width: 96px; height: 64px; margin: 0; padding: 0; }
div { display: block; }
#outer { position: absolute; left: 4px; top: 4px; width: 68px; height: 56px; border-radius: 12px; overflow: hidden; }
#clip { position: absolute; left: 4px; top: 4px; width: 60px; height: 48px; border-radius: 16px; overflow: hidden; }
.filtered { position: absolute; left: -8px; top: -8px; width: 76px; height: 64px; background-color: #e04020; filter: blur(2px); }
</style></head><body><div id="outer"><div id="clip"><div class="filtered"/></div></div></body></rml>)";
    for (int samples : {1, 4}){
        SCOPED_TRACE(samples);
        std::vector<uint8_t> rgba;
        ASSERT_TRUE(renderLayer(rml, samples, rgba));
        expectClipped(rgba, kFilters[0], Sibling::NONE);
    }
}
