// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The compat element instancers in a live context (phase-2 §3.11.2): button (submit with name
// and value, reset, form=, disabled, Enter/Space), anchor (fragment scroll, htmlnavigate,
// external links), input (type mapping keeping the attribute, number filtering/steps/clamp,
// date placeholders, file, color, reset, implicit submission, autofocus), details/summary
// (toggle and the toggle event, exclusive accordions), dialog (show/showModal/close, cancel),
// img (intrinsic size, object-fit/position, pixelated, SVG sources), label (HTML targets).

#include "RuntimeTestCommon.h"

#include "htmlui/compat/runtime/HtmlAnchorElement.h"
#include "htmlui/compat/runtime/HtmlButtonElement.h"
#include "htmlui/compat/runtime/HtmlDetailsElement.h"
#include "htmlui/compat/runtime/HtmlDialogElement.h"
#include "htmlui/compat/runtime/HtmlImageElement.h"
#include "htmlui/compat/runtime/HtmlInputElement.h"
#include "htmlui/compat/runtime/HtmlLabelBehavior.h"

#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>

#include <gtest/gtest.h>

#include <map>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::test::compat;
using namespace jasperengine::htmlui::compat;

namespace {

    // records the events of one type with their parameters
    class Recorder : public Rml::EventListener {
    public:
        void ProcessEvent(Rml::Event& event) override{
            std::map<std::string, std::string> values;
            for (const auto& parameter : event.GetParameters())
                values[parameter.first] = parameter.second.Get<Rml::String>();
            events.push_back(values);
            targets.push_back(event.GetTargetElement());
            if (prevent)
                event.PreventDefault();
        }
        std::vector<std::map<std::string, std::string>> events;
        std::vector<Rml::Element*> targets;
        bool prevent = false;
    };

    class CompatElementTest : public RuntimeContextTest {
    protected:
        void TearDown() override{
            for (auto& entry : listening)
                entry.first->RemoveEventListener(entry.second.first, entry.second.second);
            listening.clear();
            RuntimeContextTest::TearDown();
        }

        void listen(Rml::Element* element, const std::string& type, Recorder& recorder){
            element->AddEventListener(type, &recorder);
            listening.push_back({element, {type, &recorder}});
        }

        std::vector<std::pair<Rml::Element*, std::pair<std::string, Recorder*>>> listening;
    };

    const char* kControls = "input, button, select, textarea, a, summary, label, img { display: inline-block; } "
                            "input, button, a, summary { tab-index: auto; } input { width: 100px; height: 20px; } "
                            "button, a { width: 80px; height: 20px; } label { width: 200px; height: 20px; } ";

}

// ---------------------------------------------------------------- button

TEST_F(CompatElementTest, ButtonSubmitsItsFormWithNameAndValue){
    ASSERT_NE(show("<form id='f'><input name='x' value='1'/><button id='go' name='go' value='yes'>Go</button>"
                   "<button id='plain' type='button'>B</button></form>"
                   "<button id='outside' form='f' name='out' value='o'>Out</button>", kControls), nullptr);
    ASSERT_NE(rmlui_dynamic_cast<HtmlButtonElement*>(byId("go")), nullptr);
    Recorder submits;
    listen(byId("f"), "submit", submits);

    click(byId("go"));
    ASSERT_EQ(submits.events.size(), 1u);
    EXPECT_EQ(submits.events[0]["go"], "yes");
    EXPECT_EQ(submits.events[0]["x"], "1");

    click(byId("plain"));   // type=button: nothing
    EXPECT_EQ(submits.events.size(), 1u);

    click(byId("outside"));   // form="f"
    ASSERT_EQ(submits.events.size(), 2u);
    EXPECT_EQ(submits.events[1]["out"], "o");
}

// P4-G2: a <button> taller than its content centers it vertically, as Chromium's anonymous button
// content box does (safe; in the content box), whatever align-content says; a flex button is a
// flex container
TEST_F(CompatElementTest, ButtonCentersItsContentVertically){
    ASSERT_NE(show("<button id='a'><div id='ac' style='height: 20px;'/></button>"
                   "<button id='b' style='padding: 10px 0 30px; align-content: start;'><div id='bc' style='height: 20px;'/></button>"
                   "<button id='c' style='height: 10px;'><div id='cc' style='height: 20px;'/></button>"
                   "<button id='d' style='display: flex;'><div id='dc' style='height: 20px;'/></button>",
                   "button { display: inline-block; box-sizing: border-box; width: 60px; height: 80px; padding: 0; border: 0; }"), nullptr);
    ASSERT_NE(rmlui_dynamic_cast<HtmlButtonElement*>(byId("a")), nullptr);
    const auto top = [this](const char* button, const char* child){
        return byId(child)->GetAbsoluteOffset(Rml::BoxArea::Border).y - byId(button)->GetAbsoluteOffset(Rml::BoxArea::Content).y;
    };
    EXPECT_FLOAT_EQ(top("a", "ac"), 30.0f);
    EXPECT_FLOAT_EQ(top("b", "bc"), 10.0f);
    EXPECT_FLOAT_EQ(top("c", "cc"), 0.0f);
    EXPECT_FLOAT_EQ(top("d", "dc"), 0.0f);
}

