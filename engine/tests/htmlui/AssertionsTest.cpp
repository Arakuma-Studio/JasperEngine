// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Phase 4 (p4-inspector, spec §3.3.2, §5.1): htmlui::HtmlUIAssertions. Each kind gets a pass
// case, a fail case with the numbers of its message checked, and its edge cases: zero matches,
// count with max 0, NoOverlap's ancestor/hidden/with rules, WithinParent's container,
// TextFits' allow_scroll/single_line/ancestor clipping, Contrast's WCAG ratios, large text,
// images, the transparent root and composited colours, MinSize's 44 px default, Visible's
// unobstructed test, invalid selectors and other viewports.

#include "InspectorTestCommon.h"

#include "HtmlUIContext.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <regex>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::htmlui;

namespace {

    class AssertionsTest : public InspectorTestBase {
    protected:
        std::unique_ptr<SceneFixture> fixture;
        Entity entity = NULL_ENTITY;

        void TearDown() override{
            fixture.reset();
            InspectorTestBase::TearDown();
        }

        void load(const std::string& name, const std::string& css, const std::string& body){
            page(name, css, body);
            fixture.reset(new SceneFixture());
            UIDocument document = fixture->addDocument("ui/" + name + ".html");
            ASSERT_TRUE(fixture->waitUntilLoaded());
            entity = document.getEntity();
        }

        Scene* scene(){
            return &fixture->getScene();
        }

        AssertResult run(AssertSpec spec){
            return HtmlUIAssertions::run(scene(), entity, spec);
        }

        static AssertSpec make(AssertKind kind, const std::string& selector){
            AssertSpec spec;
            spec.kind = kind;
            spec.selector = selector;
            return spec;
        }

        InspectRect rectOf(const std::string& selector){
            InspectRect rect;
            std::string error;
            EXPECT_TRUE(HtmlUIInspector::elementRect(scene(), entity, selector, rect, error)) << error;
            return rect;
        }
    };

    std::string describe(const AssertResult& result){
        std::string text = result.error.empty() ? std::string() : "error: " + result.error + "\n";
        for (const AssertFailure& failure : result.failures)
            text += failure.ref + ": " + failure.message + "\n";
        for (const std::string& ref : result.inconclusive)
            text += "inconclusive: " + ref + "\n";
        return text;
    }

    // the numbers of a message, in order
    std::vector<double> numbers(const std::string& message){
        std::vector<double> out;
        const std::regex number("-?[0-9]+(\\.[0-9]+)?");
        for (auto it = std::sregex_iterator(message.begin(), message.end(), number); it != std::sregex_iterator(); ++it)
            out.push_back(std::atof(it->str().c_str()));
        return out;
    }

    std::string px(float value){
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.1f", value);
        std::string text = buffer;
        if (text.size() > 2 && text.compare(text.size() - 2, 2, ".0") == 0)
            text.resize(text.size() - 2);
        return text + "px";
    }

    // a number as the messages print it: at most 2 decimals, no trailing zeros
    std::string fmt(double value){
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.2f", value);
        std::string text = buffer;
        while (!text.empty() && text.back() == '0')
            text.pop_back();
        if (!text.empty() && text.back() == '.')
            text.pop_back();
        return text;
    }

    double linear(double channel){
        return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
    }

    double ratio(double grayA, double grayB){
        const double a = linear(grayA), b = linear(grayB);
        return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
    }

    const char* const kLayoutCss =
        "* { margin: 0; padding: 0; }\n"
        "body { font-size: 16px; }\n"
        ".abs { position: absolute; }\n"
        ".btn { position: absolute; width: 120px; height: 40px; }\n";

}

