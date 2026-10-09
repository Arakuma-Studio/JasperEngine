// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Transitions (phase-2 §3.9.7 "Transitions"): RmlUi reads seconds only, tween keywords only,
// property names it knows, and `all` alone. CSS transitions are rewritten entry by entry:
// property names mapped to the RCSS properties compat writes, ms to s, CSS easing to the
// nearest tween until the fork has TimingFunctions; longhands merge with the rule's shorthand
// or a base rule.

#include "htmlui/compat/css/props/Rewriters.h"

#include "htmlui/compat/css/BaseRules.h"
#include "htmlui/compat/css/CssParser.h"
#include "htmlui/compat/css/CssSerialize.h"
#include "htmlui/compat/css/Easing.h"
#include "htmlui/compat/css/RcssGrammar.h"
#include "htmlui/compat/css/Units.h"
#include "htmlui/compat/css/ValueNormalizer.h"

#include <set>

using namespace jasperengine::htmlui::compat;
using namespace jasperengine::htmlui::compat::props;

namespace jasperengine::htmlui::compat::props {

    std::optional<EasingResult> easingOf(Session& session, const ComponentValue& v){
        EasingResult result;
        const bool fork = session.options.fork.has(ForkFeature::TimingFunctions);
        if (v.isToken(TokenType::Ident)){
            const std::string k = lowerAscii(v.token.value);
            if (fork && (k == "linear" || k == "ease" || k == "ease-in" || k == "ease-out" || k == "ease-in-out" || k == "step-start" ||
                         k == "step-end")){
                result.text = k;
                return result;
            }
            if (std::optional<std::string> tween = Easing::keywordTween(k)){
                result.text = *tween;
                return result;
            }
            if (k == "step-start" || k == "step-end"){
                result.text = "linear-in-out";
                result.code = codes::StepsApproximated;
                result.severity = Severity::Warning;
                result.note = "'" + k + "' is drawn as a linear easing until the fork has TimingFunctions";
                return result;
            }
            // RmlUi's own tween names are accepted as an engine extension
            for (const char* name : Easing::tweenNames()){
                if (k == name){
                    result.text = k;
                    return result;
                }
            }
            return std::nullopt;
        }
        if (v.kind != ComponentValue::Kind::Function)
            return std::nullopt;
        const std::string name = lowerAscii(v.name());
        if (name != "cubic-bezier" && name != "steps" && name != "linear")
            return std::nullopt;
        if (fork){
            result.text = serialize(v);
            return result;
        }
        if (name == "steps"){
            result.text = "linear-in-out";
            result.code = codes::StepsApproximated;
            result.severity = Severity::Warning;
            result.note = "'" + serialize(v) + "' is drawn as a linear easing until the fork has TimingFunctions";
            return result;
        }
        std::vector<std::vector<ComponentValue>> args = commaGroups(v.children);
        if (name == "cubic-bezier"){
            if (args.size() != 4)
                return std::nullopt;
            double p[4];
            for (int i = 0; i < 4; i++){
                if (args[(size_t)i].size() != 1 || !args[(size_t)i][0].isToken(TokenType::Number))
                    return std::nullopt;
                p[i] = args[(size_t)i][0].token.number;
            }
            if (p[0] < 0 || p[0] > 1 || p[2] < 0 || p[2] > 1)
                return std::nullopt;
            const TweenChoice choice = Easing::nearestTween([&](double t){ return Easing::cubicBezier(p[0], p[1], p[2], p[3], t); });
            result.text = choice.name;
            result.code = codes::EasingApproximated;
            result.note = "'" + serialize(v) + "' is drawn with the nearest tween '" + choice.name + "' (maximum error " +
                          formatNumber(choice.error, 3) + ") until the fork has TimingFunctions";
            return result;
        }
        // linear(): stops "<output> [<input>%]{0,2}"
        std::vector<std::pair<double, double>> points;
        std::vector<std::pair<double, std::optional<double>>> raw;
        for (const std::vector<ComponentValue>& arg : args){
            std::vector<ComponentValue> w = words(arg);
            if (w.empty() || !w[0].isToken(TokenType::Number))
                return std::nullopt;
            const double output = w[0].token.number;
            if (w.size() == 1)
                raw.emplace_back(output, std::nullopt);
            for (size_t i = 1; i < w.size(); i++){
                if (!w[i].isToken(TokenType::Percentage))
                    return std::nullopt;
                raw.emplace_back(output, w[i].token.number / 100.0);
            }
        }
        if (raw.size() < 2)
            return std::nullopt;
        if (!raw.front().second)
            raw.front().second = 0.0;
        if (!raw.back().second)
            raw.back().second = 1.0;
        for (size_t i = 0; i < raw.size(); i++){
            if (raw[i].second)
                continue;
            size_t j = i;
            while (j < raw.size() && !raw[j].second)
                j++;
            const double start = *raw[i - 1].second, end = *raw[j].second;
            for (size_t k = i; k < j; k++)
                raw[k].second = start + (end - start) * (double)(k - i + 1) / (double)(j - i + 1);
        }
        for (const auto& r : raw)
            points.emplace_back(*r.second, r.first);
        const TweenChoice choice = Easing::nearestTween([&](double t){ return Easing::linearFunction(points, t); });
        result.text = choice.name;
        result.code = codes::EasingApproximated;
        result.note = "'linear(...)' is drawn with the nearest tween '" + choice.name + "' (maximum error " + formatNumber(choice.error, 3) +
                      ") until the fork has TimingFunctions";
        return result;
    }

}

