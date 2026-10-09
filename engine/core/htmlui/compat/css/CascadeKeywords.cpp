// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/css/CascadeKeywords.h"

#include "htmlui/compat/SupportTables.h"
#include "htmlui/compat/css/Companions.h"
#include "htmlui/compat/css/CssParser.h"
#include "htmlui/compat/css/RcssGrammar.h"
#include "htmlui/compat/css/RewriteSession.h"
#include "htmlui/compat/css/Units.h"
#include "htmlui/compat/css/ValueNormalizer.h"

#include <map>

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

std::string CascadeKeywords::keywordOf(const std::vector<ComponentValue>& value){
    const ComponentValue* sole = soleValue(value);
    if (!sole || !sole->isToken(TokenType::Ident) || sole->token.value.size() > 12)
        return std::string();
    const std::string k = lowerAscii(sole->token.value);
    if (k == "inherit" || k == "initial" || k == "unset" || k == "revert" || k == "revert-layer")
        return k;
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

std::vector<std::string> CascadeKeywords::outputProperties(const std::string& cssProperty){
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
    std::vector<std::string> outputs = outputProperties(property);
    if (context.fork(ForkFeature::CssWideKeywords)){
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
