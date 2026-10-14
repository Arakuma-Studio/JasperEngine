// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Effect manifests (phase-5 spec 3.9): the "// @" comment lines right after #version 450 of an
// effect's fragment file, read into an EffectManifest and checked.

#include "EffectManifest.h"
#include "EffectValue.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <set>

using namespace jasperengine;
using namespace jasperengine::htmlui;
namespace hc = jasperengine::htmlui::compat;

namespace {

    const char* const kCategories[] = {"gradient", "noise", "pattern", "light", "energy", "space", "interactive",
                                       "glass", "blur", "stylize", "color", "distort", "transition"};
    const char* const kUniversalParams[] = {"opacity", "time-scale", "time-offset", "blend"};

    std::string trim(std::string_view text){
        size_t begin = 0, end = text.size();
        while (begin < end && std::isspace((unsigned char)text[begin]))
            begin++;
        while (end > begin && std::isspace((unsigned char)text[end - 1]))
            end--;
        return std::string(text.substr(begin, end - begin));
    }

    std::vector<std::string> splitLines(const std::string& source){
        std::vector<std::string> lines;
        size_t start = 0;
        while (start <= source.size()){
            size_t end = source.find('\n', start);
            if (end == std::string::npos)
                end = source.size();
            std::string line = source.substr(start, end - start);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            lines.push_back(line);
            start = end + 1;
        }
        return lines;
    }

    // the index of the #version line (-1 without one before any code)
    int versionLine(const std::vector<std::string>& lines){
        for (size_t i = 0; i < lines.size(); i++){
            const std::string t = trim(lines[i]);
            if (t.empty())
                continue;
            if (t.rfind("#version", 0) == 0)
                return (int)i;
            return -1;
        }
        return -1;
    }

    // "@name rest" of a manifest line ("// @name rest"); false when the line is not one
    bool directiveOf(const std::string& line, std::string& name, std::string& rest){
        std::string t = trim(line);
        if (t.rfind("//", 0) != 0)
            return false;
        t = trim(std::string_view(t).substr(2));
        if (t.empty() || t[0] != '@')
            return false;
        size_t i = 1;
        while (i < t.size() && !std::isspace((unsigned char)t[i]))
            i++;
        name = t.substr(1, i - 1);
        rest = trim(std::string_view(t).substr(i));
        return true;
    }

    // whitespace-separated words, "double quoted" words kept whole (quotes removed)
    bool wordsOf(const std::string& text, std::vector<std::string>& words){
        words.clear();
        size_t i = 0;
        while (i < text.size()){
            while (i < text.size() && std::isspace((unsigned char)text[i]))
                i++;
            if (i >= text.size())
                break;
            if (text[i] == '"'){
                const size_t close = text.find('"', i + 1);
                if (close == std::string::npos)
                    return false;
                words.push_back(text.substr(i + 1, close - i - 1));
                i = close + 1;
                continue;
            }
            size_t j = i;
            while (j < text.size() && !std::isspace((unsigned char)text[j]))
                j++;
            words.push_back(text.substr(i, j - i));
            i = j;
        }
        return true;
    }

    bool isEffectName(const std::string& name){
        if (name.empty() || !(name[0] >= 'a' && name[0] <= 'z'))
            return false;
        for (char c : name){
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
                return false;
        }
        return true;
    }

    bool typeOf(const std::string& word, EffectParamType& type){
        static const std::pair<const char*, EffectParamType> types[] = {
            {"number", EffectParamType::Number}, {"integer", EffectParamType::Integer}, {"percentage", EffectParamType::Percentage},
            {"length", EffectParamType::Length}, {"angle", EffectParamType::Angle}, {"time", EffectParamType::Time},
            {"color", EffectParamType::Color}, {"position", EffectParamType::Position}, {"enum", EffectParamType::Enum},
            {"boolean", EffectParamType::Boolean}, {"palette", EffectParamType::Palette}};
        for (const auto& entry : types){
            if (word == entry.first){
                type = entry.second;
                return true;
            }
        }
        return false;
    }

    // a range bound in the type's normalised unit
    bool boundOf(EffectParamType type, const std::string& text, float& out, std::string& error){
        EffectParamDef def;
        def.type = type;
        EffectValue value;
        if (!EffectValueParser::parse(def, text, value, error) || value.isVar)
            return false;
        if (type == EffectParamType::Length){
            if (value.length.unit != EffectLength::Unit::Px){
                error = "length ranges are in px";
                return false;
            }
            out = value.length.number;
            return true;
        }
        out = value.f;
        return true;
    }