TEST_F(AssertionsTest, ExistsAndCount){
    load("count", kLayoutCss, "<button class=\"btn\" style=\"left: 0\">a</button><button class=\"btn\" style=\"left: 200px\">b</button>"
                              "<button class=\"btn\" style=\"left: 400px\">c</button>");
    AssertResult result = run(make(AssertKind::Exists, ".btn"));
    EXPECT_TRUE(result.passed) << describe(result);
    EXPECT_EQ(result.matched, 3u);

    result = run(make(AssertKind::Exists, ".nope"));
    EXPECT_FALSE(result.passed);
    EXPECT_EQ(result.matched, 0u);
    ASSERT_EQ(result.failures.size(), 1u);
    EXPECT_EQ(result.failures[0].message, "selector matched nothing");
    EXPECT_EQ(result.failures[0].ref, ".nope");
    EXPECT_TRUE(result.error.empty());

    AssertSpec spec = make(AssertKind::Count, ".btn");
    spec.count = 3;
    EXPECT_TRUE(run(spec).passed);
    spec.count = 2;
    result = run(spec);
    ASSERT_EQ(result.failures.size(), 1u);
    EXPECT_EQ(result.failures[0].message, "matched 3, expected exactly 2");
    spec.count = -1;
    spec.min = 4;
    EXPECT_EQ(run(spec).failures.at(0).message, "matched 3, expected at least 4");
    spec.min = -1;
    spec.max = 2;
    EXPECT_EQ(run(spec).failures.at(0).message, "matched 3, expected at most 2");
    spec.min = 1;
    spec.max = 3;
    EXPECT_TRUE(run(spec).passed);

    // zero matches: only max 0 / count 0 pass
    spec = make(AssertKind::Count, ".nope");
    spec.max = 0;
    result = run(spec);
    EXPECT_TRUE(result.passed) << describe(result);
    EXPECT_EQ(result.matched, 0u);
    spec.max = -1;
    spec.count = 0;
    EXPECT_TRUE(run(spec).passed);
    spec.count = -1;
    spec.min = 0;
    result = run(spec);
    EXPECT_FALSE(result.passed);
    EXPECT_EQ(result.failures.at(0).message, "selector matched nothing");
    spec = make(AssertKind::Count, ".btn");
    spec.max = 0;
    EXPECT_EQ(run(spec).failures.at(0).message, "matched 3, expected at most 0");

    // an invalid selector is an error, not a failure
    result = run(make(AssertKind::Exists, "div[["));
    EXPECT_FALSE(result.passed);
    EXPECT_TRUE(result.failures.empty());
    EXPECT_EQ(result.error.rfind("selector \"div[[\" is not valid: ", 0), 0u) << result.error;
    result = run(make(AssertKind::Visible, ""));
    EXPECT_FALSE(result.passed);
    EXPECT_EQ(result.error, "selector is empty");
    result = HtmlUIAssertions::run(scene(), fixture->getCamera().getEntity(), make(AssertKind::Exists, ".btn"));
    EXPECT_FALSE(result.passed);
    EXPECT_NE(result.error.find("has no UI Document component"), std::string::npos) << result.error;
}

