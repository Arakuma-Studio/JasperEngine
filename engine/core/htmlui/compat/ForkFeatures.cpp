// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ForkFeatures.h"

#include "RmlLogTranslator.h"
#include "htmlui/effects/EffectRuntime.h"

#include <RmlUi/Core.h>

#include <cmath>
#include <cstring>

using namespace jasperengine::htmlui::compat;

namespace {

    // Polarity: true = the fork has the feature (compat passes the CSS through). Every entry
    // false in phase 2; phase 3 flips them with the fork patch that implements the feature
    // and records the flip in docs/html-ui/FORK.md.
    constexpr bool kPresent[(size_t)ForkFeature::Count] = {
        false,   // None (never present)
        true,    // Calc (phase 3, p3-base: upstream PR #983; docs/html-ui/FORK.md)
        true,    // BorderStyle (phase 3, p3-paint: docs/html-ui/FORK.md)
        true,    // PercentRadius (phase 3, p3-paint: percentages and elliptical radii; docs/html-ui/FORK.md)
        true,    // PseudoElements (phase 3, p3-cascade: docs/html-ui/FORK.md)
        true,    // RootIsWhere (phase 3, p3-cascade: docs/html-ui/FORK.md)
        true,    // Fixed (phase 3, p3-layout: position: fixed against the viewport; docs/html-ui/FORK.md P3-L8)
        true,    // GroupOpacity (phase 3, p3-paint: opacity groups, not inherited; docs/html-ui/FORK.md)
        true,    // TimingFunctions (phase 3, p3-animation: CSS easing functions; docs/html-ui/FORK.md P3-A1)
        true,    // Important (phase 3, p3-cascade: docs/html-ui/FORK.md)
        true,    // CssWideKeywords (phase 3, p3-cascade: docs/html-ui/FORK.md)
        true,    // Grid (phase 3, p3-grid: CSS Grid, a port of Taffy; docs/html-ui/FORK.md P3-G1..P3-G3)
        true,    // TextShadow (phase 3, p3-text: native text-shadow; docs/html-ui/FORK.md)
        true,    // TextProperties (phase 3, p3-text: word-spacing, white-space longhands, overflow-wrap, text-decoration-*, ...)
        true,    // AnimationLonghands (phase 3, p3-animation: FORK.md P3-A1..P3-A4)
        true,    // IndividualTransforms (phase 3, p3-animation: FORK.md P3-A5)
        true,    // CurrentColor (phase 3, p3-values: rmlui: currentcolor; docs/html-ui/FORK.md)
        true,    // ExtraUnits (phase 3, p3-values: rmlui: CSS units vmin, vmax, ch, ex, turn, grad)
        true,    // LineHeightNormal (phase 3, p3-values: rmlui: line-height: normal)
        true,    // Sticky (phase 3, p3-layout: position: sticky, overflow: clip; docs/html-ui/FORK.md P3-L1, P3-L8)
        true,    // BoxParity (phase 3, p3-layout: flex, block and sizing parity; docs/html-ui/FORK.md P3-L2..P3-L10)
        true,    // Has (phase 3, p3-cascade)
        true,    // FocusPseudoClasses (phase 3, p3-cascade)
        true,    // SelectorsLevel4 (phase 3, p3-cascade)
        true,    // ListMarkers (phase 3, p3-cascade)
        true,    // Outline (phase 3, p3-paint: outline-* drawn by the fork, the focus ring; docs/html-ui/FORK.md)
        true,    // ObjectFit (phase 3, p3-paint: object-fit, object-position with <position>; docs/html-ui/FORK.md)
        true,    // BackgroundLayers (phase 3, p3-paint: background-* and mask-* layers, the canvas background; docs/html-ui/FORK.md)
        true,    // RelativeFontWeight (phase 3, p3-values: bolder/lighter computed against the parent's weight, FORK.md P3-V4)
        true,    // ColorFunctions (phase 3, p3-values: CSS Color 4 functions parsed at runtime, a var() inside them too, FORK.md P3-V2)
        true,    // TextFillFilter (phase 5, p5-infra: -jfx-text-fill applies before filter, FORK.md P5-1)
        true,    // RegisteredProperties (phase 5, p5-property: registered custom properties, @property; FORK.md P5-4)
        true,    // Justify (phase 6, p6-justify: justified lines, text-align-last, text-justify; FORK.md P6-J1)
        true,    // TextIndent (phase 6, p6-justify: text-indent with hanging and each-line; FORK.md P6-J2)
        true,    // LineClamp (phase 6, p6-line-clamp: Chromium 153's legacy -webkit-line-clamp with its ellipsis; FORK.md P6-L1)
    };

