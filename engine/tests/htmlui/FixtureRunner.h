// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUI_FIXTURE_RUNNER_H
#define HTMLUI_FIXTURE_RUNNER_H

// The engine side of the phase-3 browser-parity fixtures (phase-3 spec 5.2, 5.3).
//
// - fixtures/p3/<area>/<name>.html + <name>.json (the sidecar: viewport, steps, boxes, tolerance,
//   selfTest), references in golden/browser/<area>/<name>[.<capture>].png and <name>.boxes.json,
//   written by tools/htmlui/render-reference.mjs --fixtures. FixtureRunner loads the fixture
//   through the compat layer into a context of the sidecar's viewport and runs the same steps
//   through the element API (UIElement: classes, styles, attributes, scrolling) and injected input
//   (hover, focus, press: pointer and Tab key events), with the UI clock fixed and moved by the
//   `advance` steps (HtmlUIClock::setFixedTime), so captures are frame-exact on both sides.
// - fixtures/taffy/<suite>/<name>.html (Taffy's layout fixtures), references in
//   golden/browser/taffy/<suite>.boxes.json: the boxes of #test-root and every element under it.
// - known-failures.txt / excluded.txt next to the fixtures (fixtures/taffy/, fixtures/p3/): one
//   fixture per line, "<id> owner=<package> reason=<class> # note"; a known failure is expected to
//   fail (the test fails once it passes: the package that fixed it removes its line).
//
// The asset root is engine/tests/htmlui while a runner exists, so fixtures load their fonts from
// engine/tests/htmlui/fonts with relative URLs, in Chromium and in the engine alike.

#include "compat/BrowserCompare.h"

#include <cstddef>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace Rml {
    class Context;
    class Element;
    class ElementDocument;
}

namespace jasperengine::test {

    struct FixtureStep {
        enum class Kind { Hover, Focus, Press, Release, AddClass, RemoveClass, SetStyle, SetAttr, Scroll, Advance, Capture };
        Kind kind = Kind::Capture;
        std::string id;              // the target element's id ("#id" in the sidecar); release: "" = where the pointer is
        std::string name;            // class, property or attribute name
        std::string value;           // property or attribute value
        bool remove = false;         // setAttr with null, setStyle with ""
        bool keyboard = false;       // focus: by Tab navigation (:focus-visible), else by a click
        float x = 0.0f, y = 0.0f;    // scroll
        double seconds = 0.0;        // advance
        std::string suffix;          // capture: <name>.<suffix>.png ("" = <name>.png)
    };

    struct FixtureSidecar {
        int width = 0, height = 0;
        std::vector<FixtureStep> steps;
        // text-heavy fixture: the boxes are the oracle, the image is compared and reported only
        // (stb_truetype and Skia rasterize glyphs differently)
        bool boxes = false;
        compat::StrictGate gate;             // kStrictGate unless "tolerance" overrides it
        float boxTolerance = 1.0f;           // px, |delta| of x, y, w, h
        bool toleranceOverridden = false;
        // "samples": 1 renders without MSAA (default: the engine's 4x), with a comment saying why. For
        // fixtures that scroll content past a clip edge: llvmpipe and lavapipe (the test image's GL and
        // Vulkan drivers, Mesa 23.2.1) let half the samples of the row above and the column left of a
        // scissor rectangle through under MSAA (MsaaScissor.FourSamples documents it)
        int sampleCount = 4;
        std::string comment;
        // the harness's own fixtures (area testkit): the reference had `referenceCss` injected, so
        // the comparison must report the engine as different
        bool expectMismatch = false;
        std::string referenceCss;

        // the capture suffixes in order (one "" when there is no capture step: a capture at the end)
        std::vector<std::string> captures() const;
    };

    bool loadSidecar(const std::string& path, FixtureSidecar& sidecar, std::string* error = nullptr);

    // directories
    std::string htmluiTestSourceDirectory();   // <source> = engine/tests/htmlui (the fixtures' asset root)
    std::string browserFixturesDirectory();    // <source>/fixtures/p3
    std::string taffyFixturesDirectory();      // <source>/fixtures/taffy
    std::string browserReferenceDirectory();   // <source>/golden/browser
    std::string browserArtifactDirectory();    // <binary>/htmlui-test-output/browser

