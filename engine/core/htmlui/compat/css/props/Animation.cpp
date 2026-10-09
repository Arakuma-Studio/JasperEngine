// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Animations (phase-2 §3.9.7 "Animations"): written in the order RmlUi reads
// (<duration>s [<delay>s] [<count>|infinite] [alternate] [paused] <tween> <name>), reverse
// directions through mirrored keyframe copies (<name>-rev), fill modes through the
// AnimationFill runtime (-html-animation-fill), longhands merged with the rule or a base rule.

#include "htmlui/compat/css/props/Rewriters.h"

#include "htmlui/compat/css/BaseRules.h"
#include "htmlui/compat/css/CssParser.h"
#include "htmlui/compat/css/CssSerialize.h"
#include "htmlui/compat/css/Units.h"
#include "htmlui/compat/css/ValueNormalizer.h"

#include <cmath>
#include <set>

using namespace jasperengine::htmlui::compat;
using namespace jasperengine::htmlui::compat::props;

namespace {

    struct Entry {
        std::string name;                  // CSS keyframes name ("" = none)
        std::string duration;              // RCSS text
        double durationSeconds = 0;
        bool durationVar = false;
        std::string delay;
        std::optional<EasingResult> easing;
        std::string count;                 // "" = 1, "infinite", or a number
        double countValue = 1;
        std::string direction = "normal";
        std::string fill = "none";
        bool paused = false;
        std::string raw;
    };

    bool seconds(const ComponentValue& v, double& value){
        if (v.isToken(TokenType::Dimension) && equalsNoCase(v.token.unit, "s")){
            value = v.token.number;
            return true;
        }
        return false;
    }

    const std::set<std::string>& directions(){
        static const std::set<std::string> d = {"normal", "reverse", "alternate", "alternate-reverse"};
        return d;
    }
    const std::set<std::string>& fills(){
        static const std::set<std::string> f = {"none", "forwards", "backwards", "both"};
        return f;
    }