    constexpr const char* kNames[(size_t)ForkFeature::Count] = {
        "None", "Calc", "BorderStyle", "PercentRadius", "PseudoElements", "RootIsWhere", "Fixed",
        "GroupOpacity", "TimingFunctions", "Important", "CssWideKeywords", "Grid",
        "TextShadow", "TextProperties",
        "AnimationLonghands", "IndividualTransforms",
        "CurrentColor", "ExtraUnits", "LineHeightNormal",
        "Sticky", "BoxParity",
        "Has", "FocusPseudoClasses", "SelectorsLevel4", "ListMarkers",
        "Outline", "ObjectFit", "BackgroundLayers",
        "RelativeFontWeight", "ColorFunctions",
        "TextFillFilter",
        "RegisteredProperties",
        "Justify",
        "TextIndent",
        "LineClamp",
    };

    constexpr uint64_t bitOf(ForkFeature feature){
        return uint64_t(1) << (unsigned)feature;
    }

    // The probe document's context lays out but draws nothing
    class NullRenderInterface : public Rml::RenderInterface {
    public:
        Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override { return 1; }
        void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override {}
        void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
        Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String&) override {
            dimensions = Rml::Vector2i(1, 1);
            return 1;
        }
        Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override { return 1; }
        void ReleaseTexture(Rml::TextureHandle) override {}
        void EnableScissorRegion(bool) override {}
        void SetScissorRegion(Rml::Rectanglei) override {}
    };

    NullRenderInterface& nullRenderInterface(){
        // outlives Rml::Shutdown, which releases what the probe context compiled
        static NullRenderInterface instance;
        return instance;
    }

    bool parses(const char* property, const char* value){
        Rml::PropertyDictionary dictionary;
        return Rml::StyleSheetSpecification::ParsePropertyDeclaration(dictionary, property, value);
    }

    // Layout results of one probe document (PseudoElements, RootIsWhere, Fixed, Sticky, BoxParity)
    struct DocumentProbe {
        bool loaded = false;
        bool pseudoElements = false;
        bool isSelector = false;
        bool rootSelector = false;
        bool fixed = false;
        bool focusPseudoClasses = false;
        bool listMarkers = false;
        bool sticky = false;
        bool marginCollapse = false;
    };

    const char* probeDocument = R"(<rml>