namespace {

    void unsupportedValue(DeclContext& context, const std::string& suggestion = std::string()){
        context.reportAs(Severity::Warning, codes::UnsupportedValue, "'" + context.name + ": " + context.valueText() +
                         "' is not supported; declaration dropped", suggestion);
        context.dropped = true;
    }

    // RmlUi shorthands a transition may name
    bool rcssShorthand(const std::string& name){
        static const std::set<std::string> names = {"margin", "padding", "border-width", "border-color", "border-top", "border-right",
                                                    "border-bottom", "border-left", "border", "border-radius", "inset", "gap", "flex",
                                                    "flex-flow", "overflow", "perspective-origin", "transform-origin", "font", "background",
                                                    "nav"};
        return names.count(name) > 0;
    }

    // the RCSS properties a CSS property name transitions; empty when none
    std::vector<std::string> transitionTargets(const std::string& css){
        std::string name = css;
        for (const char* prefix : {"-webkit-", "-moz-"}){
            const std::string p(prefix);
            if (name.compare(0, p.size(), p) == 0 && (findProperty(name.substr(p.size())) || RcssGrammar::known(name.substr(p.size()))))
                name = name.substr(p.size());
        }
        if (name == "background")
            return {"background-color", "decorator"};
        if (name.compare(0, 11, "background-") == 0 && name != "background-color")
            return {"decorator"};
        if (name.compare(0, 7, "outline") == 0 || name == "box-shadow")
            return {"box-shadow"};
        if (name == "text-shadow" || name.compare(0, 19, "-webkit-text-stroke") == 0)
            return {"font-effect"};
        if (name == "translate" || name == "rotate" || name == "scale")
            return {"transform"};
        if (name == "clip-path" || name == "mask" || name == "mask-image")
            return {"mask-image"};
        if (name == "direction")
            return {"-rmlui-direction"};
        if (name == "grid-gap" || name == "grid-row-gap" || name == "grid-column-gap")
            return {name.substr(5)};
        if (name == "inset-inline" || name == "inset-block" || name == "margin-inline" || name == "margin-block" || name == "padding-inline" ||
            name == "padding-block")
            return {};
        if (name.size() > 2 && name[0] == '-' && name[1] == '-')
            return {};
        if (rcssShorthand(name) || RcssGrammar::known(name))
            return {name};
        return {};
    }

    struct Entry {
        std::string property = "all";      // CSS name ("all", "none")
        std::string duration;              // RCSS seconds text ("" = 0s)
        double durationSeconds = 0;
        std::string delay;
        std::optional<props::EasingResult> easing;
        bool varTiming = false;            // a var() in the timing
        std::string raw;                   // the entry as written (messages)
    };

