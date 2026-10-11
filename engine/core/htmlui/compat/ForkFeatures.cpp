// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ForkFeatures.h"

#include "RmlLogTranslator.h"

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
        false,   // BorderStyle
        false,   // PercentRadius
        true,    // PseudoElements (phase 3, p3-cascade: docs/html-ui/FORK.md)
        true,    // RootIsWhere (phase 3, p3-cascade: docs/html-ui/FORK.md)
        false,   // Fixed
        false,   // GroupOpacity
        true,    // TimingFunctions (phase 3, p3-animation: CSS easing functions; docs/html-ui/FORK.md P3-A1)
        true,    // Important (phase 3, p3-cascade: docs/html-ui/FORK.md)
        true,    // CssWideKeywords (phase 3, p3-cascade: docs/html-ui/FORK.md)
        true,    // Grid (phase 3, p3-grid: CSS Grid, a port of Taffy; docs/html-ui/FORK.md P3-G1..P3-G3)
        true,    // TextShadow (phase 3, p3-text: native text-shadow; docs/html-ui/FORK.md)
        true,    // TextProperties (phase 3, p3-text: word-spacing, white-space longhands, overflow-wrap, text-decoration-*, ...)
        true,    // AnimationLonghands (phase 3, p3-animation: FORK.md P3-A1..P3-A4)
        true,    // IndividualTransforms (phase 3, p3-animation: FORK.md P3-A5)
        true,    // Has (phase 3, p3-cascade)
        true,    // FocusPseudoClasses (phase 3, p3-cascade)
        true,    // SelectorsLevel4 (phase 3, p3-cascade)
        true,    // ListMarkers (phase 3, p3-cascade)
    };

    constexpr const char* kNames[(size_t)ForkFeature::Count] = {
        "None", "Calc", "BorderStyle", "PercentRadius", "PseudoElements", "RootIsWhere", "Fixed",
        "GroupOpacity", "TimingFunctions", "Important", "CssWideKeywords", "Grid",
        "TextShadow", "TextProperties",
        "AnimationLonghands", "IndividualTransforms",
        "Has", "FocusPseudoClasses", "SelectorsLevel4", "ListMarkers",
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

    // Layout results of one probe document (PseudoElements, RootIsWhere, Fixed)
    struct DocumentProbe {
        bool loaded = false;
        bool pseudoElements = false;
        bool isSelector = false;
        bool rootSelector = false;
        bool fixed = false;
        bool focusPseudoClasses = false;
        bool listMarkers = false;
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
</style>
</head>
<body>
<div id="a"></div>
<div id="b"></div>
<div id="s"><div id="tall"></div><div id="f"></div></div>
<div id="fw"><div id="fc"></div></div>
<div id="lm" style="display: list-item; list-style-type: decimal;"></div>
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

            Rml::Element* a = document->GetElementById("a");
            Rml::Element* b = document->GetElementById("b");
            Rml::Element* f = document->GetElementById("f");
            if (a && b && f){
                result.loaded = true;
                // the generated 3x3 block gives #a (height auto, no content) its height
                result.pseudoElements = std::fabs(a->GetClientHeight() - 3.0f) < 0.5f;
                result.isSelector = std::fabs(b->GetClientWidth() - 5.0f) < 0.5f;
                result.rootSelector = std::fabs(document->GetClientWidth() - 7.0f) < 0.5f;
                // fixed: placed against the viewport, so the scroll of #s does not move it
                result.fixed = std::fabs(f->GetAbsoluteTop() - 2.0f) < 0.5f && std::fabs(f->GetAbsoluteLeft() - 2.0f) < 0.5f;
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

    // a sheet loads and every selector of it is valid (an invalid selector is dropped with a warning)
    bool loadsCleanly(const char* rcss, RmlLogTranslator::Capture& capture){
        const size_t before = capture.problems();
        Rml::SharedPtr<Rml::StyleSheetContainer> sheet = Rml::Factory::InstanceStyleSheetString(rcss);
        return sheet != nullptr && capture.problems() == before;
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