TEST_F(CompatElementTest, DisabledButtonGetsNoClick){
    ASSERT_NE(show("<form id='f'><button id='go' disabled=''>Go</button></form>", kControls), nullptr);
    Recorder submits, clicks;
    listen(byId("f"), "submit", submits);
    listen(byId("go"), "click", clicks);
    EXPECT_TRUE(byId("go")->IsPseudoClassSet("disabled"));
    click(byId("go"));
    EXPECT_TRUE(submits.events.empty());
    EXPECT_TRUE(clicks.events.empty());

    byId("go")->RemoveAttribute("disabled");
    frame();
    EXPECT_FALSE(byId("go")->IsPseudoClassSet("disabled"));
    click(byId("go"));
    EXPECT_EQ(submits.events.size(), 1u);
    EXPECT_EQ(clicks.events.size(), 1u);
}

TEST_F(CompatElementTest, EnterAndSpaceActivateButtons){
    ASSERT_NE(show("<form id='f'><button id='go'>Go</button></form>", kControls), nullptr);
    Recorder submits;
    listen(byId("f"), "submit", submits);
    ASSERT_TRUE(byId("go")->Focus(true));
    key(Rml::Input::KI_RETURN);
    EXPECT_EQ(submits.events.size(), 1u);
    key(Rml::Input::KI_SPACE);
    EXPECT_EQ(submits.events.size(), 2u);
}

TEST_F(CompatElementTest, ResetRestoresInitialValues){
    ASSERT_NE(show("<form id='f'><input id='t' name='t' value='start'/><input id='c' type='checkbox' name='c'/>"
                   "<input id='n' type='number' value='5'/>"
                   "<textarea id='area' name='area'>hello</textarea>"
                   "<select id='s' name='s'><option value='a'>A</option><option value='b' selected=''>B</option></select>"
                   "<button id='reset' type='reset'>R</button><input id='ireset' type='reset' value='R2'/></form>", kControls), nullptr);
    frame();
    auto* text = rmlui_dynamic_cast<HtmlInputElement*>(byId("t"));
    auto* box = rmlui_dynamic_cast<HtmlInputElement*>(byId("c"));
    auto* area = rmlui_dynamic_cast<Rml::ElementFormControlTextArea*>(byId("area"));
    auto* select = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(byId("s"));
    ASSERT_TRUE(text && box && area && select);
    EXPECT_EQ(text->getDefaultValue(), "start");
    EXPECT_EQ(select->GetSelection(), 1);

    text->SetValue("changed");
    click(box);
    area->SetValue("bye");
    select->SetSelection(0);
    rmlui_dynamic_cast<HtmlInputElement*>(byId("n"))->SetValue("9");
    frame();
    EXPECT_TRUE(box->HasAttribute("checked"));

    Recorder resets;
    listen(byId("f"), "reset", resets);
    click(byId("reset"));
    EXPECT_EQ(resets.events.size(), 1u);
    EXPECT_EQ(text->GetValue(), "start");
    EXPECT_FALSE(box->HasAttribute("checked"));
    EXPECT_EQ(area->GetValue(), "hello");
    EXPECT_EQ(select->GetSelection(), 1);
    EXPECT_EQ(rmlui_dynamic_cast<HtmlInputElement*>(byId("n"))->GetValue(), "5");

    // input type=reset does the same; a prevented reset changes nothing
    text->SetValue("again");
    resets.prevent = true;
    click(byId("ireset"));
    EXPECT_EQ(text->GetValue(), "again");
    resets.prevent = false;
    click(byId("ireset"));
    EXPECT_EQ(text->GetValue(), "start");
}

// ---------------------------------------------------------------- anchor

