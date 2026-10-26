// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// effect(<name>, <param> <value>, ...) and its typed RCSS form (phase-5 spec 3.2, 3.5).

#include "EffectSyntax.h"
#include "EffectValue.h"

#include <algorithm>
#include <cctype>
#include <map>

using namespace jasperengine;
using namespace jasperengine::htmlui;
namespace hc = jasperengine::htmlui::compat;

namespace {

    bool isSpace(char c){
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
    }

    std::string lower(std::string_view text){
        std::string out(text);
        for (char& c : out){
            if (c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 'a');
        }
        return out;
    }

    bool isIdentStart(char c){
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
    }

    bool isIdentChar(char c){
        return isIdentStart(c) || (c >= '0' && c <= '9');
    }

    // [begin, end) trimmed of white space
    void trimRange(std::string_view text, size_t& begin, size_t& end){
        while (begin < end && isSpace(text[begin]))
            begin++;
        while (end > begin && isSpace(text[end - 1]))
            end--;
    }

    // the top-level comma-separated ranges of a text (parentheses, brackets and strings nest)
    std::vector<std::pair<size_t, size_t>> commaRanges(std::string_view text){
        std::vector<std::pair<size_t, size_t>> ranges;
        int depth = 0;
        char quote = 0;
        size_t start = 0;
        for (size_t i = 0; i < text.size(); i++){
            const char c = text[i];
            if (quote){
                if (c == '\\')
                    i++;
                else if (c == quote)
                    quote = 0;
                continue;
            }
            if (c == '"' || c == '\'')
                quote = c;
            else if (c == '(' || c == '[')
                depth++;
            else if ((c == ')' || c == ']') && depth > 0)
                depth--;
            else if (c == ',' && depth == 0){
                ranges.push_back({start, i});
                start = i + 1;
            }
        }
        ranges.push_back({start, text.size()});
        return ranges;
    }

    std::vector<EffectParamDef> makeUniversal(){
        std::vector<EffectParamDef> list;
        EffectParamDef opacity;
        opacity.name = "opacity";
        opacity.uniform = "opacity";
        opacity.type = EffectParamType::Percentage;
        opacity.defaultText = "1";
        opacity.hasRange = true;
        opacity.minValue = 0.f;
        opacity.maxValue = 1.f;
        opacity.description = "Paint: multiplies the output alpha. Filter: mixes the result with the unfiltered pixels (0 = no effect)";
        list.push_back(opacity);
        EffectParamDef timeScale;
        timeScale.name = "time-scale";
        timeScale.uniform = "timeScale";
        timeScale.type = EffectParamType::Number;
        timeScale.defaultText = "1";
        timeScale.hasRange = true;
        timeScale.minValue = -10.f;
        timeScale.maxValue = 10.f;
        timeScale.description = "Multiplies the clock; 0 freezes the effect";
        list.push_back(timeScale);
        EffectParamDef timeOffset;
        timeOffset.name = "time-offset";
        timeOffset.uniform = "timeOffset";
        timeOffset.type = EffectParamType::Time;
        timeOffset.defaultText = "0s";
        timeOffset.hasRange = true;
        timeOffset.minValue = -3600.f;
        timeOffset.maxValue = 3600.f;
        timeOffset.description = "Added to the clock (desynchronises repeated elements)";
        list.push_back(timeOffset);
        EffectParamDef blend;
        blend.name = "blend";
        blend.uniform = "blend";
        blend.type = EffectParamType::Enum;
        blend.defaultText = "normal";
        blend.options = {"normal", "add", "screen", "multiply"};
        blend.description = "How a paint effect combines with what is below it (filter form: with the content)";
        list.push_back(blend);
        return list;
    }

    std::string nearest(const std::string& name, const std::vector<std::string>& candidates){
        std::string best;
        size_t bestDistance = 4;
        for (const std::string& candidate : candidates){
            const size_t d = EffectSyntax::editDistance(name, candidate);
            if (d < bestDistance){
                bestDistance = d;
                best = candidate;
            }
        }
        return best;
    }

}

