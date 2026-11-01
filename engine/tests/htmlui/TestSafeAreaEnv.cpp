// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Safe area in HTML UI (docs/superpowers/specs/2026-10-31-safe-area-design.md §6.4, §9):
// env(safe-area-inset-*) is compiled to var(--jasper-safe-area-inset-*, <fallback or 0px>), a
// variable the compat layer never folds (calc() keeps it; a value folded statically reads 0px with
// an Info), and UIDocumentSystem sets those variables on each document from Screen, so a change of
// the safe area lays the document out again without a reload.

#include "InspectorTestCommon.h"
#include "TestDocumentCommon.h"

#include "Engine.h"
#include "Screen.h"
#include "htmlui/compat/ForkFeatures.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/css/CssRewriter.h"
#include "object/ui/UIElementInternal.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::htmlui::compat;

namespace {

    struct Rewritten {
        std::string rcss;
        std::vector<Diagnostic> diagnostics;

        // the value written for `property` in the first rule that has it ("" when none)
        std::string value(const std::string& property) const {
            const std::string key = "\n" + property + ": ";
            const size_t at = rcss.find(key);
            if (at == std::string::npos)
                return std::string();
            const size_t begin = at + key.size();
            const size_t end = rcss.find(";\n", begin);
            return rcss.substr(begin, end - begin);
        }
        bool reported(const std::string& text) const {
            for (const Diagnostic& d : diagnostics){
                if (d.message.find(text) != std::string::npos)
                    return true;
            }
            return false;
        }
        size_t warnings() const {
            size_t count = 0;
            for (const Diagnostic& d : diagnostics){
                if (d.severity != Severity::Info)
                    count++;
            }
            return count;
        }
    };

    Rewritten rewrite(const std::string& css, const ForkFeatures& fork = ForkFeatures::present()){
        SourceFiles files;
        const uint32_t file = files.add("ui/safe.css", css);
        DiagnosticSink sink(files);
        CompatOptions options;
        options.fork = fork;
        RewriteContext context;
        context.files.read = [](const std::string&, std::string&){ return false; };
        context.files.exists = [](const std::string&){ return true; };
        CssRewriter rewriter(options, files, sink, context);
        StylesheetInput input;
        input.file = file;
        input.text = files.text(file);
        const RewriteResult result = rewriter.rewrite(Span<const StylesheetInput>(&input, 1));
        Rewritten out;
        for (const StylesheetOutput& sheet : result.sheets)
            out.rcss += sheet.rcss;
        out.diagnostics = sink.diagnostics();
        return out;
    }

    bool contains(const std::string& text, const std::string& part){
        return text.find(part) != std::string::npos;
    }

}

// ---- the normalizer ---------------------------------------------------------------------------

TEST(SafeAreaEnv, InsetBecomesTheRuntimeVariable){
    Rewritten r = rewrite(".hud { padding-top: env(safe-area-inset-top); }");
    EXPECT_EQ(r.value("padding-top"), "var(--jasper-safe-area-inset-top, 0px)") << r.rcss;
    EXPECT_EQ(r.warnings(), 0u) << r.rcss;

    // the fallback is the variable's
    r = rewrite(".hud { padding-bottom: env(safe-area-inset-bottom, 12px); }");
    EXPECT_EQ(r.value("padding-bottom"), "var(--jasper-safe-area-inset-bottom, 12px)") << r.rcss;

    // Safari's max-inset names: the same variables
    r = rewrite(".hud { padding-left: env(safe-area-max-inset-left); }");
    EXPECT_EQ(r.value("padding-left"), "var(--jasper-safe-area-inset-left, 0px)") << r.rcss;

    // every edge, case-insensitive
    r = rewrite(".hud { margin-right: ENV(Safe-Area-Inset-Right); }");
    EXPECT_EQ(r.value("margin-right"), "var(--jasper-safe-area-inset-right, 0px)") << r.rcss;
}

