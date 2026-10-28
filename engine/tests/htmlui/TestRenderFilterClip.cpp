// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A filtered element inside a rounded overflow clip. The clip is a stencil clip mask in the
// depth-stencil every layer shares (layer 0's); the filtered element draws into a pushed layer
// and is composited into layer 0 through that mask. When the filtered element was the first
// thing drawn under the clip (its only child, or the first of several), the mask went into the
// shared stencil in the pushed layer's pass, the frame's first; then layer 0's first pass (the
// composite) cleared its colour and the shared depth-stencil, mask and all: the composite, and
// every sibling drawn after it through the same mask, tested an empty stencil and drew nothing.
// A sibling drawn before it bound layer 0 first, and the stencil was cleared before the mask.
// The shared depth-stencil is now cleared by the frame's first layer pass, whichever layer it is.
//
// A filter applies to its element's content unclipped by the clips established outside the
// element, and those clips cut its result (CSS, Chromium): the content is drawn into the filter's
// layer clipped by the element's own clips and its descendants' only, the ancestors' scissor and
// clip mask clip the layer's composite (fork patch P7-F1). The engine once drew the content through
// the ancestors' clips and filtered that: a blur faded inwards along a rounded clip's curve, and a
// drop shadow lost what the content beyond the clip casts into it.

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

    // pixels with some coverage but not all
    size_t partialPixels(const std::vector<uint8_t>& rgba){
        size_t count = 0;
        for (size_t i = 0; i + 3 < rgba.size(); i += 4)
            count += rgba[i + 3] > 0 && rgba[i + 3] < 255;
        return count;
    }

    void expectClipped(const std::vector<uint8_t>& rgba, const FilterCase& filter, Sibling sibling, int samples){
        const PixelProbe probe{rgba, kWidth};
        // 2.6 px inside each corner's arc, on its diagonal: the filtered element overflows the clip
        // by 8 px, so a blur there averages the element's colour only (it once faded: the content
        // was clipped by the curve before it was blurred)
        EXPECT_TRUE(probe.is(14, 14, filter.r, filter.g, filter.b, 255, 4)) << "near the top-left corner's arc";
        EXPECT_TRUE(probe.is(61, 14, filter.r, filter.g, filter.b, 255, 4)) << "near the top-right corner's arc";
        EXPECT_TRUE(probe.is(14, 49, filter.r, filter.g, filter.b, 255, 4)) << "near the bottom-left corner's arc";
        EXPECT_TRUE(probe.is(61, 49, filter.r, filter.g, filter.b, 255, 4)) << "near the bottom-right corner's arc";
        // one sample: the clip is the stencil's, every pixel is in or out; nothing fades
        if (samples == 1)
            EXPECT_EQ(partialPixels(rgba), 0u) << "pixels partly covered: the filter's result faded at the clip";
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
                expectClipped(rgba, filter, sibling, samples);
                if (sibling == Sibling::NONE)
                    onlyChild = rgba;
                else if (sibling == Sibling::BEFORE)
                    EXPECT_EQ(differingPixels(onlyChild, rgba), 0u) << "the sibling hidden under the filtered element changed the image";
            }
        }
    }
}

// The frame that creates the filter's layer and post-process targets (layer 1 the frame's first
// pass) draws what the next frames draw, no pass begun inside another; the rounded clip inside a
// rectangular overflow clip (a scissor, no clip mask: the rounded mask is still first drawn in
// layer 1) and inside a second rounded clip (an intersected mask, stencil reference 2; the outer
// mask goes into layer 0 when the inner box is drawn, before the filter's layer is pushed)
TEST(RenderFilterClip, FirstFrameAndNestedClips){
    JASPER_REQUIRE_GPU();
    const std::string head = R"(<rml><head><style>
body { width: 96px; height: 64px; margin: 0; padding: 0; }
div { display: block; }
#rect { position: absolute; left: 8px; top: 8px; width: 60px; height: 48px; overflow: hidden; }
#outer { position: absolute; left: 4px; top: 4px; width: 68px; height: 56px; border-radius: 12px; overflow: hidden; }
#clip { position: absolute; left: 0px; top: 0px; width: 60px; height: 48px; border-radius: 16px; overflow: hidden; }
#outer #clip { left: 4px; top: 4px; }
.filtered { position: absolute; left: -8px; top: -8px; width: 76px; height: 64px; background-color: #e04020; filter: blur(2px); }
</style></head><body>)";
    const std::string clip = R"(<div id="clip"><div class="filtered"/></div>)";
    const std::string documents[] = {
        head + R"(<div id="rect">)" + clip + "</div></body></rml>",
        head + R"(<div id="outer">)" + clip + "</div></body></rml>",
    };
    for (int samples : {1, 4}){
        for (const std::string& rml : documents){
            SCOPED_TRACE(std::to_string(samples) + " samples, " + rml.substr(head.size()));
            const uint64_t nestedBefore = SystemRender::getNestedPassCount();
            RenderTestContext render(kWidth, kHeight, kWidth, kHeight, samples);
            ASSERT_TRUE(render.isValid());
            ASSERT_NE(render.loadFromMemory(rml, "filter-clip"), nullptr);

            bool drawn = false;
            for (int i = 0; i < 30 && !drawn; i++)
                drawn = render.renderFrame();
            ASSERT_TRUE(drawn);
            std::vector<uint8_t> first;
            ASSERT_TRUE(render.readLayer(first));
            expectClipped(first, kFilters[0], Sibling::NONE, samples);

            ASSERT_TRUE(render.renderFrame());
            std::vector<uint8_t> second;
            ASSERT_TRUE(render.readLayer(second));
            EXPECT_EQ(differingPixels(first, second), 0u) << "the frame that created the targets drew differently";
            EXPECT_EQ(SystemRender::getNestedPassCount(), nestedBefore) << "a render pass began inside another";
        }
    }
}

