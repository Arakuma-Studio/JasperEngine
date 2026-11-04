// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/css/CascadeKeywords.h"
#include "htmlui/compat/ForkFeatures.h"
#include "htmlui/compat/LiteralCompare.h"

#include "htmlui/compat/SupportTables.h"
#include "htmlui/compat/css/Companions.h"
#include "htmlui/compat/css/CssParser.h"
#include "htmlui/compat/css/RcssGrammar.h"
#include "htmlui/compat/css/RewriteSession.h"
#include "htmlui/compat/css/Units.h"
#include "htmlui/compat/css/ValueNormalizer.h"

#include <map>
#include "object/ui/Text.h"

using namespace jasperengine::htmlui::compat;

namespace {

    const std::map<std::string, std::vector<std::string>>& shorthands(){
        static const std::map<std::string, std::vector<std::string>> table = {
            {"background", {"background-color", "decorator"}},
            {"border", {"border-top-width", "border-right-width", "border-bottom-width", "border-left-width", "border-top-color",
                        "border-right-color", "border-bottom-color", "border-left-color"}},
            {"border-top", {"border-top-width", "border-top-color"}},
            {"border-right", {"border-right-width", "border-right-color"}},
            {"border-bottom", {"border-bottom-width", "border-bottom-color"}},
            {"border-left", {"border-left-width", "border-left-color"}},
            {"border-width", {"border-top-width", "border-right-width", "border-bottom-width", "border-left-width"}},
            {"border-color", {"border-top-color", "border-right-color", "border-bottom-color", "border-left-color"}},
            {"border-style", {"border-top-width", "border-right-width", "border-bottom-width", "border-left-width"}},
            {"border-top-style", {"border-top-width"}},
            {"border-right-style", {"border-right-width"}},
            {"border-bottom-style", {"border-bottom-width"}},
            {"border-left-style", {"border-left-width"}},
            {"border-radius", {"border-top-left-radius", "border-top-right-radius", "border-bottom-right-radius", "border-bottom-left-radius"}},
            {"margin", {"margin-top", "margin-right", "margin-bottom", "margin-left"}},
            {"padding", {"padding-top", "padding-right", "padding-bottom", "padding-left"}},
            {"inset", {"top", "right", "bottom", "left"}},
            {"font", {"font-style", "font-weight", "font-size", "line-height", "font-family"}},
            {"flex", {"flex-grow", "flex-shrink", "flex-basis"}},
            {"flex-flow", {"flex-direction", "flex-wrap"}},
            {"gap", {"row-gap", "column-gap"}},
            {"grid-gap", {"row-gap", "column-gap"}},
            {"grid-row-gap", {"row-gap"}},
            {"grid-column-gap", {"column-gap"}},
            {"overflow", {"overflow-x", "overflow-y"}},
            {"transform-origin", {"transform-origin-x", "transform-origin-y", "transform-origin-z"}},
            {"perspective-origin", {"perspective-origin-x", "perspective-origin-y"}},
            {"list-style", {"list-style-type", "list-style-position", "list-style-image"}},
            {"text-shadow", {"font-effect"}},
            {"-webkit-text-stroke", {"font-effect"}},
            {"-webkit-text-stroke-width", {"font-effect"}},
            {"-webkit-text-stroke-color", {"font-effect"}},
            {"-webkit-text-fill-color", {"color"}},
            {"direction", {"-rmlui-direction"}},
            {"text-decoration-line", {"text-decoration"}},
            {"place-content", {"align-content", "justify-content"}},
            {"place-items", {"align-items"}},
            {"place-self", {"align-self"}},
            {"mask", {"mask-image"}},
            {"clip-path", {"mask-image"}},
            {"outline", {"box-shadow"}},
            {"outline-width", {"box-shadow"}},
            {"outline-color", {"box-shadow"}},
            {"outline-style", {"box-shadow"}},
            {"outline-offset", {"box-shadow"}},
            {"overflow-wrap", {"word-break"}},
            {"word-wrap", {"word-break"}},
            {"white-space-collapse", {"white-space"}},
            {"text-wrap", {"white-space"}},
            {"text-wrap-mode", {"white-space"}},
            {"border-spacing", {"row-gap", "column-gap"}},
            {"inline-size", {"width"}},
            {"block-size", {"height"}},
            {"min-inline-size", {"min-width"}},
            {"max-inline-size", {"max-width"}},
            {"min-block-size", {"min-height"}},
            {"max-block-size", {"max-height"}},
            {"translate", {"transform"}},
            {"rotate", {"transform"}},
            {"scale", {"transform"}},
        };
        return table;
    }

}