<head>
<style>
body { display: block; width: 64px; height: 64px; margin: 0; padding: 0; }
div { display: block; }
#a { width: 10px; }
#a::before { content: "x"; display: block; width: 3px; height: 3px; }
:is(#b) { width: 5px; }
:root { width: 7px; }
#s { position: relative; margin-left: 20px; margin-top: 20px; overflow: scroll; width: 20px; height: 20px; }
#tall { height: 100px; }
#f { position: fixed; top: 2px; left: 2px; width: 1px; height: 1px; }
#st { position: absolute; left: 44px; top: 30px; width: 10px; height: 10px; overflow: scroll; }
#st-tall { height: 100px; }
#sticky { position: sticky; top: 0; height: 1px; }
#mc-root { position: absolute; left: 44px; top: 0; width: 10px; }
#mc-child { margin-top: 4px; height: 1px; }
</style>
</head>
<body>
<div id="a"></div>
<div id="b"></div>
<div id="s"><div id="tall"></div><div id="f"></div></div>
<div id="fw"><div id="fc"></div></div>
<div id="lm" style="display: list-item; list-style-type: decimal;"></div>
<div id="st"><div id="sticky"></div><div id="st-tall"></div></div>
<div id="mc-root"><div id="mc"><div id="mc-child"></div></div></div>
</body>
</rml>)";

    DocumentProbe probeWithDocument(){
        DocumentProbe result;

        const Rml::String name = "htmlui-compat-fork-probe";
        Rml::Context* context = Rml::CreateContext(name, Rml::Vector2i(64, 64), &nullRenderInterface());
        if (!context)
            return result;

        Rml::ElementDocument* document = context->LoadDocumentFromMemory(probeDocument, "[htmlui-compat-fork-probe]");
        if (document){
            document->Show();
            context->Update();

            Rml::Element* scroller = document->GetElementById("s");
            if (scroller){
                scroller->SetScrollTop(10.0f);
                context->Update();
            }
            Rml::Element* stickyScroller = document->GetElementById("st");
            if (stickyScroller){
                stickyScroller->SetScrollTop(20.0f);
                context->Update();
            }

            Rml::Element* a = document->GetElementById("a");
            Rml::Element* b = document->GetElementById("b");
            Rml::Element* f = document->GetElementById("f");
            Rml::Element* sticky = document->GetElementById("sticky");
            Rml::Element* mc = document->GetElementById("mc");
            if (a && b && f && sticky && mc && stickyScroller){
                result.loaded = true;
                // the generated 3x3 block gives #a (height auto, no content) its height
                result.pseudoElements = std::fabs(a->GetClientHeight() - 3.0f) < 0.5f;
                result.isSelector = std::fabs(b->GetClientWidth() - 5.0f) < 0.5f;
                result.rootSelector = std::fabs(document->GetClientWidth() - 7.0f) < 0.5f;
                // fixed: placed against the viewport, so the scroll of #s does not move it
                result.fixed = std::fabs(f->GetAbsoluteTop() - 2.0f) < 0.5f && std::fabs(f->GetAbsoluteLeft() - 2.0f) < 0.5f;
                // sticky: stays at the top of its scrolled container (a relative box would scroll away)
                result.sticky = std::fabs(sticky->GetAbsoluteTop() - stickyScroller->GetAbsoluteTop()) < 0.5f;
                // margin collapsing: #mc's top margin collapses with its first child's, so #mc starts 4px down
                result.marginCollapse = std::fabs(mc->GetAbsoluteTop() - 4.0f) < 0.5f;
            }
            // a list item generates its ::marker
            if (Rml::Element* item = document->GetElementById("lm"))
                result.listMarkers = item->GetPseudoElement(Rml::PseudoElementType::Marker) != nullptr;
            // :focus on the focused element only, :focus-within on its ancestors
            Rml::Element* within = document->GetElementById("fw");
            Rml::Element* focused = document->GetElementById("fc");
            if (within && focused && focused->Focus()){
                result.focusPseudoClasses = focused->IsPseudoClassSet("focus") && !within->IsPseudoClassSet("focus") &&
                                            within->IsPseudoClassSet("focus-within");
            }
            document->Close();
        }

        Rml::RemoveContext(name);
        return result;
    }

    // TextFillFilter: -jfx-text-fill holds a filter list, and an element that has only it compiles
    // one filter when drawn (the fork's ElementEffects applies it as a filter). Needs the effects'
    // instancers (EffectRegistry::initialise, in HtmlUI::acquire)
    std::optional<bool> probeTextFill(){
        if (!Rml::StyleSheetSpecification::GetProperty("-jfx-text-fill"))
            return false;
        if (!Rml::Factory::GetFilterInstancer("jfx-pixelate"))
            return std::nullopt;
        const Rml::String name = "htmlui-compat-textfill-probe";
        Rml::Context* context = Rml::CreateContext(name, Rml::Vector2i(64, 64), &nullRenderInterface());
        if (!context)
            return std::nullopt;
        std::optional<bool> result;
        Rml::ElementDocument* document = context->LoadDocumentFromMemory(R"(<rml><head><style>
body { display: block; width: 64px; height: 64px; }
#t { display: block; width: 10px; height: 10px; -jfx-text-fill: jfx-pixelate(v(8px)); }
</style></head><body><div id="t"/></body></rml>)", "[htmlui-compat-textfill-probe]");
        if (document){
            document->Show();
            context->Update();
            const uint64_t before = jasperengine::htmlui::EffectRuntime::get().getFilterCompileCount();
            context->Render();
            result = jasperengine::htmlui::EffectRuntime::get().getFilterCompileCount() - before == 1;
            document->Close();
        }
        Rml::RemoveContext(name);
        return result;
    }

    // a sheet loads and every selector of it is valid (an invalid selector is dropped with a warning)
    bool loadsCleanly(const char* rcss, RmlLogTranslator::Capture& capture){
        const size_t before = capture.problems();
        Rml::SharedPtr<Rml::StyleSheetContainer> sheet = Rml::Factory::InstanceStyleSheetString(rcss);
        return sheet != nullptr && capture.problems() == before;
    }

    // RegisteredProperties: '--htmlui-probe' registered as <length> (process-global, as every registration), set to 3px on a
    // scratch element of a scratch document, and read back through a dependent 'width: var(--htmlui-probe)'
    bool probeRegisteredProperties(){
        const char* name = "--htmlui-probe";
        Rml::PropertyId id = Rml::StyleSheetSpecification::GetPropertyId(name);
        if (id == Rml::PropertyId::Invalid)
            id = Rml::StyleSheetSpecification::RegisterCustomProperty(name, Rml::RegisteredPropertySyntax::Length, false, "0px");
        if (id == Rml::PropertyId::Invalid || !Rml::StyleSheetSpecification::IsRegisteredCustomProperty(id))
            return false;

        const Rml::String contextName = "htmlui-compat-fork-probe-property";
        Rml::Context* context = Rml::CreateContext(contextName, Rml::Vector2i(64, 64), &nullRenderInterface());
        if (!context)
            return false;
        bool result = false;
        const char* rml = R"(<rml><head><style>body { display: block; width: 64px; font-family: sans-serif; font-size: 10px; } #p { display: block; width: var(--htmlui-probe); height: 1px; }</style></head>
<body><div id="p"/></body></rml>)";
        if (Rml::ElementDocument* document = context->LoadDocumentFromMemory(rml, "[htmlui-compat-fork-probe-property]")){
            document->Show();
            context->Update();
            if (Rml::Element* element = document->GetElementById("p")){
                element->SetProperty(name, "3px");
                context->Update();
                const Rml::Property* value = element->GetProperty(name);
                result = std::fabs(element->GetClientWidth() - 3.0f) < 0.01f && value && value->unit == Rml::Unit::PX;
            }
            document->Close();
        }
        Rml::RemoveContext(contextName);
        return result;
    }

    // LineClamp: a 'display: -webkit-box; -webkit-box-orient: vertical; -webkit-line-clamp: 2' box of three lines (an
    // inline-block 7px tall on each, no font needed) is two lines tall, and computes to flow-root
    std::optional<bool> probeLineClamp(){
        if (!parses("-webkit-line-clamp", "2") || !parses("-webkit-box-orient", "vertical") || !parses("display", "-webkit-box"))
            return false;
        const Rml::String contextName = "htmlui-compat-fork-probe-line-clamp";
        Rml::Context* context = Rml::CreateContext(contextName, Rml::Vector2i(64, 64), &nullRenderInterface());
        if (!context)
            return std::nullopt;
        std::optional<bool> result;
        const char* rml = R"(<rml><head><style>body { display: block; width: 64px; line-height: 0; }
#c { display: -webkit-box; -webkit-box-orient: vertical; -webkit-line-clamp: 2; width: 10px; }
#u { display: block; width: 10px; }
span { display: inline-block; width: 10px; height: 7px; }</style></head>
<body><div id="c"><span/><span/><span/></div><div id="u"><span/><span/><span/></div></body></rml>)";
        if (Rml::ElementDocument* document = context->LoadDocumentFromMemory(rml, "[htmlui-compat-fork-probe-line-clamp]")){
            document->Show();
            context->Update();
            Rml::Element* clamped = document->GetElementById("c");
            Rml::Element* unclamped = document->GetElementById("u");
            if (clamped && unclamped){
                result = std::fabs(clamped->GetClientHeight() - 14.0f) < 0.5f && std::fabs(unclamped->GetClientHeight() - 21.0f) < 0.5f &&
                         clamped->GetComputedValues().display() == Rml::Style::Display::FlowRoot;
            }
            document->Close();
        }
        Rml::RemoveContext(contextName);
        return result;
    }

    bool isRmlInitialised(){
        // the system interface is set before Rml::Initialise and cleared by Rml::Shutdown
        // (HtmlUI::acquire/release)
        return Rml::GetSystemInterface() != nullptr;
    }

}

