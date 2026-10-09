// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_BROWSER_COMPARE_H
#define COMPAT_BROWSER_COMPARE_H

// BrowserCompare: the engine against Chromium (phase-2 spec 5.3). The golden pages of
// fixtures/compat/pages are rendered by Chromium with tools/htmlui/render-reference.mjs into
// golden/compat/reference (<page>.png, <page>.notext.png, <page>.layout.json); BrowserPage
// renders the same page through the compat layer the way the reference script does, and the
// comparisons below are the spec's gates:
//
//   1. no-text image vs Chromium: fuzzy-bad <= 0.5 % (channel threshold 48), max-block <= 64
//   2. layout vs Chromium: every element with an id within 3 px (data-ref-tol="text": 6 px or
//      4 % of the reference extent, whichever is larger)
//   3. engine golden (golden/compat/engine/<page>.png): fuzzy-bad <= 0.1 % (threshold 8),
//      max-block <= 8
//   4. full image vs Chromium: reported only
//   5. diagnostics snapshot (golden/compat/engine/<page>.diag.txt): exact
//
// Metrics, on premultiplied RGBA8 (the calibration of spec 5.3):
// - fuzzy-bad: a pixel is bad when no pixel of the 3x3 neighbourhood at its position in the
//   other image is within the threshold in every channel; counted in both directions (a pixel
//   bad in either counts once), in percent of all pixels;
// - max-block: the image cut in 8x8 blocks; per block and channel the difference of the two
//   images' mean values; the largest over channels and blocks.
// Phase 3 adds its stricter metric (compareStrict) and the fixture runner next to these.

#include "Export.h"
#include "htmlui/compat/ForkFeatures.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Rml {
    class Context;
    class Element;
    class ElementDocument;
}

namespace jasperengine::htmlui::compat {
    struct CompatDocument;
}

namespace jasperengine::test::compat {

    // premultiplied RGBA8, rows top first
    struct Image {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> rgba;

        bool empty() const{ return width <= 0 || height <= 0; }
    };

    Image premultiplied(int width, int height, const std::vector<uint8_t>& straightRgba);
    std::vector<uint8_t> unpremultiplied(const Image& image);
    // PNG files hold straight alpha (Chromium's omitBackground screenshots, our outputs)
    bool loadPng(const std::string& path, Image& image);
    bool savePng(const std::string& path, const Image& image);

    struct ImageGate {
        const char* name;
        int channelThreshold;
        double maxFuzzyBadPercent;
        double maxBlock;
    };
    inline constexpr ImageGate kChromiumNoTextGate{"no-text image vs Chromium", 48, 0.5, 64.0};
    inline constexpr ImageGate kChromiumFullGate{"full image vs Chromium (reported only)", 48, 0.5, 64.0};
    inline constexpr ImageGate kEngineGoldenGate{"engine golden", 8, 0.1, 8.0};

    struct ImageMetrics {
        bool comparable = false;
        std::string error;                  // when not comparable (missing image, sizes)
        int channelThreshold = 0;
        size_t fuzzyBadPixels = 0;
        double fuzzyBadPercent = 0.0;
        double maxBlock = 0.0;
        int maxBlockX = 0, maxBlockY = 0;   // top-left pixel of the worst block

        bool passes(const ImageGate& gate) const;
        std::string text() const;
    };

    ImageMetrics compareImages(const Image& actual, const Image& expected, int channelThreshold);
    // `expected` at 30 % gray, bad pixels red (brighter = larger difference), the worst 8x8
    // block outlined in yellow; opaque
    Image heatMap(const Image& actual, const Image& expected, int channelThreshold);

    struct LayoutBox {
        std::string id;
        std::string tag;
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;   // border box in viewport px
        std::string tolerance;                          // "", "text" or "table" (data-ref-tol)
        bool transformed = false;                       // under a CSS transform: not compared
    };

