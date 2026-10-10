// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Animations (phase-2 §3.9.7 "Animations"): written in the order RmlUi reads
// (<duration>s [<delay>s] [<count>|infinite] [alternate] [paused] <tween> <name>), reverse
// directions through mirrored keyframe copies (<name>-rev), fill modes through the
// AnimationFill runtime (-html-animation-fill), longhands merged with the rule or a base rule.
// Each declaration of the family is read once into entries (texts as views of the values
// written, keywords as enums), merged on them, and the value is written once into one buffer.

#include "htmlui/compat/css/props/Rewriters.h"
#include "htmlui/compat/LiteralCompare.h"

#include "htmlui/compat/css/BaseRules.h"
#include "htmlui/compat/css/CssParser.h"
#include "htmlui/compat/css/CssSerialize.h"
#include "htmlui/compat/css/Units.h"
#include "htmlui/compat/css/ValueNormalizer.h"

#include <cmath>

using namespace jasperengine::htmlui::compat;
using namespace jasperengine::htmlui::compat::props;

namespace {

    enum class Direction : uint8_t { Normal, Reverse, Alternate, AlternateReverse };
    enum class Fill : uint8_t { None, Forwards, Backwards, Both };
    enum class Count : uint8_t { One, Infinite, Number };

    // (trivially copied: the entries of a declaration are copied from one to the next)
    struct Entry {
        Text name;                         // CSS keyframes name ("" = none)
        Timing duration;
        double durationSeconds = 0;
        bool durationVar = false;
        Timing delay;
        bool hasEasing = false;
        EasingResult easing;
        Count count = Count::One;          // One: written ""; Number: `countText`
        const ComponentValue* countText = nullptr;
        double countValue = 1;
        Direction direction = Direction::Normal;
        Fill fill = Fill::None;
        bool paused = false;
        ValueRange raw;                    // the entry as written (messages)
    };

    bool seconds(const ComponentValue& v, double& value){
        if (v.isToken(TokenType::Dimension) && equalsNoCase(v.token.unit, "s")){
            value = v.token.number;
            return true;
        }
        return false;
    }

    Timing secondsTiming(double s){
        Timing timing;
        timing.kind = Timing::Kind::Seconds;
        timing.seconds = s;
        return timing;
    }

    Timing varTiming(const ComponentValue& v){
        Timing timing;
        timing.kind = Timing::Kind::Var;
        timing.var = &v;
        return timing;
    }

    bool directionOf(std::string_view k, Direction& direction){
        if (k == "normal") direction = Direction::Normal;
        else if (k == "reverse") direction = Direction::Reverse;
        else if (k == "alternate") direction = Direction::Alternate;
        else if (k == "alternate-reverse") direction = Direction::AlternateReverse;
        else return false;
        return true;
    }

    bool fillOf(std::string_view k, Fill& fill){
        if (k == "none") fill = Fill::None;
        else if (k == "forwards") fill = Fill::Forwards;
        else if (k == "backwards") fill = Fill::Backwards;
        else if (k == "both") fill = Fill::Both;
        else return false;
        return true;
    }