size_t EffectSyntax::editDistance(std::string_view a, std::string_view b){
    std::vector<size_t> previous(b.size() + 1), current(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++)
        previous[j] = j;
    for (size_t i = 1; i <= a.size(); i++){
        current[0] = i;
        for (size_t j = 1; j <= b.size(); j++){
            const size_t substitution = previous[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
            current[j] = std::min(std::min(previous[j] + 1, current[j - 1] + 1), substitution);
        }
        std::swap(previous, current);
    }
    return previous[b.size()];
}

const std::vector<EffectParamDef>& EffectSyntax::universalParams(){
    static const std::vector<EffectParamDef> list = makeUniversal();
    return list;
}

std::vector<std::string> EffectSyntax::slotNames(const EffectManifest& m){
    std::vector<std::string> names;
    for (const EffectParamDef& def : m.params){
        if (def.type == EffectParamType::Position){
            names.push_back(def.name + "-x");
            names.push_back(def.name + "-y");
        }else{
            names.push_back(def.name);
        }
    }
    for (const EffectParamDef& def : universalParams()){
        if (def.name == "blend" && m.kind != EffectKind::Paint)
            continue;
        names.push_back(def.name);
    }
    return names;
}

std::string EffectSyntax::defaultCanonical(const EffectParamDef& def){
    EffectValue value;
    std::string error;
    if (!EffectValueParser::parse(def, def.defaultText, value, error))
        return def.defaultText;
    return EffectValueParser::canonical(def, value);
}

std::string EffectSyntax::slot(const std::string& text, char axis){
    if (axis == 'x')
        return "vx(" + text + ")";
    if (axis == 'y')
        return "vy(" + text + ")";
    return "v(" + text + ")";
}

bool EffectSyntax::unwrapSlot(std::string_view text, std::string& inner, char& axis){
    size_t begin = 0, end = text.size();
    trimRange(text, begin, end);
    std::string_view t = text.substr(begin, end - begin);
    axis = 0;
    size_t open = 0;
    if (t.size() >= 3 && (t[0] == 'v' || t[0] == 'V') && t[1] == '(')
        open = 2;
    else if (t.size() >= 4 && (t[0] == 'v' || t[0] == 'V') && (t[1] == 'x' || t[1] == 'y') && t[2] == '('){
        axis = t[1];
        open = 3;
    }else
        return false;
    if (t.back() != ')')
        return false;
    // the parenthesis that closes the slot is the last one
    int depth = 0;
    for (size_t i = open - 1; i < t.size(); i++){
        if (t[i] == '(')
            depth++;
        else if (t[i] == ')'){
            depth--;
            if (depth == 0 && i != t.size() - 1)
                return false;
        }
    }
    inner = std::string(t.substr(open, t.size() - open - 1));
    size_t b = 0, e = inner.size();
    trimRange(inner, b, e);
    inner = inner.substr(b, e - b);
    return true;
}

bool EffectSyntax::parse(std::string_view functionText, EffectCall& out, std::vector<EffectDiagnostic>& diags){
    out = EffectCall();
    size_t begin = 0, end = functionText.size();
    trimRange(functionText, begin, end);
    const std::string_view t = functionText.substr(begin, end - begin);
    if (t.size() < 7 || lower(t.substr(0, 7)) != "effect("){
        diags.push_back(makeEffectDiagnostic(hc::codes::EffectUnknown, "'" + std::string(t) + "' is not an effect() call", std::string(),
                                             begin, t.size()));
        return false;
    }
    const size_t open = begin + 7;
    size_t close = functionText.rfind(')');
    if (close == std::string_view::npos || close < open)
        close = end;   // an unclosed function ends with the text (CSS)
    const size_t before = diags.size();
    const bool ok = parseArguments(functionText.substr(open, close - open), out, diags);
    // offsets from the start of the function text
    out.nameOffset += open;
    for (EffectArg& arg : out.args){
        arg.nameOffset += open;
        arg.valueOffset += open;
    }
    for (size_t i = before; i < diags.size(); i++)
        diags[i].column += (uint32_t)open;
    return ok;
}

bool EffectSyntax::parseArguments(std::string_view text, EffectCall& out, std::vector<EffectDiagnostic>& diags){
    out = EffectCall();
    const std::vector<std::pair<size_t, size_t>> ranges = commaRanges(text);
    // the effect's name
    size_t nb = ranges[0].first, ne = ranges[0].second;
    trimRange(text, nb, ne);
    const std::string_view name = text.substr(nb, ne - nb);
    bool nameOk = !name.empty() && isIdentStart(name[0]);
    for (char c : name)
        nameOk = nameOk && isIdentChar(c);
    if (!nameOk){
        diags.push_back(makeEffectDiagnostic(hc::codes::EffectUnknown,
                                             name.empty() ? std::string("effect() names no effect") : "'" + std::string(name) + "' is not an effect name",
                                             "write effect(<name>, <param> <value>, ...); see jasper://docs/html-ui-effects", nb, name.size()));
        return false;
    }
    out.name = lower(name);
    out.nameOffset = nb;

    for (size_t r = 1; r < ranges.size(); r++){
        size_t b = ranges[r].first, e = ranges[r].second;
        trimRange(text, b, e);
        if (b == e){
            // a trailing comma (or two in a row)
            diags.push_back(makeEffectDiagnostic(hc::codes::EffectPositionalArg, "an empty argument in effect(" + out.name + ")",
                                                 "remove the stray comma", ranges[r].first, 0));
            continue;
        }
        size_t i = b;
        while (i < e && isIdentChar(text[i]))
            i++;
        const bool named = i > b && isIdentStart(text[b]) && i < e && isSpace(text[i]);
        if (!named){
            // a value without a name ("2"), or a name without a value ("speed")
            const bool bareName = i == e && isIdentStart(text[b]);
            EffectArg arg;
            arg.name = bareName ? lower(text.substr(b, e - b)) : std::string();
            arg.nameOffset = b;
            arg.valueOffset = e;
            arg.length = e - b;
            if (bareName){
                out.args.push_back(arg);   // checked against the effect: a parameter without a value
                continue;
            }
            diags.push_back(makeEffectDiagnostic(hc::codes::EffectPositionalArg,
                                                 "'" + std::string(text.substr(b, e - b)) + "' in effect(" + out.name + ") has no parameter name",
                                                 "name the parameter: <param> <value>", b, e - b));
            continue;
        }
        EffectArg arg;
        arg.name = lower(text.substr(b, i - b));
        arg.nameOffset = b;
        size_t vb = i, ve = e;
        trimRange(text, vb, ve);
        arg.value = std::string(text.substr(vb, ve - vb));
        arg.valueOffset = vb;
        arg.length = e - b;
        out.args.push_back(arg);
    }
    return true;
}

std::string EffectSyntax::toRcss(const EffectCall& call, const EffectManifest& m, bool filterForm, std::string_view paintArea,
                                 std::vector<EffectDiagnostic>& diags){
    (void)filterForm;
    // the given value of each parameter (the last one wins)
    std::map<std::string, const EffectArg*> given;
    std::vector<std::string> paramNames;
    for (const EffectParamDef& def : m.params)
        paramNames.push_back(def.name);
    for (const EffectParamDef& def : universalParams())
        paramNames.push_back(def.name);

    for (const EffectArg& arg : call.args){
        const bool known = m.findParam(arg.name) ||
                           std::any_of(universalParams().begin(), universalParams().end(), [&](const EffectParamDef& u){ return u.name == arg.name; });
        if (!known){
            const std::string suggestion = nearest(arg.name, paramNames);
            diags.push_back(makeEffectDiagnostic(hc::codes::EffectUnknownParam,
                                                 "'" + m.name + "' has no parameter '" + arg.name + "'; it is ignored",
                                                 suggestion.empty() ? std::string() : suggestion, arg.nameOffset, arg.name.size()));
            continue;
        }
        if (arg.name == "blend" && m.kind == EffectKind::Filter){
            diags.push_back(makeEffectDiagnostic(hc::codes::EffectBlendOnFilter,
                                                 "'" + m.name + "' is a filter effect: blend applies to paint effects and is ignored",
                                                 "remove blend", arg.nameOffset, arg.length));
            continue;
        }
        auto it = given.find(arg.name);
        if (it != given.end()){
            diags.push_back(makeEffectDiagnostic(hc::codes::EffectDuplicateParam,
                                                 "'" + arg.name + "' is given more than once in effect(" + m.name + "); the last value is used",
                                                 std::string(), it->second->nameOffset, it->second->length));
        }
        given[arg.name] = &arg;
    }

    // @preset: the chosen preset (written literally) fills the slots of the parameters the call
    // does not write; written ones keep their values
    const EffectPresetDef* preset = nullptr;
    if (!m.presets.empty()){
        auto chosen = given.find("preset");
        const EffectParamDef* presetDef = m.findParam("preset");
        EffectValue value;
        std::string reason;
        if (chosen != given.end() && presetDef && EffectValueParser::parse(*presetDef, chosen->second->value, value, reason) && !value.isVar)
            preset = m.findPreset(EffectValueParser::canonical(*presetDef, value));
    }
    auto presetText = [&](const EffectParamDef& def) -> std::string {
        if (preset){
            for (const auto& entry : preset->values){
                if (entry.first != def.name)
                    continue;
                EffectValue value;
                std::string reason;
                if (EffectValueParser::parse(def, entry.second, value, reason) && !value.isVar){
                    EffectValueParser::clamp(def, value);
                    return EffectValueParser::canonical(def, value);
                }
            }
        }
        return defaultCanonical(def);
    };

    auto slotText = [&](const EffectParamDef& def) -> std::string {
        auto it = given.find(def.name);
        if (it == given.end())
            return presetText(def);
        const EffectArg& arg = *it->second;
        if (arg.value.empty()){
            diags.push_back(makeEffectDiagnostic(hc::codes::EffectBadValue, "'" + def.name + "' has no value; the default is used",
                                                 "expected " + EffectValueParser::expected(def), arg.nameOffset, arg.length));
            return defaultCanonical(def);
        }
        EffectValue value;
        std::string reason;
        if (!EffectValueParser::parse(def, arg.value, value, reason)){
            diags.push_back(makeEffectDiagnostic(hc::codes::EffectBadValue,
                                                 "'" + arg.value + "' is not a valid " + def.name + " (" + reason + "); the default is used",
                                                 "expected " + EffectValueParser::expected(def), arg.valueOffset, arg.value.size()));
            return defaultCanonical(def);
        }
        if (value.isVar)
            return arg.value;
        if (EffectValueParser::clamp(def, value)){
            diags.push_back(makeEffectDiagnostic(hc::codes::EffectOutOfRange,
                                                 "'" + arg.value + "' is outside the range of " + def.name + "; clamped to " +
                                                 EffectValueParser::canonical(def, value),
                                                 "expected " + EffectValueParser::expected(def), arg.valueOffset, arg.value.size()));
        }
        return EffectValueParser::canonical(def, value);
    };

    std::string out = "jfx-" + m.name + "(";
    bool first = true;
    auto append = [&](const std::string& s){
        if (!first)
            out += ' ';
        first = false;
        out += s;
    };
    for (const EffectParamDef& def : m.params){
        const std::string text = slotText(def);
        if (def.type == EffectParamType::Position){
            append(slot(text, 'x'));
            append(slot(text, 'y'));
        }else{
            append(slot(text));
        }
    }
    for (const EffectParamDef& def : universalParams()){
        if (def.name == "blend" && m.kind != EffectKind::Paint)
            continue;
        append(slot(slotText(def)));
    }
    out += ')';
    if (!paintArea.empty()){
        out += ' ';
        out += paintArea;
    }
    return out;
}
