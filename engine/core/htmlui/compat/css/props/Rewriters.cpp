// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/css/props/Rewriters.h"
#include "htmlui/compat/LiteralCompare.h"

#include "htmlui/compat/ImportManifest.h"
#include "htmlui/compat/VirtualFiles.h"
#include "htmlui/compat/css/CascadeKeywords.h"
#include "htmlui/compat/css/ColorValue.h"
#include "htmlui/compat/css/CssParser.h"
#include "htmlui/compat/css/CssSerialize.h"
#include "htmlui/compat/css/RcssGrammar.h"
#include "htmlui/compat/css/Units.h"
#include "htmlui/compat/css/ValueNormalizer.h"

#include <algorithm>
#include <cstring>

using namespace jasperengine::htmlui::compat;

namespace {

    const RewriteRegistration kRegistry[] = {
        {RewriteId::Background, rewriteBackground},
        {RewriteId::BackgroundLonghand, rewriteBackgroundLonghand},
        {RewriteId::Border, rewriteBorder},
        {RewriteId::BorderStyle, rewriteBorderStyle},
        {RewriteId::BorderRadius, rewriteBorderRadius},
        {RewriteId::BorderImage, rewriteBorderImage},
        {RewriteId::Outline, rewriteOutline},
        {RewriteId::BoxShadow, rewriteBoxShadow},
        {RewriteId::Font, rewriteFont},
        {RewriteId::FontFamily, rewriteFontFamily},
        {RewriteId::FontSize, rewriteFontSize},
        {RewriteId::FontWeight, rewriteFontWeight},
        {RewriteId::FontStyle, rewriteFontStyle},
        {RewriteId::LineHeight, rewriteLineHeight},
        {RewriteId::TextShadow, rewriteTextShadow},
        {RewriteId::TextStroke, rewriteTextStroke},
        {RewriteId::TextFillColor, rewriteTextFillColor},
        {RewriteId::TextDecoration, rewriteTextDecoration},
        {RewriteId::TextAlign, rewriteTextAlign},
        {RewriteId::Transition, rewriteTransition},
        {RewriteId::TransitionLonghand, rewriteTransitionLonghand},
        {RewriteId::Animation, rewriteAnimation},
        {RewriteId::AnimationLonghand, rewriteAnimationLonghand},
        {RewriteId::Display, rewriteDisplay},
        {RewriteId::Position, rewritePosition},
        {RewriteId::LogicalBox, rewriteLogicalBox},
        {RewriteId::PlaceAlign, rewritePlaceAlign},
        {RewriteId::AlignKeyword, rewriteAlignKeyword},
        {RewriteId::Overflow, rewriteOverflow},
        {RewriteId::OverflowWrap, rewriteOverflowWrap},
        {RewriteId::WhiteSpace, rewriteWhiteSpace},
        {RewriteId::Filter, rewriteFilter},
        {RewriteId::MaskImage, rewriteMaskImage},
        {RewriteId::ClipPath, rewriteClipPath},
        {RewriteId::ListStyle, rewriteListStyle},
        {RewriteId::ObjectFit, rewriteObjectFit},
        {RewriteId::ImageRendering, rewriteImageRendering},
        {RewriteId::AccentColor, rewriteAccentColor},
        {RewriteId::Appearance, rewriteAppearance},
        {RewriteId::IndividualTransform, rewriteIndividualTransform},
        {RewriteId::ColorValue, rewriteColorValue},
        {RewriteId::Cursor, rewriteCursor},
        {RewriteId::PointerEvents, rewritePointerEvents},
        {RewriteId::TabIndexLike, rewriteTabIndexLike},
        {RewriteId::Direction, rewriteDirection},
        {RewriteId::SvgPaint, rewriteSvgPaint},
        {RewriteId::ScrollbarStyle, rewriteScrollbarStyle},
        {RewriteId::Gap, rewriteGap},
        {RewriteId::BorderSpacing, rewriteBorderSpacing},
        {RewriteId::Visibility, rewriteVisibility},
        {RewriteId::ZIndex, rewriteZIndex},
        {RewriteId::Opacity, rewriteOpacity},
        {RewriteId::Sizing, rewriteSizing},
        {RewriteId::Flex, rewriteFlex},
        {RewriteId::Grid, rewriteGrid},
        {RewriteId::Content, rewriteContent},
        {RewriteId::All, rewriteAll},
        {RewriteId::LineClamp, rewriteLineClamp},
    };