ForkFeatures ForkFeatures::present(){
    ForkFeatures features;
    for (size_t i = 1; i < (size_t)ForkFeature::Count; i++){
        if (kPresent[i])
            features.mask |= bitOf((ForkFeature)i);
    }
    return features;
}

ForkFeatures ForkFeatures::none(){
    return ForkFeatures();
}

ForkFeatures ForkFeatures::all(){
    ForkFeatures features;
    for (size_t i = 1; i < (size_t)ForkFeature::Count; i++)
        features.mask |= bitOf((ForkFeature)i);
    return features;
}

std::optional<bool> ForkFeatures::probe(ForkFeature feature){
    if (!isRmlInitialised())
        return std::nullopt;

    // the probes provoke warnings on a fork without the feature: none reaches the log
    RmlLogTranslator::Capture capture;

    switch (feature){
        case ForkFeature::Calc:
            return parses("width", "calc(1px + 2px)");
        case ForkFeature::BorderStyle:
            return Rml::StyleSheetSpecification::GetProperty("border-top-style") != nullptr;
        case ForkFeature::PercentRadius:
            return parses("border-top-left-radius", "10%");
        case ForkFeature::TimingFunctions:
            return parses("transition", "opacity 1s cubic-bezier(0.1, 0.2, 0.3, 0.4)");
        case ForkFeature::Grid:
            return parses("display", "grid");
        case ForkFeature::AnimationLonghands:
            return parses("animation-fill-mode", "both") && parses("transition-behavior", "allow-discrete") &&
                   parses("animation", "spin 1s 2.5 alternate-reverse backwards paused");
        case ForkFeature::IndividualTransforms:
            return parses("rotate", "45deg") && parses("scale", "1.5") && parses("translate", "10px 20%");
        case ForkFeature::CssWideKeywords:
            return parses("color", "inherit");
        case ForkFeature::CurrentColor:
            return parses("border-top-color", "currentcolor") && parses("box-shadow", "0 0 1px currentcolor");
        case ForkFeature::ExtraUnits:
            return parses("width", "1vmin") && parses("width", "1vmax") && parses("width", "1ch") && parses("width", "1ex") &&
                   parses("transform", "rotate(1turn)") && parses("transform", "rotate(1grad)") && parses("width", "1svw");
        case ForkFeature::LineHeightNormal:
            return parses("line-height", "normal");
        case ForkFeature::RelativeFontWeight:
            return parses("font-weight", "bolder") && parses("font-weight", "lighter");
        case ForkFeature::ColorFunctions:
            // the space syntax, a percentage alpha, hwb() and color-mix(), as a var() substitution gives them
            return parses("color", "hsl(120 50% 50% / 0.5)") && parses("color", "rgb(255 0 0 / 50%)") &&
                   parses("color", "rgba(255, 0, 0, 0.2)") && parses("color", "hwb(120 10% 20%)") &&
                   parses("color", "color-mix(in srgb, #ff0000 40%, #0000ff)");
        case ForkFeature::Important: {
            Rml::SharedPtr<Rml::StyleSheetContainer> sheet = Rml::Factory::InstanceStyleSheetString("div { color: #f00 !important; }");
            return sheet != nullptr && capture.problems() == 0;
        }
        case ForkFeature::PseudoElements: {
            const DocumentProbe result = probeWithDocument();
            if (!result.loaded)
                return std::nullopt;
            return result.pseudoElements;
        }
        case ForkFeature::RootIsWhere: {
            const DocumentProbe result = probeWithDocument();
            if (!result.loaded)
                return std::nullopt;
            return result.isSelector && result.rootSelector;
        }
        case ForkFeature::Fixed: {
            const DocumentProbe result = probeWithDocument();
            if (!result.loaded)
                return std::nullopt;
            return result.fixed;
        }
        case ForkFeature::TextShadow:
            return parses("text-shadow", "1px 2px 3px #000000ff, 0 0 2px");
        case ForkFeature::TextProperties:
            // a sample of the family: the word-spacing property, the white-space and text-decoration shorthands
            return parses("word-spacing", "0.25em") && parses("white-space", "break-spaces") &&
                   parses("text-decoration", "underline dotted #ff0000ff 2px") && parses("overflow-wrap", "anywhere");
        case ForkFeature::Outline:
            return parses("outline", "2px dashed #ff0000ff") && parses("outline-style", "auto") && parses("outline-offset", "-2px");
        case ForkFeature::BackgroundLayers:
            return parses("background", "url(a.png) center / cover no-repeat, linear-gradient(#ff0000ff, #0000ffff) #000000ff") &&
                   parses("mask", "linear-gradient(#000000ff, #00000000) 0% 0% / 10px 10px repeat-x");
        case ForkFeature::ObjectFit:
            return parses("object-fit", "scale-down") && parses("object-position", "right 10px bottom 20%");
        case ForkFeature::Sticky: {
            if (!parses("overflow", "clip"))
                return false;
            const DocumentProbe result = probeWithDocument();
            if (!result.loaded)
                return std::nullopt;
            return result.sticky;
        }
        case ForkFeature::BoxParity: {
            if (!parses("aspect-ratio", "16 / 9") || !parses("width", "fit-content") || !parses("display", "contents") || !parses("order", "1"))
                return false;
            const DocumentProbe result = probeWithDocument();
            if (!result.loaded)
                return std::nullopt;
            return result.marginCollapse;
        }
        case ForkFeature::GroupOpacity:
            // not observable without rendering: the table is authoritative
            return std::nullopt;
        case ForkFeature::Has:
            return loadsCleanly("li:has(> input:checked), .a:has(+ .b) { color: #f00; }", capture);
        case ForkFeature::FocusPseudoClasses: {
            const DocumentProbe result = probeWithDocument();
            if (!result.loaded)
                return std::nullopt;
            return result.focusPseudoClasses;
        }
        case ForkFeature::ListMarkers: {
            const DocumentProbe result = probeWithDocument();
            if (!result.loaded)
                return std::nullopt;
            return result.listMarkers;
        }
        case ForkFeature::TextFillFilter:
            return probeTextFill();
        case ForkFeature::RegisteredProperties:
            return probeRegisteredProperties();
        case ForkFeature::TextIndent:
            // the shorthand with its keywords and a percentage in calc()
            return parses("text-indent", "1em hanging each-line") && parses("text-indent", "calc(10% + 2px)") && !parses("text-indent", "hanging");
        case ForkFeature::Justify:
            // the two properties of the feature; justify-all is not parsed, as in Chromium
            return parses("text-align-last", "justify") && parses("text-justify", "inter-character") && parses("text-justify", "distribute") &&
                   !parses("text-align", "justify-all");
        case ForkFeature::LineClamp:
            return probeLineClamp();
        case ForkFeature::SelectorsLevel4:
            return loadsCleanly("input:enabled, [lang=\"en\" i], li:nth-child(2 of .x), :read-write, :default, :target { color: #f00; }", capture);
        case ForkFeature::None:
        case ForkFeature::Count:
            break;
    }
    return std::nullopt;
}