    bool seconds(const ComponentValue& v, double& value){
        if (v.isToken(TokenType::Dimension) && equalsNoCase(v.token.unit, "s")){
            value = v.token.number;
            return true;
        }
        if (v.isToken(TokenType::Number) && v.token.number == 0){
            value = 0;
            return true;
        }
        return false;
    }

    // one shorthand entry; false when malformed
    bool parseEntry(Session& session, const std::vector<ComponentValue>& group, Entry& entry){
        entry.raw = text(group);
        bool propertySet = false, durationSet = false, delaySet = false;
        for (const ComponentValue& v : words(group)){
            double s = 0;
            if (seconds(v, s)){
                if (!durationSet){
                    durationSet = true;
                    entry.durationSeconds = s;
                    entry.duration = formatNumber(s) + "s";
                }else if (!delaySet){
                    delaySet = true;
                    entry.delay = formatNumber(s) + "s";
                }else{
                    return false;
                }
                continue;
            }
            if (isVar(v)){
                const std::string kind = varKind(session, v);
                if (kind == "time"){
                    entry.varTiming = true;
                    if (!durationSet){
                        durationSet = true;
                        entry.duration = text(v);
                        entry.durationSeconds = 1;
                    }else{
                        delaySet = true;
                        entry.delay = text(v);
                    }
                    continue;
                }
                std::optional<TypedVar> typed = session.typedVar(v, "easing");
                if (typed){
                    props::EasingResult e;
                    e.text = typed->reference;
                    entry.easing = e;
                    continue;
                }
                return false;
            }
            if (std::optional<props::EasingResult> e = props::easingOf(session, v)){
                if (entry.easing)
                    return false;
                entry.easing = e;
                continue;
            }
            if (v.isToken(TokenType::Ident)){
                const std::string k = lowerAscii(v.token.value);
                if (k == "allow-discrete" || k == "normal")
                    continue;
                if (propertySet)
                    return false;
                propertySet = true;
                entry.property = v.token.value;
                if (k == "all" || k == "none")
                    entry.property = k;
                continue;
            }
            return false;
        }
        return true;
    }

    // entries -> the RCSS value; "" when nothing remains
    std::string writeEntries(DeclContext* context, Session& session, std::vector<Entry>& entries, bool fork, bool report){
        std::vector<std::string> out;
        // `all` cannot be combined with other entries in RmlUi
        const Entry* all = nullptr;
        for (const Entry& e : entries){
            if (e.property == "all")
                all = &e;
        }
        if (all && entries.size() > 1 && !fork){
            if (report && context)
                context->reportAs(Severity::Info, codes::ApproximatedMinor, "'transition: all' with other entries becomes one 'all' entry with the timing of '" +
                                  all->raw + "' (RmlUi limitation)");
            Entry only = *all;
            entries = {only};
        }
        for (Entry& e : entries){
            if (e.property == "none"){
                if (entries.size() == 1)
                    return "none";
                continue;
            }
            if (!e.varTiming && e.durationSeconds <= 0)
                continue;   // a 0s transition does nothing
            std::vector<std::string> targets;
            if (e.property == "all"){
                targets.push_back("all");
            }else if (fork){
                targets.push_back(e.property);
            }else{
                targets = transitionTargets(lowerAscii(e.property));
                if (targets.empty()){
                    if (report && context)
                        context->reportAs(Severity::Info, codes::ApproximatedMinor, "transition entry '" + e.raw +
                                          "' names a property the engine does not transition; entry dropped", std::string(), e.raw);
                    continue;
                }
            }
            std::string easing = e.easing ? e.easing->text : (fork ? std::string() : std::string("quadratic-out"));
            if (e.easing && report && context && !e.easing->note.empty())
                context->reportAs(e.easing->severity, e.easing->code, e.easing->note, std::string(), e.raw);
            std::string entry = join(targets) + " " + e.duration;
            if (!easing.empty())
                entry += " " + easing;
            if (!e.delay.empty() && e.delay != "0s")
                entry += " " + e.delay;
            out.push_back(entry);
        }
        (void)session;
        return join(out, ", ");
    }