namespace {

    constexpr int kShadowHeight = 80;

    // #clip covers (8, 24)-(67, 71); a red bar at (28, 4)-(47, 19) lies above it, outside, and its
    // shadow 16 px lower, at (28, 20)-(47, 35): the part from y = 24 on is inside the clip. Nested:
    // the bar in a group (covering (0, 0)-(75, 55)) filtered with brightness(0.5)
    std::string shadowDocument(const std::string& clipStyle, const std::string& shadow, bool nested){
        const std::string bar = "<div class=\"bar\" style=\"filter: " + shadow + ";\"/>";
        return R"(<rml><head><style>
body { width: 96px; height: 80px; margin: 0; padding: 0; }
div { display: block; }
#clip { position: absolute; left: 8px; top: 24px; width: 60px; height: 48px; overflow: hidden; )" +
               clipStyle + R"( }
#group { position: absolute; left: -8px; top: -24px; width: 76px; height: 56px; filter: brightness(0.5); }
.bar { position: absolute; left: 20px; top: -20px; width: 20px; height: 16px; background-color: #e04020; }
#group .bar { left: 28px; top: 4px; }
</style></head><body><div id="clip">)" +
               (nested ? "<div id=\"group\">" + bar + "</div>" : bar) + "</div></body></rml>";
    }

}

// A drop shadow that the filtered element casts from beyond its ancestor's clip into it: drawn
// within the clip (it was lost: the element was clipped away before its shadow was cast), and
// nothing of the element or the shadow outside the clip. A rectangular clip (a scissor: a
// rectangle of the composite's clip mask, the filter reads beyond it), a rounded one (a clip
// mask), and the shadow's element inside a colour-filtered group (nested layers: the shadow's
// composite into the group's layer is not clipped by #clip, the group's composite is)
TEST(RenderFilterClip, DropShadowCastFromBeyondTheClip){
    JASPER_REQUIRE_GPU();
    // sharp, and blurred (6 px of soft edge on each side of the 20 x 16 px shadow; the probes are at
    // least 5.5 px in)
    const char* shadows[] = {"drop-shadow(#2040e0 0px 16px 0px)", "drop-shadow(#2040e0 0px 16px 2px)"};
    const char* clips[] = {"", "border-radius: 16px;"};
    for (int samples : {1, 4}){
        for (const char* shadow : shadows){
            for (const char* clip : clips){
                for (bool nested : {false, true}){
                    SCOPED_TRACE(std::string(shadow) + ", clip { " + clip + " }" + (nested ? ", in a brightness(0.5) group" : "") + ", " +
                                 std::to_string(samples) + " samples");
                    RenderTestContext render(kWidth, kShadowHeight, kWidth, kShadowHeight, samples);
                    ASSERT_TRUE(render.isValid());
                    ASSERT_NE(render.loadFromMemory(shadowDocument(clip, shadow, nested), "filter-clip-shadow"), nullptr);
                    ASSERT_TRUE(render.renderUntilDrawn());
                    std::vector<uint8_t> rgba;
                    ASSERT_TRUE(render.readLayer(rgba));
                    const PixelProbe probe{rgba, kWidth};

                    // the group halves the shadow's colour
                    const int r = nested ? 0x10 : 0x20, g = nested ? 0x20 : 0x40, b = nested ? 0x70 : 0xe0;
                    for (int y : {25, 28, 30}){
                        for (int x : {35, 38, 41})
                            EXPECT_TRUE(probe.is(x, y, r, g, b, 255, 4)) << "the shadow inside the clip";
                    }
                    // the bar, the shadow's part above the clip, beside and below the shadow
                    const int outside[][2] = {{38, 8}, {38, 18}, {38, 21}, {38, 23}, {30, 22}, {45, 22}, {4, 30}, {70, 30}, {38, 40}, {38, 60}};
                    for (const auto& p : outside)
                        EXPECT_EQ(int(probe.at(p[0], p[1])[3]), 0) << "drawn at (" << p[0] << ", " << p[1] << ")";
                }
            }
        }
    }
}

