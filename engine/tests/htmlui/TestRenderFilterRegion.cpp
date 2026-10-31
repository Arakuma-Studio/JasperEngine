// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The region a filter reads and draws (fork patch P7-F2, docs/html-ui/FORK.md).
//
// Beyond the viewport: a filter's layer was a viewport-sized image, so the content of a filtered
// element beyond the screen's edge was never drawn into it. A blur at the edge then mirrored the
// content inside the screen instead of reading what lies beyond (it did not pick up the off-screen
// colour), and a drop shadow cast onto the screen by content just beyond it was missing. A filter's
// layer now covers its window wherever that lies (a region layer of RmlRenderTarget: the viewport's
// pixel grid continued beyond its edges), cropped to RmlRenderer::regionLimit.
//
// A descendant's filters: the filter region of an element (the layer's window, the composite's
// scissor) covered its subtree's boxes but not its descendants' own filters' extent, so a child's
// drop shadow or blur reaching past a colour-filtered parent's box was cut at the box.

#include "TestRenderCommon.h"

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    constexpr int kSize = 160;

    struct Frame {
        std::vector<uint8_t> rgba;
        HtmlUIRenderStats stats;
    };

    // layer 0 of the drawn frame (premultiplied, transparent where nothing was drawn) and its stats
    bool renderLayer(const std::string& rml, int samples, Frame& frame, int width = kSize, int height = kSize){
        RenderTestContext render(width, height, width, height, samples);
        if (!render.isValid() || !render.loadFromMemory(rml, "filter-region"))
            return false;
        if (!render.renderUntilDrawn())
            return false;
        frame.stats = render.renderer.getStats();
        return render.readLayer(frame.rgba);
    }

    std::string document(const std::string& style, const std::string& body, int width = kSize, int height = kSize){
        return "<rml><head><style>body { width: " + std::to_string(width) + "px; height: " + std::to_string(height) +
               "px; margin: 0; padding: 0; } div { display: block; } " + style + "</style></head><body>" + body + "</body></rml>";
    }

    // the largest channel difference between a's rect at (ax, ay) and b's at (bx, by), w x h
    int maxDifference(const std::vector<uint8_t>& a, int ax, int ay, const std::vector<uint8_t>& b, int bx, int by, int w, int h,
                      int width = kSize){
        int worst = 0;
        for (int y = 0; y < h; y++){
            for (int x = 0; x < w; x++){
                const uint8_t* p = &a[(size_t(ay + y) * width + size_t(ax + x)) * 4];
                const uint8_t* q = &b[(size_t(by + y) * width + size_t(bx + x)) * 4];
                for (int c = 0; c < 4; c++)
                    worst = std::max(worst, std::abs(int(p[c]) - int(q[c])));
            }
        }
        return worst;
    }

    // a 64 px square with its two halves coloured, the half beyond the edge blue, the other red
    enum class Edge { LEFT, RIGHT, TOP, BOTTOM };

    const char* edgeName(Edge edge){
        switch (edge){
        case Edge::LEFT: return "left";
        case Edge::RIGHT: return "right";
        case Edge::TOP: return "top";
        case Edge::BOTTOM: return "bottom";
        }
        return "";
    }

    // the square straddles the edge (shift 0) or lies 64 px further in (a whole number of the
    // region layers' 64 px granules: the blur's halved passes sample the same grid)
    std::string edgeDocument(Edge edge, int shift, const std::string& filter, const std::string& extra = ""){
        const bool horizontal = (edge == Edge::LEFT || edge == Edge::RIGHT);
        int left = 48, top = 48;
        if (edge == Edge::LEFT)
            left = -32 + shift;
        else if (edge == Edge::RIGHT)
            left = kSize - 32 - shift;
        else if (edge == Edge::TOP)
            top = -32 + shift;
        else
            top = kSize - 32 - shift;
        // the half beyond the edge
        const bool beyondFirst = (edge == Edge::LEFT || edge == Edge::TOP);
        const std::string half = horizontal ? "width: 32px; height: 64px;" : "width: 64px; height: 32px;";
        const std::string second = horizontal ? "left: 32px; top: 0px;" : "left: 0px; top: 32px;";
        const std::string style = "#square { position: absolute; left: " + std::to_string(left) + "px; top: " + std::to_string(top) +
                                  "px; width: 64px; height: 64px; filter: " + filter + "; " + extra + " } .half { position: absolute; " + half +
                                  " } #first { left: 0px; top: 0px; background-color: " + (beyondFirst ? "#2040e0" : "#e04020") +
                                  "; } #second { " + second + " background-color: " + (beyondFirst ? "#e04020" : "#2040e0") + "; }";
        return document(style, R"(<div id="square"><div id="first" class="half"/><div id="second" class="half"/></div>)");
    }

    // the strip of the screen next to the edge that the square's blur covers, and where the same
    // pixels are in the shifted reference
    struct Strip {
        int x, y, w, h, dx, dy;
    };

    Strip edgeStrip(Edge edge, int reach){
        switch (edge){
        case Edge::LEFT: return {0, 48 - reach, 32 + reach, 64 + 2 * reach, 64, 0};
        case Edge::RIGHT: return {kSize - 32 - reach, 48 - reach, 32 + reach, 64 + 2 * reach, -64, 0};
        case Edge::TOP: return {48 - reach, 0, 64 + 2 * reach, 32 + reach, 0, 64};
        case Edge::BOTTOM: return {48 - reach, kSize - 32 - reach, 64 + 2 * reach, 32 + reach, 0, -64};
        }
        return {};
    }

    // the blue of the half beyond the edge at the screen's edge pixel next to the square's middle
    int blueAtEdge(const std::vector<uint8_t>& rgba, Edge edge){
        const PixelProbe probe{rgba, kSize};
        switch (edge){
        case Edge::LEFT: return probe.at(0, 80)[2];
        case Edge::RIGHT: return probe.at(kSize - 1, 80)[2];
        case Edge::TOP: return probe.at(80, 0)[2];
        case Edge::BOTTOM: return probe.at(80, kSize - 1)[2];
        }
        return 0;
    }

}