TEST_F(AssertionsTest, Visible){
    load("visible", std::string(kLayoutCss) +
         "#shown { left: 0; top: 0; }\n"
         "#hidden-parent { display: none; }\n"
         "#veiled { left: 200px; visibility: hidden; }\n"
         "#faint { left: 400px; opacity: 0.02; }\n"
         "#flat { left: 600px; width: 0; }\n"
         "#far { left: 1300px; }\n"
         "#clipper { position: absolute; left: 0; top: 100px; width: 100px; height: 50px; overflow: hidden; }\n"
         "#cut { left: 0; top: 60px; }\n"
         "#half { left: 50px; top: 0; }\n"
         "#target { left: 0; top: 300px; }\n"
         "#cover { position: absolute; left: 40px; top: 290px; width: 60px; height: 60px; background: #000; }\n"
         "#ghost { left: 200px; top: 300px; pointer-events: none; }\n",
         "<button class=\"btn\" id=\"shown\">ok</button>\n"
         "<div id=\"hidden-parent\"><button class=\"btn\" id=\"inside\">x</button></div>\n"
         "<button class=\"btn\" id=\"veiled\">x</button>\n"
         "<button class=\"btn\" id=\"faint\">x</button>\n"
         "<div class=\"btn\" id=\"flat\">x</div>\n"
         "<button class=\"btn\" id=\"far\">x</button>\n"
         "<div id=\"clipper\"><button class=\"btn\" id=\"cut\">x</button><button class=\"btn\" id=\"half\">x</button></div>\n"
         "<button class=\"btn\" id=\"target\">x</button><div id=\"cover\"></div>\n"
         "<button class=\"btn\" id=\"ghost\">x</button>\n");

    AssertResult result = run(make(AssertKind::Visible, "#shown"));
    EXPECT_TRUE(result.passed) << describe(result);
    AssertSpec spec = make(AssertKind::Visible, "#shown");
    spec.unobstructed = true;
    EXPECT_TRUE(run(spec).passed) << describe(run(spec));

    const auto message = [this](const std::string& selector, bool unobstructed = false){
        AssertSpec visible = make(AssertKind::Visible, selector);
        visible.unobstructed = unobstructed;
        const AssertResult checked = run(visible);
        EXPECT_FALSE(checked.passed) << selector;
        return checked.failures.empty() ? std::string() : checked.failures[0].message;
    };
    EXPECT_EQ(message("#inside"), "display: none on #hidden-parent");
    EXPECT_EQ(message("#hidden-parent"), "display: none");
    EXPECT_EQ(message("#veiled"), "visibility: hidden");
    EXPECT_EQ(message("#faint"), "effective opacity 0.02 (below 0.05)");
    EXPECT_EQ(message("#flat"), "border box is 0px x 40px");
    EXPECT_EQ(message("#far"), "border box [1300,0 120x40] is outside the 1280x720 viewport");
    EXPECT_EQ(message("#cut"), "fully clipped by the overflow of #clipper");
    EXPECT_TRUE(run(make(AssertKind::Visible, "#half")).passed) << "partly clipped is visible";

    // unobstructed: the hit at the centre of the visible part
    EXPECT_TRUE(run(make(AssertKind::Visible, "#target")).passed);
    spec = make(AssertKind::Visible, "#target");
    spec.unobstructed = true;
    result = run(spec);
    ASSERT_EQ(result.failures.size(), 1u) << describe(result);
    EXPECT_EQ(result.failures[0].message, "covered by #cover at (60, 320)");
    EXPECT_EQ(result.failures[0].otherRef, "#cover");
    EXPECT_RECT(result.failures[0].otherRect, 40, 290, 60, 60);
    EXPECT_RECT(result.failures[0].rect, 0, 300, 120, 40);
    // the half the clip leaves: its centre
    spec = make(AssertKind::Visible, "#half");
    spec.unobstructed = true;
    EXPECT_TRUE(run(spec).passed) << describe(run(spec));
    EXPECT_EQ(message("#ghost", true), "pointer-events: none: the pointer goes through it at (260, 320)");

    // several matches: each failing one is a failure
    result = run(make(AssertKind::Visible, ".btn"));
    EXPECT_EQ(result.matched, 10u);
    EXPECT_EQ(result.failures.size(), 6u) << describe(result);
}

TEST_F(AssertionsTest, VisibleCoveredByAnotherDocument){
    page("top", "* { margin: 0; }\n#sheet { position: absolute; left: 0; top: 0; width: 300px; height: 300px; background: #222; }\n",
         "<div id=\"sheet\"></div>");
    load("under", std::string(kLayoutCss) + "#b { left: 10px; top: 10px; }\n#free { left: 500px; top: 10px; }\n",
         "<button class=\"btn\" id=\"b\">b</button><button class=\"btn\" id=\"free\">f</button>");
    fixture->addDocument("ui/top.html", 10);
    ASSERT_TRUE(fixture->waitUntilLoaded());
    AssertSpec spec = make(AssertKind::Visible, "#b");
    spec.unobstructed = true;
    AssertResult result = run(spec);
    ASSERT_EQ(result.failures.size(), 1u) << describe(result);
    EXPECT_EQ(result.failures[0].message, "covered by #sheet of the UI document ui/top.html at (70, 30)");
    // where the upper document's page is transparent the pointer passes through it, as the engine routes it
    spec.selector = "#free";
    result = run(spec);
    EXPECT_TRUE(result.passed) << describe(result);
    UIDocumentSystem& system = *scene()->getSystem<UIDocumentSystem>();
    EXPECT_TRUE(system.eventOnPointerMove(560, 30, 0)) << "the document below takes the pointer";
    Rml::Element* hover = system.getContext()->getRmlContext()->GetHoverElement();
    ASSERT_NE(hover, nullptr);
    EXPECT_EQ(hover->GetId(), "free");
    system.eventOnMouseLeave();
}