    // a bare number: is it one (ranges of number params)?
    bool isRangeWord(EffectParamType type, const std::string& word){
        if (word.empty())
            return false;
        float bound = 0.f;
        std::string error;
        switch (type){
            case EffectParamType::Number:
            case EffectParamType::Integer:
            case EffectParamType::Percentage:
            case EffectParamType::Length:
            case EffectParamType::Angle:
            case EffectParamType::Time:
                return boundOf(type, word, bound, error);
            default:
                return false;
        }
    }

}

const EffectParamDef* EffectManifest::findParam(const std::string& paramName) const{
    for (const EffectParamDef& def : params){
        if (def.name == paramName)
            return &def;
    }
    return nullptr;
}

int EffectManifest::paramIndex(const std::string& paramName) const{
    for (size_t i = 0; i < params.size(); i++){
        if (params[i].name == paramName)
            return (int)i;
    }
    return -1;
}

std::string EffectManifest::customShaderName() const{
    return builtin ? "@effects/" + name : sourcePath;
}

EffectDiagnostic jasperengine::htmlui::makeEffectDiagnostic(hc::DiagCode code, std::string message, std::string suggestion,
                                                             size_t offset, size_t length){
    const hc::DiagCodeInfo* info = hc::findDiagnosticCode(code);
    return makeEffectDiagnostic(info ? info->severity : hc::Severity::Warning, code, std::move(message), std::move(suggestion), offset, length);
}

EffectDiagnostic jasperengine::htmlui::makeEffectDiagnostic(hc::Severity severity, hc::DiagCode code, std::string message,
                                                             std::string suggestion, size_t offset, size_t length){
    EffectDiagnostic d;
    d.severity = severity;
    d.code = code;
    d.message = std::move(message);
    d.suggestion = std::move(suggestion);
    d.line = 0;
    d.column = (uint32_t)(offset + 1);
    d.length = (uint32_t)length;
    return d;
}

const char* jasperengine::htmlui::effectKindName(EffectKind kind){
    return kind == EffectKind::Paint ? "paint" : "filter";
}

const char* jasperengine::htmlui::effectParamTypeName(EffectParamType type){
    switch (type){
        case EffectParamType::Number: return "number";
        case EffectParamType::Integer: return "integer";
        case EffectParamType::Percentage: return "percentage";
        case EffectParamType::Length: return "length";
        case EffectParamType::Angle: return "angle";
        case EffectParamType::Time: return "time";
        case EffectParamType::Color: return "color";
        case EffectParamType::Position: return "position";
        case EffectParamType::Enum: return "enum";
        case EffectParamType::Boolean: return "boolean";
        case EffectParamType::Palette: return "palette";
    }
    return "number";
}

const char* jasperengine::htmlui::effectCostName(EffectCost cost){
    switch (cost){
        case EffectCost::Low: return "low";
        case EffectCost::Medium: return "medium";
        case EffectCost::High: return "high";
    }
    return "low";
}

const char* jasperengine::htmlui::effectPointerName(EffectPointer pointer){
    switch (pointer){
        case EffectPointer::None: return "none";
        case EffectPointer::Position: return "position";
        case EffectPointer::Trail: return "trail";
        case EffectPointer::Ripples: return "ripples";
    }
    return "none";
}

std::string jasperengine::htmlui::effectUniformName(const std::string& kebab){
    std::string out;
    bool upper = false;
    for (char c : kebab){
        if (c == '-'){
            upper = true;
            continue;
        }
        out += upper ? (char)std::toupper((unsigned char)c) : c;
        upper = false;
    }
    return out;
}

bool jasperengine::htmlui::isReservedEffectName(const std::string& name){
    static const char* const reserved[] = {"linear-gradient", "radial-gradient", "conic-gradient", "url", "image", "blur",
                                           "drop-shadow", "brightness", "contrast", "grayscale", "hue-rotate", "invert",
                                           "opacity", "saturate", "sepia",
                                           // the fallback instancers' name
                                           "effect"};
    for (const char* r : reserved){
        if (name == r)
            return true;
    }
    return false;
}

