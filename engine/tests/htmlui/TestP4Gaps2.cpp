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
// - the coordinator's follow-up (phase-4 spec 4.3.3 G5-G9): AppearanceNone.IsNotInherited,
//   AppearanceNone.ThemedCheckboxIsACenteredSquare (Chromium's theme, its computed values),
//   ButtonDisplay.ComputedAsChromiumReportsIt (display types, fit-content block buttons).

#include "harness/RecordingRenderInterface.h"

#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/compat/VirtualFiles.h"
#include "htmlui/compat/runtime/HtmlFormSupport.h"
#include "htmlui/compat/runtime/HtmlInputElement.h"

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

    // the checkbox loses its theme, and has no frame or fill; with it its box is the theme's
    EXPECT_FALSE(page.byId("c")->HasNativeAppearance());
    EXPECT_FALSE(page.byId("c")->HasThemedBox());
    EXPECT_FLOAT_EQ(page.byId("c")->GetComputedValues().border_top_width(), 0.0f);
    EXPECT_EQ(page.byId("c")->GetComputedValues().background_color().alpha, 0);
    EXPECT_TRUE(page.byId("ca")->HasThemedBox());

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

// `appearance` is not inherited: a container's appearance: none leaves the controls in it themed
// (the user-agent sheet resets the companions on every control); the control's own still applies
TEST(AppearanceNone, IsNotInherited){
    Page page("inherit", ".plain { appearance: none; } #own { appearance: none; }",
              "<div class='plain'><input id='c' type='checkbox'><select id='s'><option>a</option></select><button id='b'>b</button>"
              "<input id='own' type='checkbox'><input id='g' type='range'><progress id='p' value='0.5'></progress></div>");
    ASSERT_TRUE(page.document);
    EXPECT_TRUE(page.byId("c")->HasNativeAppearance());
    EXPECT_TRUE(page.byId("c")->HasThemedBox());
    EXPECT_EQ(page.byId("s")->GetComputedValues().background_color(), Rml::Colourb(255, 255, 255, 255));
    EXPECT_FLOAT_EQ(part(page.byId("s"), "selectarrow")->GetComputedValues().width().value, 16.0f);
    EXPECT_FLOAT_EQ(page.byId("b")->GetComputedValues().border_top_width(), 1.0f);
    EXPECT_FLOAT_EQ(page.byId("g")->GetComputedValues().height().value, 16.0f);   // the themed range's height
    EXPECT_EQ(page.byId("p")->GetComputedValues().background_color().alpha, 0);   // not the gray bar of appearance: none
    EXPECT_FALSE(page.byId("own")->HasNativeAppearance());
    EXPECT_EQ(borderSize(page.byId("own")), Rml::Vector2f(0, 0));
}