    const VarFamily* const kFamilies[] = {
        &kDecoratorFamily, &kMaskFamily, &kFontEffectFamily, &kTransitionFamily, &kAnimationFamily, &kFontFamilyFamily,
        &kFilterFamily, &kEasingFamily,
    };

}

Span<const RewriteRegistration> jasperengine::htmlui::compat::rewriteRegistry(){
    return Span<const RewriteRegistration>(kRegistry);
}

RewriteFunction jasperengine::htmlui::compat::findRewrite(RewriteId id){
    // the registry by id (the first registration of an id, as a search from the start finds)
    static const std::vector<RewriteFunction> byId = [](){
        std::vector<RewriteFunction> table;
        for (const RewriteRegistration& r : kRegistry){
            const size_t index = (size_t)r.id;
            if (table.size() <= index)
                table.resize(index + 1, nullptr);
            if (!table[index])
                table[index] = r.function;
        }
        return table;
    }();
    const size_t index = (size_t)id;
    return index < byId.size() ? byId[index] : nullptr;
}

const VarFamily* jasperengine::htmlui::compat::findVarFamily(const std::string& name){
    for (const VarFamily* family : kFamilies){
        if (name == family->name)
            return family;
    }
    return nullptr;
}

std::string_view jasperengine::htmlui::compat::varFamilyOf(std::string_view property){
    if (property == "background" || property == "background-image")
        return "decorator";
    if (property == "mask" || property == "mask-image" || property == "-webkit-mask" || property == "-webkit-mask-image")
        return "mask";
    if (property == "text-shadow")
        return "font-effect";
    if (property == "transition")
        return "transition";
    if (property == "animation")
        return "animation";
    if (property == "font-family")
        return "font-family";
    if (property == "filter" || property == "backdrop-filter" || property == "-webkit-backdrop-filter")
        return "filter";
    if (property == "transition-timing-function" || property == "animation-timing-function")
        return "easing";
    return std::string_view();
}

// ---- helpers

using namespace jasperengine::htmlui::compat::props;

bool props::isWs(const ComponentValue& value){
    return value.isWhitespace() || value.isToken(TokenType::Comment);
}

ValueList props::words(const ValueList& value){
    ValueList out;
    size_t count = 0;
    for (const ComponentValue& v : value)
        count += isWs(v) ? 0 : 1;
    out.reserve(count);
    for (const ComponentValue& v : value){
        if (!isWs(v))
            out.push_back(v);
    }
    return out;
}

std::vector<ValueList> props::commaGroups(const ValueList& value){
    // splitAtCommas + trimWhitespace of each group, each group copied once
    std::vector<ValueList> groups;
    size_t commas = 0;
    for (const ComponentValue& v : value)
        commas += v.isToken(TokenType::Comma) ? 1 : 0;
    groups.reserve(commas + 1);
    size_t begin = 0;
    for (size_t i = 0; i <= value.size(); i++){
        if (i < value.size() && !value[i].isToken(TokenType::Comma))
            continue;
        size_t a = begin, b = i;
        while (a < b && value[a].isWhitespace())
            a++;
        while (b > a && value[b - 1].isWhitespace())
            b--;
        groups.emplace_back(value.begin() + (std::ptrdiff_t)a, value.begin() + (std::ptrdiff_t)b);
        begin = i + 1;
    }
    return groups;
}

std::string props::text(const ComponentValue& value){
    return serialize(value);
}

std::string props::text(const ValueList& value){
    size_t a = 0, b = value.size();
    while (a < b && value[a].isWhitespace())
        a++;
    while (b > a && value[b - 1].isWhitespace())
        b--;
    if (a == 0 && b == value.size())
        return serialize(value);
    return serialize(ValueList(value.begin() + (std::ptrdiff_t)a, value.begin() + (std::ptrdiff_t)b));
}

