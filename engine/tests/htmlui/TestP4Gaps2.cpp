// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// p4-gaps2 (phase-4 spec 4.3.3): form controls with `appearance: none` and buttons with
// `display: inline` as Chromium has them, through the whole pipeline (HTML, the user-agent
// sheet and its companions, the fork). Their browser-parity fixtures are
// paint/appearance-none-controls, layout/checkbox-content-size and layout/button-display-inline.
//
// - AppearanceNone.ControlsKeepTheirOwnHtmlCssLook: html.css's look of each control without its
//   theme: a select keeps its 1px #767676 border on ButtonFace without the menu list's inner
//   padding or arrow, a button gets `padding: 1px 6px; border: 2px outset ButtonBorder`, a
//   progress bar a gray bar and a green value, while a checkbox loses its frame;
// - AppearanceNone.CheckboxIsSizedByItsContent: a checkbox or radio without its theme is an
//   inline-block sized by its ::before/::after, padding and border; with it an auto size is 13px;
// - ButtonDisplay.InlineIsInlineBlock: the engine's <button> with display: inline.

#include "harness/RecordingRenderInterface.h"

#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/compat/VirtualFiles.h"

#include "HtmlUI.h"
#include "RmlSystem.h"

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

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
        std::vector<std::string> errors;   // RmlUi's errors and warnings while the page lives

        Page(const std::string& name, const std::string& style, const std::string& body): ok(HtmlUI::acquire()), path("@htmlui/test/p4-gaps2/" + name + ".html"){
            if (!ok)
                return;
            HtmlUI::getSystemInterface().setLogListener([this](Rml::Log::Type type, const std::string& message){
                if (type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT || type == Rml::Log::LT_WARNING)
                    errors.push_back(message);
            });
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
            if (ok){
                HtmlUI::getSystemInterface().setLogListener(nullptr);
                HtmlUI::release();
            }
        }

        Rml::Element* byId(const char* id) const{
            return document ? document->GetElementById(id) : nullptr;
        }
    };

    // a widget part of a control (selectvalue, selectarrow, fill), outside the DOM
    Rml::Element* part(Rml::Element* control, const std::string& tag){
        for (int i = 0; control && i < control->GetNumChildren(true); i++){
            Rml::Element* child = control->GetChild(i);
            if (child->GetTagName() == tag)
                return child;
            if (Rml::Element* nested = part(child, tag))
                return nested;
        }
        return nullptr;
    }

    Rml::Vector2f borderSize(Rml::Element* element){
        return element->GetBox().GetSize(Rml::BoxArea::Border);
    }

}

TEST(AppearanceNone, ControlsKeepTheirOwnHtmlCssLook){
    Page page("controls", "select, button, input, progress { appearance: none; } .auto { appearance: auto; }",
              "<select id='s'><option>a</option></select><select id='sa' class='auto'><option>a</option></select>"
              "<button id='b'>b</button><input id='i' type='submit' value='i'><button id='ba' class='auto'>b</button>"
              "<input id='c' type='checkbox'><input id='ca' type='checkbox' class='auto'><input id='r' type='radio' checked>"
              "<progress id='p' value='0.5'></progress><progress id='pa' class='auto' value='0.5'></progress>");
    ASSERT_TRUE(page.document);
    const Rml::Colourb gray(118, 118, 118, 255), black(0, 0, 0, 255), buttonFace(239, 239, 239, 255), white(255, 255, 255, 255);

    // select: html.css's 1px #767676 border on ButtonFace, no inner padding and no arrow (the
    // checkbox's companions cleared its border before)
    const Rml::ComputedValues& select = page.byId("s")->GetComputedValues();
    EXPECT_FLOAT_EQ(select.border_top_width(), 1.0f);
    EXPECT_EQ(select.border_top_style(), Rml::Style::BorderStyle::Solid);
    EXPECT_EQ(select.border_top_color(), gray);
    EXPECT_EQ(select.border_left_color(), gray);
    EXPECT_EQ(select.background_color(), buttonFace);
    Rml::Element* value = part(page.byId("s"), "selectvalue");
    Rml::Element* arrow = part(page.byId("s"), "selectarrow");
    ASSERT_TRUE(value && arrow);
    EXPECT_FLOAT_EQ(value->GetComputedValues().padding_left().value, 0.0f);
    EXPECT_FLOAT_EQ(value->GetComputedValues().padding_top().value, 0.0f);
    EXPECT_FLOAT_EQ(arrow->GetComputedValues().width().value, 0.0f);
    // with its look: the theme's white box, inner padding and arrow
    EXPECT_EQ(page.byId("sa")->GetComputedValues().background_color(), white);
    EXPECT_EQ(page.byId("sa")->GetComputedValues().border_top_color(), gray);
    EXPECT_FLOAT_EQ(part(page.byId("sa"), "selectvalue")->GetComputedValues().padding_left().value, 4.0f);
    EXPECT_FLOAT_EQ(part(page.byId("sa"), "selectvalue")->GetComputedValues().padding_top().value, 1.0f);
    EXPECT_FLOAT_EQ(part(page.byId("sa"), "selectarrow")->GetComputedValues().width().value, 16.0f);

    // buttons: padding 1px 6px, border 2px outset ButtonBorder (black)
    for (const char* id : {"b", "i"}){
        SCOPED_TRACE(id);
        const Rml::ComputedValues& button = page.byId(id)->GetComputedValues();
        EXPECT_FLOAT_EQ(button.border_top_width(), 2.0f);
        EXPECT_EQ(button.border_top_style(), Rml::Style::BorderStyle::Outset);
        EXPECT_EQ(button.border_top_color(), black);
        EXPECT_EQ(button.background_color(), buttonFace);
        EXPECT_FLOAT_EQ(page.byId(id)->GetBox().GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Top), 1.0f);
        EXPECT_FLOAT_EQ(page.byId(id)->GetBox().GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Left), 6.0f);
    }
    EXPECT_FLOAT_EQ(page.byId("ba")->GetComputedValues().border_top_width(), 1.0f);
    EXPECT_EQ(page.byId("ba")->GetComputedValues().border_top_color(), gray);

    // the checkbox still loses its frame and fill
    EXPECT_FLOAT_EQ(page.byId("c")->GetComputedValues().border_top_width(), 0.0f);
    EXPECT_EQ(page.byId("c")->GetComputedValues().background_color().alpha, 0);
    EXPECT_FLOAT_EQ(page.byId("ca")->GetComputedValues().border_top_width(), 1.0f);

    // progress: the whole box gray, the value green, no rounding
    const Rml::ComputedValues& progress = page.byId("p")->GetComputedValues();
    EXPECT_EQ(progress.background_color(), Rml::Colourb(128, 128, 128, 255));
    EXPECT_FLOAT_EQ(progress.border_top_width(), 0.0f);
    Rml::Element* fill = part(page.byId("p"), "fill");
    ASSERT_TRUE(fill);
    EXPECT_EQ(fill->GetComputedValues().background_color(), Rml::Colourb(0, 128, 0, 255));
    EXPECT_EQ(page.byId("pa")->GetComputedValues().background_color().alpha, 0);

    // the shadows they clear (the progress track, the checked radio's ring) are valid list entries
    EXPECT_TRUE(page.errors.empty()) << page.errors.front();
}

