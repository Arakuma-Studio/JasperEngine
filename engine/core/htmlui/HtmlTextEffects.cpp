// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "HtmlTextEffects.h"

#include "Engine.h"
#include "htmlui/HtmlUIClock.h"
#include "htmlui/compat/runtime/CompatProperties.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/PropertyParser.h>
#include <RmlUi/Core/StyleSheetSpecification.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

using namespace jasperengine;
using namespace jasperengine::htmlui;

namespace {

    constexpr float PI = 3.14159265358979323846f;

    std::string trim(const std::string& text){
        size_t a = 0, b = text.size();
        while (a < b && std::isspace((unsigned char)text[a])) a++;
        while (b > a && std::isspace((unsigned char)text[b - 1])) b--;
        return text.substr(a, b - a);
    }

    std::string lower(std::string text){
        for (char& c : text) c = (char)std::tolower((unsigned char)c);
        return text;
    }

    // splits at `separator` outside parentheses (a whitespace separator splits on any run)
    std::vector<std::string> split(const std::string& text, char separator){
        std::vector<std::string> parts;
        std::string current;
        int depth = 0;
        for (char c : text){
            if (c == '(') depth++;
            if (c == ')') depth = std::max(0, depth - 1);
            const bool split = depth == 0 && (separator == ' ' ? std::isspace((unsigned char)c) != 0 : c == separator);
            if (split){
                if (!trim(current).empty() || separator != ' ') parts.push_back(trim(current));
                current.clear();
            }else{
                current += c;
            }
        }
        if (!trim(current).empty() || (separator != ' ' && !parts.empty())) parts.push_back(trim(current));
        return parts;
    }

    // a number and its unit ("0.15em" -> 0.15, "em"); false when it does not start with a number
    bool number(const std::string& text, float& value, std::string& unit){
        const char* begin = text.c_str();
        char* end = nullptr;
        value = std::strtof(begin, &end);
        if (end == begin || !std::isfinite(value)) return false;
        unit = lower(std::string(end));
        return true;
    }

    enum class Arg { Length, Time, Angle, Number, Percentage };

    // one positional argument; a bare 0 is any quantity
    bool readArg(const std::string& text, Arg kind, float& value, bool& inPx, std::string& error){
        std::string unit;
        if (!number(text, value, unit)){
            error = "\"" + text + "\" is not a number";
            return false;
        }
        inPx = false;
        if (unit.empty() && value == 0.0f && kind != Arg::Number) return true;
        switch (kind){
            case Arg::Length:
                if (unit == "em" || unit == "rem") return true;
                if (unit == "px"){ inPx = true; return true; }
                error = "\"" + text + "\" must be a length (em or px)";
                return false;
            case Arg::Time:
                if (unit == "s") return true;
                if (unit == "ms"){ value /= 1000.0f; return true; }
                error = "\"" + text + "\" must be a time (s or ms)";
                return false;
            case Arg::Angle:
                if (unit == "deg"){ value *= PI / 180.0f; return true; }
                if (unit == "rad") return true;
                if (unit == "turn"){ value *= 2.0f * PI; return true; }
                if (unit == "grad"){ value *= PI / 200.0f; return true; }
                error = "\"" + text + "\" must be an angle (deg, rad, turn)";
                return false;
            case Arg::Number:
                if (unit.empty()) return true;
                error = "\"" + text + "\" must be a number without a unit";
                return false;
            case Arg::Percentage:
                if (unit == "%"){ value /= 100.0f; return true; }
                if (unit.empty()) return true;   // 0..1
                error = "\"" + text + "\" must be a percentage";
                return false;
        }
        return false;
    }

    bool parseColor(const std::string& text, Vector4& color){
        Rml::PropertyParser* parser = Rml::StyleSheetSpecification::GetParser("color");
        if (!parser) return false;
        Rml::Property property;
        if (!parser->ParseValue(property, text, Rml::ParameterMap())) return false;
        const Rml::Colourb c = property.Get<Rml::Colourb>();
        color = Vector4(c.red / 255.0f, c.green / 255.0f, c.blue / 255.0f, c.alpha / 255.0f);
        return true;
    }