std::string props::join(const std::vector<std::string>& parts, const char* separator){
    std::string out;
    for (const std::string& p : parts){
        if (p.empty())
            continue;
        if (!out.empty())
            out += separator;
        out += p;
    }
    return out;
}

bool props::isIdent(const ComponentValue& value, const char* lowercase){
    return value.isToken(TokenType::Ident) && equalsNoCase(value.token.value, lowercase);
}

std::string props::identOf(const ComponentValue& value){
    return value.isToken(TokenType::Ident) ? lowerAscii(value.token.value) : std::string();
}

bool props::isVar(const ComponentValue& value){
    return value.kind == ComponentValue::Kind::Function && equalsNoCase(value.name(), "var");
}

namespace {

    std::string kindOfValue(const ValueList& raw){
        ValueList value = props::words(raw);
        if (value.size() != 1)
            return value.empty() ? "empty" : "other";
        const ComponentValue& v = value[0];
        if (v.isToken(TokenType::Hash))
            return "color";
        if (v.kind == ComponentValue::Kind::Function && equalsNoCase(v.name(), "var") &&
            CustomPropertyIndex::varName(v) == "--html-current-color")
            return "color";
        if (v.isToken(TokenType::Dimension)){
            const UnitCategory category = unitCategory(v.token.unit);
            if (isLengthCategory(category))
                return "length";
            if (category == UnitCategory::Time)
                return "time";
            if (category == UnitCategory::Angle)
                return "angle";
            return "other";
        }
        if (v.isToken(TokenType::Number))
            return v.token.number == 0 ? "zero" : "number";
        if (v.isToken(TokenType::Percentage))
            return "percentage";
        if (v.isToken(TokenType::String))
            return "string";
        return "other";
    }

}

namespace {

    std::string computeVarKind(Session& session, const std::string& name, const ValueList& fallback, bool hasFallback);

}

std::string props::varKind(Session& session, const ComponentValue& var){
    ValueList fallback;
    bool hasFallback = false;
    const std::string name = CustomPropertyIndex::varName(var, &fallback, &hasFallback);
    if (hasFallback)
        return computeVarKind(session, name, fallback, true);
    // the kind of a variable without fallback depends on the index alone: kept per name
    const uint32_t revision = session.vars.revision();
    std::pair<uint32_t, std::string>& kept = session.varKinds.get(name, [&](std::string_view){
        return std::make_pair(revision, computeVarKind(session, name, fallback, false));
    });
    if (kept.first != revision)
        kept = std::make_pair(revision, computeVarKind(session, name, fallback, false));
    return kept.second;
}

namespace {

    std::string computeVarKind(Session& session, const std::string& name, const ValueList& fallback, bool hasFallback){
        if (name == "--html-current-color")
            return "color";
        if (name.empty())
            return "other";
        std::vector<ValueList> values;
        if (!session.vars.declared(name)){
            if (!hasFallback)
                return "undeclared";
            values.push_back(fallback);
        }else if (!session.vars.expandedValues(name, values)){
            return "mixed";
        }
        std::string kind;
        for (const ValueList& value : values){
            std::string k = kindOfValue(value);
            if (k == "zero")
                k = kind.empty() ? "zero" : (kind == "length" || kind == "number" ? kind : "zero");
            if (kind.empty() || kind == "zero")
                kind = k;
            else if (k != kind && k != "zero")
                return "mixed";
        }
        if (kind == "zero")
            kind = "length";
        return kind;
    }

}

bool props::isColor(Session& session, const ComponentValue& value){
    if (value.isToken(TokenType::Hash))
        return true;
    if (isVar(value))
        return varKind(session, value) == "color";
    return false;
}