// A blurred square half beyond each edge of the screen: the blur at the edge picks up the colour
// of the half beyond it, as the same square 64 px further in draws it (the reference: every
// pixel of the strip the blur covers, to the screen's edge). It once mirrored the half on screen
// at the edge (no blue at all there).
TEST(RenderFilterRegion, BlurReadsTheContentBeyondTheViewport){
    JASPER_REQUIRE_GPU();
    for (int samples : {1, 4}){
        for (Edge edge : {Edge::LEFT, Edge::RIGHT, Edge::TOP, Edge::BOTTOM}){
            for (const char* blur : {"blur(4px)", "blur(9px)"}){
                SCOPED_TRACE(std::string(edgeName(edge)) + " edge, " + blur + ", " + std::to_string(samples) + " samples");
                const uint64_t nestedBefore = SystemRender::getNestedPassCount();
                Frame atEdge, reference;
                ASSERT_TRUE(renderLayer(edgeDocument(edge, 0, blur), samples, atEdge));
                ASSERT_TRUE(renderLayer(edgeDocument(edge, 64, blur), samples, reference));
                EXPECT_EQ(atEdge.stats.regionLayers, 1u) << "the filter's layer covers its window beyond the viewport";
                EXPECT_EQ(reference.stats.regionLayers, 0u) << "a filter within the viewport draws into an ordinary layer";

                // the blue half beyond the edge shows at it (half of it blurred in there)
                EXPECT_GT(blueAtEdge(atEdge.rgba, edge), 0x50) << "the blur at the edge did not read the half beyond it";
                const Strip strip = edgeStrip(edge, 27);
                const int difference = maxDifference(atEdge.rgba, strip.x, strip.y, reference.rgba, strip.x + strip.dx, strip.y + strip.dy,
                                                     strip.w, strip.h);
                EXPECT_LE(difference, 2) << "the blur at the edge differs from the square's blur further in";
                EXPECT_EQ(SystemRender::getNestedPassCount(), nestedBefore) << "a render pass began inside another";
            }
        }
    }
}

