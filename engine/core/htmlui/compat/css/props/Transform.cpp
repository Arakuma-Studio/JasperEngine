// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Transforms (phase-2 §3.9.7 "Transforms"): RmlUi has no individual translate / rotate /
// scale properties; they combine into transform in CSS order (translate, rotate, scale, then
// the transform list), with the rule's or a base rule's transform.

#include "htmlui/compat/css/props/Rewriters.h"
#include "htmlui/compat/LiteralCompare.h"

#include "htmlui/compat/css/BaseRules.h"
#include "htmlui/compat/css/CssParser.h"
#include "htmlui/compat/css/CssSerialize.h"
#include "htmlui/compat/css/StaticCalc.h"
#include "htmlui/compat/css/Units.h"
#include "htmlui/compat/css/ValueNormalizer.h"

using namespace jasperengine::htmlui::compat;
using namespace jasperengine::htmlui::compat::props;

namespace {

    bool isIndividual(std::string_view name){
        return name == "translate" || name == "rotate" || name == "scale";
    }

    std::string number(const ComponentValue& v){
        if (v.isToken(TokenType::Percentage))
            return formatNumber(v.token.number / 100.0);
        return text(v);
    }

    // calc(), min(), max(), clamp() kept for the fork (its Calc and IndividualTransforms; checked by the normalizer)
    bool isCalc(const ComponentValue& v){
        return v.kind == ComponentValue::Kind::Function && StaticCalc::isCalcFamily(lowerAscii(v.name().view()));
    }

    // the transform function of an individual property; "" for none, nullopt when invalid
    std::optional<std::string> functionOf(Session& session, const std::string& property, const ValueList& value){
        ValueList w = words(value);
        if (w.size() == 1 && isIdent(w[0], "none"))
            return std::string();
        if (property == "translate"){
            for (const ComponentValue& v : w){
                if (!isLength(session, v, true) && !isVar(v) && !isCalc(v))
                    return std::nullopt;
            }
            if (w.size() == 1)
                return "translate(" + lengthText(w[0]) + ", 0px)";
            if (w.size() == 2)
                return "translate(" + lengthText(w[0]) + ", " + lengthText(w[1]) + ")";
            if (w.size() == 3)
                return "translate3d(" + lengthText(w[0]) + ", " + lengthText(w[1]) + ", " + lengthText(w[2]) + ")";
            return std::nullopt;
        }
        if (property == "rotate"){
            auto angle = [](const ComponentValue& v){
                return (v.isToken(TokenType::Dimension) && unitCategory(v.token.unit) == UnitCategory::Angle) || isCalc(v) ||
                       (v.isToken(TokenType::Number) && v.token.number == 0);
            };
            auto angleText = [](const ComponentValue& v){ return v.isToken(TokenType::Number) ? std::string("0deg") : text(v); };
            if (w.size() == 1 && (angle(w[0]) || isVar(w[0])))
                return "rotate(" + angleText(w[0]) + ")";
            if (w.size() == 2 && angle(w[1])){
                const std::string axis = identOf(w[0]);
                if (axis == "x")
                    return "rotateX(" + angleText(w[1]) + ")";
                if (axis == "y")
                    return "rotateY(" + angleText(w[1]) + ")";
                if (axis == "z")
                    return "rotateZ(" + angleText(w[1]) + ")";
                return std::nullopt;
            }
            if (w.size() == 4 && angle(w[3]) && w[0].isToken(TokenType::Number) && w[1].isToken(TokenType::Number) && w[2].isToken(TokenType::Number))
                return "rotate3d(" + text(w[0]) + ", " + text(w[1]) + ", " + text(w[2]) + ", " + angleText(w[3]) + ")";
            return std::nullopt;
        }
        // scale
        for (const ComponentValue& v : w){
            if (!v.isToken(TokenType::Number) && !v.isToken(TokenType::Percentage) && !isVar(v) && !isCalc(v))
                return std::nullopt;
        }
        if (w.size() == 1)
            return "scale(" + number(w[0]) + ")";
        if (w.size() == 2)
            return "scale(" + number(w[0]) + ", " + number(w[1]) + ")";
        if (w.size() == 3)
            return "scale3d(" + number(w[0]) + ", " + number(w[1]) + ", " + number(w[2]) + ")";
        return std::nullopt;
    }