TEST_F(AssertionsTest, TextFits){
    load("text", std::string(kLayoutCss) +
         "#fits { left: 0; top: 0; }\n"
         "#buy { left: 0; top: 50px; white-space: nowrap; }\n"
         "#wrapped { position: absolute; left: 0; top: 100px; width: 90px; }\n"
         ".list { position: absolute; left: 300px; top: 0; width: 200px; height: 50px; overflow: auto; }\n"
         ".list p { height: 20px; }\n"
         ".notes { position: absolute; left: 600px; top: 0; width: 150px; height: 30px; overflow: auto; }\n"
         "#clipper { position: absolute; left: 0; top: 300px; width: 200px; height: 20px; overflow: hidden; }\n"
         "#low { position: relative; top: 30px; }\n"
         "#parent { position: absolute; left: 0; top: 400px; width: 300px; }\n"
         "#ellipsis { left: 0; top: 500px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }\n",
         "<button class=\"btn\" id=\"fits\">Buy</button>\n"
         "<button class=\"btn\" id=\"buy\">Buy the legendary sword of the north</button>\n"
         "<div id=\"wrapped\">several words that wrap</div>\n"
         "<div class=\"list\"><p>one</p><p>two</p><p>three</p><p>four</p><p>five</p></div>\n"
         "<div class=\"notes\">a long note that wraps over many lines in a short scroll box</div>\n"
         "<div id=\"clipper\"><div id=\"low\">cut</div></div>\n"
         "<div id=\"parent\"><span id=\"inline\">inline text in a wide block</span></div>\n"
         "<button class=\"btn\" id=\"ellipsis\">Buy the legendary sword of the north</button>\n");

    AssertResult result = run(make(AssertKind::TextFits, "#fits"));
    EXPECT_TRUE(result.passed) << describe(result);
    result = run(make(AssertKind::TextFits, "#parent"));
    EXPECT_TRUE(result.passed) << "inline text measured against its block: " << describe(result);

    result = run(make(AssertKind::TextFits, "#buy"));
    ASSERT_EQ(result.failures.size(), 1u) << describe(result);
    EXPECT_EQ(result.failures[0].ref, "#buy");
    const std::string& message = result.failures[0].message;
    InspectDetail detail;
    std::string error;
    ASSERT_TRUE(HtmlUIInspector::detail(scene(), entity, "#buy", {"width"}, detail, error)) << error;
    EXPECT_EQ(message.find(" content box ("), message.find("wide in a ") + 10 + px(detail.box.content.width).size()) << message;
    const std::vector<double> values = numbers(message);
    ASSERT_EQ(values.size(), 3u) << message;
    EXPECT_GT(values[0], 200.0) << message;
    EXPECT_EQ(values[1], detail.box.content.width);
    EXPECT_GE(values[2], values[0] - 120.0 - 0.11) << "the over reaches past the box: " << message;
    EXPECT_RECT(result.failures[0].rect, 0, 50, 120, 40);
    AssertSpec spec = make(AssertKind::TextFits, "#buy");
    spec.tolerance = 1000;
    EXPECT_TRUE(run(spec).passed) << "within the tolerance";

    // single_line
    EXPECT_TRUE(run(make(AssertKind::TextFits, "#wrapped")).passed) << "wrapping fits";
    spec = make(AssertKind::TextFits, "#wrapped");
    spec.singleLine = true;
    result = run(spec);
    ASSERT_EQ(result.failures.size(), 1u) << describe(result);
    EXPECT_EQ(result.failures[0].message.rfind("text wraps to ", 0), 0u) << result.failures[0].message;
    EXPECT_GE(numbers(result.failures[0].message).at(0), 2.0);
    spec.selector = "#fits";
    EXPECT_TRUE(run(spec).passed);

    // allow_scroll: the rows of a scroll box are cut by it unless allowed; own text taller than its box too
    result = run(make(AssertKind::TextFits, ".list"));
    EXPECT_FALSE(result.passed);
    ASSERT_FALSE(result.failures.empty());
    EXPECT_NE(result.failures.back().message.find("cut by the overflow of body > div:nth-child(4)"), std::string::npos) << describe(result);
    spec = make(AssertKind::TextFits, ".list");
    spec.allowScroll = true;
    result = run(spec);
    EXPECT_TRUE(result.passed) << describe(result);
    result = run(make(AssertKind::TextFits, ".notes"));
    ASSERT_EQ(result.failures.size(), 1u) << describe(result);
    EXPECT_NE(result.failures[0].message.find("tall in a 30px content box"), std::string::npos) << result.failures[0].message;
    spec.selector = ".notes";
    EXPECT_TRUE(run(spec).passed) << describe(run(spec));
    // allow_scroll does not excuse a box that does not scroll
    spec.selector = "#buy";
    EXPECT_FALSE(run(spec).passed);

    // text that fits its own box but an ancestor's overflow cuts
    result = run(make(AssertKind::TextFits, "#clipper"));
    ASSERT_EQ(result.failures.size(), 1u) << describe(result);
    EXPECT_EQ(result.failures[0].ref, "#low");
    EXPECT_EQ(result.failures[0].message.rfind("text is cut by the overflow of #clipper (", 0), 0u) << result.failures[0].message;

    // an ellipsis truncates the text: it does not fit
    EXPECT_FALSE(run(make(AssertKind::TextFits, "#ellipsis")).passed);
    // hidden text is not checked; zero matches fail
    EXPECT_EQ(run(make(AssertKind::TextFits, ".nope")).failures.at(0).message, "selector matched nothing");
}