// The square's halves inside layers of their own in the blurred square's layer: a group with an
// opacity (an ordinary content layer, on a region layer it covers the region), and a blurred half
// (a filter's layer pushed on a region layer, a region layer of its own). Three layers deep at the
// edge, as further in
TEST(RenderFilterRegion, LayersInsideARegionLayer){
    JASPER_REQUIRE_GPU();
    struct Case {
        const char* name;
        const char* extra;
    };
    const Case cases[] = {
        {"a group with an opacity", "#first { opacity: 0.6; } #first div { position: absolute; left: 4px; top: 4px; width: 24px; height: 56px; background-color: #40e020; }"},
        {"a blurred half", "#first { filter: blur(2px); } #first div { position: absolute; left: 4px; top: 4px; width: 24px; height: 56px; background-color: #40e020; }"},
        {"a blurred half in a group", "#first { opacity: 0.6; } #first div { position: absolute; left: 4px; top: 4px; width: 24px; height: 56px; background-color: #40e020; filter: blur(2px); }"},
    };
    for (int samples : {1, 4}){
        for (const Case& c : cases){
            SCOPED_TRACE(std::string(c.name) + ", " + std::to_string(samples) + " samples");
            auto rml = [&](int shift){
                std::string text = edgeDocument(Edge::LEFT, shift, "blur(4px)");
                // the extra rules, and a child in the first half
                text.insert(text.find("</style>"), c.extra);
                const std::string first = R"(<div id="first" class="half"/>)";
                text.replace(text.find(first), first.size(), R"(<div id="first" class="half"><div/></div>)");
                return text;
            };
            Frame atEdge, reference;
            ASSERT_TRUE(renderLayer(rml(0), samples, atEdge));
            ASSERT_TRUE(renderLayer(rml(64), samples, reference));
            EXPECT_GE(atEdge.stats.regionLayers, 1u);
            const Strip strip = edgeStrip(Edge::LEFT, 27);
            const int difference = maxDifference(atEdge.rgba, strip.x, strip.y, reference.rgba, strip.x + strip.dx, strip.y + strip.dy, strip.w,
                                                 strip.h);
            EXPECT_LE(difference, 2) << "the layers in the region layer lost what lies beyond the viewport";
        }
    }
}

// A square turned half a turn (its halves swap sides: the blue one is on the screen, the red one
// beyond) and one scaled by 1.5 (its blur too; the filter region of the transformed box)
TEST(RenderFilterRegion, TransformedElementAtTheEdge){
    JASPER_REQUIRE_GPU();
    for (int samples : {1, 4}){
        for (const char* transform : {"transform: rotate(180deg);", "transform: scale(1.5);"}){
            SCOPED_TRACE(std::string(transform) + ", " + std::to_string(samples) + " samples");
            Frame atEdge, reference;
            ASSERT_TRUE(renderLayer(edgeDocument(Edge::LEFT, 0, "blur(4px)", transform), samples, atEdge));
            ASSERT_TRUE(renderLayer(edgeDocument(Edge::LEFT, 64, "blur(4px)", transform), samples, reference));
            EXPECT_EQ(atEdge.stats.regionLayers, 1u);
            const Strip strip = edgeStrip(Edge::LEFT, 27);
            const int difference = maxDifference(atEdge.rgba, strip.x, strip.y, reference.rgba, strip.x + strip.dx, strip.y + strip.dy, strip.w,
                                                 strip.h);
            EXPECT_LE(difference, 2) << "the transformed square's blur at the edge differs from its blur further in";
        }
    }
}