TEST(CompatAnchor, ResolveHrefs){
    AnchorTarget t = HtmlAnchorElement::resolve("#top", "ui/menu.html");
    EXPECT_EQ(t.kind, AnchorTarget::Kind::Fragment);
    EXPECT_EQ(t.fragment, "top");
    t = HtmlAnchorElement::resolve("settings.html#audio", "ui/menu.html");
    EXPECT_EQ(t.kind, AnchorTarget::Kind::Document);
    EXPECT_EQ(t.path, "ui/settings.html");
    EXPECT_EQ(t.fragment, "audio");
    t = HtmlAnchorElement::resolve("../credits.htm?x=1", "ui/sub/menu.html");
    EXPECT_EQ(t.path, "ui/credits.htm");
    EXPECT_EQ(t.kind, AnchorTarget::Kind::Document);
    t = HtmlAnchorElement::resolve("asset://ui/a.html", "ui/menu.html");
    EXPECT_EQ(t.path, "ui/a.html");
    t = HtmlAnchorElement::resolve("/ui/b.HTML", "x/menu.html");
    EXPECT_EQ(t.path, "ui/b.HTML");
    EXPECT_EQ(t.kind, AnchorTarget::Kind::Document);
    t = HtmlAnchorElement::resolve("img/map.png", "ui/menu.html");
    EXPECT_EQ(t.kind, AnchorTarget::Kind::Resource);
    t = HtmlAnchorElement::resolve("menu.html#credits", "ui/menu.html");
    EXPECT_EQ(t.kind, AnchorTarget::Kind::Fragment);
    for (const char* external : {"https://example.com/x.html", "http://a.b", "//cdn.example.com/x", "mailto:a@b.c"}){
        t = HtmlAnchorElement::resolve(external, "ui/menu.html");
        EXPECT_EQ(t.kind, AnchorTarget::Kind::External) << external;
        EXPECT_EQ(t.path, external);
    }
    EXPECT_EQ(HtmlAnchorElement::resolve("  ", "ui/menu.html").kind, AnchorTarget::Kind::None);
}

TEST_F(CompatElementTest, AnchorsNavigateAndScroll){
    ASSERT_NE(show("<a id='doc' href='other.html#part'>doc</a><a id='ext' href='https://example.com/'>ext</a>"
                   "<a id='jump' href='#far'>jump</a><a id='none'>no href</a>"
                   "<div style='height: 2000px;'/><div id='far' style='height: 20px;'>far</div>",
                   std::string(kControls) + "body { overflow-y: auto; } scrollbarvertical { width: 10px; } sliderbar { height: 20px; }"), nullptr);
    Recorder navigations;
    listen(document, "htmlnavigate", navigations);

    click(byId("doc"));
    ASSERT_EQ(navigations.events.size(), 1u);
    EXPECT_EQ(navigations.events[0]["href"], "other.html#part");
    EXPECT_EQ(navigations.events[0]["url"], "compat/runtime/other.html");
    EXPECT_EQ(navigations.events[0]["fragment"], "part");
    EXPECT_EQ(navigations.events[0]["document"], "1");
    EXPECT_EQ(navigations.events[0]["external"], "0");

    click(byId("ext"));
    ASSERT_EQ(navigations.events.size(), 2u);
    EXPECT_EQ(navigations.events[1]["external"], "1");
    EXPECT_EQ(navigations.events[1]["url"], "https://example.com/");

    click(byId("none"));
    EXPECT_EQ(navigations.events.size(), 2u);

    // a fragment scrolls, no htmlnavigate
    EXPECT_FLOAT_EQ(document->GetScrollTop(), 0.0f);
    click(byId("jump"));
    frame();
    EXPECT_GT(document->GetScrollTop(), 1000.0f);
    EXPECT_EQ(navigations.events.size(), 2u);

    // Enter on a focused link; a prevented click does not navigate
    ASSERT_TRUE(byId("doc")->Focus(true));
    key(Rml::Input::KI_RETURN);
    EXPECT_EQ(navigations.events.size(), 3u);
    Recorder clicks;
    clicks.prevent = true;
    listen(byId("ext"), "click", clicks);
    click(byId("ext"));
    EXPECT_EQ(navigations.events.size(), 3u);
}

// ---------------------------------------------------------------- input

TEST(CompatInput, TypeMappingAndNumberFilter){
    EXPECT_EQ(HtmlInputElement::mapType("email"), "text");
    EXPECT_EQ(HtmlInputElement::mapType("number"), "text");
    EXPECT_EQ(HtmlInputElement::mapType("date"), "text");
    EXPECT_EQ(HtmlInputElement::mapType("hidden"), "text");
    EXPECT_EQ(HtmlInputElement::mapType("password"), "password");
    EXPECT_EQ(HtmlInputElement::mapType("checkbox"), "checkbox");
    EXPECT_EQ(HtmlInputElement::mapType("range"), "range");
    EXPECT_EQ(HtmlInputElement::mapType("reset"), "button");
    EXPECT_EQ(HtmlInputElement::mapType("file"), "button");
    EXPECT_EQ(HtmlInputElement::mapType("image"), "submit");
    EXPECT_EQ(HtmlInputElement::mapType("frobnicate"), "text");
    EXPECT_EQ(HtmlInputElement::datePlaceholder("date"), "yyyy-mm-dd");
    EXPECT_EQ(HtmlInputElement::datePlaceholder("time"), "hh:mm");
    EXPECT_EQ(HtmlInputElement::datePlaceholder("text"), "");

    EXPECT_EQ(HtmlInputElement::filterNumber("12a3"), "123");
    EXPECT_EQ(HtmlInputElement::filterNumber("1.2.3"), "1.23");
    EXPECT_EQ(HtmlInputElement::filterNumber("--5"), "-5");
    EXPECT_EQ(HtmlInputElement::filterNumber("+3e-2x"), "+3e-2");
    EXPECT_EQ(HtmlInputElement::filterNumber("1e5e3"), "1e53");
    EXPECT_EQ(HtmlInputElement::filterNumber("e5"), "5");
    EXPECT_EQ(HtmlInputElement::filterNumber(".5"), ".5");
    EXPECT_EQ(HtmlInputElement::filterNumber("abc"), "");
}