    bool parseEntry(Session& session, const std::vector<ComponentValue>& group, Entry& entry){
        entry.raw = text(group);
        bool durationSet = false, delaySet = false, countSet = false, directionSet = false, fillSet = false, stateSet = false;
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
            if (isVar(v) && varKind(session, v) == "time"){
                if (!durationSet){
                    durationSet = true;
                    entry.duration = text(v);
                    entry.durationVar = true;
                }else{
                    delaySet = true;
                    entry.delay = text(v);
                }
                continue;
            }
            if (v.isToken(TokenType::Number) && !countSet){
                countSet = true;
                entry.countValue = v.token.number;
                entry.count = text(v);
                continue;
            }
            if (std::optional<EasingResult> e = easingOf(session, v)){
                if (entry.easing)
                    return false;
                entry.easing = e;
                continue;
            }
            if (isVar(v)){
                std::optional<TypedVar> typed = session.typedVar(v, "easing");
                if (!typed)
                    return false;
                EasingResult r;
                r.text = typed->reference;
                entry.easing = r;
                continue;
            }
            const std::string k = identOf(v);
            if (k == "infinite" && !countSet){
                countSet = true;
                entry.count = "infinite";
                continue;
            }
            if (directions().count(k) && !directionSet){
                directionSet = true;
                entry.direction = k;
                continue;
            }
            if (fills().count(k) && !fillSet){
                // "none" is read as the fill mode; a name-less entry is no animation
                fillSet = true;
                entry.fill = k;
                continue;
            }
            if ((k == "running" || k == "paused") && !stateSet){
                stateSet = true;
                entry.paused = k == "paused";
                continue;
            }
            if (v.isToken(TokenType::Ident) || v.isToken(TokenType::String)){
                if (!entry.name.empty())
                    return false;
                entry.name = v.token.value;
                continue;
            }
            return false;
        }
        if (entry.fill == "none" && fillSet && entry.name.empty())
            entry.name = "none";
        if (entry.name == "none")
            entry.name.clear();
        return true;
    }

    bool isAnimationFamily(const std::string& name){
        static const std::set<std::string> names = {"animation", "animation-name", "animation-duration", "animation-timing-function",
                                                    "animation-delay", "animation-iteration-count", "animation-direction",
                                                    "animation-play-state", "animation-fill-mode"};
        const std::string plain = name.compare(0, 8, "-webkit-") == 0 ? name.substr(8) : name;
        return names.count(plain) > 0;
    }

    std::string plain(const std::string& name){
        return name.compare(0, 8, "-webkit-") == 0 ? name.substr(8) : name;
    }

    bool applyAnimation(Session& session, const std::string& rawName, const std::vector<ComponentValue>& value, std::vector<Entry>& entries){
        const std::string name = plain(rawName);
        std::vector<std::vector<ComponentValue>> groups = commaGroups(value);
        if (name == "animation"){
            entries.clear();
            for (const std::vector<ComponentValue>& group : groups){
                Entry e;
                if (!parseEntry(session, group, e))
                    return false;
                entries.push_back(e);
            }
            return true;
        }
        if (name == "animation-name"){
            std::vector<Entry> previous = entries;
            entries.clear();
            for (size_t i = 0; i < groups.size(); i++){
                Entry e = previous.empty() ? Entry() : previous[i % previous.size()];
                std::vector<ComponentValue> w = words(groups[i]);
                if (w.size() != 1 || !(w[0].isToken(TokenType::Ident) || w[0].isToken(TokenType::String)))
                    return false;
                e.name = w[0].token.value;
                if (w[0].isToken(TokenType::Ident) && equalsNoCase(e.name, "none"))
                    e.name.clear();
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
            const ComponentValue& v = w[0];
            double s = 0;
            if (name == "animation-duration" || name == "animation-delay"){
                std::string written;
                if (seconds(v, s))
                    written = formatNumber(s) + "s";
                else if (isVar(v) && varKind(session, v) == "time"){
                    written = text(v);
                    if (name == "animation-duration")
                        e.durationVar = true;
                }else
                    return false;
                if (name == "animation-duration"){
                    e.duration = written;
                    e.durationSeconds = s;
                }else{
                    e.delay = written;
                }
            }else if (name == "animation-timing-function"){
                if (isVar(v)){
                    std::optional<TypedVar> typed = session.typedVar(v, "easing");
                    if (!typed)
                        return false;
                    EasingResult r;
                    r.text = typed->reference;
                    e.easing = r;
                    continue;
                }
                std::optional<EasingResult> easing = easingOf(session, v);
                if (!easing)
                    return false;
                e.easing = easing;
            }else if (name == "animation-iteration-count"){
                if (isIdent(v, "infinite"))
                    e.count = "infinite";
                else if (v.isToken(TokenType::Number)){
                    e.count = text(v);
                    e.countValue = v.token.number;
                }else
                    return false;
            }else if (name == "animation-direction"){
                if (!directions().count(identOf(v)))
                    return false;
                e.direction = identOf(v);
            }else if (name == "animation-play-state"){
                const std::string k = identOf(v);
                if (k != "running" && k != "paused")
                    return false;
                e.paused = k == "paused";
            }else if (name == "animation-fill-mode"){
                if (!fills().count(identOf(v)))
                    return false;
                e.fill = identOf(v);
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

    // the RCSS animation value; fill: the -html-animation-fill value
    std::string writeEntries(DeclContext* context, Session& session, const std::vector<Entry>& entries, std::string& fill, bool report){
        const bool fork = session.options.fork.has(ForkFeature::TimingFunctions);
        std::vector<std::string> out;
        fill = "none";
        for (const Entry& e : entries){
            if (e.name.empty())
                continue;
            if (!e.durationVar && e.durationSeconds <= 0){
                if (report && context)
                    context->reportAs(Severity::Info, codes::AnimationApproximated, "animation '" + e.raw + "' has no duration; it does nothing and is dropped", std::string(), e.raw);
                continue;
            }
            std::string count = e.count;
            if (!count.empty() && count != "infinite"){
                if (e.countValue <= 0){
                    if (report && context)
                        context->reportAs(Severity::Info, codes::AnimationApproximated, "animation '" + e.raw + "' plays 0 times; dropped", std::string(), e.raw);
                    continue;
                }
                const double rounded = std::max(1.0, std::round(e.countValue));
                if (rounded != e.countValue && report && context)
                    context->reportAs(Severity::Info, codes::AnimationApproximated, "fractional iteration count " + count + " is rounded to " +
                                      formatNumber(rounded), std::string(), e.raw);
                count = rounded == 1.0 ? std::string() : formatNumber(rounded);
            }
            std::string name = session.keyframeName(e.name);
            bool alternate = false;
            if (e.direction == "reverse"){
                name += "-rev";
            }else if (e.direction == "alternate"){
                alternate = true;
            }else if (e.direction == "alternate-reverse"){
                alternate = true;
                name += "-rev";
            }
            if (e.fill == "forwards" || e.fill == "both"){
                fill = "forwards";
                session.fillKeyframes.insert(name);
                if (e.fill == "both" && report && context && !e.delay.empty() && e.delay != "0s")
                    context->reportAs(Severity::Info, codes::AnimationApproximated, "the backwards part of 'both' (first keyframe during the delay) is not emulated",
                                      "put the first keyframe's values in the element's own style", e.raw);
            }else if (e.fill == "backwards" && report && context){
                context->reportAs(Severity::Info, codes::AnimationApproximated, "'animation-fill-mode: backwards' is not emulated",
                                  "put the first keyframe's values in the element's own style", e.raw);
            }
            std::string easing = e.easing ? e.easing->text : (fork ? std::string() : std::string("quadratic-out"));
            if (e.easing && report && context && !e.easing->note.empty())
                context->reportAs(e.easing->severity, e.easing->code, e.easing->note, std::string(), e.raw);
            std::vector<std::string> parts{e.duration};
            if (!e.delay.empty() && e.delay != "0s")
                parts.push_back(e.delay);
            else if (!count.empty() && !e.delay.empty())
                parts.push_back("0s");
            if (!count.empty())
                parts.push_back(count);
            if (alternate)
                parts.push_back("alternate");
            if (e.paused)
                parts.push_back("paused");
            if (!easing.empty())
                parts.push_back(easing);
            parts.push_back(name);
            out.push_back(join(parts));
        }
        return join(out, ", ");
    }

    std::string classifyAnimation(Session& session, const std::vector<ComponentValue>& value){
        std::vector<Entry> entries;
        return applyAnimation(session, "animation", value, entries) ? "animations" : std::string();
    }

    std::optional<std::string> transformAnimation(Session& session, const std::vector<ComponentValue>& value, const std::string&, SourceRef){
        std::vector<Entry> entries;
        if (!applyAnimation(session, "animation", value, entries))
            return std::nullopt;
        std::string fill;
        const std::string text = writeEntries(nullptr, session, entries, fill, false);
        return text.empty() ? std::string("none") : text;
    }

}

const VarFamily jasperengine::htmlui::compat::kAnimationFamily = {"animation", classifyAnimation, transformAnimation};

void jasperengine::htmlui::compat::rewriteAnimation(DeclContext& context){
    rewriteAnimationLonghand(context);
}

void jasperengine::htmlui::compat::rewriteAnimationLonghand(DeclContext& context){
    if (context.keyframe){
        context.dropped = true;
        return;
    }
    if (context.value.size() == 1 && isVar(context.value[0]) && plain(context.name) == "animation"){
        std::optional<TypedVar> typed = wholeVar(context, "animation");
        context.emit("animation", typed ? typed->reference : context.valueText());
        return;
    }
    std::vector<Entry> entries;
    bool onlyFill = plain(context.name) == "animation-fill-mode";
    if (context.rule){
        const Declaration* last = nullptr;
        bool hasShorthand = false;
        onlyFill = true;
        for (const Declaration& d : context.rule->declarations){
            if (!isAnimationFamily(d.name))
                continue;
            last = &d;
            const std::string p = plain(d.name);
            if (p == "animation" || p == "animation-name")
                hasShorthand = true;
            if (p != "animation-fill-mode")
                onlyFill = false;
        }
        if (last && last->ref != context.declaration.ref)
            return;
        if (!hasShorthand){
            const FlatRule* base = BaseRules::find(context.session, *context.rule, [](const FlatRule& candidate){
                for (const Declaration& d : candidate.declarations){
                    if (plain(d.name) == "animation" || plain(d.name) == "animation-name")
                        return true;
                }
                return false;
            });
            if (!base){
                if (onlyFill){
                    // fill mode alone: the AnimationFill property only
                    std::vector<ComponentValue> w = words(context.value);
                    const std::string k = w.size() == 1 ? identOf(w[0]) : std::string();
                    context.emit("-html-animation-fill", (k == "forwards" || k == "both") ? "forwards" : "none");
                    return;
                }
                context.reportAs(Severity::Warning, codes::LonghandWithoutBase, "'" + context.name +
                                 "' without animation or animation-name in the rule or a base rule is dropped",
                                 "write the animation shorthand in this rule");
                context.dropped = true;
                return;
            }
            for (const Declaration& d : base->declarations){
                if (isAnimationFamily(d.name))
                    applyAnimation(context.session, d.name, normalizedValue(context.session, d), entries);
            }
            context.reportAs(Severity::Info, codes::MergedFromBaseRule, "animation longhands are merged with the base rule at " +
                             BaseRules::location(context.session, *base));
        }
        for (const Declaration& d : context.rule->declarations){
            if (!isAnimationFamily(d.name))
                continue;
            const std::vector<ComponentValue> value = d.ref == context.declaration.ref ? context.value : normalizedValue(context.session, d);
            if (!applyAnimation(context.session, d.name, value, entries))
                context.reportAt(d.ref, Severity::Warning, codes::UnsupportedValue, "'" + d.name + ": " + text(value) + "' is not supported; dropped",
                                 "write animation: <name> <duration> <easing> <delay> <count> <direction> <fill-mode>");
        }
    }else if (!applyAnimation(context.session, context.name, context.value, entries)){
        context.reportAs(Severity::Warning, codes::UnsupportedValue, "'" + context.sourceText() + "' is not supported; declaration dropped",
                         "write animation: <name> <duration> <easing> <delay> <count> <direction> <fill-mode>");
        context.dropped = true;
        return;
    }
    std::string fill;
    const std::string value = writeEntries(&context, context.session, entries, fill, true);
    context.emit("animation", value.empty() ? "none" : value);
    context.emit("-html-animation-fill", fill);
}