    bool parseFunction(const std::string& name, const std::string& args, TextEffectLayer& layer, bool& amplitudeInPx, std::string& error){
        struct Slot { Arg kind; float* target; bool amplitude; };
        std::vector<Slot> slots;
        layer = TextEffectLayer();
        amplitudeInPx = false;

        if (name == "wave"){
            layer.type = TextEffectType::Wave; layer.amplitude = 0.15f; layer.duration = 1.5f; layer.phaseStep = 0.3f;
            slots = {{Arg::Length, &layer.amplitude, true}, {Arg::Time, &layer.duration, false}, {Arg::Angle, &layer.phaseStep, false}};
        }else if (name == "bounce"){
            layer.type = TextEffectType::Bounce; layer.amplitude = 0.3f; layer.duration = 1.0f; layer.spread = 0.5f;
            slots = {{Arg::Length, &layer.amplitude, true}, {Arg::Time, &layer.duration, false}, {Arg::Number, &layer.spread, false}};
        }else if (name == "shake"){
            layer.type = TextEffectType::Shake; layer.amplitude = 0.05f; layer.frequency = 12.0f;
            slots = {{Arg::Length, &layer.amplitude, true}, {Arg::Number, &layer.frequency, false}};
        }else if (name == "pulse"){
            layer.type = TextEffectType::Pulse; layer.amplitude = 0.15f; layer.duration = 1.0f; layer.spread = 0.5f;
            slots = {{Arg::Number, &layer.amplitude, false}, {Arg::Time, &layer.duration, false}, {Arg::Number, &layer.spread, false}};
        }else if (name == "typewriter"){
            layer.type = TextEffectType::Typewriter; layer.charactersPerSecond = 30.0f; layer.fadeTime = 0.0f;
            slots = {{Arg::Number, &layer.charactersPerSecond, false}, {Arg::Time, &layer.fadeTime, false}};
        }else if (name == "fade-in" || name == "grow" || name == "unfold"){
            layer.type = (name == "fade-in") ? TextEffectType::FadeIn : (name == "grow") ? TextEffectType::Grow : TextEffectType::Unfold;
            layer.duration = 0.5f; layer.spread = 0.5f;
            slots = {{Arg::Time, &layer.duration, false}, {Arg::Number, &layer.spread, false}};
        }else if (name == "rainbow"){
            layer.type = TextEffectType::Rainbow; layer.duration = 2.0f; layer.phaseStep = 20.0f * PI / 180.0f;
            layer.saturation = 0.8f; layer.lightness = 0.6f;
            slots = {{Arg::Time, &layer.duration, false}, {Arg::Angle, &layer.phaseStep, false},
                     {Arg::Percentage, &layer.saturation, false}, {Arg::Percentage, &layer.lightness, false}};
        }else if (name == "gradient"){
            layer.type = TextEffectType::Gradient; layer.duration = 1.5f; layer.spread = 0.5f;
            // <time>? [multiply|add|replace]? then the colour stops, comma separated
            std::vector<std::string> stops = split(args, ',');
            if (stops.empty() || stops[0].empty()){
                error = "gradient() needs colour stops";
                return false;
            }
            std::vector<std::string> head = split(stops[0], ' ');
            size_t used = 0;
            float value = 0.0f;
            std::string unit;
            bool px = false;
            if (!head.empty() && number(head[0], value, unit) && (unit == "s" || unit == "ms")){
                if (!readArg(head[0], Arg::Time, layer.duration, px, error)) return false;
                used = 1;
            }
            if (used < head.size()){
                TextEffectMix mix;
                if (TextEffects::parseMix(lower(head[used]).c_str(), mix)){
                    layer.mix = mix;
                    used++;
                }
            }
            std::string first;
            for (size_t i = used; i < head.size(); i++) first += (first.empty() ? "" : " ") + head[i];
            stops[0] = first;
            for (size_t i = 0; i < stops.size(); i++){
                std::vector<std::string> words = split(stops[i], ' ');
                if (words.empty() || words.size() > 2){
                    error = "gradient() colour stop \"" + stops[i] + "\" must be <color> [<percentage>]";
                    return false;
                }
                TextEffectGradientStop stop;
                if (!parseColor(words[0], stop.color)){
                    error = "gradient() colour \"" + words[0] + "\" is not a colour";
                    return false;
                }
                stop.position = stops.size() > 1 ? (float)i / (float)(stops.size() - 1) : 0.0f;
                if (words.size() == 2 && !readArg(words[1], Arg::Percentage, stop.position, px, error)) return false;
                layer.gradient.push_back(stop);
            }
            return true;
        }else{
            error = "unknown function " + name + "() (wave, bounce, shake, pulse, typewriter, fade-in, grow, unfold, rainbow, gradient)";
            return false;
        }

        std::vector<std::string> values = split(args, ' ');
        if (values.size() > slots.size()){
            error = name + "() takes at most " + std::to_string(slots.size()) + " arguments";
            return false;
        }
        for (size_t i = 0; i < values.size(); i++){
            bool px = false;
            if (!readArg(values[i], slots[i].kind, *slots[i].target, px, error)){
                error = name + "(): " + error;
                return false;
            }
            if (slots[i].amplitude) amplitudeInPx = px;
        }
        layer.spread = std::clamp(layer.spread, 0.0f, 1.0f);
        return true;
    }