TEST_F(AssertionsTest, NoOverlap){
    load("overlap", std::string(kLayoutCss) +
         ".card { position: absolute; width: 100px; height: 100px; }\n"
         "#a { left: 0; top: 0; }\n"
         "#b { left: 80px; top: 90px; }\n"
         "#c { left: 300px; top: 0; }\n"
         "#d { left: 400px; top: 0; }\n"
         "#e { left: 499.6px; top: 0; }\n"
         "#nested { left: 10px; top: 10px; width: 50px; height: 50px; }\n"
         "#hidden { left: 310px; top: 10px; display: none; }\n"
         "#clear { left: 320px; top: 20px; opacity: 0; }\n"
         ".icon { position: absolute; width: 20px; height: 20px; }\n"
         ".label { position: absolute; width: 100px; height: 20px; }\n",
         "<div class=\"card\" id=\"a\"></div><div class=\"card\" id=\"b\"></div>\n"
         "<div class=\"card\" id=\"c\"><div class=\"card\" id=\"nested\"></div></div><div class=\"card\" id=\"d\"></div><div class=\"card\" id=\"e\"></div>\n"
         "<div class=\"card\" id=\"hidden\"></div><div class=\"card\" id=\"clear\"></div>\n"
         "<div class=\"icon\" id=\"i1\" style=\"left: 0; top: 600px\"></div><div class=\"label\" id=\"l1\" style=\"left: 10px; top: 605px\"></div>\n"
         "<div class=\"icon\" id=\"i2\" style=\"left: 0; top: 650px\"></div><div class=\"label\" id=\"l2\" style=\"left: 30px; top: 650px\"></div>\n");

    AssertResult result = run(make(AssertKind::NoOverlap, ".card"));
    EXPECT_EQ(result.matched, 8u);
    ASSERT_EQ(result.failures.size(), 1u) << describe(result) << "(ancestors, hidden, transparent, touching and sub-tolerance pairs skipped)";
    const AssertFailure& failure = result.failures[0];
    EXPECT_EQ(failure.ref, "#a");
    EXPECT_EQ(failure.otherRef, "#b");
    EXPECT_EQ(failure.message, "overlaps #b by 20px x 10px at [80,90 20x10]");
    EXPECT_RECT(failure.rect, 0, 0, 100, 100);
    EXPECT_RECT(failure.otherRect, 80, 90, 100, 100);
    AssertSpec spec = make(AssertKind::NoOverlap, ".card");
    spec.tolerance = 10;
    EXPECT_TRUE(run(spec).passed) << "10px on one axis is within a 10px tolerance";

    // with: the pairs of two sets
    spec = make(AssertKind::NoOverlap, ".icon");
    spec.with = ".label";
    result = run(spec);
    EXPECT_EQ(result.matched, 4u);
    ASSERT_EQ(result.failures.size(), 1u) << describe(result);
    EXPECT_EQ(result.failures[0].ref, "#i1");
    EXPECT_EQ(result.failures[0].otherRef, "#l1");
    EXPECT_EQ(result.failures[0].message, "overlaps #l1 by 10px x 15px at [10,605 10x15]");
    spec.with = ".nothing";
    result = run(spec);
    EXPECT_FALSE(result.passed);
    EXPECT_EQ(result.failures.at(0).message, "with selector matched nothing");
    spec.with = "div[[";
    result = run(spec);
    EXPECT_FALSE(result.passed);
    EXPECT_FALSE(result.error.empty());
    // a set against itself: no element overlaps itself
    spec = make(AssertKind::NoOverlap, "#c");
    spec.with = "#c";
    EXPECT_TRUE(run(spec).passed);
}