TEST_F(CompatElementTest, InputTypesKeepTheirAttribute){
    ASSERT_NE(show("<input id='mail' type='email'/><input id='date' type='date'/><input id='time' type='time' placeholder='when'/>"
                   "<input id='file' type='file'/><input id='color' type='color'/><input id='box' type='CHECKBOX'/>", kControls), nullptr);
    auto* mail = rmlui_dynamic_cast<HtmlInputElement*>(byId("mail"));
    ASSERT_NE(mail, nullptr);
    EXPECT_EQ(mail->GetAttribute<Rml::String>("type", ""), "email");
    EXPECT_EQ(mail->getHtmlType(), "email");
    EXPECT_TRUE(mail->IsClassSet("text"));   // RmlUi's type class
    EXPECT_EQ(byId("date")->GetAttribute<Rml::String>("placeholder", ""), "yyyy-mm-dd");
    EXPECT_EQ(byId("time")->GetAttribute<Rml::String>("placeholder", ""), "when");
    EXPECT_TRUE(byId("file")->HasAttribute("disabled"));
    EXPECT_EQ(byId("file")->GetAttribute<Rml::String>("value", ""), "Choose File");
    EXPECT_EQ(rmlui_dynamic_cast<HtmlInputElement*>(byId("color"))->GetValue(), "#000000");
    EXPECT_TRUE(byId("box")->IsClassSet("checkbox"));

    // changing the type at run time
    mail->SetAttribute("type", "checkbox");
    frame();
    EXPECT_TRUE(mail->IsClassSet("checkbox"));
    EXPECT_EQ(mail->getHtmlType(), "checkbox");
}

TEST_F(CompatElementTest, NumberInputFiltersStepsAndClamps){
    ASSERT_NE(show("<input id='n' type='number' value='5' step='2' min='1' max='9'/><input id='other'/>", kControls), nullptr);
    auto* number = rmlui_dynamic_cast<HtmlInputElement*>(byId("n"));
    ASSERT_NE(number, nullptr);
    click(number);
    ASSERT_EQ(context->GetFocusElement(), number);

    key(Rml::Input::KI_UP);
    EXPECT_EQ(number->GetValue(), "7");
    key(Rml::Input::KI_UP);
    EXPECT_EQ(number->GetValue(), "9");
    key(Rml::Input::KI_UP);
    EXPECT_EQ(number->GetValue(), "9");
    key(Rml::Input::KI_DOWN);
    EXPECT_EQ(number->GetValue(), "7");

    // typing: only number characters
    number->SetValue("");
    frame();
    for (const char* character : {"1", "a", "x", "2"})
        text(character);
    EXPECT_EQ(number->GetValue(), "12");

    // out of range: clamped when the field loses focus
    click(byId("other"));
    EXPECT_EQ(number->GetValue(), "9");

    // and on Enter
    number->SetValue("-4");
    click(number);
    key(Rml::Input::KI_RETURN);
    EXPECT_EQ(number->GetValue(), "1");

    // empty + Up starts at min
    number->SetValue("");
    EXPECT_TRUE(number->stepBy(1));
    EXPECT_EQ(number->GetValue(), "1");
}

TEST_F(CompatElementTest, ImplicitSubmissionAndKeyboardToggles){
    ASSERT_NE(show("<form id='f'><input id='t' name='t'/><input id='c' type='checkbox'/><button id='go' name='go' value='1'>Go</button></form>"
                   "<form id='g'><input id='only' name='only' value='v'/></form>", kControls), nullptr);
    Recorder submits, gsubmits;
    listen(byId("f"), "submit", submits);
    listen(byId("g"), "submit", gsubmits);
    click(byId("t"));
    key(Rml::Input::KI_RETURN);
    ASSERT_EQ(submits.events.size(), 1u);
    EXPECT_EQ(submits.events[0]["go"], "1");   // through the submit button

    click(byId("only"));
    key(Rml::Input::KI_RETURN);
    ASSERT_EQ(gsubmits.events.size(), 1u);
    EXPECT_EQ(gsubmits.events[0]["only"], "v");

    ASSERT_TRUE(byId("c")->Focus(true));
    key(Rml::Input::KI_SPACE);
    EXPECT_TRUE(byId("c")->HasAttribute("checked"));
    key(Rml::Input::KI_SPACE);
    EXPECT_FALSE(byId("c")->HasAttribute("checked"));
}