    Rml::PropertyId propertyId(compat::CompatProperty property){
        return compat::CompatProperties::id(property);
    }

    std::string stringProperty(Rml::Element& element, compat::CompatProperty property){
        const Rml::PropertyId id = propertyId(property);
        if (id == Rml::PropertyId::Invalid) return std::string();
        const Rml::Property* value = element.GetProperty(id);
        return value ? trim(value->ToString()) : std::string();
    }

}

bool jasperengine::htmlui::parseTextEffect(const std::string& value, HtmlTextEffect& out, std::string& error){
    out = HtmlTextEffect();
    const std::string text = trim(value);
    if (text.empty() || lower(text) == "none") return true;

    for (const std::string& part : split(text, ',')){
        const size_t open = part.find('(');
        if (open == std::string::npos || part.back() != ')'){
            error = "\"" + part + "\" is not a function";
            return false;
        }
        const std::string name = lower(trim(part.substr(0, open)));
        const std::string args = part.substr(open + 1, part.size() - open - 2);
        TextEffectLayer layer;
        bool px = false;
        if (!parseFunction(name, args, layer, px, error)) return false;
        out.layers.push_back(layer);
        out.amplitudeInPx.push_back(px);
    }
    return true;
}

bool jasperengine::htmlui::parseTextEffectIteration(const std::string& value, int& iterations){
    const std::string text = lower(trim(value));
    if (text.empty() || text == "infinite"){
        iterations = -1;
        return true;
    }
    char* end = nullptr;
    const long count = std::strtol(text.c_str(), &end, 10);
    if (!end || *end != '\0' || count < 1) return false;
    iterations = (int)std::min(count, 1000000L);
    return true;
}

HtmlTextEffects& HtmlTextEffects::get(){
    static HtmlTextEffects effects;
    return effects;
}

void HtmlTextEffects::install(){
    Rml::ElementText::SetTextEffectProvider(&get());
}

void HtmlTextEffects::uninstall(){
    if (Rml::ElementText::GetTextEffectProvider() == &get())
        Rml::ElementText::SetTextEffectProvider(nullptr);
    get().scopes.clear();
    get().reducedMotion.clear();
    get().animated.clear();
}

Rml::Element* HtmlTextEffects::findRoot(Rml::Element* text) const{
    const Rml::PropertyId id = propertyId(compat::CompatProperty::TextEffect);
    Rml::Element* parent = text ? text->GetParentNode() : nullptr;
    for (Rml::Element* element = parent; element; element = element->GetParentNode()){
        if (element->GetLocalProperty(id))
            return element;
    }
    return parent;
}