TEST_F(AssertionsTest, WithinParent){
    load("parent", std::string(kLayoutCss) +
         ".outer { position: absolute; left: 100px; top: 100px; width: 400px; height: 300px; padding: 10px; border: 5px solid #fff; }\n"
         ".inner { position: absolute; left: 0; top: 0; width: 200px; height: 100px; }\n"
         "#fits { width: 100px; height: 50px; }\n"
         "#wide { position: absolute; left: 150px; top: 0; width: 80px; height: 120px; }\n"
         "#out { position: absolute; left: 380px; top: 0; width: 60px; height: 10px; }\n"
         "#gone { position: absolute; left: 900px; display: none; }\n",
         "<div class=\"outer\" id=\"outer\"><div class=\"inner\" id=\"inner\"><div id=\"fits\"></div><div id=\"wide\"></div><div id=\"out\"></div>"
         "<div id=\"gone\"></div></div></div>");

    AssertResult result = run(make(AssertKind::WithinParent, "#fits"));
    EXPECT_TRUE(result.passed) << describe(result);
    EXPECT_TRUE(run(make(AssertKind::WithinParent, "#inner")).passed);
    EXPECT_TRUE(run(make(AssertKind::WithinParent, "#gone")).passed) << "hidden elements are skipped";

    result = run(make(AssertKind::WithinParent, "#wide"));
    ASSERT_EQ(result.failures.size(), 1u) << describe(result);
    EXPECT_EQ(result.failures[0].message, "extends past the padding box of #inner: 30px right, 20px bottom");
    EXPECT_EQ(result.failures[0].otherRef, "#inner");
    EXPECT_RECT(result.failures[0].otherRect, 105, 105, 200, 100);
    EXPECT_RECT(result.failures[0].rect, 255, 105, 80, 120);

    // container: the closest ancestor matching it
    AssertSpec spec = make(AssertKind::WithinParent, "#wide");
    spec.container = ".outer";
    EXPECT_TRUE(run(spec).passed) << describe(run(spec));
    spec.selector = "#out";
    result = run(spec);
    ASSERT_EQ(result.failures.size(), 1u) << describe(result);
    EXPECT_EQ(result.failures[0].message, "extends past the padding box of #outer: 20px right");
    EXPECT_RECT(result.failures[0].otherRect, 105, 105, 420, 320);
    spec.container = ".nothing";
    result = run(spec);
    ASSERT_EQ(result.failures.size(), 1u);
    EXPECT_EQ(result.failures[0].message, "no ancestor matches \".nothing\"");
    spec.container = "a[[";
    EXPECT_FALSE(run(spec).error.empty());
    spec.container = "";
    spec.tolerance = 300;
    EXPECT_TRUE(run(spec).passed);
}