TEST_F(CompatElementTest, Autofocus){
    ASSERT_NE(show("<input id='a'/><input id='b' autofocus=''/>", kControls), nullptr);
    frame();
    EXPECT_EQ(context->GetFocusElement(), byId("b"));
}

// ---------------------------------------------------------------- details / summary

TEST_F(CompatElementTest, DetailsToggle){
    // the summary's marker of phase 2's emulation (ListMarkers absent)
    HtmlListItemElement::setMarkerEmulation(true);
    struct Reset { ~Reset(){ HtmlListItemElement::resetMarkerEmulation(); } } reset;
    ASSERT_NE(show("<details id='d'><summary id='s'>More</summary><div id='body'>text</div></details>"
                   "<details id='a1' name='acc' open=''><summary id='s1'>1</summary></details>"
                   "<details id='a2' name='acc'><summary id='s2'>2</summary></details>",
                   std::string(kControls) + "summary { display: block; list-style-type: disclosure-closed; list-style-position: inside; } "
                   "details[open] > summary { list-style-type: disclosure-open; } details:not([open]) > div { display: none; }"), nullptr);
    auto* details = rmlui_dynamic_cast<HtmlDetailsElement*>(byId("d"));
    ASSERT_NE(details, nullptr);
    Recorder toggles;
    listen(details, "toggle", toggles);
    EXPECT_FALSE(details->isOpen());
    EXPECT_EQ(rmlui_dynamic_cast<HtmlListItemElement*>(byId("s"))->getMarkerText(), "\xE2\x96\xB8 ");

    click(byId("s"));
    EXPECT_TRUE(details->isOpen());
    frame();
    ASSERT_EQ(toggles.events.size(), 1u);
    EXPECT_EQ(toggles.events[0]["newState"], "open");
    EXPECT_EQ(toggles.events[0]["oldState"], "closed");
    frame();
    EXPECT_EQ(rmlui_dynamic_cast<HtmlListItemElement*>(byId("s"))->getMarkerText(), "\xE2\x96\xBE ");
    EXPECT_TRUE(byId("body")->IsVisible());

    // Enter on the focused summary closes it again
    ASSERT_TRUE(byId("s")->Focus(true));
    key(Rml::Input::KI_RETURN);
    frame();
    EXPECT_FALSE(details->isOpen());
    ASSERT_EQ(toggles.events.size(), 2u);
    EXPECT_EQ(toggles.events[1]["newState"], "closed");

    // two changes before the update: no event (back where it was)
    details->setOpen(true);
    details->setOpen(false);
    frame();
    EXPECT_EQ(toggles.events.size(), 2u);

    // exclusive accordion
    click(byId("s2"));
    EXPECT_TRUE(byId("a2")->HasAttribute("open"));
    EXPECT_FALSE(byId("a1")->HasAttribute("open"));
}

// ---------------------------------------------------------------- dialog

TEST_F(CompatElementTest, DialogShowCloseAndCancel){
    ASSERT_NE(show("<dialog id='d'><p>Sure?</p><button id='ok'>OK</button></dialog><button id='outside'>X</button>",
                   std::string(kControls) + "dialog { display: none; } dialog[open] { display: block; }"), nullptr);
    auto* dialog = rmlui_dynamic_cast<HtmlDialogElement*>(byId("d"));
    ASSERT_NE(dialog, nullptr);
    Recorder closes, cancels;
    listen(dialog, "close", closes);
    listen(dialog, "cancel", cancels);

    dialog->show();
    frame();
    EXPECT_TRUE(dialog->isOpen());
    EXPECT_FALSE(dialog->isModal());
    dialog->close("ok");
    EXPECT_FALSE(dialog->isOpen());
    EXPECT_TRUE(closes.events.empty());   // queued
    frame();
    ASSERT_EQ(closes.events.size(), 1u);
    EXPECT_EQ(closes.events[0]["returnValue"], "ok");
    EXPECT_EQ(dialog->getReturnValue(), "ok");

    dialog->showModal();
    frame();
    EXPECT_TRUE(dialog->isModal());
    EXPECT_EQ(context->GetFocusElement(), byId("ok"));

    // Escape: cancel (prevented: stays open), then closes
    cancels.prevent = true;
    key(Rml::Input::KI_ESCAPE);
    EXPECT_EQ(cancels.events.size(), 1u);
    EXPECT_TRUE(dialog->isOpen());
    cancels.prevent = false;
    key(Rml::Input::KI_ESCAPE);
    EXPECT_FALSE(dialog->isOpen());
    frame();
    EXPECT_EQ(closes.events.size(), 2u);
}