double HtmlTextEffects::now(const Scope& scope) const{
    return scope.gameTime ? Engine::getGameTime() : HtmlUIClock::now();
}

bool HtmlTextEffects::animates(const Scope& scope) const{
    if (!scope.finished) return true;
    auto reduced = reducedMotion.find(scope.context);
    const bool still = reduced != reducedMotion.end() && reduced->second;
    for (const TextEffectLayer& layer : scope.effect.layers){
        if (layer.enabled && layer.type != TextEffectType::None && !TextEffects::isOneShot(layer.type) &&
            !(still && TextEffects::isMotion(layer.type)))
            return true;
    }
    return false;
}

const void* HtmlTextEffects::GetTextEffect(Rml::ElementText& text, int& first_index){
    first_index = 0;
    const std::string value = stringProperty(text, compat::CompatProperty::TextEffect);
    if (value.empty() || lower(value) == "none")
        return nullptr;

    Rml::Element* root = findRoot(&text);
    if (!root)
        return nullptr;

    Scope& scope = scopes[root];
    const bool fresh = !scope.root;
    if (fresh || scope.value != value){
        std::string error;
        HtmlTextEffect parsed;
        // (compat reported an invalid value at load; a scripted one is ignored)
        if (!parseTextEffect(value, parsed, error))
            parsed = HtmlTextEffect();
        scope.root = root->GetObserverPtr();
        scope.context = text.GetContext();
        scope.value = value;
        scope.effect = parsed;
        scope.start = scope.cycleStart = HtmlUIClock::now();
        scope.iterationsDone = 0;
        scope.finished = false;
        scope.texts.clear();
    }
    int iterations = -1;
    scope.iterations = parseTextEffectIteration(stringProperty(text, compat::CompatProperty::TextEffectIteration), iterations) ? iterations : -1;
    const bool gameTime = lower(stringProperty(text, compat::CompatProperty::TextEffectTime)) == "game";
    if (gameTime != scope.gameTime || fresh){
        scope.gameTime = gameTime;
        scope.start = scope.cycleStart = now(scope);
    }
    if (scope.effect.layers.empty())
        return nullptr;

    // this text's first character and every character of the effect in its inline formatting
    // context (texts of other effects inside it keep their own)
    Rml::ElementText* first = &text;
    while (first->GetContextPrevious())
        first = first->GetContextPrevious();
    int count = 0;
    for (Rml::ElementText* item = first; item; item = item->GetContextNext()){
        if (item != &text && findRoot(item) != root)
            continue;
        if (item == &text)
            first_index = count;
        for (const Rml::ElementText::Line& line : item->GetLines())
            count += Rml::ElementText::CountEffectCharacters(line.text);
    }
    scope.lastCount = count;

    // a text that shows something else starts the effect again
    std::string shown;
    for (const Rml::ElementText::Line& line : text.GetLines())
        shown += line.text;
    auto known = scope.texts.find(&text);
    if (known != scope.texts.end() && known->second != shown){
        scope.start = scope.cycleStart = now(scope);
        scope.iterationsDone = 0;
        scope.finished = false;
    }
    scope.texts[&text] = shown;

    const double t = now(scope);
    scratch.effect = &scope.effect;
    scratch.count = count;
    scratch.time = (float)std::max(0.0, t - scope.start);
    scratch.cycleTime = (float)std::max(0.0, t - scope.cycleStart);
    scratch.em = std::max(text.GetComputedValues().font_size(), 1.0f);
    auto reduced = reducedMotion.find(scope.context);
    scratch.reducedMotion = reduced != reducedMotion.end() && reduced->second;
    // a finished one-shot holds its end
    if (scope.finished){
        float end = 0.0f;
        for (const TextEffectLayer& layer : scope.effect.layers)
            end = std::max(end, TextEffects::oneShotLength(layer, count));
        scratch.cycleTime = std::max(scratch.cycleTime, end);
    }

    if (animates(scope))
        animated.push_back(text.GetObserverPtr());
    return &scratch;
}