TEST_F(AssertionsTest, WithinViewportAndOtherViewports){
    load("viewport", std::string(kLayoutCss) +
         "#in { left: 1000px; top: 600px; }\n"
         "#out { left: 1200px; top: 700px; }\n"
         "#hidden { left: 2000px; visibility: hidden; }\n",
         "<button class=\"btn\" id=\"in\">in</button><button class=\"btn\" id=\"out\">out</button><button class=\"btn\" id=\"hidden\">h</button>");
    EXPECT_TRUE(run(make(AssertKind::WithinViewport, "#in")).passed);
    EXPECT_TRUE(run(make(AssertKind::WithinViewport, "#hidden")).passed) << "hidden elements are skipped";
    AssertResult result = run(make(AssertKind::WithinViewport, "#out"));
    ASSERT_EQ(result.failures.size(), 1u) << describe(result);
    EXPECT_EQ(result.failures[0].message, "extends past the 1280x720 viewport: 40px right, 20px bottom");
    EXPECT_RECT(result.failures[0].rect, 1200, 700, 120, 40);

    // the same check at another viewport (assert_ui viewports)
    std::string error;
    ASSERT_TRUE(HtmlUIInspector::withViewport(scene(), 800, 600, [&](){
        const AssertResult small = run(make(AssertKind::WithinViewport, "#in"));
        ASSERT_EQ(small.failures.size(), 1u) << describe(small);
        EXPECT_EQ(small.failures[0].message, "extends past the 800x600 viewport: 320px right, 40px bottom");
    }, error)) << error;
    EXPECT_TRUE(run(make(AssertKind::WithinViewport, "#in")).passed);
}

TEST_F(AssertionsTest, Contrast){
    load("contrast", std::string(kLayoutCss) +
         "body { background: #ffffff; }\n"
         "#black { left: 0; top: 0; background: #000; color: #fff; }\n"
         "#gray { left: 200px; top: 0; background: #888888; color: #777777; }\n"
         ".mid { position: absolute; color: #8a8a8a; width: 300px; }\n"
         "#large { left: 0; top: 100px; font-size: 24px; }\n"
         "#small { left: 0; top: 150px; font-size: 16px; }\n"
         "#bold { left: 0; top: 200px; font-size: 19px; font-weight: bold; }\n"
         "#normal { left: 0; top: 250px; font-size: 19px; }\n"
         "#gradient { left: 400px; top: 0; background: linear-gradient(#000, #fff); color: #fff; }\n"
         "#alpha-box { left: 600px; top: 0; background: #000; }\n"
         "#alpha { color: rgba(255, 255, 255, 0.3); }\n"
         "#group { left: 800px; top: 0; background: #000; }\n"
         "#halved { opacity: 0.5; background: #fff; color: #000; }\n",
         "<button class=\"btn\" id=\"black\">Play</button>\n"
         "<button class=\"btn\" id=\"gray\">Gray</button>\n"
         "<div class=\"mid\" id=\"large\">Large text</div><div class=\"mid\" id=\"small\">Small text</div>\n"
         "<div class=\"mid\" id=\"bold\">Bold text</div><div class=\"mid\" id=\"normal\">Normal text</div>\n"
         "<button class=\"btn\" id=\"gradient\">Fade</button>\n"
         "<div class=\"btn\" id=\"alpha-box\"><span id=\"alpha\">Faint</span></div>\n"
         "<div class=\"btn\" id=\"group\"><div id=\"halved\">Half</div></div>\n");

    AssertResult result = run(make(AssertKind::Contrast, "#black"));
    EXPECT_TRUE(result.passed) << describe(result);
    AssertSpec spec = make(AssertKind::Contrast, "#black");
    spec.minRatio = 21;
    EXPECT_TRUE(run(spec).passed) << "white on black is exactly 21:1: " << describe(run(spec));

    result = run(make(AssertKind::Contrast, "#gray"));
    ASSERT_EQ(result.failures.size(), 1u) << describe(result);
    EXPECT_EQ(result.failures[0].message, "contrast " + fmt(ratio(0x77 / 255.0, 0x88 / 255.0)) + ":1 (#777777 on #888888) is below 4.5:1");
    EXPECT_EQ(result.failures[0].ref, "#gray");

    // large text: 3:1 (24px, or 18.66px bold)
    const std::string mid = fmt(ratio(0x8a / 255.0, 1.0));
    EXPECT_TRUE(run(make(AssertKind::Contrast, "#large")).passed) << mid;
    EXPECT_TRUE(run(make(AssertKind::Contrast, "#bold")).passed) << mid;
    result = run(make(AssertKind::Contrast, "#small"));
    ASSERT_EQ(result.failures.size(), 1u);
    EXPECT_EQ(result.failures[0].message, "contrast " + mid + ":1 (#8a8a8a on #ffffff) is below 4.5:1");
    EXPECT_EQ(run(make(AssertKind::Contrast, "#normal")).failures.size(), 1u);
    spec = make(AssertKind::Contrast, "#large");
    spec.minRatio = 7;
    result = run(spec);
    ASSERT_EQ(result.failures.size(), 1u);
    EXPECT_EQ(result.failures[0].message, "contrast " + mid + ":1 (#8a8a8a on #ffffff) is below 7:1");

    // an image background before an opaque layer: inconclusive, not a failure
    result = run(make(AssertKind::Contrast, "#gradient"));
    EXPECT_TRUE(result.passed) << describe(result);
    EXPECT_EQ(result.inconclusive, (std::vector<std::string>{"#gradient"}));

    // a translucent colour is composited
    result = run(make(AssertKind::Contrast, "#alpha"));
    ASSERT_EQ(result.failures.size(), 1u) << describe(result);
    const double faint = 0.3 * 255.0;
    EXPECT_NEAR(numbers(result.failures[0].message).at(0), ratio(std::round(faint) / 255.0, 0.0), 0.02) << result.failures[0].message;
    EXPECT_NE(result.failures[0].message.find(" on #000000)"), std::string::npos) << result.failures[0].message;
    // group opacity: the text is drawn over its own background first, then the group at 0.5
    result = run(make(AssertKind::Contrast, "#halved"));
    EXPECT_TRUE(result.passed) << describe(result);

    // a match without text: its own colour
    result = run(make(AssertKind::Contrast, "#alpha-box"));
    EXPECT_FALSE(result.passed) << "checks the span inside";
}