// A square entirely beyond the screen's top (left) edge casts its drop shadow onto the screen:
// drawn (it was missing: nothing of the square was in the layer), sharp and blurred, as the same
// square and shadow 64 px further in draw them
TEST(RenderFilterRegion, DropShadowCastOntoTheScreenFromBeyondIt){
    JASPER_REQUIRE_GPU();
    for (int samples : {1, 4}){
        for (bool above : {true, false}){
            for (int blur : {0, 3}){
                SCOPED_TRACE(std::string(above ? "above" : "left") + ", blur " + std::to_string(blur) + ", " + std::to_string(samples) + " samples");
                // the square 32 px thick, 8 px beyond the edge; its shadow 48 px further, 8 px in
                auto rml = [&](int shift){
                    const std::string position = above ? "left: 48px; top: " + std::to_string(-40 + shift) + "px; width: 64px; height: 32px;"
                                                       : "top: 48px; left: " + std::to_string(-40 + shift) + "px; width: 32px; height: 64px;";
                    const std::string shadow = std::string("drop-shadow(#2040e0 ") + (above ? "0px 48px " : "48px 0px ") + std::to_string(blur) + "px)";
                    return document("#square { position: absolute; " + position + " background-color: #e04020; filter: " + shadow + "; }",
                                    R"(<div id="square"/>)");
                };
                Frame beyond, reference;
                ASSERT_TRUE(renderLayer(rml(0), samples, beyond));
                ASSERT_TRUE(renderLayer(rml(64), samples, reference));
                EXPECT_EQ(beyond.stats.regionLayers, 1u);
                const PixelProbe probe{beyond.rgba, kSize};
                EXPECT_TRUE(above ? probe.is(80, 24, 0x20, 0x40, 0xe0, 255, 4) : probe.is(24, 80, 0x20, 0x40, 0xe0, 255, 4))
                    << "the shadow cast onto the screen";
                const Strip strip = above ? Strip{36, 0, 88, 52, 0, 64} : Strip{0, 36, 52, 88, 64, 0};
                const int difference = maxDifference(beyond.rgba, strip.x, strip.y, reference.rgba, strip.x + strip.dx, strip.y + strip.dy,
                                                     strip.w, strip.h);
                EXPECT_LE(difference, 2) << "the shadow differs from the one cast further in";
                EXPECT_EQ(int(probe.at(80, 80)[3]), 0);
                EXPECT_EQ(int(probe.at(kSize - 1, kSize - 1)[3]), 0);
            }
        }
    }
}

// A blurred element half beyond the screen in a rounded overflow clip that extends beyond it
// too: the clip's mask, set while the region layer is the top one (its own depth-stencil), is
// drawn again into layer 0's for the composite. The clip's corners on screen cut the result,
// what is inside matches the clip and element 64 px further in
TEST(RenderFilterRegion, RoundedClipAroundARegionLayer){
    JASPER_REQUIRE_GPU();
    for (int samples : {1, 4}){
        SCOPED_TRACE(std::to_string(samples) + " samples");
        auto rml = [&](int shift){
            return document("#clip { position: absolute; left: " + std::to_string(-40 + shift) +
                                "px; top: 40px; width: 120px; height: 80px; border-radius: 24px; overflow: hidden; } "
                                "#f { position: absolute; left: 0px; top: 0px; width: 120px; height: 80px; background-color: #e04020; filter: blur(4px); } "
                                "#blue { position: absolute; left: 0px; top: 0px; width: 40px; height: 80px; background-color: #2040e0; }",
                            R"(<div id="clip"><div id="f"><div id="blue"/></div></div>)");
        };
        Frame atEdge, reference;
        ASSERT_TRUE(renderLayer(rml(0), samples, atEdge));
        ASSERT_TRUE(renderLayer(rml(64), samples, reference));
        EXPECT_EQ(atEdge.stats.regionLayers, 1u);
        EXPECT_GE(atEdge.stats.clipMasksRedrawn, 1u) << "the clip mask was not drawn into the composite's depth-stencil";
        const PixelProbe probe{atEdge.rgba, kSize};
        // the clip's right corners: cut
        EXPECT_EQ(int(probe.at(79, 40)[3]), 0);
        EXPECT_EQ(int(probe.at(79, 119)[3]), 0);
        EXPECT_EQ(int(probe.at(100, 80)[3]), 0);
        EXPECT_TRUE(probe.is(60, 80, 0xe0, 0x40, 0x20, 255, 4));
        EXPECT_GT(int(probe.at(0, 80)[2]), 0x50) << "the blue beyond the edge, blurred into it";
        const int difference = maxDifference(atEdge.rgba, 0, 36, reference.rgba, 64, 36, 84, 88);
        EXPECT_LE(difference, samples > 1 ? 3 : 2) << "the clipped blur at the edge differs from the one further in";
    }
}