// ---------------------------------------------------------------- img

TEST(CompatImage, ObjectFit){
    decorators::CssPosition centre;
    LayerQuad quad;
    // a 4 x 2 image in a 40 x 40 box
    ASSERT_TRUE(computeObjectFit(ObjectFit::Fill, {4, 2}, {40, 40}, centre, quad));
    EXPECT_TRUE(quad.rect == (LayerRect{0, 0, 40, 40}));
    ASSERT_TRUE(computeObjectFit(ObjectFit::Contain, {4, 2}, {40, 40}, centre, quad));
    EXPECT_TRUE(quad.rect == (LayerRect{0, 10, 40, 20}));
    ASSERT_TRUE(computeObjectFit(ObjectFit::Cover, {4, 2}, {40, 40}, centre, quad));
    EXPECT_TRUE(quad.rect == (LayerRect{0, 0, 40, 40}));
    EXPECT_FLOAT_EQ(quad.u0, 0.25f);
    EXPECT_FLOAT_EQ(quad.u1, 0.75f);
    ASSERT_TRUE(computeObjectFit(ObjectFit::None, {4, 2}, {40, 40}, centre, quad));
    EXPECT_TRUE(quad.rect == (LayerRect{18, 19, 4, 2}));
    ASSERT_TRUE(computeObjectFit(ObjectFit::ScaleDown, {4, 2}, {40, 40}, centre, quad));
    EXPECT_TRUE(quad.rect == (LayerRect{18, 19, 4, 2}));
    ASSERT_TRUE(computeObjectFit(ObjectFit::ScaleDown, {80, 40}, {40, 40}, centre, quad));
    EXPECT_TRUE(quad.rect == (LayerRect{0, 10, 40, 20}));
    // object-position: left top, and right 5px bottom 5px
    decorators::CssPosition corner;
    ASSERT_TRUE(decorators::parseCssPosition({"left", "top"}, nullptr, corner));
    ASSERT_TRUE(computeObjectFit(ObjectFit::None, {4, 2}, {40, 40}, corner, quad));
    EXPECT_TRUE(quad.rect == (LayerRect{0, 0, 4, 2}));
    ASSERT_TRUE(decorators::parseCssPosition({"right", "5px", "bottom", "5px"}, nullptr, corner));
    ASSERT_TRUE(computeObjectFit(ObjectFit::None, {4, 2}, {40, 40}, corner, quad));
    EXPECT_TRUE(quad.rect == (LayerRect{31, 33, 4, 2}));
    ASSERT_TRUE(decorators::parseCssPosition({"top"}, nullptr, corner));
    EXPECT_FLOAT_EQ(corner.percentX, 50.0f);
    EXPECT_FLOAT_EQ(corner.percentY, 0.0f);
    EXPECT_FALSE(decorators::parseCssPosition({"left", "right"}, nullptr, corner));
    EXPECT_FALSE(decorators::parseCssPosition({"bogus"}, nullptr, corner));
    // nothing visible
    EXPECT_FALSE(computeObjectFit(ObjectFit::Fill, {4, 2}, {0, 40}, centre, quad));
    EXPECT_EQ(parseObjectFit("cover"), ObjectFit::Cover);
    EXPECT_EQ(parseObjectFit("weird"), ObjectFit::Fill);
}