    // the declarations merged into one transition (transition-behavior is Ignored in the table:
    // it never reaches the merge, so it cannot be the family's last declaration)
    bool isTransitionFamily(const std::string& name){
        return name == "transition" || name == "transition-property" || name == "transition-duration" || name == "transition-timing-function" ||
               name == "transition-delay" || name == "-webkit-transition" ||
               name == "-webkit-transition-property" || name == "-webkit-transition-duration" ||
               name == "-webkit-transition-timing-function" || name == "-webkit-transition-delay";
    }

    std::string plain(const std::string& name){
        return name.compare(0, 8, "-webkit-") == 0 ? name.substr(8) : name;
    }

    // applies one declaration of the family to the entries; false when malformed
    bool applyTransition(Session& session, const std::string& rawName, const std::vector<ComponentValue>& value, std::vector<Entry>& entries){
        const std::string name = plain(rawName);
        std::vector<std::vector<ComponentValue>> groups = commaGroups(value);
        if (name == "transition"){
            entries.clear();
            for (const std::vector<ComponentValue>& group : groups){
                Entry e;
                if (!parseEntry(session, group, e))
                    return false;
                entries.push_back(e);
            }
            return true;
        }
        if (name == "transition-behavior")
            return true;
        if (name == "transition-property"){
            std::vector<Entry> previous = entries;
            entries.clear();
            for (size_t i = 0; i < groups.size(); i++){
                Entry e = previous.empty() ? Entry() : previous[i % previous.size()];
                std::vector<ComponentValue> w = words(groups[i]);
                if (w.size() != 1 || !w[0].isToken(TokenType::Ident))
                    return false;
                e.property = w[0].token.value;
                const std::string k = lowerAscii(e.property);
                if (k == "all" || k == "none")
                    e.property = k;
                e.raw = text(groups[i]);
                entries.push_back(e);
            }
            return true;
        }
        if (entries.empty())
            entries.push_back(Entry());
        for (size_t i = 0; i < entries.size(); i++){
            std::vector<ComponentValue> w = words(groups[i % groups.size()]);
            if (w.size() != 1)
                return false;
            Entry& e = entries[i];
            double s = 0;
            if (name == "transition-duration" || name == "transition-delay"){
                std::string written;
                if (seconds(w[0], s)){
                    written = formatNumber(s) + "s";
                }else if (isVar(w[0]) && varKind(session, w[0]) == "time"){
                    written = text(w[0]);
                    s = 1;
                    e.varTiming = true;
                }else{
                    return false;
                }
                if (name == "transition-duration"){
                    e.duration = written;
                    e.durationSeconds = s;
                }else{
                    e.delay = written;
                }
            }else if (name == "transition-timing-function"){
                if (isVar(w[0])){
                    std::optional<TypedVar> typed = session.typedVar(w[0], "easing");
                    if (!typed)
                        return false;
                    props::EasingResult r;
                    r.text = typed->reference;
                    e.easing = r;
                    continue;
                }
                std::optional<props::EasingResult> easing = props::easingOf(session, w[0]);
                if (!easing)
                    return false;
                e.easing = easing;
            }
        }
        return true;
    }

    std::vector<ComponentValue> normalizedValue(Session& session, const Declaration& d){
        NormalizeContext context;
        context.property = plain(d.name);
        context.colors = false;
        context.options = session.options;
        context.resolveVar = [&session](const ComponentValue& var, std::vector<ComponentValue>& out){ return session.staticVar(var, out); };
        NormalizeResult result = ValueNormalizer::normalize(d.value, context);
        return result.ok ? trimWhitespace(result.value) : std::vector<ComponentValue>();
    }

    std::string classifyTransition(Session& session, const std::vector<ComponentValue>& value){
        std::vector<Entry> entries;
        return applyTransition(session, "transition", value, entries) ? "transitions" : std::string();
    }