    ValueList normalized(Session& session, const Declaration& d){
        NormalizeContext context;
        context.options = &session.options;
        context.resolveVar = [&session](const ComponentValue& var, ValueList& out){ return session.staticVar(var, out); };
        NormalizeResult result = ValueNormalizer::normalize(d.value, context);
        return result.ok ? trimWhitespace(result.value) : ValueList();
    }

}

bool jasperengine::htmlui::compat::ruleHasIndividualTransform(const FlatRule& rule){
    for (const Declaration& d : rule.declarations){
        if (isIndividual(d.name))
            return true;
    }
    return false;
}

void jasperengine::htmlui::compat::rewriteIndividualTransform(DeclContext& context){
    if (context.fork(ForkFeature::IndividualTransforms)){
        // the fork has translate / rotate / scale (phase 3): written as they are, checked as phase 2 reads them
        // (an angle after the axis of 'rotate' read either way, as CSS allows)
        ValueList value = context.value;
        ValueList w = words(value);
        if (context.name == "rotate" && w.size() == 2 && !w[0].isToken(TokenType::Ident) && w[1].isToken(TokenType::Ident))
        {
            value.clear();
            value.push_back(w[1]);
            value.push_back(ValueNormalizer::whitespace());
            value.push_back(w[0]);
        }
        bool hasVar = false;
        for (const ComponentValue& v : context.value)
            hasVar = hasVar || isVar(v);
        if (!hasVar && !functionOf(context.session, context.name, value)){
            context.reportAs(Severity::Warning, codes::UnsupportedValue, "'" + context.sourceText() + "' is not supported; declaration dropped");
            context.dropped = true;
            return;
        }
        context.emit(context.name, context.valueText());
        return;
    }
    std::string translate, rotate, scale, transform;
    bool transformSet = false;
    auto apply = [&](const std::string& name, const ValueList& value) -> bool {
        if (name == "transform"){
            transform = text(value);
            if (transform == "none")
                transform.clear();
            transformSet = true;
            return true;
        }
        std::optional<std::string> function = functionOf(context.session, name, value);
        if (!function)
            return false;
        (name == "translate" ? translate : name == "rotate" ? rotate : scale) = *function;
        return true;
    };
    if (context.rule){
        const Declaration* last = nullptr;
        for (const Declaration& d : context.rule->declarations){
            if (isIndividual(d.name))
                last = &d;
        }
        if (last && last->ref != context.declaration.ref)
            return;
        bool hasTransform = false;
        for (const Declaration& d : context.rule->declarations){
            if (d.name == "transform")
                hasTransform = true;
        }
        if (!hasTransform){
            const FlatRule* base = BaseRules::find(context.session, *context.rule, [](const FlatRule& candidate){
                for (const Declaration& d : candidate.declarations){
                    if (d.name == "transform" || isIndividual(d.name))
                        return true;
                }
                return false;
            });
            if (base){
                for (const Declaration& d : base->declarations){
                    if (d.name == "transform" || isIndividual(d.name))
                        apply(d.name, normalized(context.session, d));
                }
                context.reportAs(Severity::Info, codes::MergedFromBaseRule, "'" + context.name + "' is combined with the transform of the base rule at " +
                                 BaseRules::location(context.session, *base));
            }
        }
        for (const Declaration& d : context.rule->declarations){
            if (d.name != "transform" && !isIndividual(d.name))
                continue;
            const ValueList value = d.ref == context.declaration.ref ? context.value : normalized(context.session, d);
            if (!apply(d.name, value))
                context.reportAt(d.ref, Severity::Warning, codes::UnsupportedValue, "'" + d.name + ": " + text(value) + "' is not supported; dropped");
        }
    }else if (!apply(context.name, context.value)){
        context.reportAs(Severity::Warning, codes::UnsupportedValue, "'" + context.sourceText() + "' is not supported; declaration dropped");
        context.dropped = true;
        return;
    }
    (void)transformSet;
    const std::string combined = join({translate, rotate, scale, transform});
    context.emit("transform", combined.empty() ? "none" : combined);
}