bool props::isLength(Session& session, const ComponentValue& value, bool allowPercent){
    if (value.isToken(TokenType::Dimension)){
        const UnitCategory category = unitCategory(value.token.unit);
        return isLengthCategory(category);
    }
    if (value.isToken(TokenType::Number))
        return value.token.number == 0;
    if (value.isToken(TokenType::Percentage))
        return allowPercent;
    if (isVar(value)){
        const std::string kind = varKind(session, value);
        return kind == "length" || (allowPercent && kind == "percentage");
    }
    return false;
}

bool props::isNumber(const ComponentValue& value){
    return value.isToken(TokenType::Number);
}

bool props::isTime(Session& session, const ComponentValue& value){
    if (value.isToken(TokenType::Dimension))
        return unitCategory(value.token.unit) == UnitCategory::Time;
    if (isVar(value))
        return varKind(session, value) == "time";
    return false;
}

std::optional<double> props::lengthPx(const ComponentValue& value){
    if (value.isToken(TokenType::Number) && value.token.number == 0)
        return 0.0;
    if (value.isToken(TokenType::Dimension) && unitCategory(value.token.unit) == UnitCategory::AbsoluteLength)
        return value.token.number * canonicalFactor(value.token.unit).value_or(1.0);
    return std::nullopt;
}

std::string props::lengthText(const ComponentValue& value){
    if (value.isToken(TokenType::Number) && value.token.number == 0)
        return "0px";
    return serialize(value);
}

std::optional<std::string> props::urlOf(const ComponentValue& value){
    if (value.isToken(TokenType::Url) || value.isToken(TokenType::String))
        return value.token.value;
    if (value.isFunction("url") || value.isFunction("src")){
        for (const ComponentValue& c : value.children){
            if (c.isToken(TokenType::String))
                return c.token.value;
        }
        return std::string();
    }
    return std::nullopt;
}

std::string props::resolveUrlIn(Session& session, const std::string& url, uint32_t file, SourceRef ref, const char* kind){
    const std::string fromPath = session.files.path(file);
    const ResolvedUrl resolved = session.urls.resolve(url, fromPath, kind, ref, session.sink);
    if (resolved.kind == UrlKind::Remote){
        RemoteReference reference;
        reference.url = url;
        reference.kind = kind;
        reference.ref = ref;
        session.result->remoteRefs.push_back(reference);
        if (resolved.entry)
            session.result->dependencies.push_back(ImportManifest::kPath);
    }
    if (resolved.dropped())
        return std::string();
    if (resolved.kind == UrlKind::Data && resolved.data){
        session.result->virtualFiles.emplace_back(resolved.assetPath, resolved.data);
    }else if (resolved.local() && !resolved.assetPath.empty()){
        const std::string extension = lowerAscii(resolved.assetPath.substr(resolved.assetPath.rfind('.') == std::string::npos ? resolved.assetPath.size()
                                                                                                                                 : resolved.assetPath.rfind('.')));
        if (std::strcmp(kind, "font") == 0 || extension == ".svg")
            session.result->dependencies.push_back(resolved.assetPath);
    }
    return resolved.emitted;
}

std::string props::resolveUrl(DeclContext& context, const ComponentValue& value, const char* kind){
    std::optional<std::string> url = urlOf(value);
    if (!url)
        return std::string();
    const SourceRef ref = value.length > 0 ? SourceRef{context.declaration.ref.file, value.offset, value.length} : context.valueRef();
    return resolveUrlIn(context.session, *url, context.declaration.ref.file, ref, kind);
}

std::optional<TypedVar> props::wholeVar(DeclContext& context, const std::string& family){
    if (context.value.size() != 1 || !isVar(context.value[0]))
        return std::nullopt;
    std::optional<TypedVar> typed = context.session.typedVar(context.value[0], family);
    if (!typed){
        context.reportAs(Severity::Warning, codes::VarUntyped, "'" + context.sourceText() +
                         "': the variable has no declaration in the document, or declarations of different kinds; written as is",
                         "declare the variable in the CSS with one kind of value");
    }
    return typed;
}