bool EffectManifestParser::parseScaled(std::string_view text, EffectScaledParam& out){
    out = EffectScaledParam();
    std::string t;
    for (char c : text){
        if (!std::isspace((unsigned char)c))
            t += c;
    }
    if (t.empty())
        return false;
    auto parsePx = [](const std::string& s, float& value){
        if (s.size() < 3 || s.compare(s.size() - 2, 2, "px") != 0)
            return false;
        char* end = nullptr;
        const std::string number = s.substr(0, s.size() - 2);
        value = std::strtof(number.c_str(), &end);
        return end && *end == '\0' && !number.empty();
    };
    // <number>px
    float px = 0.f;
    if (parsePx(t, px)){
        out.addPx = px;
        return true;
    }
    // <param>[*<number>][+<number>px]
    size_t i = 0;
    while (i < t.size() && (std::isalnum((unsigned char)t[i]) || t[i] == '-'))
        i++;
    out.param = t.substr(0, i);
    if (!isEffectName(out.param))
        return false;
    if (i < t.size() && t[i] == '*'){
        size_t j = i + 1;
        while (j < t.size() && (std::isdigit((unsigned char)t[j]) || t[j] == '.'))
            j++;
        const std::string number = t.substr(i + 1, j - i - 1);
        if (number.empty())
            return false;
        out.scale = std::strtof(number.c_str(), nullptr);
        i = j;
    }
    if (i < t.size() && (t[i] == '+' || t[i] == '-')){
        const bool negative = t[i] == '-';
        if (!parsePx(t.substr(i + 1), px))
            return false;
        out.addPx = negative ? -px : px;
        i = t.size();
    }
    return i == t.size();
}

std::string EffectManifestParser::manifestBlock(const std::string& source){
    const std::vector<std::string> lines = splitLines(source);
    const int version = versionLine(lines);
    if (version < 0)
        return std::string();
    std::string block;
    for (size_t i = (size_t)version + 1; i < lines.size(); i++){
        std::string name, rest;
        if (!directiveOf(lines[i], name, rest))
            break;
        block += lines[i];
        block += '\n';
    }
    return block;
}

bool EffectManifestParser::hasEffectManifest(const std::string& source){
    const std::vector<std::string> lines = splitLines(source);
    const int version = versionLine(lines);
    if (version < 0 || (size_t)version + 1 >= lines.size())
        return false;
    std::string name, rest;
    return directiveOf(lines[version + 1], name, rest) && name == "effect";
}