namespace {

    // a 48 px parent (filtered with parentFilter) holding a 24 px child (filtered with childFilter)
    // whose filter reaches beyond the parent's box; the reference: the parent unfiltered
    std::string descendantDocument(const std::string& parentFilter, const std::string& childFilter, const std::string& childStyle = ""){
        return document("#parent { position: absolute; left: 32px; top: 32px; width: 48px; height: 48px; background-color: #808080; filter: " +
                            parentFilter + "; } #child { position: absolute; left: 8px; top: 8px; width: 24px; height: 24px; background-color: #e04020; filter: " +
                            childFilter + "; " + childStyle + " }",
                        R"(<div id="parent"><div id="child"/></div>)");
    }

    // the alpha channels' largest difference (colour filters keep the coverage)
    int maxAlphaDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b){
        int worst = 0;
        for (size_t i = 3; i < a.size() && i < b.size(); i += 4)
            worst = std::max(worst, std::abs(int(a[i]) - int(b[i])));
        return worst;
    }

}

// A child's drop shadow and blur reaching past its brightness()/grayscale() parent's box: drawn
// whole (they were cut at the parent's box: its filter region left out the child's filter)
TEST(RenderFilterRegion, DescendantFilterExtentInTheParentsRegion){
    JASPER_REQUIRE_GPU();
    const char* childFilters[] = {"drop-shadow(#2040e0 40px 40px 0px)", "drop-shadow(#2040e0 40px 40px 4px)", "drop-shadow(#2040e0 -36px 0px 2px)"};
    for (int samples : {1, 4}){
        for (const char* parentFilter : {"brightness(1)", "grayscale(1)"}){
            for (const char* childFilter : childFilters){
                SCOPED_TRACE(std::string(parentFilter) + " parent, " + childFilter + " child, " + std::to_string(samples) + " samples");
                Frame filtered, reference;
                ASSERT_TRUE(renderLayer(descendantDocument(parentFilter, childFilter), samples, filtered));
                ASSERT_TRUE(renderLayer(descendantDocument("none", childFilter), samples, reference));
                EXPECT_LE(maxAlphaDifference(filtered.rgba, reference.rgba), 1) << "the child's filter was cut by its parent's filter region";
                if (std::string(parentFilter) == "brightness(1)") {
                    EXPECT_LE(maxDifference(filtered.rgba, 0, 0, reference.rgba, 0, 0, kSize, kSize), 1);
                }
            }
            // a blurred child at the parent's edge: its blur spreads 18 px beyond the parent's box
            SCOPED_TRACE(std::string(parentFilter) + " parent, blurred child at its edge");
            Frame filtered, reference;
            ASSERT_TRUE(renderLayer(descendantDocument(parentFilter, "blur(6px)", "left: 24px;"), samples, filtered));
            ASSERT_TRUE(renderLayer(descendantDocument("none", "blur(6px)", "left: 24px;"), samples, reference));
            EXPECT_LE(maxAlphaDifference(filtered.rgba, reference.rgba), 1) << "the child's blur was cut by its parent's filter region";
        }
    }
}