// A clip with a transform is a clip mask only (no scissor): the filtered content is still drawn
// unclipped into the layer, the mask cuts the composite. The clip is a 48 px square with 16 px
// corners turned a quarter, which leaves its shape as it is.
TEST(RenderFilterClip, TransformedRoundedClip){
    JASPER_REQUIRE_GPU();
    for (int samples : {1, 4}){
        for (const FilterCase& filter : kFilters){
            SCOPED_TRACE(std::string(filter.filter) + ", " + std::to_string(samples) + " samples");
            const std::string rml = R"(<rml><head><style>
body { width: 96px; height: 64px; margin: 0; padding: 0; }
div { display: block; }
#clip { position: absolute; left: 16px; top: 8px; width: 48px; height: 48px; border-radius: 16px; overflow: hidden; transform: rotate(90deg); }
.filtered { position: absolute; left: -8px; top: -8px; width: 64px; height: 64px; background-color: #e04020; filter: )" +
                                    std::string(filter.filter) + R"(; }
</style></head><body><div id="clip"><div class="filtered"/></div></body></rml>)";
            std::vector<uint8_t> rgba;
            ASSERT_TRUE(renderLayer(rml, samples, rgba));
            const PixelProbe probe{rgba, kWidth};
            // near the four arcs (2.6 px in on their diagonals), at the four edges and in the middle
            const int inside[][2] = {{22, 14}, {57, 14}, {22, 49}, {57, 49}, {17, 31}, {62, 31}, {40, 9}, {40, 54}, {40, 31}};
            for (const auto& p : inside)
                EXPECT_TRUE(probe.is(p[0], p[1], filter.r, filter.g, filter.b, 255, 4)) << "inside the clip";
            const int outside[][2] = {{16, 8}, {63, 8}, {16, 55}, {63, 55}, {12, 31}, {68, 31}, {40, 5}, {40, 58}};
            for (const auto& p : outside)
                EXPECT_EQ(int(probe.at(p[0], p[1])[3]), 0) << "drawn at (" << p[0] << ", " << p[1] << "), outside the clip";
            if (samples == 1)
                EXPECT_EQ(partialPixels(rgba), 0u) << "pixels partly covered: the filter's result faded at the clip";
        }
    }
}

// The filtered element's own rounded overflow clip is inside the filter: its content is clipped
// to it before the blur, which spreads the clipped box beyond it (the ancestor's clip, larger,
// still cuts the result)
TEST(RenderFilterClip, OwnOverflowClipIsFiltered){
    JASPER_REQUIRE_GPU();
    const std::string rml = R"(<rml><head><style>
body { width: 96px; height: 64px; margin: 0; padding: 0; }
div { display: block; }
#clip { position: absolute; left: 8px; top: 4px; width: 80px; height: 56px; border-radius: 16px; overflow: hidden; }
#own { position: absolute; left: 16px; top: 12px; width: 48px; height: 32px; border-radius: 8px; overflow: hidden; filter: blur(2px); }
.fill { position: absolute; left: -12px; top: -12px; width: 72px; height: 56px; background-color: #e04020; }
</style></head><body><div id="clip"><div id="own"><div class="fill"/></div></div></body></rml>)";
    for (int samples : {1, 4}){
        SCOPED_TRACE(std::to_string(samples) + " samples");
        std::vector<uint8_t> rgba;
        ASSERT_TRUE(renderLayer(rml, samples, rgba));
        const PixelProbe probe{rgba, kWidth};
        // #own covers (24, 16)-(71, 47): its middle is the colour, its edges blurred both ways
        EXPECT_TRUE(probe.is(48, 32, 0xe0, 0x40, 0x20, 255, 4));
        const int beyond[][2] = {{21, 32}, {74, 32}, {48, 13}, {48, 50}};
        for (const auto& p : beyond){
            const int alpha = probe.at(p[0], p[1])[3];
            EXPECT_GT(alpha, 0) << "the blur spreads 2.5 px beyond #own's clip at (" << p[0] << ", " << p[1] << ")";
            EXPECT_LT(alpha, 96) << "#own's clip did not cut its content before the blur at (" << p[0] << ", " << p[1] << ")";
        }
        const int within[][2] = {{25, 32}, {70, 32}, {48, 17}, {48, 46}};
        for (const auto& p : within){
            const int alpha = probe.at(p[0], p[1])[3];
            EXPECT_GT(alpha, 160) << "1.5 px inside #own's clip at (" << p[0] << ", " << p[1] << ")";
            EXPECT_LT(alpha, 240) << "not blurred at #own's edge at (" << p[0] << ", " << p[1] << ")";
        }
        // beyond the blur, and outside #clip: nothing
        const int outside[][2] = {{12, 32}, {84, 32}, {48, 6}, {48, 57}, {0, 0}, {95, 63}};
        for (const auto& p : outside)
            EXPECT_EQ(int(probe.at(p[0], p[1])[3]), 0) << "drawn at (" << p[0] << ", " << p[1] << ")";
    }
}