std::string CascadeKeywords::keywordOf(const ValueList& value){
    const ComponentValue* sole = soleValue(value);
    if (!sole || !sole->isToken(TokenType::Ident) || sole->token.value.size() > 12)
        return std::string();
    const Text& v = sole->token.value;
    static constexpr std::string_view keywords[] = {"inherit", "initial", "unset", "revert", "revert-layer"};
    for (std::string_view k : keywords){
        if (equalsNoCase(v, k))
            return std::string(k);
    }
    return std::string();
}

bool CascadeKeywords::isKeywordText(std::string_view value){
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\n'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\n'))
        value.remove_suffix(1);
    return equalsNoCase(value, "inherit") || equalsNoCase(value, "initial") || equalsNoCase(value, "unset") ||
           equalsNoCase(value, "revert") || equalsNoCase(value, "revert-layer");
}

std::vector<std::string> CascadeKeywords::outputProperties(const std::string& cssProperty, const ForkFeatures& fork){
    // the fork's own text properties (phase 3, p3-text) are written as themselves
    if (cssProperty == "text-shadow" && fork.has(ForkFeature::TextShadow))
        return {cssProperty};
    if (fork.has(ForkFeature::TextProperties)){
        if (cssProperty == "word-wrap")
            return {"overflow-wrap"};
        if (cssProperty == "overflow-wrap" || cssProperty == "white-space-collapse" || cssProperty == "text-wrap" ||
            cssProperty == "text-wrap-mode" || cssProperty == "text-decoration-line")
            return {cssProperty};
    }
    // the fork's borders and outlines (phase 3, p3-paint)
    if (fork.has(ForkFeature::Outline) && cssProperty.compare(0, 7, "outline") == 0)
        return cssProperty == "outline" ? std::vector<std::string>{"outline-width", "outline-style", "outline-color"}
                                        : std::vector<std::string>{cssProperty};
    if (fork.has(ForkFeature::BorderStyle)){
        static const char* const sides[] = {"top", "right", "bottom", "left"};
        auto perSide = [](const char* const* list, size_t count, const char* suffix){
            std::vector<std::string> out;
            for (size_t i = 0; i < count; i++)
                out.push_back(std::string("border-") + list[i] + suffix);
            return out;
        };
        if (cssProperty == "border-style")
            return perSide(sides, 4, "-style");
        for (const char* side : sides){
            if (cssProperty == std::string("border-") + side + "-style")
                return {cssProperty};
            if (cssProperty == std::string("border-") + side)
                return {cssProperty + "-width", cssProperty + "-style", cssProperty + "-color"};
        }
        if (cssProperty == "border"){
            std::vector<std::string> out = perSide(sides, 4, "-width");
            for (const std::string& p : perSide(sides, 4, "-style"))
                out.push_back(p);
            for (const std::string& p : perSide(sides, 4, "-color"))
                out.push_back(p);
            return out;
        }
    }
    // the fork's background and mask layers (phase 3, p3-paint)
    if (fork.has(ForkFeature::BackgroundLayers)){
        if (cssProperty == "background")
            return {"background-color", "background-image", "background-position-x", "background-position-y", "background-size",
                    "background-repeat", "background-origin", "background-clip", "background-attachment"};
        if (cssProperty == "background-position")
            return {"background-position-x", "background-position-y"};
        if (cssProperty == "-webkit-background-clip")
            return {"background-clip"};
        if (cssProperty == "mask" || cssProperty == "-webkit-mask")
            return {"mask-image", "mask-position-x", "mask-position-y", "mask-size", "mask-repeat"};
        if (cssProperty == "mask-position" || cssProperty == "-webkit-mask-position")
            return {"mask-position-x", "mask-position-y"};
        const std::string plain = cssProperty.compare(0, 8, "-webkit-") == 0 ? cssProperty.substr(8) : cssProperty;
        static const char* const layerLonghands[] = {"background-image", "background-position-x", "background-position-y",
                                                     "background-size", "background-repeat", "background-origin", "background-clip",
                                                     "background-attachment", "mask-image", "mask-position-x", "mask-position-y",
                                                     "mask-size", "mask-repeat"};
        for (const char* longhand : layerLonghands){
            if (plain == longhand)
                return {plain};
        }
    }
    const auto it = shorthands().find(cssProperty);
    if (it != shorthands().end())
        return it->second;
    if (cssProperty.compare(0, 11, "background-") == 0 || cssProperty == "-webkit-background-clip")
        return {"decorator"};
    if (cssProperty.compare(0, 11, "transition-") == 0)
        return {"transition"};
    if (cssProperty.compare(0, 10, "animation-") == 0)
        return {"animation"};
    if (cssProperty.compare(0, 5, "mask-") == 0 || cssProperty.compare(0, 13, "-webkit-mask") == 0)
        return {"mask-image"};
    if (cssProperty.compare(0, 12, "border-image") == 0)
        return {"decorator"};
    if (cssProperty.compare(0, 7, "margin-") == 0 || cssProperty.compare(0, 8, "padding-") == 0 ||
        cssProperty.compare(0, 6, "inset-") == 0 || cssProperty.compare(0, 13, "border-inline") == 0 ||
        cssProperty.compare(0, 12, "border-block") == 0){
        if (!RcssGrammar::known(cssProperty))
            return {};   // logical properties: handled by their rewrite with the keyword
    }
    if (RcssGrammar::known(cssProperty))
        return {cssProperty};
    return {};
}