std::optional<ValueList> props::ruleValue(Session& session, const FlatRule& rule, const std::string& property){
    std::optional<ValueList> result;
    for (const Declaration& d : rule.declarations){
        if (d.name != property)
            continue;
        NormalizeContext context;
        context.property = property;
        context.colors = ValueNormalizer::colorKeywordsApply(property);
        context.options = session.options;
        context.resolveVar = [&session](const ComponentValue& var, ValueList& out){ return session.staticVar(var, out); };
        NormalizeResult normalized = ValueNormalizer::normalize(d.value, context);
        if (normalized.ok)
            result = trimWhitespace(normalized.value);
    }
    return result;
}

const std::string& props::ruleStaticColor(Session& session, const FlatRule& rule){
    if (session.staticColors.size() < session.rules.size())
        session.staticColors.resize(session.rules.size());
    if (rule.index < session.staticColors.size() && session.staticColors[rule.index])
        return *session.staticColors[rule.index];
    std::string color;
    std::optional<ValueList> value = ruleValue(session, rule, "color");
    if (value && value->size() == 1 && (*value)[0].isToken(TokenType::Hash))
        color = "#" + (*value)[0].token.value;
    if (rule.index < session.staticColors.size()){
        session.staticColors[rule.index] = std::move(color);
        return *session.staticColors[rule.index];
    }
    session.scratchColor = std::move(color);
    return session.scratchColor;
}

std::optional<double> props::ruleFontSizePx(Session& session, const FlatRule& rule){
    if (session.fontSizes.size() < session.rules.size())
        session.fontSizes.resize(session.rules.size());
    if (rule.index < session.fontSizes.size() && session.fontSizes[rule.index])
        return *session.fontSizes[rule.index];
    std::optional<double> px;
    std::optional<ValueList> value = ruleValue(session, rule, "font-size");
    if (value && value->size() == 1){
        px = lengthPx((*value)[0]);
        if (!px && (*value)[0].isToken(TokenType::Dimension) && equalsNoCase((*value)[0].token.unit, "rem"))
            px = (*value)[0].token.number * session.rootFontSize.value_or(16.0);
        if (!px && (*value)[0].isToken(TokenType::Ident)){
            static const std::pair<const char*, double> keywords[] = {{"xx-small", 9}, {"x-small", 10}, {"small", 13}, {"medium", 16},
                                                                      {"large", 18}, {"x-large", 24}, {"xx-large", 32}, {"xxx-large", 48}};
            for (const auto& k : keywords){
                if (equalsNoCase((*value)[0].token.value, k.first))
                    px = k.second;
            }
        }
    }
    if (rule.index < session.fontSizes.size())
        session.fontSizes[rule.index] = px;
    return px;
}

std::string props::physicalSide(Session& session, const std::string& logical){
    const bool rtl = session.rootDirection == "rtl";
    if (logical == "inline-start" || logical == "start")
        return rtl ? "right" : "left";
    if (logical == "inline-end" || logical == "end")
        return rtl ? "left" : "right";
    if (logical == "block-start")
        return "top";
    if (logical == "block-end")
        return "bottom";
    return logical;
}

bool props::accepts(const std::string& property, const std::string& value){
    return RcssGrammar::accepts(property, value);
}

bool jasperengine::htmlui::compat::reachesRewrite(Session& session, const Declaration& declaration, const std::vector<Declaration>& block){
    if (!CascadeKeywords::keywordOf(declaration.value).empty())
        return false;
    const PropertySupport* row = findProperty(declaration.name);
    if (!row){
        // a vendor prefix is read as the standard property unless the block has that too
        for (const char* prefix : {"-webkit-", "-moz-", "-ms-", "-o-"}){
            const std::string p(prefix);
            if (declaration.name.compare(0, p.size(), p) != 0)
                continue;
            const std::string base = declaration.name.substr(p.size());
            row = findProperty(base);
            if (!row)
                continue;
            for (const Declaration& other : block){
                if (other.name == base)
                    return false;
            }
            break;
        }
    }
    if (!row)
        return false;
    const Support support = effectiveSupport(*row, session.options.fork);
    return support != Support::Unsupported && support != Support::Ignored;
}