TEST(SafeAreaEnv, KeptInsideCalcAndMinMax){
    Rewritten r = rewrite(".bar { padding-bottom: calc(16px + env(safe-area-inset-bottom)); }");
    const std::string calc = r.value("padding-bottom");
    EXPECT_TRUE(contains(calc, "calc(")) << r.rcss;
    EXPECT_TRUE(contains(calc, "var(--jasper-safe-area-inset-bottom, 0px)")) << r.rcss;
    EXPECT_EQ(r.warnings(), 0u) << r.rcss;

    r = rewrite(".bar { bottom: max(12px, env(safe-area-inset-bottom)); }");
    EXPECT_TRUE(contains(r.value("bottom"), "var(--jasper-safe-area-inset-bottom, 0px)")) << r.rcss;
    EXPECT_EQ(r.warnings(), 0u) << r.rcss;
}

TEST(SafeAreaEnv, InShorthands){
    Rewritten r = rewrite(".hud { padding: env(safe-area-inset-top) env(safe-area-inset-right) 8px env(safe-area-inset-left); }");
    EXPECT_TRUE(contains(r.rcss, "var(--jasper-safe-area-inset-top, 0px)")) << r.rcss;
    EXPECT_TRUE(contains(r.rcss, "var(--jasper-safe-area-inset-left, 0px)")) << r.rcss;
    EXPECT_EQ(r.warnings(), 0u) << r.rcss;
}

TEST(SafeAreaEnv, FoldedStaticallyItIsZero){
    // without the fork's calc() the expression is folded at load: the inset counts as 0
    Rewritten r = rewrite(".bar { padding-bottom: calc(16px + env(safe-area-inset-bottom)); }", ForkFeatures::none());
    EXPECT_EQ(r.value("padding-bottom"), "16px") << r.rcss;
    EXPECT_TRUE(r.reported("env(safe-area-inset-*) is 0 here")) << r.rcss;
}

TEST(SafeAreaEnv, TokenRewrites){
    // transform, box-shadow and gradients are rewritten for RmlUi: with the fork's features they are
    // written for the fork with the live variable in them; without (values read token by token, compat
    // decorator arguments) the inset folds to 0 with an Info. Never a dropped declaration
    for (const char* css : {".fab { transform: translateY(env(safe-area-inset-bottom)) rotate(10deg); }",
                            ".card { box-shadow: 0 env(safe-area-inset-top, 4px) 8px #000; }",
                            ".bar { background: linear-gradient(#f00 env(safe-area-inset-top), #00f); }"}){
        const Rewritten live = rewrite(css);
        EXPECT_EQ(live.warnings(), 0u) << css << "\n" << live.rcss;
        EXPECT_TRUE(contains(live.rcss, "var(--jasper-safe-area-inset-")) << css << "\n" << live.rcss;
        const Rewritten folded = rewrite(css, ForkFeatures::none());
        EXPECT_EQ(folded.warnings(), 0u) << css << "\n" << folded.rcss;
        EXPECT_TRUE(contains(folded.rcss, "var(--jasper-safe-area-inset-") || folded.reported("env(safe-area-inset-*) is 0 here"))
            << "kept live or folded with an Info: " << css << "\n" << folded.rcss;
    }
}

TEST(SafeAreaEnv, OtherEnvironmentVariablesAsBefore){
    Rewritten r = rewrite(".bar { padding-top: env(titlebar-area-height, 3px); }");
    EXPECT_EQ(r.value("padding-top"), "3px") << r.rcss;
    r = rewrite(".bar { padding-top: env(titlebar-area-height); }");
    EXPECT_EQ(r.value("padding-top"), "") << r.rcss;
    // an edge that does not exist
    r = rewrite(".bar { padding-top: env(safe-area-inset-middle); }");
    EXPECT_EQ(r.value("padding-top"), "0px") << r.rcss;
}

// ---- live values on documents -------------------------------------------------------------------

namespace {

    class SafeAreaDocumentTest : public InspectorTestBase {
    protected:
        void TearDown() override{
            Screen::setPlatformSafeArea(EdgeInsets());
            Screen::resetSafeAreaState();
            InspectorTestBase::TearDown();
        }

        float offsetTop(UIDocument& document, const std::string& id){
            Rml::Element* element = UIElementAccess::get(document.getElementById(id));
            return element ? element->GetAbsoluteOffset(Rml::BoxArea::Border).y : -1.0f;
        }
    };

}

