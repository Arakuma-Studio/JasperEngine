// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Root font sizes through the compatibility layer (phase 3 p3-base, phase-3 spec 3.1.1): the
// HTML <html> is RmlUi's document element, so `rem` is relative to it; root `%` and `em` font
// sizes resolve against the initial size (compat converts the stylesheet's to px, the fork
// resolves inline ones, FORK.md P3-3). html { font-size: 62.5% } body { font-size: 1.6rem }
// gives the body 16px, in every fork-feature state.

#include "harness/RecordingRenderInterface.h"

#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/compat/VirtualFiles.h"

#include "HtmlUI.h"

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::htmlui::compat;

namespace {

    struct RmlUiSession {
        bool ok;
        RecordingRenderInterface recording;
        Rml::Context* context = nullptr;
        RmlUiSession(): ok(HtmlUI::acquire()){
            if (ok)
                context = Rml::CreateContext("compat-root-font-size", Rml::Vector2i(1280, 720), &recording);
        }
        ~RmlUiSession(){
            if (context)
                Rml::RemoveContext("compat-root-font-size");
            Rml::ReleaseTextures(&recording);
            Rml::ReleaseRenderManagers();
            if (ok)
                HtmlUI::release();
        }
    };

    float fontSize(Rml::Element* element){
        return element ? element->GetComputedValues().font_size() : -1.0f;
    }

    Rml::Element* body(Rml::ElementDocument* document){
        Rml::ElementList bodies;
        document->GetElementsByTagName(bodies, "htmlbody");
        return bodies.empty() ? nullptr : bodies[0];
    }

}

TEST(CompatRootFontSize, HtmlPercentAndBodyRem){
    const char* path = "@htmlui/test/root-font-size/page.html";
    VirtualFiles::add(path, "<!DOCTYPE html><html><head><style>"
                            "html { font-size: 62.5%; } body { font-size: 1.6rem; } #a { font-size: 2rem; } #b { font-size: 1.5em; }"
                            "</style></head><body><p id=\"a\">a</p><p id=\"b\">b</p></body></html>");
    for (const ForkFeatures& fork : {ForkFeatures::present(), ForkFeatures::none(), ForkFeatures::all()}){
        RmlUiSession rml;
        ASSERT_TRUE(rml.context);
        CompatOptions options;
        options.viewport = CompatSize{1280, 720};
        options.fork = fork;
        std::shared_ptr<const CompatDocument> compiled;
        Rml::ElementDocument* document = DocumentLoader::load(rml.context, path, options, &compiled);
        ASSERT_NE(document, nullptr);
        document->Show();
        rml.context->Update();

        EXPECT_FLOAT_EQ(fontSize(document), 10.0f);
        EXPECT_FLOAT_EQ(fontSize(body(document)), 16.0f);
        EXPECT_FLOAT_EQ(fontSize(document->GetElementById("a")), 20.0f);   // rem: the document element's 10px
        EXPECT_FLOAT_EQ(fontSize(document->GetElementById("b")), 24.0f);   // em: the body's 16px

        // a percentage set on the document element at run time (style attribute, Lua) resolves
        // against the initial size in the fork, as the stylesheet's does in compat
        document->SetProperty("font-size", "50%");
        rml.context->Update();
        EXPECT_FLOAT_EQ(fontSize(document), 8.0f);
        EXPECT_FLOAT_EQ(fontSize(document->GetElementById("a")), 16.0f);
        document->Close();
        rml.context->Update();
    }
    VirtualFiles::remove(path);
    DocumentLoader::clearCache();
}