    std::optional<std::string> transformTransition(Session& session, const std::vector<ComponentValue>& value, const std::string&, SourceRef){
        std::vector<Entry> entries;
        if (!applyTransition(session, "transition", value, entries))
            return std::nullopt;
        std::string text = writeEntries(nullptr, session, entries, session.options.fork.has(ForkFeature::TimingFunctions), false);
        return text.empty() ? std::string("none") : text;
    }

    std::string classifyEasing(Session& session, const std::vector<ComponentValue>& value){
        std::vector<ComponentValue> w = words(value);
        return w.size() == 1 && props::easingOf(session, w[0]) ? "easing" : std::string();
    }

    std::optional<std::string> transformEasing(Session& session, const std::vector<ComponentValue>& value, const std::string&, SourceRef){
        std::vector<ComponentValue> w = words(value);
        if (w.size() != 1)
            return std::nullopt;
        std::optional<props::EasingResult> e = props::easingOf(session, w[0]);
        if (!e)
            return std::nullopt;
        return e->text;
    }

}

const VarFamily jasperengine::htmlui::compat::kTransitionFamily = {"transition", classifyTransition, transformTransition};
const VarFamily jasperengine::htmlui::compat::kEasingFamily = {"easing", classifyEasing, transformEasing};

void jasperengine::htmlui::compat::rewriteTransition(DeclContext& context){
    rewriteTransitionLonghand(context);
}

void jasperengine::htmlui::compat::rewriteTransitionLonghand(DeclContext& context){
    const bool fork = context.fork(ForkFeature::TimingFunctions);
    if (context.keyframe){
        context.dropped = true;
        return;
    }
    std::vector<Entry> entries;
    if (context.value.size() == 1 && isVar(context.value[0]) && plain(context.name) == "transition"){
        std::optional<TypedVar> typed = wholeVar(context, "transition");
        context.emit("transition", typed ? typed->reference : context.valueText());
        return;
    }
    if (context.rule){
        const Declaration* last = nullptr;
        bool hasShorthand = false;
        for (const Declaration& d : context.rule->declarations){
            if (isTransitionFamily(d.name) && reachesRewrite(context.session, d, context.rule->declarations))
                last = &d;
            if (plain(d.name) == "transition" || plain(d.name) == "transition-property")
                hasShorthand = true;
        }
        if (last && last->ref != context.declaration.ref)
            return;
        if (!hasShorthand){
            const FlatRule* base = BaseRules::find(context.session, *context.rule, [](const FlatRule& candidate){
                for (const Declaration& d : candidate.declarations){
                    if (plain(d.name) == "transition" || plain(d.name) == "transition-property")
                        return true;
                }
                return false;
            });
            if (!base){
                context.reportAs(Severity::Warning, codes::LonghandWithoutBase, "'" + context.name +
                                 "' without transition or transition-property in the rule or a base rule is dropped",
                                 "write the transition shorthand in this rule");
                context.dropped = true;
                return;
            }
            for (const Declaration& d : base->declarations){
                if (isTransitionFamily(d.name) && reachesRewrite(context.session, d, base->declarations))
                    applyTransition(context.session, d.name, normalizedValue(context.session, d), entries);
            }
            context.reportAs(Severity::Info, codes::MergedFromBaseRule, "transition longhands are merged with the base rule at " +
                             BaseRules::location(context.session, *base));
        }
        for (const Declaration& d : context.rule->declarations){
            if (!isTransitionFamily(d.name) || !reachesRewrite(context.session, d, context.rule->declarations))
                continue;
            const std::vector<ComponentValue> value = d.ref == context.declaration.ref ? context.value : normalizedValue(context.session, d);
            if (!applyTransition(context.session, d.name, value, entries)){
                context.reportAt(d.ref, Severity::Warning, codes::UnsupportedValue, "'" + d.name + ": " + text(value) + "' is not supported; dropped",
                                 "write transition: <property> <duration> <easing> <delay>");
            }
        }
    }else if (!applyTransition(context.session, context.name, context.value, entries)){
        unsupportedValue(context, "write transition: <property> <duration> <easing> <delay>");
        return;
    }
    const std::string value = writeEntries(&context, context.session, entries, fork, true);
    context.emit("transition", value.empty() ? "none" : value);
}