// Three levels: a brightness(1) group in a brightness(1) group, the innermost child's drop
// shadow (and a scaled child's: its shadow's offset and blur at the transform's scale, and a
// turned group around it) far beyond both groups' boxes
TEST(RenderFilterRegion, DescendantFilterExtentThreeLevelsDeep){
    JASPER_REQUIRE_GPU();
    struct Case {
        const char* name;
        const char* child;
        const char* inner;
    };
    const Case cases[] = {
        {"shadow", "filter: drop-shadow(#2040e0 56px 48px 3px);", ""},
        {"scaled child", "filter: drop-shadow(#2040e0 24px 20px 2px); transform: scale(2); transform-origin: 0 0;", ""},
        {"turned group", "filter: drop-shadow(#2040e0 40px 0px 2px);", "transform: rotate(90deg);"},
    };
    for (int samples : {1, 4}){
        for (const Case& c : cases){
            SCOPED_TRACE(std::string(c.name) + ", " + std::to_string(samples) + " samples");
            auto rml = [&](bool filtered){
                const std::string groupFilter = filtered ? "filter: brightness(1);" : "";
                return document("#outer { position: absolute; left: 16px; top: 16px; width: 64px; height: 64px; " + groupFilter +
                                    " } #inner { position: absolute; left: 8px; top: 8px; width: 40px; height: 40px; background-color: #808080; " +
                                    groupFilter + " " + c.inner + " } #child { position: absolute; left: 8px; top: 8px; width: 16px; height: 16px; "
                                    "background-color: #e04020; " + c.child + " }",
                                R"(<div id="outer"><div id="inner"><div id="child"/></div></div>)");
            };
            Frame filtered, reference;
            ASSERT_TRUE(renderLayer(rml(true), samples, filtered));
            ASSERT_TRUE(renderLayer(rml(false), samples, reference));
            EXPECT_LE(maxDifference(filtered.rgba, 0, 0, reference.rgba, 0, 0, kSize, kSize), 1) << "a filter region three levels up cut the shadow";
        }
    }
}

// Regions far beyond the viewport are cropped to the region limit (672 px here: twice the
// target's side, at least the side plus 512 px): no error, the region images within the limit's
// bytes. What the crop keeps is drawn (a 200 px blur of a 3000 px wide element beyond the left
// edge, the region around the screen; a shadow cast 400 px), what it leaves out is not (a
// shadow cast from 2000 px beyond the top edge: the documented degradation)
TEST(RenderFilterRegion, HugeRegionsAreCropped){
    JASPER_REQUIRE_GPU();
    struct Case {
        std::string rml;
        bool drawn;
    };
    const Case cases[] = {
        {document("#big { position: absolute; left: -3000px; top: 40px; width: 3100px; height: 80px; background-color: #e04020; filter: blur(200px); }",
                  R"(<div id="big"/>)"),
         true},
        {document("#big { position: absolute; left: 40px; top: -380px; width: 80px; height: 80px; background-color: #e04020; "
                  "filter: drop-shadow(#2040e0 0px 400px 0px); }",
                  R"(<div id="big"/>)"),
         true},
        {document("#big { position: absolute; left: 40px; top: -2040px; width: 80px; height: 80px; background-color: #e04020; "
                  "filter: drop-shadow(#2040e0 0px 2060px 0px); }",
                  R"(<div id="big"/>)"),
         false},
    };
    const Rml::Vector2i limit = RmlRenderer::regionLimit(Rml::Vector2i(kSize, kSize), SystemRender::getFeatures().maxTextureSize2D);
    EXPECT_EQ(limit, Rml::Vector2i(672, 672));
    for (int samples : {1, 4}){
        for (const Case& c : cases){
            const std::string& rml = c.rml;
            SCOPED_TRACE(std::to_string(samples) + " samples, " + rml.substr(rml.find("#big")));
            TestSystem::get().clearLogs();
            RenderTestContext render(kSize, kSize, kSize, kSize, samples);
            ASSERT_TRUE(render.isValid());
            ASSERT_NE(render.loadFromMemory(rml, "filter-region-huge"), nullptr);
            ASSERT_TRUE(render.renderUntilDrawn());
            EXPECT_EQ(render.renderer.getStats().regionLayers, 1u);
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u) << "an error while drawing the cropped region";
            std::vector<uint8_t> rgba;
            ASSERT_TRUE(render.readLayer(rgba));
            const PixelProbe probe{rgba, kSize};
            if (c.drawn) {
                EXPECT_GT(int(probe.at(40, 80)[3]), 0) << "nothing drawn";
            }
            else
                EXPECT_EQ(int(probe.at(40, 80)[3]), 0) << "drawn from beyond the region limit";
            // one layer and one post-process set of at most the limit's size
            const size_t pixels = size_t(limit.x) * size_t(limit.y);
            EXPECT_LE(render.target.getRegionBytes(), pixels * (8u * size_t(samples) + 16u));
        }
    }
}

