// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Overflow clipping at one and four samples. Mesa 23 may leave its known
// half-covered top/left edge under MSAA; all other pixels obey the clip.

#include "TestRenderCommon.h"

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // The pixels that differ from an exact scissor, other than Mesa's pattern at the left and top
    // edges; `mesaPattern` tells whether that pattern was there.
    std::vector<std::string> unexpectedLeaks(const std::vector<uint8_t>& rgba, int size, int left, int top, int width, int height,
                                             bool& mesaPattern){
        auto alpha = [&](int x, int y){ return int(rgba[(size_t(y) * size + x) * 4 + 3]); };
        std::vector<std::string> leaks;
        int halfCovered = 0;
        for (int y = 0; y < size; y++){
            for (int x = 0; x < size; x++){
                const bool inside = x >= left && x < left + width && y >= top && y < top + height;
                const bool topRow = (y == top - 1 && x >= left && x < left + width);
                const bool leftColumn = (x == left - 1 && y >= top && y < top + height);
                const bool corner = (x == left - 1 && y == top - 1);
                const int a = alpha(x, y);
                bool ok = inside ? a == 255 : a == 0;
                if (!ok && (topRow || leftColumn) && std::abs(a - 128) <= 1){
                    ok = true;
                    halfCovered++;
                }
                if (!ok && corner && std::abs(a - 64) <= 1)
                    ok = true;
                if (!ok)
                    leaks.push_back("(" + std::to_string(x) + ", " + std::to_string(y) + ") alpha " + std::to_string(a));
            }
        }
        mesaPattern = halfCovered > 0;
        if (mesaPattern && halfCovered != width + height)
            leaks.push_back("a partial pattern: " + std::to_string(halfCovered) + " of " + std::to_string(width + height) +
                            " edge pixels half covered");
        return leaks;
    }

}

// 4 samples: exact, or Mesa's half-covered row above and column left (and nothing else)

// The same through the HTML UI renderer: overflow: hidden scrolled past its top edge
TEST(MsaaScissor, HtmlUIClipEdge){
    JASPER_REQUIRE_GPU();
    for (int samples : {1, 4}){
        SCOPED_TRACE(samples);
        RenderTestContext render(64, 48, 64, 48, samples);
        ASSERT_TRUE(render.isValid());
        Rml::ElementDocument* document = render.loadFromMemory(R"(<rml><head><style>
body { width: 64px; height: 48px; margin: 0; padding: 0; }
div { display: block; }
#clip { position: absolute; left: 10px; top: 10px; width: 30px; height: 20px; overflow: hidden; }
#content { height: 200px; width: 100px; margin-left: -20px; background-color: #ffffff; }
</style></head><body><div id="clip"><div id="content"/></div></body></rml>)", "msaa-scissor");
        ASSERT_NE(document, nullptr);
        ASSERT_TRUE(render.renderUntilDrawn());
        document->GetElementById("clip")->SetScrollTop(30.0f);
        ASSERT_TRUE(render.renderFrame());
        std::vector<uint8_t> rgba;
        ASSERT_TRUE(render.readLayer(rgba));
        // (the 64 x 48 layer: compare in a 64-wide square window of its rows)
        rgba.resize(size_t(64) * 64 * 4, 0);
        bool mesaPattern = false;
        const std::vector<std::string> leaks = unexpectedLeaks(rgba, 64, 10, 10, 30, 20, mesaPattern);
        EXPECT_TRUE(leaks.empty()) << leaks.size() << " pixels, first " << (leaks.empty() ? "" : leaks[0]);
        if (samples == 1){
            EXPECT_FALSE(mesaPattern) << "without MSAA the clip edge is exact";
        }
        std::cout << "[ MSAA     ] HTML UI, " << samples << " sample(s): " << (mesaPattern ? "Mesa's half-covered edges" : "exact") << "\n";
    }
}