bool CascadeKeywords::inherited(const std::string& rcssProperty){
    if (const PropertySupport* row = findProperty(rcssProperty))
        return row->inherited;
    return false;
}

void CascadeKeywords::apply(DeclContext& context, const std::string& keywordIn){
    std::string keyword = keywordIn;
    const std::string& property = context.name;
    std::vector<std::string> outputs = outputProperties(property, context.session.options.fork);
    if (context.fork(ForkFeature::CssWideKeywords)){
        // the fork's own keywords, on the properties the declaration is written to: the
        // background color is a property of its own, the other background-* are the decorator's
        if (property == "background-color")
            outputs = {"background-color"};
        else if (property == "background" && !context.fork(ForkFeature::BackgroundLayers))
            outputs = {"background-color", "decorator"};
        if (outputs.empty())
            outputs.push_back(property);
        for (const std::string& p : outputs)
            context.emit(p, keyword);
        return;
    }
    if (keyword == "revert" || keyword == "revert-layer"){
        context.reportAs(Severity::Info, codes::ApproximatedMinor, "'" + keyword + "' is treated as 'unset' (no cascade origins)");
        keyword = "unset";
    }
    if (outputs.empty()){
        context.reportAs(Severity::Warning, codes::UnsupportedValue, "'" + property + ": " + keyword + "' cannot be written; declaration dropped",
                         "set the value explicitly");
        context.dropped = true;
        return;
    }
    if (property == "font-size" && keyword == "inherit"){
        context.emit("font-size", "1em");
        return;
    }
    const bool cssInherited = context.row ? context.row->inherited : inherited(property);
    for (const std::string& p : outputs){
        const PropertySupport* row = findProperty(p);
        const bool inheritedProperty = (outputs.size() == 1 && context.row) ? cssInherited : (row ? row->inherited : cssInherited);
        const bool differs = row ? row->rmluiDefaultDiffers : false;
        const std::string initial = row ? row->initial : std::string();
        if (keyword == "inherit" || keyword == "unset"){
            if (inheritedProperty){
                context.emit(p, std::string("var(") + companions::kUnset + ")");
                continue;
            }
            if (keyword == "inherit"){
                std::string fallback = initial;
                if (fallback.empty() || differs || !RcssGrammar::accepts(p, fallback))
                    fallback = "";
                context.emit(p, "var(" + companions::inheritName(p) + (fallback.empty() ? "" : ", " + fallback) + ")");
                context.reportAs(Severity::Info, codes::InheritApproximated,
                                 "'" + property + ": inherit' copies the nearest ancestor's declared " + p + " (relative units re-resolve here)");
                continue;
            }
        }
        // initial (and unset on a non-inherited property)
        if (!inheritedProperty && !differs && keyword == "unset"){
            context.emit(p, std::string("var(") + companions::kUnset + ")");
            continue;
        }
        if (!inheritedProperty && !differs && keyword == "initial"){
            context.emit(p, std::string("var(") + companions::kUnset + ")");
            continue;
        }
        if (initial.empty()){
            context.emit(p, std::string("var(") + companions::kUnset + ")");
            continue;
        }
        processSynthetic(context, p == property ? property : p, initial);
    }
}