bool EffectManifestParser::parse(const std::string& source, const std::string& path, bool builtin, EffectManifest& out,
                                 std::vector<EffectDiagnostic>& diags){
    out = EffectManifest();
    out.builtin = builtin;
    out.sourcePath = path;
    const size_t diagsBefore = diags.size();
    auto error = [&](int line, const std::string& message, const std::string& suggestion = std::string()){
        EffectDiagnostic d = makeEffectDiagnostic(hc::codes::EffectManifest, message, suggestion);
        d.file = path;
        d.line = (uint32_t)line;
        d.column = 1;
        d.length = 0;
        diags.push_back(d);
    };

    const std::vector<std::string> lines = splitLines(source);
    const int version = versionLine(lines);
    if (version < 0){
        error(1, "an effect file starts with #version 450", "put the manifest's // @ lines right after #version 450");
        return false;
    }
    bool kindSet = false;
    bool any = false;
    for (size_t i = (size_t)version + 1; i < lines.size(); i++){
        std::string name, rest;
        if (!directiveOf(lines[i], name, rest))
            break;
        const int line = (int)i + 1;
        if (!any){
            out.line = line;
            any = true;
        }
        if (name == "effect"){
            out.name = rest;
        }else if (name == "kind"){
            if (rest == "paint") out.kind = EffectKind::Paint;
            else if (rest == "filter") out.kind = EffectKind::Filter;
            else{
                error(line, "@kind is paint or filter, not '" + rest + "'");
                continue;
            }
            kindSet = true;
        }else if (name == "category"){
            out.category = rest;
        }else if (name == "summary"){
            out.summary = rest;
        }else if (name == "source"){
            out.source = rest;
        }else if (name == "cost"){
            if (rest == "low") out.cost = EffectCost::Low;
            else if (rest == "medium") out.cost = EffectCost::Medium;
            else if (rest == "high") out.cost = EffectCost::High;
            else error(line, "@cost is low, medium or high, not '" + rest + "'");
        }else if (name == "backdrop"){
            if (rest == "yes") out.backdrop = true;
            else if (rest == "no") out.backdrop = false;
            else error(line, "@backdrop is yes or no, not '" + rest + "'");
        }else if (name == "time"){
            out.timeParams.clear();
            std::string item;
            for (char c : rest + ","){
                if (c == ','){
                    const std::string param = trim(item);
                    if (!param.empty())
                        out.timeParams.push_back(param);
                    item.clear();
                }else{
                    item += c;
                }
            }
            if (out.timeParams.empty() || out.timeParams.size() > 4)
                error(line, "@time lists 1 to 4 rate parameters", "@time speed");
        }else if (name == "pointer"){
            if (rest == "none") out.pointer = EffectPointer::None;
            else if (rest == "position") out.pointer = EffectPointer::Position;
            else if (rest == "trail") out.pointer = EffectPointer::Trail;
            else if (rest == "ripples") out.pointer = EffectPointer::Ripples;
            else error(line, "@pointer is none, position, trail or ripples, not '" + rest + "'");
        }else if (name == "prepass"){
            std::vector<std::string> words;
            wordsOf(rest, words);
            if (words.size() < 2 || words[0] != "blur" || !parseScaled(words[1], out.prepassSigma) ||
                !(words.size() == 2 || (words.size() == 4 && words[2] == "threshold"))){
                error(line, "@prepass is 'blur <scaled> [threshold <param>]'", "@prepass blur blur");
                continue;
            }
            out.hasPrepass = true;
            if (words.size() == 4)
                out.prepassThreshold = words[3];
        }else if (name == "margin"){
            if (!parseScaled(rest, out.margin))
                error(line, "@margin is '<param>[*<number>][+<number>px]' or '<number>px'", "@margin blur*3");
        }else if (name == "geometry"){
            std::vector<std::string> words;
            wordsOf(rest, words);
            if (words.size() == 1 && words[0] == "fill"){
                out.boundsGeometry = false;
            }else if (words.size() == 2 && words[0] == "bounds" && parseScaled(words[1], out.boundsExtent)){
                out.boundsGeometry = true;
            }else{
                error(line, "@geometry is 'fill' or 'bounds <scaled>'", "@geometry bounds extent");
            }
        }else if (name == "suggest-for"){
            std::vector<std::string> words;
            wordsOf(rest, words);
            out.suggestFor.insert(out.suggestFor.end(), words.begin(), words.end());
        }else if (name == "param"){
            EffectParamDef def;
            def.line = line;
            std::string head = rest;
            const size_t dashes = rest.find(" -- ");
            if (dashes != std::string::npos){
                head = rest.substr(0, dashes);
                def.description = trim(std::string_view(rest).substr(dashes + 4));
            }
            std::vector<std::string> words;
            if (!wordsOf(head, words)){
                error(line, "an unterminated \" in @param");
                continue;
            }
            if (words.size() < 3){
                error(line, "@param is '<name> <type> <default> [<min> <max>] [<opt>|<opt>...] -- <description>'");
                continue;
            }
            def.name = words[0];
            def.uniform = effectUniformName(def.name);
            if (!typeOf(words[1], def.type)){
                error(line, "unknown parameter type '" + words[1] + "'",
                      "number, integer, percentage, length, angle, time, color, position, enum, boolean or palette");
                continue;
            }
            def.defaultText = words[2];
            size_t next = 3;
            if (def.type == EffectParamType::Enum){
                if (next >= words.size()){
                    error(line, "an enum parameter lists its options: a|b|c");
                    continue;
                }
                std::string item;
                for (char c : words[next] + "|"){
                    if (c == '|'){
                        if (!item.empty())
                            def.options.push_back(item);
                        item.clear();
                    }else{
                        item += c;
                    }
                }
                next++;
            }else if (next + 1 < words.size() && isRangeWord(def.type, words[next]) && isRangeWord(def.type, words[next + 1])){
                std::string boundError;
                if (!boundOf(def.type, words[next], def.minValue, boundError) || !boundOf(def.type, words[next + 1], def.maxValue, boundError)){
                    error(line, "bad range of '" + def.name + "': " + boundError);
                    continue;
                }
                def.hasRange = true;
                next += 2;
            }
            if (next != words.size()){
                error(line, "unexpected '" + words[next] + "' in @param " + def.name);
                continue;
            }
            out.params.push_back(def);
        }else{
            error(line, "unknown manifest directive '@" + name + "'",
                  "@effect, @kind, @category, @summary, @source, @cost, @backdrop, @time, @pointer, @prepass, @margin, @geometry, "
                  "@suggest-for, @param");
        }
    }
    if (!any || out.name.empty()){
        error(version + 2, "the manifest starts with '// @effect <name>' right after #version 450");
        return false;
    }
    if (!kindSet)
        error(out.line, "the manifest has no @kind", "// @kind paint (or filter)");
    if (!builtin && out.category.empty())
        out.category = "interactive";
    return diags.size() == diagsBefore;
}