// The pool: a region that moves a pixel a frame reuses its images (none made after the first
// frame), and they go once no region was pushed for RmlRenderTarget::kRegionKeepFrames frames
TEST(RenderFilterRegion, RegionImagesArePooled){
    JASPER_REQUIRE_GPU();
    RenderTestContext render(kSize, kSize, kSize, kSize, 1);
    ASSERT_TRUE(render.isValid());
    Rml::ElementDocument* doc = render.loadFromMemory(edgeDocument(Edge::LEFT, 0, "blur(4px)"), "filter-region-pool");
    ASSERT_NE(doc, nullptr);
    ASSERT_TRUE(render.renderUntilDrawn());
    Rml::Element* square = doc->GetElementById("square");
    ASSERT_NE(square, nullptr);
    for (int i = 1; i <= 8; i++){
        square->SetProperty("left", std::to_string(-32 + i) + "px");
        ASSERT_TRUE(render.renderFrame());
        EXPECT_EQ(render.renderer.getStats().regionLayers, 1u);
        EXPECT_EQ(render.renderer.getStats().regionTargetsCreated, 0u) << "frame " << i << " made region images";
    }
    EXPECT_EQ(render.target.getRegionLayerCount(), 1u);
    EXPECT_EQ(render.target.getRegionPostSetCount(), 1u);

    square->SetProperty("filter", "none");
    for (uint64_t i = 0; i <= RmlRenderTarget::kRegionKeepFrames + 1; i++)
        ASSERT_TRUE(render.renderFrame());
    EXPECT_EQ(render.target.getRegionLayerCount(), 0u);
    EXPECT_EQ(render.target.getRegionPostSetCount(), 0u);
    EXPECT_EQ(render.target.getRegionBytes(), 0u);
}

// regionFrame and regionLimit (pure)
TEST(RenderFilterRegion, RegionFrameCropsAndAligns){
    const Rml::Rectanglei viewport = Rml::Rectanglei::FromSize(Rml::Vector2i(160, 160));
    const Rml::Vector2i limit = RmlRenderer::regionLimit(Rml::Vector2i(160, 160), 16384);
    EXPECT_EQ(limit, Rml::Vector2i(672, 672));
    EXPECT_EQ(RmlRenderer::regionLimit(Rml::Vector2i(1920, 1080), 16384), Rml::Vector2i(3840, 2160));
    EXPECT_EQ(RmlRenderer::regionLimit(Rml::Vector2i(1920, 1080), 2048), Rml::Vector2i(2048, 2048));

    // within the limit: whole 64 px granules of the target plane around it
    EXPECT_EQ(RmlRenderer::regionFrame(Rml::Rectanglei::FromCorners({-24, 28}, {44, 132}), viewport, limit),
              Rml::Rectanglei::FromCorners({-64, 0}, {64, 192}));
    // cropped: the part on screen kept, as much as fits on both sides
    const Rml::Rectanglei cropped = RmlRenderer::regionFrame(Rml::Rectanglei::FromCorners({-3600, 0}, {700, 100}), viewport, limit);
    EXPECT_LE(cropped.Width(), limit.x);
    EXPECT_EQ(cropped.Left() % 64, 0);
    EXPECT_GE(-cropped.Left(), 160);
    EXPECT_GE(cropped.Right() - 160, 160);
    // the part inside a region layer it is pushed on, kept
    const Rml::Rectanglei parent = Rml::Rectanglei::FromCorners({-192, -64}, {320, 256});
    const Rml::Rectanglei nested = RmlRenderer::regionFrame(Rml::Rectanglei::FromCorners({-5000, -5000}, {5000, 5000}), parent, limit);
    EXPECT_LE(nested.Left(), parent.Left());
    EXPECT_GE(nested.Right(), parent.Right());
    EXPECT_LE(nested.Top(), parent.Top());
    EXPECT_GE(nested.Bottom(), parent.Bottom());
    EXPECT_LE(nested.Width(), limit.x);
    EXPECT_LE(nested.Height(), limit.y);
}