    bool parseEntry(Session& session, const ValueRange& group, Entry& entry){
        entry.raw = group;
        bool durationSet = false, delaySet = false, countSet = false, directionSet = false, fillSet = false, stateSet = false;
        for (const ComponentValue* value : wordsOf(group)){
            const ComponentValue& v = *value;
            double s = 0;
            if (seconds(v, s)){
                if (!durationSet){
                    durationSet = true;
                    entry.durationSeconds = s;
                    entry.duration = secondsTiming(s);
                }else if (!delaySet){
                    delaySet = true;
                    entry.delay = secondsTiming(s);
                }else{
                    return false;
                }
                continue;
            }
            if (isVar(v) && varKind(session, v) == "time"){
                if (!durationSet){
                    durationSet = true;
                    entry.duration = varTiming(v);
                    entry.durationVar = true;
                }else{
                    delaySet = true;
                    entry.delay = varTiming(v);
                }
                continue;
            }
            if (v.isToken(TokenType::Number) && !countSet){
                countSet = true;
                entry.countValue = v.token.number;
                entry.count = Count::Number;
                entry.countText = &v;
                continue;
            }
            if (std::optional<EasingResult> e = easingOf(session, v)){
                if (entry.hasEasing)
                    return false;
                entry.hasEasing = true;
                entry.easing = *e;
                continue;
            }
            if (isVar(v)){
                std::optional<TypedVar> typed = session.typedVar(v, "easing");
                if (!typed)
                    return false;
                entry.hasEasing = true;
                entry.easing = EasingResult();
                entry.easing.text = Text(typed->reference);
                continue;
            }
            const std::string_view k = identView(v);
            if (k == "infinite" && !countSet){
                countSet = true;
                entry.count = Count::Infinite;
                continue;
            }
            Direction direction;
            if (directionOf(k, direction) && !directionSet){
                directionSet = true;
                entry.direction = direction;
                continue;
            }
            Fill fill;
            if (fillOf(k, fill) && !fillSet){
                // "none" is read as the fill mode; a name-less entry is no animation
                fillSet = true;
                entry.fill = fill;
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
        if (entry.name == "none")
            entry.name = Text();
        return true;
    }

    bool startsWith(std::string_view text, std::string_view prefix){
        return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
    }

    std::string_view plain(std::string_view name){
        return startsWith(name, "-webkit-") ? name.substr(8) : name;
    }

    bool isAnimationFamily(std::string_view name){
        const std::string_view p = plain(name);
        return p == "animation" || p == "animation-name" || p == "animation-duration" || p == "animation-timing-function" ||
               p == "animation-delay" || p == "animation-iteration-count" || p == "animation-direction" || p == "animation-play-state" ||
               p == "animation-fill-mode";
    }

    bool applyAnimation(Session& session, std::string_view rawName, const ValueList& value, ArenaList<Entry>& entries){
        const std::string_view name = plain(rawName);
        const ArenaList<ValueRange> groups = commaRanges(value);
        if (name == "animation"){
            entries.clear();
            for (const ValueRange& group : groups){
                Entry e;
                if (!parseEntry(session, group, e))
                    return false;
                entries.push_back(e);
            }
            return true;
        }
        if (name == "animation-name"){
            const ArenaList<Entry> previous = entries;
            entries.clear();
            for (size_t i = 0; i < groups.size(); i++){
                Entry e = previous.empty() ? Entry() : previous[i % previous.size()];
                const Words w = wordsOf(groups[i]);
                if (w.size() != 1 || !(w[0]->isToken(TokenType::Ident) || w[0]->isToken(TokenType::String)))
                    return false;
                e.name = w[0]->token.value;
                if (w[0]->isToken(TokenType::Ident) && equalsNoCase(e.name, "none"))
                    e.name = Text();
                e.raw = groups[i];
                entries.push_back(e);
            }
            return true;
        }
        if (entries.empty())
            entries.push_back(Entry());
        for (size_t i = 0; i < entries.size(); i++){
            const Words w = wordsOf(groups[i % groups.size()]);
            if (w.size() != 1)
                return false;
            Entry& e = entries[i];
            const ComponentValue& v = *w[0];
            double s = 0;
            if (name == "animation-duration" || name == "animation-delay"){
                Timing written;
                if (seconds(v, s))
                    written = secondsTiming(s);
                else if (isVar(v) && varKind(session, v) == "time"){
                    written = varTiming(v);
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
                    e.hasEasing = true;
                    e.easing = EasingResult();
                    e.easing.text = Text(typed->reference);
                    continue;
                }
                std::optional<EasingResult> easing = easingOf(session, v);
                if (!easing)
                    return false;
                e.hasEasing = true;
                e.easing = *easing;
            }else if (name == "animation-iteration-count"){
                if (isIdent(v, "infinite"))
                    e.count = Count::Infinite;
                else if (v.isToken(TokenType::Number)){
                    e.count = Count::Number;
                    e.countText = &v;
                    e.countValue = v.token.number;
                }else
                    return false;
            }else if (name == "animation-direction"){
                Direction direction;
                if (!directionOf(identView(v), direction))
                    return false;
                e.direction = direction;
            }else if (name == "animation-play-state"){
                const std::string_view k = identView(v);
                if (k != "running" && k != "paused")
                    return false;
                e.paused = k == "paused";
            }else if (name == "animation-fill-mode"){
                Fill fill;
                if (!fillOf(identView(v), fill))
                    return false;
                e.fill = fill;
            }
        }
        return true;
    }

    ValueList normalizedValue(Session& session, const Declaration& d){
        NormalizeContext context;
        context.property = std::string(plain(d.name));
        context.colors = false;
        context.options = session.options;
        context.resolveVar = [&session](const ComponentValue& var, ValueList& out){ return session.staticVar(var, out); };
        NormalizeResult result = ValueNormalizer::normalize(d.value, context);
        return result.ok ? trimWhitespace(result.value) : ValueList();
    }

    // the RCSS animation value appended to `out`; fillForwards: the -html-animation-fill value is forwards
    void writeEntries(DeclContext* context, Session& session, const ArenaList<Entry>& entries, bool& fillForwards, bool report, std::string& out){
        const bool fork = session.options.fork.has(ForkFeature::TimingFunctions);
        fillForwards = false;
        const size_t start = out.size();
        for (const Entry& e : entries){
            if (e.name.empty())
                continue;
            if (!e.durationVar && e.durationSeconds <= 0){
                if (report && context){
                    const std::string raw = text(e.raw);
                    context->reportAs(Severity::Info, codes::AnimationApproximated, "animation '" + raw + "' has no duration; it does nothing and is dropped", std::string(), raw);
                }
                continue;
            }
            // the iteration count as written: "" (once), "infinite", or a whole number
            bool countWritten = e.count != Count::One;
            double rounded = 1;
            if (e.count == Count::Number){
                if (e.countValue <= 0){
                    if (report && context){
                        const std::string raw = text(e.raw);
                        context->reportAs(Severity::Info, codes::AnimationApproximated, "animation '" + raw + "' plays 0 times; dropped", std::string(), raw);
                    }
                    continue;
                }
                rounded = std::max(1.0, std::round(e.countValue));
                if (rounded != e.countValue && report && context)
                    context->reportAs(Severity::Info, codes::AnimationApproximated, "fractional iteration count " + text(*e.countText) + " is rounded to " +
                                      formatNumber(rounded), std::string(), text(e.raw));
                countWritten = rounded != 1.0;
            }
            std::string name = session.keyframeName(e.name);
            bool alternate = false;
            if (e.direction == Direction::Reverse){
                name += "-rev";
            }else if (e.direction == Direction::Alternate){
                alternate = true;
            }else if (e.direction == Direction::AlternateReverse){
                alternate = true;
                name += "-rev";
            }
            if (e.fill == Fill::Forwards || e.fill == Fill::Both){
                fillForwards = true;
                session.fillKeyframes.insert(name);
                if (e.fill == Fill::Both && report && context && e.delay.given() && !writtenAsZero(e.delay))
                    context->reportAs(Severity::Info, codes::AnimationApproximated, "the backwards part of 'both' (first keyframe during the delay) is not emulated",
                                      "put the first keyframe's values in the element's own style", text(e.raw));
            }else if (e.fill == Fill::Backwards && report && context){
                context->reportAs(Severity::Info, codes::AnimationApproximated, "'animation-fill-mode: backwards' is not emulated",
                                  "put the first keyframe's values in the element's own style", text(e.raw));
            }
            if (e.hasEasing && report && context && !e.easing.note.empty())
                context->reportAs(e.easing.severity, e.easing.code, e.easing.note, std::string(), text(e.raw));
            if (out.size() > start)
                out += ", ";
            // the parts, separated by a space (an empty part is not written)
            const size_t entryStart = out.size();
            auto separate = [&](){
                if (out.size() > entryStart)
                    out += ' ';
            };
            appendTiming(out, e.duration);
            if (e.delay.given() && !writtenAsZero(e.delay)){
                separate();
                appendTiming(out, e.delay);
            }else if (countWritten && e.delay.given()){
                separate();
                out += "0s";
            }
            if (countWritten){
                separate();
                if (e.count == Count::Infinite)
                    out += "infinite";
                else
                    appendNumber(out, rounded);
            }
            if (alternate){
                separate();
                out += "alternate";
            }
            if (e.paused){
                separate();
                out += "paused";
            }
            if (e.hasEasing){
                if (!e.easing.text.empty()){
                    separate();
                    out += e.easing.text.view();
                }
            }else if (!fork){
                separate();
                out += "quadratic-out";
            }
            if (!name.empty()){
                separate();
                out += name;
            }
        }
    }

    std::string classifyAnimation(Session& session, const ValueList& value){
        ArenaList<Entry> entries;
        return applyAnimation(session, "animation", value, entries) ? "animations" : std::string();
    }

    std::optional<std::string> transformAnimation(Session& session, const ValueList& value, const std::string&, SourceRef){
        ArenaList<Entry> entries;
        if (!applyAnimation(session, "animation", value, entries))
            return std::nullopt;
        bool fillForwards = false;
        std::string text;
        writeEntries(nullptr, session, entries, fillForwards, false, text);
        return text.empty() ? std::string("none") : text;
    }

    // (one buffer for the animations of the thread: the emitted value is a copy)
    std::string& animationBuffer(){
        static thread_local std::string buffer;
        return buffer;
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
    ArenaList<Entry> entries;
    bool onlyFill = plain(context.name) == "animation-fill-mode";
    if (context.rule){
        const Declaration* last = nullptr;
        bool hasShorthand = false;
        onlyFill = true;
        for (const Declaration& d : context.rule->declarations){
            if (!isAnimationFamily(d.name))
                continue;
            if (reachesRewrite(context.session, d, context.rule->declarations))
                last = &d;
            const std::string_view p = plain(d.name);
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
                    const Words w = wordsOf(context.value);
                    const std::string_view k = w.size() == 1 ? identView(*w[0]) : std::string_view();
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
                if (isAnimationFamily(d.name) && reachesRewrite(context.session, d, base->declarations))
                    applyAnimation(context.session, d.name, normalizedValue(context.session, d), entries);
            }
            context.reportAs(Severity::Info, codes::MergedFromBaseRule, "animation longhands are merged with the base rule at " +
                             BaseRules::location(context.session, *base));
        }
        for (const Declaration& d : context.rule->declarations){
            if (!isAnimationFamily(d.name) || !reachesRewrite(context.session, d, context.rule->declarations))
                continue;
            ValueList storage;
            const ValueList& value = d.ref == context.declaration.ref ? context.value : (storage = normalizedValue(context.session, d));
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
    bool fillForwards = false;
    std::string& value = animationBuffer();
    value.clear();
    writeEntries(&context, context.session, entries, fillForwards, true, value);
    context.emit("animation", value.empty() ? Text::view("none") : Text(std::string_view(value)));
    context.emit("-html-animation-fill", fillForwards ? "forwards" : "none");
}