void HtmlTextEffects::update(Rml::Context* context){
    frame++;

    // what animates is generated again (geometry only: its layout stays)
    std::vector<Rml::ObserverPtr<Rml::Element>> texts;
    texts.swap(animated);
    for (Rml::ObserverPtr<Rml::Element>& element : texts){
        if (Rml::Element* alive = element.get()){
            if (alive->GetContext() == context)
                static_cast<Rml::ElementText*>(alive)->DirtyGeometry();
            else
                animated.push_back(element);
        }
    }

    // one-shot effects that reached their end: again (text-effect-iteration), or texteffectend
    std::vector<Rml::ObserverPtr<Rml::Element>> ended;
    for (auto it = scopes.begin(); it != scopes.end();){
        Scope& scope = it->second;
        if (!scope.root.get()){
            it = scopes.erase(it);
            continue;
        }
        if (scope.context == context && !scope.finished && scope.lastCount > 0){
            float length = 0.0f;
            bool oneShot = false;
            auto reduced = reducedMotion.find(context);
            const bool still = reduced != reducedMotion.end() && reduced->second;
            for (const TextEffectLayer& layer : scope.effect.layers){
                if (!layer.enabled || !TextEffects::isOneShot(layer.type) || (still && TextEffects::isMotion(layer.type)))
                    continue;
                oneShot = true;
                length = std::max(length, TextEffects::oneShotLength(layer, scope.lastCount));
            }
            const double t = now(scope);
            if (oneShot && t - scope.cycleStart >= (double)length){
                scope.iterationsDone++;
                if (scope.iterations < 0 || scope.iterationsDone < scope.iterations){
                    scope.cycleStart = t;
                }else{
                    scope.finished = true;
                    ended.push_back(scope.root);
                }
            }
        }
        ++it;
    }
    for (Rml::ObserverPtr<Rml::Element>& element : ended){
        if (Rml::Element* alive = element.get())
            alive->DispatchEvent("texteffectend", Rml::Dictionary(), false, true);
    }
}

void HtmlTextEffects::setReducedMotion(Rml::Context* context, bool reduced){
    reducedMotion[context] = reduced;
}

void HtmlTextEffects::restart(Rml::Element* element){
    if (!element)
        return;
    // the effect specified on it, or the one it inherits
    const Rml::PropertyId id = propertyId(compat::CompatProperty::TextEffect);
    Rml::Element* root = element;
    while (root && !root->GetLocalProperty(id))
        root = root->GetParentNode();
    if (!root)
        root = element;
    for (auto& [key, scope] : scopes){
        Rml::Element* scopeRoot = scope.root.get();
        if (!scopeRoot)
            continue;
        // the element's own effect, and the effects of its descendants
        bool inside = false;
        for (Rml::Element* e = scopeRoot; e; e = e->GetParentNode()){
            if (e == root){
                inside = true;
                break;
            }
        }
        if (!inside)
            continue;
        scope.start = scope.cycleStart = now(scope);
        scope.iterationsDone = 0;
        scope.finished = false;
    }
    // its texts draw from the start at the next frame
    std::vector<Rml::Element*> stack{element};
    while (!stack.empty()){
        Rml::Element* e = stack.back();
        stack.pop_back();
        if (Rml::ElementText* text = rmlui_dynamic_cast<Rml::ElementText*>(e))
            text->DirtyGeometry();
        for (int i = 0; i < e->GetNumChildren(); i++)
            stack.push_back(e->GetChild(i));
    }
}

void HtmlTextEffects::forget(Rml::Context* context){
    for (auto it = scopes.begin(); it != scopes.end();){
        if (it->second.context == context) it = scopes.erase(it);
        else ++it;
    }
    reducedMotion.erase(context);
}