ForkFeatures ForkFeatures::probe(){
    ForkFeatures features = present();
    for (size_t i = 1; i < (size_t)ForkFeature::Count; i++){
        const std::optional<bool> result = probe((ForkFeature)i);
        if (result)
            features = features.with((ForkFeature)i, *result);
    }
    return features;
}

bool ForkFeatures::has(ForkFeature feature) const{
    if (feature == ForkFeature::None || feature >= ForkFeature::Count)
        return false;
    return (mask & bitOf(feature)) != 0;
}

ForkFeatures ForkFeatures::with(ForkFeature feature, bool present) const{
    ForkFeatures result = *this;
    if (feature == ForkFeature::None || feature >= ForkFeature::Count)
        return result;
    if (present)
        result.mask |= bitOf(feature);
    else
        result.mask &= ~bitOf(feature);
    return result;
}

ForkFeatures ForkFeatures::fromBits(uint64_t bits){
    ForkFeatures result;
    result.mask = bits & all().mask;
    return result;
}

const char* jasperengine::htmlui::compat::forkFeatureName(ForkFeature feature){
    if (feature >= ForkFeature::Count)
        return "None";
    return kNames[(size_t)feature];
}

ForkFeature jasperengine::htmlui::compat::findForkFeature(std::string_view name){
    for (size_t i = 1; i < (size_t)ForkFeature::Count; i++){
        if (name == kNames[i])
            return (ForkFeature)i;
    }
    return ForkFeature::None;
}