TEST_F(CompatElementTest, ImageSizeFitAndSampling){
    ASSERT_NE(show("<img id='natural' src='wide4x2.png'/><img id='attr' src='wide4x2.png' width='40'/>"
                   "<img id='contain' src='wide4x2.png' style='width: 40px; height: 40px; object-fit: contain;'/>"
                   "<img id='pixel' src='pixels2x2.png' style='width: 20px; height: 20px; image-rendering: pixelated;'/>"
                   "<img id='svg' src='svg/mono.svg' style='width: 60px; height: 60px; color: #00ff00;'/>", kControls), nullptr);
    ASSERT_NE(rmlui_dynamic_cast<HtmlImageElement*>(byId("natural")), nullptr);
    EXPECT_EQ(byId("natural")->GetBox().GetSize(), Rml::Vector2f(4, 2));
    EXPECT_EQ(byId("attr")->GetBox().GetSize(), Rml::Vector2f(40, 20));
    EXPECT_EQ(rmlui_dynamic_cast<HtmlImageElement*>(byId("contain"))->getNaturalSize(), Rml::Vector2f(4, 2));

    // contain: the quad is 40 x 20, 10px down
    const Rml::Vector2f containAt = byId("contain")->GetAbsoluteOffset(Rml::BoxArea::Content);
    bool found = false;
    for (const RecordingRenderInterface::Draw& draw : recording.getDraws()){
        const RecordingRenderInterface::Geometry* geometry = recording.getGeometry(draw.geometry);
        if (geometry && draw.translation == byId("contain")->GetAbsoluteOffset(Rml::BoxArea::Border)){
            float top = 1e9f, bottom = -1e9f;
            for (const Rml::Vertex& vertex : geometry->vertices){
                top = std::min(top, vertex.position.y);
                bottom = std::max(bottom, vertex.position.y);
            }
            EXPECT_FLOAT_EQ(bottom - top, 20.0f);
            EXPECT_FLOAT_EQ(top + draw.translation.y, containAt.y + 10.0f);
            found = true;
        }
    }
    EXPECT_TRUE(found);

    // pixelated: nearest sampling requested
    const std::vector<std::string>& requests = recording.getLoadRequests();
    EXPECT_TRUE(std::any_of(requests.begin(), requests.end(), [](const std::string& r){
        return r.find("pixels2x2.png?htmlui-filter=nearest") != std::string::npos;
    }));

    // an SVG source: rasterized at its drawn size, tinted by color (monochrome currentColor)
    bool svg = false;
    for (const RecordingRenderInterface::Draw& draw : recording.getDraws()){
        const RecordingRenderInterface::Texture* texture = recording.getTexture(draw.texture);
        if (texture && texture->source.empty() && texture->dimensions == Rml::Vector2i(60, 60)){
            svg = true;
            EXPECT_EQ(recording.getGeometry(draw.geometry)->vertices[0].colour, Rml::ColourbPremultiplied(0, 255, 0, 255));
        }
    }
    EXPECT_TRUE(svg);
}

// P4-G1: the picture of a rounded <img> is trimmed to its content edge curve: by the mesh (the
// content box filled like a background, feathered curves) when it covers the content box or lies
// inside the curve, under a clip mask when it crosses the curve
TEST_F(CompatElementTest, RoundedImageIsTrimmedToTheContentCurve){
    const auto pictureOf = [this](const char* id, size_t& vertices){
        vertices = 0;
        for (const RecordingRenderInterface::Draw& draw : recording.getDraws()){
            const RecordingRenderInterface::Texture* texture = recording.getTexture(draw.texture);
            if (texture && texture->source.find("wide4x2.png") != std::string::npos &&
                draw.translation == byId(id)->GetAbsoluteOffset(Rml::BoxArea::Border)){
                vertices = recording.getGeometry(draw.geometry)->vertices.size();
                return true;
            }
        }
        return false;
    };
    size_t vertices = 0;

    // square: a quad, no mask
    ASSERT_NE(show("<img id='a' src='wide4x2.png' style='width: 40px; height: 40px;'/>", kControls), nullptr);
    ASSERT_TRUE(pictureOf("a", vertices));
    EXPECT_EQ(vertices, 4u);
    EXPECT_EQ(recording.clipMaskRenders, 0);

    // fill, rounded: the rounded content box, no mask; texture coordinates follow the quad
    ASSERT_NE(show("<img id='a' src='wide4x2.png' style='width: 40px; height: 40px; border-radius: 50%;'/>", kControls), nullptr);
    ASSERT_TRUE(pictureOf("a", vertices));
    EXPECT_GT(vertices, 16u);
    EXPECT_EQ(recording.clipMaskRenders, 0);

    // padding and a border: the curve's radii lose them (20 - 4 - 10 = 6), the picture is the content box
    ASSERT_NE(show("<img id='a' src='wide4x2.png' style='width: 40px; height: 40px; padding: 10px; border: 4px solid #000; "
                   "border-radius: 20px;'/>", kControls), nullptr);
    ASSERT_TRUE(pictureOf("a", vertices));
    EXPECT_GT(vertices, 16u);
    for (const RecordingRenderInterface::Draw& draw : recording.getDraws()){
        const RecordingRenderInterface::Texture* texture = recording.getTexture(draw.texture);
        if (!texture || texture->source.find("wide4x2.png") == std::string::npos)
            continue;
        for (const Rml::Vertex& vertex : recording.getGeometry(draw.geometry)->vertices){
            // (the feathered ring reaches a little over the edge)
            EXPECT_GE(vertex.position.x, 14.0f - 1.5f);
            EXPECT_LE(vertex.position.x, 54.0f + 1.5f);
            EXPECT_NEAR(vertex.tex_coord.x, (vertex.position.x - 14.0f) / 40.0f, 1e-4f);
        }
    }
    EXPECT_EQ(recording.clipMaskRenders, 0);

    // contain: a 40 x 20 picture across the curves of a 50% radius: a quad under a clip mask
    ASSERT_NE(show("<img id='a' src='wide4x2.png' style='width: 40px; height: 40px; border-radius: 50%; object-fit: contain;'/>", kControls),
              nullptr);
    ASSERT_TRUE(pictureOf("a", vertices));
    EXPECT_EQ(vertices, 4u);
    EXPECT_EQ(recording.clipMaskSets, 1);

    // none: a 4 x 2 picture in the middle, inside the curve: a quad, no mask
    ASSERT_NE(show("<img id='a' src='wide4x2.png' style='width: 40px; height: 40px; border-radius: 8px; object-fit: none;'/>", kControls),
              nullptr);
    ASSERT_TRUE(pictureOf("a", vertices));
    EXPECT_EQ(vertices, 4u);
    EXPECT_EQ(recording.clipMaskRenders, 0);
}