TEST(AppearanceNone, CheckboxIsSizedByItsContent){
    Page page("checkbox", ".n { appearance: none; } .b::before { content: ''; display: block; width: 12px; height: 8px; } "
                          ".pb { padding: 3px 4px; border: 2px solid #556677; } .e::before { content: ''; }",
              "<input id='empty' type='checkbox' class='n'><input id='radio' type='radio' class='n b'>"
              "<input id='string' type='checkbox' class='n e'><input id='edges' type='checkbox' class='n pb'>"
              "<input id='both' type='radio' class='n b pb'><input id='content-box' type='checkbox' class='n pb' style='box-sizing: content-box'>"
              "<input id='width' type='checkbox' class='n b' style='width: 30px'>"
              "<input id='auto' type='checkbox' style='width: auto; height: auto'><input id='auto-radio' type='radio' style='width: auto; height: auto'>"
              "<input id='wide' type='checkbox' style='width: 20px; height: auto'><input id='tall' type='radio' style='width: auto; height: 20px'>");
    ASSERT_TRUE(page.document);
    EXPECT_EQ(borderSize(page.byId("empty")), Rml::Vector2f(0, 0));
    EXPECT_EQ(borderSize(page.byId("radio")), Rml::Vector2f(12, 8));
    EXPECT_EQ(borderSize(page.byId("string")), Rml::Vector2f(0, 0));
    EXPECT_EQ(borderSize(page.byId("edges")), Rml::Vector2f(12, 10));
    EXPECT_EQ(borderSize(page.byId("both")), Rml::Vector2f(24, 18));
    EXPECT_EQ(borderSize(page.byId("content-box")), Rml::Vector2f(12, 10));
    EXPECT_EQ(borderSize(page.byId("width")), Rml::Vector2f(30, 8));
    // an empty ::before gives no baseline: the bottom margin edge, as an empty inline-block (fork P4-G11)
    EXPECT_FLOAT_EQ(page.byId("string")->GetBaseline(), 0.0f);

    // with the theme an auto width or height is 13px (the border box), each on its own
    EXPECT_EQ(borderSize(page.byId("auto")), Rml::Vector2f(13, 13));
    EXPECT_EQ(borderSize(page.byId("auto-radio")), Rml::Vector2f(13, 13));
    EXPECT_EQ(borderSize(page.byId("wide")), Rml::Vector2f(20, 13));
    EXPECT_EQ(borderSize(page.byId("tall")), Rml::Vector2f(13, 20));

    // toggling the look relays the control out
    page.byId("auto")->SetClass("n", true);
    page.context->Update();
    EXPECT_EQ(borderSize(page.byId("auto")), Rml::Vector2f(0, 0));
    page.byId("auto")->SetClass("n", false);
    page.context->Update();
    EXPECT_EQ(borderSize(page.byId("auto")), Rml::Vector2f(13, 13));
}

TEST(ButtonDisplay, InlineIsInlineBlock){
    Page page("button-inline", "button, input { display: inline; box-sizing: border-box; width: 60px; height: 30px; }",
              "<p>x <button id='b'>b</button> x <input id='i' type='button' value='i'> x <input id='t' value='t'></p>");
    ASSERT_TRUE(page.document);
    EXPECT_EQ(page.byId("b")->GetDisplay(), Rml::Style::Display::InlineBlock);
    EXPECT_EQ(page.byId("i")->GetDisplay(), Rml::Style::Display::InlineBlock);
    EXPECT_EQ(page.byId("t")->GetDisplay(), Rml::Style::Display::Inline);
    EXPECT_EQ(borderSize(page.byId("b")), Rml::Vector2f(60, 30));
    EXPECT_EQ(borderSize(page.byId("i")), Rml::Vector2f(60, 30));
}