bool EffectManifestParser::validate(const EffectManifest& m, const std::string& source, std::vector<EffectDiagnostic>& diags){
    const size_t diagsBefore = diags.size();
    auto error = [&](int line, const std::string& message, const std::string& suggestion = std::string()){
        EffectDiagnostic d = makeEffectDiagnostic(hc::codes::EffectManifest, message, suggestion);
        d.file = m.sourcePath;
        d.line = (uint32_t)(line > 0 ? line : m.line);
        d.column = 1;
        d.length = 0;
        diags.push_back(d);
    };

    if (!isEffectName(m.name))
        error(m.line, "'" + m.name + "' is not an effect name: lower case letters, digits and '-', starting with a letter");
    if (isReservedEffectName(m.name))
        error(m.line, "'" + m.name + "' is the name of a CSS function; an effect cannot use it", "rename the effect");
    if (std::find_if(std::begin(kCategories), std::end(kCategories), [&](const char* c){ return m.category == c; }) == std::end(kCategories)){
        std::string list;
        for (const char* c : kCategories)
            list += (list.empty() ? "" : ", ") + std::string(c);
        error(m.line, "unknown @category '" + m.category + "'", list);
    }
    if (m.builtin && m.summary.empty())
        error(m.line, "the manifest has no @summary");
    if (m.summary.size() > 100)
        error(m.line, "@summary is longer than 100 characters");
    if (m.builtin && m.source.empty())
        error(m.line, "the manifest has no @source", "@source original (or shaders.com <Name>)");
    if (!m.source.empty() && m.source.rfind("shaders.com ", 0) != 0 && m.source.rfind("original", 0) != 0)
        error(m.line, "@source starts with 'shaders.com <Name>' or 'original'");
    if (m.kind == EffectKind::Paint){
        if (m.backdrop)
            error(m.line, "@backdrop is for filter effects");
        if (m.hasPrepass)
            error(m.line, "@prepass is for filter effects");
        if (!m.margin.param.empty() || m.margin.addPx != 0.f)
            error(m.line, "@margin is for filter effects");
    }else if (m.boundsGeometry){
        error(m.line, "@geometry is for paint effects");
    }

    // parameters: unique, kebab-case, not universal, defaults legal and in range
    std::set<std::string> names;
    for (const EffectParamDef& def : m.params){
        if (!isEffectName(def.name))
            error(def.line, "'" + def.name + "' is not a parameter name: lower case letters, digits and '-'");
        if (!names.insert(def.name).second)
            error(def.line, "parameter '" + def.name + "' is declared twice");
        for (const char* universal : kUniversalParams){
            if (def.name == universal)
                error(def.line, "'" + def.name + "' is a universal parameter of every effect; it cannot be declared");
        }
        if (def.type == EffectParamType::Position){
            // its two properties <p>-x and <p>-y must not clash with another parameter
            for (const EffectParamDef& other : m.params){
                if (other.name == def.name + "-x" || other.name == def.name + "-y")
                    error(other.line, "'" + other.name + "' clashes with the position parameter '" + def.name + "'");
            }
        }
        if (def.type == EffectParamType::Enum && def.options.empty())
            error(def.line, "enum parameter '" + def.name + "' has no options");
        if (def.hasRange && def.minValue > def.maxValue)
            error(def.line, "the range of '" + def.name + "' is reversed");
        EffectValue value;
        std::string reason;
        if (!EffectValueParser::parse(def, def.defaultText, value, reason) || value.isVar){
            error(def.line, "the default of '" + def.name + "' is not " + EffectValueParser::expected(def) +
                  (reason.empty() ? std::string() : ": " + reason));
            continue;
        }
        EffectValue clamped = value;
        if (EffectValueParser::clamp(def, clamped))
            error(def.line, "the default of '" + def.name + "' is outside its range");
    }

    auto checkRate = [&](const std::string& param, const char* what){
        const EffectParamDef* def = m.findParam(param);
        if (!def)
            error(m.line, std::string(what) + " names '" + param + "', which is not a parameter");
        return def;
    };
    for (const std::string& param : m.timeParams){
        const EffectParamDef* def = checkRate(param, "@time");
        if (def && def->type != EffectParamType::Number && def->type != EffectParamType::Integer && def->type != EffectParamType::Percentage)
            error(def->line, "@time parameter '" + param + "' is not a number");
    }
    auto checkScaled = [&](const EffectScaledParam& scaled, const char* what){
        if (scaled.param.empty())
            return;
        const EffectParamDef* def = checkRate(scaled.param, what);
        if (def && def->type != EffectParamType::Length && def->type != EffectParamType::Number && def->type != EffectParamType::Integer)
            error(def->line, std::string(what) + " parameter '" + scaled.param + "' is not a length or a number");
    };
    if (m.hasPrepass){
        checkScaled(m.prepassSigma, "@prepass");
        if (!m.prepassThreshold.empty())
            checkRate(m.prepassThreshold, "@prepass threshold");
    }
    checkScaled(m.margin, "@margin");
    if (m.boundsGeometry)
        checkScaled(m.boundsExtent, "@geometry");

    // the main include matches the kind, exactly once
    const std::string paintMain = "#include \"effects/lib/main_paint.glsl\"";
    const std::string filterMain = "#include \"effects/lib/main_filter.glsl\"";
    int paintCount = 0, filterCount = 0, mainLine = 0;
    const std::vector<std::string> lines = splitLines(source);
    for (size_t i = 0; i < lines.size(); i++){
        const std::string t = trim(lines[i]);
        if (t == paintMain){
            paintCount++;
            mainLine = (int)i + 1;
        }else if (t == filterMain){
            filterCount++;
            mainLine = (int)i + 1;
        }
    }
    const bool paint = m.kind == EffectKind::Paint;
    if ((paint ? paintCount : filterCount) != 1 || (paint ? filterCount : paintCount) != 0){
        error(mainLine, std::string("a ") + effectKindName(m.kind) + " effect ends with exactly one " + (paint ? paintMain : filterMain),
              paint ? paintMain : filterMain);
    }
    return diags.size() == diagsBefore;
}