// P4-G5: ::before and ::after on the engine's inputs as Chromium draws them: checkbox, radio and
// range yes; text-like, button-like and the types the engine approximates by a text field or a
// button (color, date, file, reset, image) no
TEST_F(CompatElementTest, InputsHaveGeneratedContentAsInChromium){
    const char* types[] = {"checkbox", "radio", "range", "text", "password", "number", "submit", "button", "reset", "image", "file", "color", "date"};
    std::string body;
    for (const char* type : types)
        body += std::string("<input id='") + type + "' type='" + type + "' class='g'/>";
    ASSERT_NE(show(body, std::string(kControls) + ".g::before { content: ''; display: block; width: 4px; height: 4px; } .g::after { content: 'x'; }"),
              nullptr);
    for (const char* type : types){
        const bool expected = std::string(type) == "checkbox" || std::string(type) == "radio" || std::string(type) == "range";
        Rml::Element* input = byId(type);
        ASSERT_NE(input, nullptr) << type;
        EXPECT_EQ(input->GetPseudoElement(Rml::PseudoElementType::Before) != nullptr, expected) << type;
        EXPECT_EQ(input->GetPseudoElement(Rml::PseudoElementType::After) != nullptr, expected) << type;
    }
    // laid out in the checkbox's content box
    Rml::Element* before = byId("checkbox")->GetPseudoElement(Rml::PseudoElementType::Before);
    EXPECT_EQ(before->GetBox().GetSize(), Rml::Vector2f(4, 4));
    EXPECT_EQ(before->GetAbsoluteOffset(Rml::BoxArea::Border), byId("checkbox")->GetAbsoluteOffset(Rml::BoxArea::Content));
}

// ---------------------------------------------------------------- label

TEST_F(CompatElementTest, LabelsActivateTheirHtmlControl){
    ASSERT_NE(show("<div id='outer'><label id='wrap'><input id='hidden' type='hidden' value='x'/><input id='box' type='checkbox'/> Remember</label></div>"
                   "<label id='forDiv' for='plain'>To a div</label><div id='plain'>div</div>"
                   "<label id='forOff' for='off'>Disabled</label><input id='off' type='checkbox' disabled=''/>"
                   "<label id='forBox' for='box2'>Second</label><input id='box2' type='checkbox'/>",
                   std::string(kControls) + "input[type=hidden] { display: none; }"), nullptr);
    auto* wrap = rmlui_dynamic_cast<HtmlLabelElement*>(byId("wrap"));
    ASSERT_NE(wrap, nullptr);
    EXPECT_EQ(wrap->getControl(), byId("box"));
    EXPECT_EQ(rmlui_dynamic_cast<HtmlLabelElement*>(byId("forDiv"))->getControl(), nullptr);

    // the label's click still reaches its ancestors, then activates the checkbox
    Recorder outerClicks;
    listen(byId("outer"), "click", outerClicks);
    const Rml::Vector2f at = wrap->GetAbsoluteOffset(Rml::BoxArea::Border) + Rml::Vector2f(190, 10);
    context->ProcessMouseMove((int)at.x, (int)at.y, 0);
    context->ProcessMouseButtonDown(0, 0);
    context->ProcessMouseButtonUp(0, 0);
    frame();
    EXPECT_TRUE(byId("box")->HasAttribute("checked"));
    EXPECT_NE(context->GetFocusElement(), byId("hidden"));
    EXPECT_GE(outerClicks.events.size(), 1u);

    click(byId("forBox"));
    EXPECT_TRUE(byId("box2")->HasAttribute("checked"));
    click(byId("forOff"));
    EXPECT_FALSE(byId("off")->HasAttribute("checked"));
    Recorder divClicks;
    listen(byId("plain"), "click", divClicks);
    click(byId("forDiv"));
    EXPECT_TRUE(divClicks.events.empty());
}