    struct ReferenceLayout {
        std::string page;
        int viewportWidth = 0, viewportHeight = 0;
        std::string hover, focus, active;               // the states the reference captured
        std::vector<std::string> requires;              // fork features: "grid", "pseudo-elements"
        std::vector<LayoutBox> elements;
    };

    bool loadReferenceLayout(const std::string& path, ReferenceLayout& layout, std::string* error = nullptr);

    struct LayoutDifference {
        std::string id;
        char field = 0;                 // 'x', 'y', 'w', 'h'; 0 = the engine has no such element
        float engine = 0.0f, reference = 0.0f, tolerance = 0.0f;
        std::string text() const;
    };

    // 3 px; for "text" boxes max(6 px, 4 % of the reference width (x, w) or height (y, h));
    // "table" (an auto-width table): as text, its width not compared
    float layoutTolerance(const LayoutBox& reference, char field);
    // every reference element that is not transformed against the engine box of the same id
    std::vector<LayoutDifference> compareLayout(const std::vector<LayoutBox>& engine, const ReferenceLayout& reference);

    // The fork features a page needs (<meta name="htmlui-test-requires">) that are missing
    // from `fork`; empty when the image and layout gates apply.
    std::vector<std::string> missingForkFeatures(const std::vector<std::string>& requires,
                                                 const htmlui::compat::ForkFeatures& fork);

    // directories of the golden pages
    std::string pagesDirectory();          // <source>/fixtures/compat/pages
    std::string referenceDirectory();      // <source>/golden/compat/reference
    std::string engineGoldenDirectory();   // <source>/golden/compat/engine
    std::string artifactDirectory();       // <binary>/htmlui-test-output/compat

    // The engine side of the reference script: a 1280x720 context at dp ratio 1 on its own
    // render target, with the pages directory as the assets directory (its import manifest
    // serves the Google Fonts URLs), the page loaded through DocumentLoader::load with
    // transitions dropped, animations run to their end on a fixed UI clock, and the states of
    // the reference (hover: a pointer move to the element's centre; focus: Tab presses until
    // the element has the focus; active: a press on it).
    class JASPERENGINE_INTERNAL BrowserPage {
    public:
        struct Options {
            int width = 1280;
            int height = 720;
            int sampleCount = 4;             // the engine's default MSAA (htmlUiMsaaSamples)
            htmlui::compat::ForkFeatures fork = htmlui::compat::ForkFeatures::present();
        };

        BrowserPage();
        explicit BrowserPage(const Options& options);
        ~BrowserPage();
        BrowserPage(const BrowserPage&) = delete;
        BrowserPage& operator=(const BrowserPage&) = delete;

        // HtmlUI and the GPU are available (false: the test skips)
        bool valid() const;
        // page = "main-menu.html"; false with `error` set when the document does not load
        bool load(const std::string& page, std::string* error = nullptr);
        // the states of the reference
        bool hover(const std::string& id);
        bool focusByKeyboard(const std::string& id, int maxTabs = 64);
        bool press(const std::string& id);
        bool applyStates(const ReferenceLayout& reference, std::string* error = nullptr);

        // one rendered frame read back (premultiplied, transparent where nothing is drawn);
        // withText = false draws no glyphs (layout unchanged; false when the build has no
        // RmlFontEngine::setRenderTextForTesting)
        bool render(Image& image, bool withText = true);
        // the border box of every element of `reference` that the document has
        std::vector<LayoutBox> layout(const ReferenceLayout& reference) const;

        Rml::ElementDocument* document() const;
        Rml::Context* context() const;
        std::shared_ptr<const htmlui::compat::CompatDocument> compiled() const;
        // the compile's diagnostics as toText() lines, sorted, one per line
        std::string diagnosticsText() const;

    private:
        struct State;
        std::unique_ptr<State> state;
        bool frame(bool draw);   // true when a frame was drawn
        void settleAnimations();
    };

}

#endif //COMPAT_BROWSER_COMPARE_H