    struct BrowserFixture {
        std::string area, name;
        std::string id() const{ return area + "/" + name; }
        std::string assetPath() const{ return "fixtures/p3/" + area + "/" + name + ".html"; }
    };
    inline void PrintTo(const BrowserFixture& fixture, std::ostream* os){ *os << fixture.id(); }
    // every fixtures/p3/<area>/<name>.html with a <name>.json, sorted
    std::vector<BrowserFixture> listBrowserFixtures();
    // every fixtures/taffy/<suite>/<name>.html as "<suite>/<name>", sorted
    std::vector<std::string> listTaffyFixtures();

    // known-failures.txt, excluded.txt
    struct ListedFixture {
        std::string id;
        std::string owner;           // owner=<package>
        std::string reason;          // reason=<class>
        std::string note;            // after '#'
        int line = 0;
    };
    struct FixtureList {
        std::vector<ListedFixture> entries;
        std::vector<std::string> errors;     // malformed lines
        const ListedFixture* find(const std::string& id) const;
    };
    FixtureList loadFixtureList(const std::string& path);   // a missing file is an empty list

    // the packages of phase 3 that may own a known failure (spec 4)
    bool isPhase3Package(const std::string& name);

    // Taffy references: golden/browser/taffy/<suite>.boxes.json
    struct TaffyBox {
        std::string path;            // child indices among element children from #test-root ("" = the root, "0.2")
        std::string tag;
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;   // root: in the viewport; others: relative to the root
    };
    struct TaffyDifference {
        std::string path;
        char field = 0;              // 'x', 'y', 'w', 'h'; 0 = structure (missing element or other tag)
        float engine = 0.0f, reference = 0.0f;
        std::string detail;          // for structure differences
        std::string text() const;
    };
    // fixture name -> boxes of one suite file; false when it cannot be read
    bool loadTaffySuite(const std::string& path, std::vector<std::pair<std::string, std::vector<TaffyBox>>>& fixtures,
                        std::string* error = nullptr);
    std::vector<TaffyDifference> compareTaffyBoxes(const std::vector<TaffyBox>& engine, const std::vector<TaffyBox>& reference,
                                                   float tolerance = 1.0f);

    // The engine side: one context of the given size on its own render target (the engine's
    // default 4x MSAA, or the sidecar's "samples"), the compat layer with the fork's present
    // features, the UI clock fixed.
    class JASPERENGINE_INTERNAL FixtureRunner {
    public:
        struct Capture {
            std::string suffix;
            compat::Image image;                    // premultiplied, transparent background; empty without images
            std::vector<compat::LayoutBox> boxes;   // every element with an id, document order
        };

        FixtureRunner(int width, int height, int sampleCount = 4);
        ~FixtureRunner();
        FixtureRunner(const FixtureRunner&) = delete;
        FixtureRunner& operator=(const FixtureRunner&) = delete;

        bool valid() const;          // HtmlUI and the GPU are available (false: the test skips)
        int width() const;
        int height() const;

        // asset path ("fixtures/p3/paint/x.html"); unloads the previous document; the clock restarts at 0
        bool load(const std::string& assetPath, std::string* error = nullptr);
        // every step of the sidecar; a capture step (or the end, without one) adds a capture
        bool run(const FixtureSidecar& sidecar, std::vector<Capture>& captures, std::string* error = nullptr, bool images = true);
        bool step(const FixtureStep& step, std::string* error = nullptr);

        double time() const;        // the fixture's clock (seconds of `advance`)
        bool capture(Capture& capture, bool image = true);
        std::vector<compat::LayoutBox> idBoxes() const;
        // #test-root and every element under it (Taffy fixtures)
        bool taffyBoxes(std::vector<TaffyBox>& boxes, std::string* error = nullptr) const;

        Rml::Context* context() const;
        Rml::ElementDocument* document() const;
        std::shared_ptr<const htmlui::compat::CompatDocument> compiled() const;
        std::string diagnosticsText() const;

    private:
        struct State;
        std::unique_ptr<State> state;
        void frame(bool draw, bool* drawn = nullptr);
        Rml::Element* element(const std::string& id, std::string* error) const;
        bool pointerTo(const std::string& id, std::string* error);
    };

}

#endif //HTMLUI_FIXTURE_RUNNER_H