TEST_F(SafeAreaDocumentTest, PaddingFollowsTheSafeAreaWithoutAReload){
    // 1280x720 pixels, a 1280x720 canvas: a pixel is a canvas unit is a CSS px
    Screen::setPlatformSafeArea(EdgeInsets(0, 47, 0, 0));
    page("safe", "html, body { margin: 0; }\n#hud { padding-top: env(safe-area-inset-top); }\n#item { height: 10px; }\n",
         "<div id=\"hud\"><div id=\"item\"></div></div>");
    std::unique_ptr<SceneFixture> fixture(new SceneFixture());
    UIDocument document = fixture->addDocument("ui/safe.html");
    ASSERT_TRUE(fixture->waitUntilLoaded());
    Rml::ElementDocument* rmlDocument = documentSystem(fixture->getScene()).getDocument(document.getEntity());
    ASSERT_NE(rmlDocument, nullptr);
    EXPECT_NEAR(offsetTop(document, "item"), 47.0f, 0.01f);

    // a rotation, a banner: laid out again at the next update, the same document
    Screen::setPlatformSafeArea(EdgeInsets(0, 20, 0, 0));
    Screen::setExtraInsets(EdgeInsets(0, 10, 0, 0));
    ASSERT_TRUE(fixture->waitUntilLoaded());
    EXPECT_NEAR(offsetTop(document, "item"), 30.0f, 0.01f);
    EXPECT_EQ(documentSystem(fixture->getScene()).getDocument(document.getEntity()), rmlDocument) << "no reload";

    // the device-only variables leave the extras out
    const Rml::Property* device = rmlDocument->GetProperty("--jasper-device-safe-area-inset-top");
    ASSERT_NE(device, nullptr);
    EXPECT_EQ(device->ToString(), "20px");
    fixture.reset();
}

TEST_F(SafeAreaDocumentTest, ShorthandsLayOutWithTheInsets){
    Screen::setPlatformSafeArea(EdgeInsets(30, 47, 0, 0));
    page("short", "html, body { margin: 0; }\n#hud { padding: env(safe-area-inset-top) 0 0 env(safe-area-inset-left); }\n"
                  "#item { height: 10px; width: 10px; }\n#box { margin: env(safe-area-inset-top) 0 0; height: 5px; }\n",
         "<div id=\"hud\"><div id=\"item\"></div></div><div id=\"box\"></div>");
    std::unique_ptr<SceneFixture> fixture(new SceneFixture());
    UIDocument document = fixture->addDocument("ui/short.html");
    ASSERT_TRUE(fixture->waitUntilLoaded());
    Rml::Element* item = UIElementAccess::get(document.getElementById("item"));
    ASSERT_NE(item, nullptr);
    EXPECT_NEAR(item->GetAbsoluteOffset(Rml::BoxArea::Border).y, 47.0f, 0.01f);
    EXPECT_NEAR(item->GetAbsoluteOffset(Rml::BoxArea::Border).x, 30.0f, 0.01f);
    // the margin of the next block: below the hud (47 + 10) and its own 47
    EXPECT_NEAR(offsetTop(document, "box"), 57.0f + 47.0f, 0.01f);
    Screen::setPlatformSafeArea(EdgeInsets(0, 20, 0, 0));
    ASSERT_TRUE(fixture->waitUntilLoaded());
    EXPECT_NEAR(item->GetAbsoluteOffset(Rml::BoxArea::Border).y, 20.0f, 0.01f);
    EXPECT_NEAR(item->GetAbsoluteOffset(Rml::BoxArea::Border).x, 0.0f, 0.01f);
    fixture.reset();
}

TEST_F(SafeAreaDocumentTest, InlineStyleAndLaterDocuments){
    Screen::setPlatformSafeArea(EdgeInsets(0, 0, 0, 34));
    page("inline", "html, body { margin: 0; }\n",
         "<div id=\"bar\" style=\"position: absolute; left: 0; bottom: env(safe-area-inset-bottom); height: 10px; width: 10px;\"></div>");
    std::unique_ptr<SceneFixture> fixture(new SceneFixture());
    UIDocument document = fixture->addDocument("ui/inline.html");
    ASSERT_TRUE(fixture->waitUntilLoaded());
    EXPECT_NEAR(offsetTop(document, "bar"), 720.0f - 34.0f - 10.0f, 0.01f);
    fixture.reset();
}