bool EffectManifestParser::checkReflection(const EffectManifest& m, ShaderData& shader, std::vector<std::string>& problems){
    const size_t before = problems.size();
    unsigned int frameSize = 0;
    if (!shader.getUniformBlockMembers("u_fs_effectFrame", frameSize))
        problems.push_back("the variant has no u_fs_effectFrame block (does the effect include effects/lib/jfx_common.glsl?)");
    unsigned int paramsSize = 0;
    const std::vector<ShaderUniform>* members = shader.getUniformBlockMembers("u_fs_effectParams", paramsSize);
    struct Expected { std::string name; ShaderUniformType type; unsigned int count; bool seen; };
    std::vector<Expected> expected;
    for (const EffectParamDef& def : m.params){
        switch (def.type){
            case EffectParamType::Position: expected.push_back({def.uniform, ShaderUniformType::FLOAT2, 1, false}); break;
            case EffectParamType::Color: expected.push_back({def.uniform, ShaderUniformType::FLOAT4, 1, false}); break;
            case EffectParamType::Palette:
                expected.push_back({def.uniform, ShaderUniformType::FLOAT4, 8, false});
                expected.push_back({def.uniform + "Stops", ShaderUniformType::FLOAT4, 2, false});
                expected.push_back({def.uniform + "Count", ShaderUniformType::FLOAT, 1, false});
                break;
            default: expected.push_back({def.uniform, ShaderUniformType::FLOAT, 1, false}); break;
        }
    }
    if (!members){
        if (!expected.empty())
            problems.push_back("the variant has no u_fs_effectParams block, but the manifest declares parameters (the effect reads none of them?)");
        return problems.size() == before;
    }
    for (const ShaderUniform& member : *members){
        const std::string name = ShaderData::getUniformShortName(member.name);
        // (a member named like a GLSL built-in, `noise`, is reflected as `_noise`)
        auto it = std::find_if(expected.begin(), expected.end(), [&](const Expected& e){ return e.name == name || "_" + e.name == name; });
        if (it == expected.end()){
            problems.push_back("u_fs_effectParams has a member '" + name + "' that no @param declares");
            continue;
        }
        it->seen = true;
        const unsigned int count = member.arrayCount > 0 ? member.arrayCount : 1;
        if (member.type != it->type || count != it->count){
            problems.push_back("u_fs_effectParams member '" + name + "' has the wrong type for its @param (int members are not allowed; "
                               "number-like params are float, position vec2, color vec4, palette vec4[8] + vec4[2] Stops + float Count)");
        }
    }
    for (const Expected& e : expected){
        if (!e.seen)
            problems.push_back("u_fs_effectParams has no member '" + e.name + "' (the effect's @param needs one)");
    }
    return problems.size() == before;
}