// A themed checkbox or radio as Chromium's theme draws it: the author's background, border, padding
// and border radius have no effect (the fork's P4-G12), the theme is a square of the box's smaller
// side, centered; getComputedStyle and the inspector report Chromium's values
TEST(AppearanceNone, ThemedCheckboxIsACenteredSquare){
    Page page("themed", "input { margin: 0; } #c { width: 30px; height: 20px; background: red; border: 3px solid blue; padding: 4px; border-radius: 6px; } "
                        "#r { width: 20px; height: 40px; box-sizing: content-box; border: 2px solid blue; }",
              "<input id='c' type='checkbox' checked><input id='r' type='radio'>");
    ASSERT_TRUE(page.document);
    Rml::Element* checkbox = page.byId("c");
    EXPECT_EQ(borderSize(checkbox), Rml::Vector2f(30, 20));
    EXPECT_EQ(checkbox->GetComputedValues().background_color().alpha, 0);
    EXPECT_FALSE(checkbox->GetComputedValues().has_border_radius());
    // the theme: a 20px square centered in the 30x20 box, filled with the accent, the check through it
    auto* input = rmlui_dynamic_cast<htmlui::compat::HtmlInputElement*>(checkbox);
    ASSERT_NE(input, nullptr);
    const std::string svg = input->themeSvg(Rml::Vector2f(30, 20));
    EXPECT_NE(svg.find("<rect x=\"5\" y=\"0\" width=\"20\" height=\"20\" rx=\"2\" fill=\"#0075ff\"/>"), std::string::npos) << svg;
    EXPECT_NE(svg.find("<path d=\"M9 10 L13 14 L21 4\" fill=\"none\" stroke=\"#ffffff\" stroke-width=\"3.2\"/>"), std::string::npos) << svg;
    // content-box: the border is 0, so the box is its width and height; the circle centered in it
    EXPECT_EQ(borderSize(page.byId("r")), Rml::Vector2f(20, 40));
    auto* radio = rmlui_dynamic_cast<htmlui::compat::HtmlInputElement*>(page.byId("r"));
    ASSERT_NE(radio, nullptr);
    // (a radio is the fork's circle geometry: a 20px circle centered at (10, 20))
    const Rml::Mesh circle = radio->themeRadioMesh(Rml::Vector2f(20, 40), 1.0f, 1.0f);
    ASSERT_FALSE(circle.vertices.empty());
    Rml::Vector2f low(1e9f, 1e9f), high(-1e9f, -1e9f);
    for (const Rml::Vertex& vertex : circle.vertices){
        low = Rml::Vector2f(std::min(low.x, vertex.position.x), std::min(low.y, vertex.position.y));
        high = Rml::Vector2f(std::max(high.x, vertex.position.x), std::max(high.y, vertex.position.y));
    }
    EXPECT_NEAR((low.x + high.x) * 0.5f, 10.0f, 0.5f);   // (the feathered fringe is not symmetric)
    EXPECT_NEAR((low.y + high.y) * 0.5f, 20.0f, 0.5f);
    EXPECT_NEAR(high.x - low.x, 20.0f, 2.0f);   // (with the feathered fringe: 0.85px each side)

    // hovered and pressed (Chromium): a checked control's accent 11% darker and lighter in HSL
    // lightness, the other way round for a light accent; the unchecked frame #4f4f4f and #8d8d8d
    const auto near = [](Rml::Colourb a, Rml::Colourb b){
        return std::abs(a.red - b.red) <= 1 && std::abs(a.green - b.green) <= 1 && std::abs(a.blue - b.blue) <= 1 && a.alpha == b.alpha;
    };
    checkbox->SetProperty("--html-theme-shade", "hover");
    page.context->Update();
    EXPECT_TRUE(near(input->themeColours().fill, Rml::Colourb(0x00, 0x5c, 0xc8, 255)));
    checkbox->SetProperty("--html-theme-shade", "active");
    page.context->Update();
    EXPECT_TRUE(near(input->themeColours().fill, Rml::Colourb(0x37, 0x93, 0xff, 255)));
    checkbox->SetProperty("--html-accent", "#f0c040");          // accent-color: #f0c040 (a dark mark)
    checkbox->SetProperty("--html-accent-contrast", "#3b3b3b");
    page.context->Update();
    EXPECT_TRUE(near(input->themeColours().fill, Rml::Colourb(0xe6, 0xac, 0x12, 255)));
    checkbox->RemoveProperty("--html-theme-shade");
    checkbox->RemoveProperty("--html-accent");
    checkbox->RemoveProperty("--html-accent-contrast");
    page.byId("r")->SetPseudoClass("hover", true);
    page.context->Update();
    EXPECT_EQ(radio->themeColours().stroke, Rml::Colourb(0x4f, 0x4f, 0x4f, 255));
    page.byId("r")->SetPseudoClass("hover", false);

    std::string text;
    ASSERT_TRUE(htmlui::compat::forms::themedComputedValue(checkbox, Rml::PropertyId::BorderTopWidth, text));
    EXPECT_EQ(text, "0px");
    ASSERT_TRUE(htmlui::compat::forms::themedComputedValue(checkbox, Rml::PropertyId::PaddingLeft, text));
    EXPECT_EQ(text, "0px");
    EXPECT_FALSE(htmlui::compat::forms::themedComputedValue(checkbox, Rml::PropertyId::BackgroundColor, text));   // Chromium: the author's

    // without the theme the author's box applies, and the part is hidden
    checkbox->SetProperty("--html-appearance", "none");
    page.context->Update();
    EXPECT_EQ(checkbox->GetComputedValues().background_color(), Rml::Colourb(255, 0, 0, 255));
    EXPECT_FLOAT_EQ(checkbox->GetComputedValues().border_top_width(), 3.0f);
    EXPECT_FALSE(checkbox->HasThemedBox());
    EXPECT_FALSE(htmlui::compat::forms::themedComputedValue(checkbox, Rml::PropertyId::BorderTopWidth, text));
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
        std::string text;
        if (htmlui::compat::forms::themedComputedValue(page.byId(id), Rml::PropertyId::Display, text))
            return text;
        return page.byId(id)->GetProperty(Rml::PropertyId::Display)->ToString();
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
