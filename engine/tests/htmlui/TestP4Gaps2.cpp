// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// p4-gaps2 (phase-4 spec 4.3.3): form controls with `appearance: none` and buttons with
// `display: inline` as Chromium has them, through the whole pipeline (HTML, the user-agent
// sheet and its companions, the fork). Their browser-parity fixtures are
// paint/appearance-none-controls, layout/checkbox-content-size and layout/button-display-inline.
//
// CSSOM values are checked through the public UIElement API; rendered controls are covered
// by the Chromium reference fixtures in BrowserParityTest.

#include "harness/RecordingRenderInterface.h"

#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/compat/VirtualFiles.h"
#include "object/ui/UIElement.h"
#include "object/ui/UIElementInternal.h"

#include "HtmlUI.h"

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

#include <string>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::htmlui::compat;

namespace {

    struct Page {
        bool ok;
        RecordingRenderInterface recording;
        Rml::Context* context = nullptr;
        Rml::ElementDocument* document = nullptr;
        std::string path;

        Page(const std::string& name, const std::string& style, const std::string& body): ok(HtmlUI::acquire()), path("@htmlui/test/p4-gaps2/" + name + ".html"){
            if (!ok)
                return;
            context = Rml::CreateContext("p4-gaps2", Rml::Vector2i(800, 600), &recording);
            VirtualFiles::add(path, "<!DOCTYPE html><html><head><style>body { margin: 0; font-size: 10px; } " + style +
                                    "</style></head><body>" + body + "</body></html>");
            CompatOptions options;
            options.viewport = CompatSize{800, 600};
            options.fork = ForkFeatures::present();
            document = DocumentLoader::load(context, path, options);
            if (document){
                document->Show();
                context->Update();
            }
        }
        ~Page(){
            if (document)
                document->Close();
            if (context){
                context->Update();
                Rml::RemoveContext("p4-gaps2");
            }
            Rml::ReleaseTextures(&recording);
            Rml::ReleaseRenderManagers();
            VirtualFiles::remove(path);
            DocumentLoader::clearCache();
            if (ok)
                HtmlUI::release();
        }

        Rml::Element* byId(const char* id) const{
            return document ? document->GetElementById(id) : nullptr;
        }
    };

    Rml::Vector2f borderSize(Rml::Element* element){
        return element->GetBox().GetSize(Rml::BoxArea::Border);
    }

}


// The themed CSSOM reports zero border/padding; disabling appearance restores author values.
TEST(AppearanceNone, ThemedCheckboxIsACenteredSquare){
    Page page("themed", "input { margin: 0; } #c { width: 30px; height: 20px; border: 3px solid blue; padding: 4px; }",
              "<input id='c' type='checkbox' checked>");
    ASSERT_TRUE(page.document);
    UIElement checkbox = UIElementAccess::wrap(page.byId("c"));
    EXPECT_EQ(checkbox.getStyle("border-top-width"), "0px");
    EXPECT_EQ(checkbox.getStyle("padding-left"), "0px");
    EXPECT_EQ(checkbox.getBoundingClientRect().width, 30.0f);
    EXPECT_EQ(checkbox.getBoundingClientRect().height, 20.0f);

    // Turning the theme off exposes the author's border and padding through the public CSSOM.
    checkbox.setStyle("appearance", "none");
    page.context->Update();
    EXPECT_EQ(checkbox.getStyle("border-top-width"), "3px");
    EXPECT_EQ(checkbox.getStyle("padding-left"), "4px");
}

// A button's computed display as Chromium reports it: with its look the theme's (inline-block for
// inline, block for table and list-item); with appearance: none the declared one (laid out as an
// inline-block or a block all the same); a block-level button is as wide as its content
TEST(ButtonDisplay, ComputedAsChromiumReportsIt){
    Page page("button-display", "button { margin: 0; } .n { appearance: none; }",
              "<div style='width: 300px'><button id='a' style='display: inline'>b</button><button id='b' class='n' style='display: inline'>b</button>"
              "<button id='c' style='display: table'>b</button><button id='d' class='n' style='display: table-cell'>b</button>"
              "<button id='e' style='display: block'>b</button><button id='f' style='display: block; margin: 0 auto'>b</button></div>"
              "<div style='display: flex; flex-direction: column; width: 300px'><button id='g'>b</button></div>");
    ASSERT_TRUE(page.document);
    const auto reported = [&](const char* id){
        return UIElementAccess::wrap(page.byId(id)).getStyle("display");
    };
    EXPECT_EQ(reported("a"), "inline-block");
    EXPECT_EQ(reported("b"), "inline");
    EXPECT_EQ(reported("c"), "block");
    EXPECT_EQ(reported("d"), "table-cell");
    EXPECT_EQ(page.byId("b")->GetDisplay(), Rml::Style::Display::InlineBlock);
    EXPECT_EQ(page.byId("d")->GetDisplay(), Rml::Style::Display::Block);
    // fit-content: the block-level buttons are as wide as their content, centered by auto margins
    const float width = borderSize(page.byId("e")).x;
    EXPECT_LT(width, 100.0f);
    EXPECT_FLOAT_EQ(borderSize(page.byId("c")).x, width);
    EXPECT_FLOAT_EQ(page.byId("f")->GetBox().GetEdge(Rml::BoxArea::Margin, Rml::BoxEdge::Left), (300.0f - width) * 0.5f);
    // a flex item is stretched as any
    EXPECT_FLOAT_EQ(borderSize(page.byId("g")).x, 300.0f);
}