TEST_F(AssertionsTest, ContrastOverATransparentRoot){
    load("transparent", "* { margin: 0; }\n#light { color: #ffffff; }\n#dark { color: #333333; }\n",
         "<p id=\"light\">Light</p><p id=\"dark\">Dark</p>");
    AssertResult result = run(make(AssertKind::Contrast, "p"));
    EXPECT_TRUE(result.passed) << describe(result);
    EXPECT_EQ(result.inconclusive, (std::vector<std::string>{"#light", "#dark"}));

    AssertSpec spec = make(AssertKind::Contrast, "p");
    spec.hasBackground = true;
    spec.background[0] = spec.background[1] = spec.background[2] = 0.0f;
    result = run(spec);
    EXPECT_TRUE(result.inconclusive.empty());
    ASSERT_EQ(result.failures.size(), 1u) << describe(result);
    EXPECT_EQ(result.failures[0].ref, "#dark");
    EXPECT_EQ(result.failures[0].message, "contrast " + fmt(ratio(0x33 / 255.0, 0.0)) + ":1 (#333333 on #000000) is below 4.5:1");
}

TEST_F(AssertionsTest, MinSize){
    load("size", std::string(kLayoutCss) +
         ".icon { position: absolute; width: 40px; height: 40px; }\n"
         "#big { left: 100px; width: 48px; height: 48px; }\n"
         "#gone { left: 200px; display: none; }\n",
         "<button class=\"icon\" id=\"small\">x</button><button class=\"icon\" id=\"big\">y</button><button class=\"icon\" id=\"gone\">z</button>");
    AssertResult result = run(make(AssertKind::MinSize, ".icon"));
    EXPECT_EQ(result.matched, 3u);
    ASSERT_EQ(result.failures.size(), 1u) << describe(result);
    EXPECT_EQ(result.failures[0].ref, "#small");
    EXPECT_EQ(result.failures[0].message, "is 40x40px, smaller than 44x44px");
    EXPECT_TRUE(run(make(AssertKind::MinSize, "#big")).passed);
    AssertSpec spec = make(AssertKind::MinSize, ".icon");
    spec.minWidth = 30;
    spec.minHeight = 30;
    EXPECT_TRUE(run(spec).passed);
    spec.minWidth = 41;
    EXPECT_EQ(run(spec).failures.at(0).message, "is 40x40px, smaller than 41x30px");
}
