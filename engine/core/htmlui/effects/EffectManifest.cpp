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

namespace {

    bool numberOf(const std::string& word, float& out){
        if (word.empty())
            return false;
        char* end = nullptr;
        out = std::strtof(word.c_str(), &end);
        return end && *end == '\0' && std::isfinite(out);
    }

    bool integerOf(const std::string& word, int& out){
        if (word.empty())
            return false;
        char* end = nullptr;
        const long value = std::strtol(word.c_str(), &end, 10);
        if (!end || *end != '\0')
            return false;
        out = (int)value;
        return true;
    }

    // "0.1", "0.1s", "100ms"
    bool secondsOf(const std::string& word, float& out){
        std::string t = trim(word);
        float scale = 1.f;
        if (t.size() > 2 && t.compare(t.size() - 2, 2, "ms") == 0){
            t.resize(t.size() - 2);
            scale = 0.001f;
        }else if (t.size() > 1 && t.back() == 's'){
            t.pop_back();
        }
        if (!numberOf(t, out))
            return false;
        out *= scale;
        return true;
    }

    bool isPassName(const std::string& name){
        return isEffectName(name);
    }

    template<typename ErrorFn>
    void parseState(const std::string& rest, int line, EffectManifest& out, ErrorFn& error){
        std::vector<std::string> words;
        wordsOf(rest, words);
        const char* grammar = "@state is '<name> rgba8|rgba16f|rg16f|r16f|rgba32f|r32f <w>x<h>|box[*<scale>] [max <px>]|aspect <short> [max <px>] "
                              "[linear|nearest] [clamp|repeat] [clear|resample]'";
        if (words.size() < 3){
            error(line, grammar, "@state vel rgba16f 256x256");
            return;
        }
        EffectStateDef st;
        st.line = line;
        st.name = words[0];
        if (!isEffectName(st.name)){
            error(line, "'" + st.name + "' is not a state name: lower case letters, digits and '-', starting with a letter");
            return;
        }
        if (words[1] == "rgba8") st.format = EffectStateFormat::RGBA8;
        else if (words[1] == "rgba16f") st.format = EffectStateFormat::RGBA16F;
        else if (words[1] == "rgba32f") st.format = EffectStateFormat::RGBA32F;
        else if (words[1] == "r32f") st.format = EffectStateFormat::R32F;
        else if (words[1] == "rg16f") st.format = EffectStateFormat::RG16F;
        else if (words[1] == "r16f") st.format = EffectStateFormat::R16F;
        else{
            error(line, "unknown state format '" + words[1] + "'", "rgba8, rgba16f, rg16f, r16f, rgba32f or r32f");
            return;
        }
        // 32-bit float images are not filterable everywhere: nearest by default
        st.linear = st.format != EffectStateFormat::RGBA32F && st.format != EffectStateFormat::R32F;
        const std::string& size = words[2];
        size_t next = 3;
        auto parseMax = [&]() -> bool {
            if (next < words.size() && words[next] == "max"){
                int max = 0;
                if (next + 1 >= words.size() || !integerOf(words[next + 1], max) || max < 1 || max > 4096){
                    error(line, "'max' takes the largest side in texels, 1 to 4096", "box max 1024");
                    return false;
                }
                st.boxMax = max;
                next += 2;
            }
            return true;
        };
        if (size == "aspect"){
            // the area's aspect, the shorter side a fixed number of texels
            int shortSide = 0;
            if (next >= words.size() || !integerOf(words[next], shortSide) || shortSide < 1 || shortSide > 4096){
                error(line, "an aspect size is 'aspect <short>': the shorter side in texels, 1 to 4096", "aspect 128 max 512");
                return;
            }
            st.fixedSize = false;
            st.aspectShort = shortSide;
            next++;
            if (!parseMax())
                return;
        }else if (size.rfind("box", 0) == 0){
            st.fixedSize = false;
            if (size.size() > 3){
                float scale = 0.f;
                if (size[3] != '*' || !numberOf(size.substr(4), scale) || scale <= 0.f || scale > 4.f){
                    error(line, "a box size is 'box' or 'box*<scale>' with a scale above 0 and at most 4", "box*0.5");
                    return;
                }
                st.boxScale = scale;
            }
            if (!parseMax())
                return;
        }else{
            const size_t x = size.find('x');
            int w = 0, h = 0;
            if (x == std::string::npos || !integerOf(size.substr(0, x), w) || !integerOf(size.substr(x + 1), h) || w < 1 || h < 1 ||
                w > 4096 || h > 4096){
                error(line, "a state size is '<w>x<h>' (1 to 4096 each), 'box[*<scale>]' or 'aspect <short>'", "256x256");
                return;
            }
            st.fixedSize = true;
            st.width = w;
            st.height = h;
        }
        for (; next < words.size(); next++){
            const std::string& w = words[next];
            if (w == "linear") st.linear = true;
            else if (w == "nearest") st.linear = false;
            else if (w == "clamp") st.repeat = false;
            else if (w == "repeat") st.repeat = true;
            else if (w == "clear") st.resample = false;
            else if (w == "resample") st.resample = true;
            else{
                error(line, "unexpected '" + w + "' in @state " + st.name, grammar);
                return;
            }
        }
        for (const EffectStateDef& other : out.states){
            if (other.name == st.name){
                error(line, "state '" + st.name + "' is declared twice");
                return;
            }
        }
        out.states.push_back(st);
    }

    template<typename ErrorFn>
    void parsePass(const std::string& rest, int line, bool init, bool frame, EffectManifest& out,
                   std::vector<std::pair<size_t, std::string>>& pendingScatter, ErrorFn& error){
        std::vector<std::string> words;
        wordsOf(rest, words);
        const char* directive = init ? "@init" : (frame ? "@frame" : "@step");
        const char* grammar = init ? "@init is '<pass> <state>'"
                                   : (frame ? "@frame is '<pass> <state> [x<count>|x<param>] [add] [scatter <state>] [if <param>]'"
                                            : "@step is '<pass> <state> [x<count>|x<param>] [add] [scatter <state>] [if <param>]'");
        if (words.size() < 2){
            error(line, grammar);
            return;
        }
        EffectPassDef pass;
        pass.line = line;
        pass.init = init;
        pass.frame = frame;
        pass.name = words[0];
        if (!isPassName(pass.name)){
            error(line, "'" + pass.name + "' is not a pass name: lower case letters, digits and '-', starting with a letter");
            return;
        }
        pass.state = out.stateIndex(words[1]);
        if (pass.state < 0){
            error(line, "pass '" + pass.name + "' writes '" + words[1] + "', which is not a @state declared before it",
                  "declare it first: // @state " + words[1] + " rgba16f box");
            return;
        }
        for (size_t i = 2; i < words.size(); i++){
            const std::string& w = words[i];
            if (init){
                error(line, "unexpected '" + w + "' in @init " + pass.name, grammar);
                return;
            }
            if (w.size() > 1 && w[0] == 'x'){
                int count = 0;
                if (integerOf(w.substr(1), count)){
                    if (count < 1 || count > 64){
                        error(line, "a pass repeats 1 to 64 times", "x10");
                        return;
                    }
                    pass.repeat = count;
                }else{
                    pass.repeatParam = w.substr(1);
                }
            }else if (w == "add"){
                pass.additive = true;
            }else if (w == "if"){
                if (i + 1 >= words.size()){
                    error(line, "if names a boolean or number parameter", "if bloom");
                    return;
                }
                pass.ifParam = words[++i];
            }else if (w == "scatter"){
                if (i + 1 >= words.size()){
                    error(line, "scatter names its placement state", "scatter place");
                    return;
                }
                pendingScatter.push_back({out.passes.size(), words[++i]});
                // a scatter pass blends its quads
                pass.additive = true;
            }else{
                error(line, "unexpected '" + w + "' in " + std::string(directive) + " " + pass.name, grammar);
                return;
            }
        }
        for (const EffectPassDef& other : out.passes){
            if (other.name == pass.name){
                error(line, "pass '" + pass.name + "' is declared twice");
                return;
            }
        }
        out.passes.push_back(pass);
    }

    // @preset <option> <param> <value>[, <param> <value>...] (a value's own commas, inside
    // parentheses, are kept)
    template<typename ErrorFn>
    void parsePreset(const std::string& rest, int line, EffectManifest& out, ErrorFn& error){
        const char* grammar = "@preset is '<option> <param> <value>[, <param> <value>...]'";
        size_t i = 0;
        while (i < rest.size() && !std::isspace((unsigned char)rest[i]))
            i++;
        EffectPresetDef preset;
        preset.line = line;
        preset.option = rest.substr(0, i);
        if (!isEffectName(preset.option)){
            error(line, grammar, "@preset smoke curl 20, splat-radius 0.5");
            return;
        }
        std::vector<std::string> items;
        std::string item;
        int depth = 0;
        const std::string list = trim(std::string_view(rest).substr(i));
        for (char c : list.empty() ? std::string() : list + ","){
            if (c == '(')
                depth++;
            else if (c == ')' && depth > 0)
                depth--;
            if (c == ',' && depth == 0){
                items.push_back(trim(item));
                item.clear();
            }else{
                item += c;
            }
        }
        for (const std::string& entry : items){
            size_t j = 0;
            while (j < entry.size() && !std::isspace((unsigned char)entry[j]))
                j++;
            const std::string param = entry.substr(0, j);
            const std::string value = trim(std::string_view(entry).substr(j));
            if (param.empty() || value.empty()){
                error(line, "@preset " + preset.option + ": '" + entry + "' is not '<param> <value>'", grammar);
                return;
            }
            preset.values.push_back({param, value});
        }
        if (preset.values.empty()){
            error(line, "@preset " + preset.option + " sets no parameter", grammar);
            return;
        }
        for (const EffectPresetDef& other : out.presets){
            if (other.option == preset.option){
                error(line, "@preset " + preset.option + " is declared twice");
                return;
            }
        }
        out.presets.push_back(std::move(preset));
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

bool EffectManifest::hasScatter() const{
    for (const EffectPassDef& pass : passes){
        if (pass.scatterState >= 0)
            return true;
    }
    return false;
}

bool EffectManifest::hasFramePasses() const{
    for (const EffectPassDef& pass : passes){
        if (pass.frame)
            return true;
    }
    return false;
}

const EffectPresetDef* EffectManifest::findPreset(const std::string& option) const{
    for (const EffectPresetDef& preset : presets){
        if (preset.option == option)
            return &preset;
    }
    return nullptr;
}

int EffectManifest::stateIndex(const std::string& stateName) const{
    for (size_t i = 0; i < states.size(); i++){
        if (states[i].name == stateName)
            return (int)i;
    }
    return -1;
}

uint64_t EffectManifest::stateSignature() const{
    uint64_t h = 14695981039346656037ull;
    auto add = [&h](const std::string& text){
        for (unsigned char c : text){
            h ^= c;
            h *= 1099511628211ull;
        }
        h ^= 0xff;
        h *= 1099511628211ull;
    };
    for (const EffectStateDef& st : states){
        add(st.name);
        add(std::to_string((int)st.format) + ":" + std::to_string(st.fixedSize) + ":" + std::to_string(st.width) + "x" +
            std::to_string(st.height) + ":" + std::to_string(st.boxScale) + ":" + std::to_string(st.boxMax) + ":" +
            std::to_string(st.linear) + std::to_string(st.repeat) + std::to_string(st.resample) +
            (st.aspectShort > 0 ? ":aspect" + std::to_string(st.aspectShort) : std::string()));
    }
    for (const EffectPassDef& pass : passes){
        add(pass.name);
        add(std::to_string(pass.init) + ":" + std::to_string(pass.state) + ":" + std::to_string(pass.repeat) + ":" + pass.repeatParam + ":" +
            std::to_string(pass.additive) + ":" + std::to_string(pass.scatterState) + (pass.frame ? ":frame" : "") +
            (pass.ifParam.empty() ? std::string() : ":if " + pass.ifParam));
    }
    add(std::to_string(warmupSteps) + ":" + std::to_string(warmupDt) + ":" + std::to_string(rateHz) + ":" + std::to_string(maxSubsteps) + ":" +
        std::to_string(replaySeconds));
    for (const std::string& param : reseedParams)
        add(param);
    return h;
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
        case EffectCost::Stateful: return "stateful";
    }
    return "low";
}

const char* jasperengine::htmlui::effectStateFormatName(EffectStateFormat format){
    switch (format){
        case EffectStateFormat::RGBA8: return "rgba8";
        case EffectStateFormat::RGBA16F: return "rgba16f";
        case EffectStateFormat::RGBA32F: return "rgba32f";
        case EffectStateFormat::R32F: return "r32f";
        case EffectStateFormat::RG16F: return "rg16f";
        case EffectStateFormat::R16F: return "r16f";
    }
    return "rgba16f";
}

int jasperengine::htmlui::effectStateFormatBytes(EffectStateFormat format){
    switch (format){
        case EffectStateFormat::RGBA8: return 4;
        case EffectStateFormat::RGBA16F: return 8;
        case EffectStateFormat::RGBA32F: return 16;
        case EffectStateFormat::R32F: return 4;
        case EffectStateFormat::RG16F: return 4;
        case EffectStateFormat::R16F: return 2;
    }
    return 8;
}

std::string jasperengine::htmlui::effectPassMacro(const std::string& passName){
    std::string out = "JFX_PASS_";
    for (char c : passName)
        out += c == '-' ? '_' : (char)std::toupper((unsigned char)c);
    return out;
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
    // scatter placement states named before they are declared: resolved after the block
    std::vector<std::pair<size_t, std::string>> pendingScatter;
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
            else if (rest == "stateful") out.cost = EffectCost::Stateful;
            else error(line, "@cost is low, medium, high or stateful, not '" + rest + "'");
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
        }else if (name == "state"){
            parseState(rest, line, out, error);
        }else if (name == "init" || name == "step" || name == "frame"){
            parsePass(rest, line, name == "init", name == "frame", out, pendingScatter, error);
        }else if (name == "preset"){
            parsePreset(rest, line, out, error);
        }else if (name == "warmup"){
            std::vector<std::string> words;
            wordsOf(rest, words);
            float dt = 0.f;
            int steps = 0;
            if (words.size() != 2 || !integerOf(words[0], steps) || steps < 1 || steps > 600 || !secondsOf(words[1], dt) || dt <= 0.f || dt > 1.f){
                error(line, "@warmup is '<steps> <dt>': 1 to 600 steps of 0 to 1 s", "@warmup 50 0.1s");
                continue;
            }
            out.warmupSteps = steps;
            out.warmupDt = dt;
        }else if (name == "rate"){
            std::vector<std::string> words;
            wordsOf(rest, words);
            float hz = 0.f;
            int max = out.maxSubsteps;
            bool ok = !words.empty() && numberOf(words[0], hz) && hz >= 1.f && hz <= 240.f;
            if (ok && words.size() == 3)
                ok = words[1] == "max" && integerOf(words[2], max) && max >= 1 && max <= 16;
            else if (ok && words.size() != 1)
                ok = false;
            if (!ok){
                error(line, "@rate is '<hz> [max <substeps>]': 1 to 240 Hz, 1 to 16 substeps", "@rate 60 max 4");
                continue;
            }
            out.rateHz = hz;
            out.maxSubsteps = max;
        }else if (name == "reseed"){
            std::string item;
            for (char c : rest + ","){
                if (c == ','){
                    const std::string param = trim(item);
                    if (!param.empty())
                        out.reseedParams.push_back(param);
                    item.clear();
                }else{
                    item += c;
                }
            }
            if (out.reseedParams.empty())
                error(line, "@reseed lists the parameters whose change restarts the simulation", "@reseed seed");
        }else if (name == "replay"){
            float seconds = 0.f;
            if (!secondsOf(rest, seconds) || seconds < 0.f || seconds > 30.f){
                error(line, "@replay is the pinned clock's history window, 0 to 30 s", "@replay 2s");
                continue;
            }
            out.replaySeconds = seconds;
        }else{
            error(line, "unknown manifest directive '@" + name + "'",
                  "@effect, @kind, @category, @summary, @source, @cost, @backdrop, @time, @pointer, @prepass, @margin, @geometry, "
                  "@suggest-for, @param, @state, @init, @step, @frame, @warmup, @rate, @replay, @reseed, @preset");
        }
    }
    for (const auto& pending : pendingScatter){
        EffectPassDef& pass = out.passes[pending.first];
        pass.scatterState = out.stateIndex(pending.second);
        if (pass.scatterState < 0)
            error(pass.line, std::string(pass.frame ? "@frame " : "@step ") + pass.name + " scatters '" + pending.second + "', which is not a @state");
    }
    // a state is one image when every pass writing it is additive (an accumulator)
    for (size_t i = 0; i < out.states.size(); i++){
        bool written = false, allAdditive = true;
        for (const EffectPassDef& pass : out.passes){
            if (pass.state != (int)i)
                continue;
            written = true;
            if (!pass.additive)
                allAdditive = false;
        }
        out.states[i].single = written && allAdditive;
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
    if (!m.source.empty() && m.source.rfind("shaders.com ", 0) != 0 && m.source.rfind("original", 0) != 0 && m.source.rfind("port ", 0) != 0)
        error(m.line, "@source starts with 'shaders.com <Name>', 'original' or 'port <project> (<author>, <licence>)'");
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

    // phase 6: the state and its passes
    if (m.stateful() != (m.cost == EffectCost::Stateful))
        error(m.line, m.stateful() ? "an effect with @state is '@cost stateful'" : "'@cost stateful' is for effects with @state",
              m.stateful() ? "// @cost       stateful" : "low, medium or high");
    if (m.states.size() > 8)
        error(m.states[8].line, "an effect has at most 8 states");
    if (m.passes.size() > 32)
        error(m.passes[32].line, "an effect has at most 32 passes");
    if (m.stateful()){
        bool hasStep = false;
        for (const EffectPassDef& pass : m.passes)
            hasStep = hasStep || (!pass.init && !pass.frame);
        if (!hasStep)
            error(m.line, "an effect with @state has at least one @step pass");
    }else if (!m.passes.empty() || m.warmupSteps > 0){
        error(m.line, "@init, @step, @frame and @warmup need a @state");
    }
    for (const std::string& param : m.reseedParams){
        if (!m.stateful())
            error(m.line, "@reseed is for effects with @state");
        else if (!m.findParam(param))
            error(m.line, "@reseed names '" + param + "', which is not a parameter");
    }
    // @preset: the options of the enum parameter `preset`, each setting other parameters to legal values
    if (!m.presets.empty()){
        const EffectParamDef* presetParam = m.findParam("preset");
        if (!presetParam || presetParam->type != EffectParamType::Enum){
            error(m.presets[0].line, "@preset needs an enum parameter named 'preset' (its options name the presets)",
                  "// @param preset enum default default|calm|wild -- The look");
        }else{
            for (const EffectPresetDef& preset : m.presets){
                if (std::find(presetParam->options.begin(), presetParam->options.end(), preset.option) == presetParam->options.end())
                    error(preset.line, "@preset " + preset.option + ": 'preset' has no option '" + preset.option + "'");
                std::set<std::string> seen;
                for (const auto& entry : preset.values){
                    const EffectParamDef* def = m.findParam(entry.first);
                    if (!def){
                        error(preset.line, "@preset " + preset.option + " sets '" + entry.first + "', which is not a parameter");
                        continue;
                    }
                    if (def->name == "preset"){
                        error(preset.line, "@preset " + preset.option + " cannot set 'preset' itself");
                        continue;
                    }
                    if (!seen.insert(def->name).second)
                        error(preset.line, "@preset " + preset.option + " sets '" + def->name + "' twice");
                    EffectValue value;
                    std::string reason;
                    if (!EffectValueParser::parse(*def, entry.second, value, reason) || value.isVar){
                        error(preset.line, "@preset " + preset.option + ": '" + entry.second + "' is not " + EffectValueParser::expected(*def) +
                              " for '" + def->name + "'" + (reason.empty() ? std::string() : ": " + reason));
                        continue;
                    }
                    EffectValue clamped = value;
                    if (EffectValueParser::clamp(*def, clamped))
                        error(preset.line, "@preset " + preset.option + ": '" + entry.second + "' is outside the range of '" + def->name + "'");
                }
            }
        }
    }
    bool hasInit = false;
    for (const EffectPassDef& pass : m.passes)
        hasInit = hasInit || pass.init;
    if (m.warmupSteps > 0 && !hasInit)
        error(m.line, "@warmup runs after the @init passes: the effect has none");
    for (const EffectStateDef& st : m.states){
        const bool full = st.format == EffectStateFormat::RGBA32F || st.format == EffectStateFormat::R32F;
        if (full && st.linear)
            error(st.line, "state '" + st.name + "' is " + effectStateFormatName(st.format) +
                  ": 32-bit float images are not filterable on GLES3/WebGL2, sample them 'nearest'", "nearest");
        bool written = false;
        for (const EffectPassDef& pass : m.passes)
            written = written || pass.state == m.stateIndex(st.name);
        if (!written)
            error(st.line, "no pass writes state '" + st.name + "'");
    }
    for (size_t i = 0; i < m.passes.size(); i++){
        const EffectPassDef& pass = m.passes[i];
        if (!pass.repeatParam.empty()){
            const EffectParamDef* def = m.findParam(pass.repeatParam);
            if (!def)
                error(pass.line, "pass '" + pass.name + "' repeats by '" + pass.repeatParam + "', which is not a parameter");
            else if (def->type != EffectParamType::Number && def->type != EffectParamType::Integer)
                error(def->line, "pass '" + pass.name + "' repeats by '" + pass.repeatParam + "', which is not a number or an integer");
        }
        if (!pass.ifParam.empty()){
            const EffectParamDef* def = m.findParam(pass.ifParam);
            if (!def)
                error(pass.line, "pass '" + pass.name + "' runs if '" + pass.ifParam + "', which is not a parameter");
            else if (def->type != EffectParamType::Boolean && def->type != EffectParamType::Number && def->type != EffectParamType::Integer)
                error(def->line, "pass '" + pass.name + "' runs if '" + pass.ifParam + "', which is not a boolean or a number");
            if (pass.init)
                error(pass.line, "@init passes always run: 'if' is for @step and @frame");
        }
        if (pass.scatterState >= 0){
            if (pass.scatterState == pass.state)
                error(pass.line, "pass '" + pass.name + "' scatters the state it writes");
            bool placed = false;
            for (size_t j = 0; j < i; j++)
                placed = placed || m.passes[j].state == pass.scatterState;
            if (!placed)
                error(pass.line, "pass '" + pass.name + "' scatters '" + m.states[(size_t)pass.scatterState].name +
                      "', which no earlier pass writes");
        }
    }

    // @frame passes write what only the draw reads (3, section 7): a state they write is not one a
    // @step or @init pass writes (it would change with the frame rate, not the clock), and they do
    // not scatter a state the steps write
    for (const EffectPassDef& pass : m.passes){
        if (!pass.frame)
            continue;
        for (const EffectPassDef& other : m.passes){
            if (other.frame || other.state != pass.state)
                continue;
            error(pass.line, "@frame pass '" + pass.name + "' writes '" + m.states[(size_t)pass.state].name + "', which " +
                  (other.init ? "@init" : "@step") + " pass '" + other.name + "' writes too: @frame passes write images only the draw reads",
                  "give the @frame pass a state of its own");
            break;
        }
    }
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
    // a stateful effect's step forms: main_step.glsl after its main include
    const std::string stepMain = "#include \"effects/lib/main_step.glsl\"";
    int stepCount = 0, stepLine = 0;
    for (size_t i = 0; i < lines.size(); i++){
        if (trim(lines[i]) == stepMain){
            stepCount++;
            stepLine = (int)i + 1;
        }
    }
    if (m.stateful() && (stepCount != 1 || stepLine < mainLine))
        error(stepCount ? stepLine : mainLine, "a stateful effect ends with " + stepMain + " (after its " + (paint ? paintMain : filterMain) + ")",
              stepMain);
    if (!m.stateful() && stepCount)
        error(stepLine, stepMain + " is for effects with @state");
    return diags.size() == diagsBefore;
}

std::vector<uint32_t> EffectManifestParser::passForms(const EffectManifest& m, uint32_t drawForm){
    std::vector<uint32_t> forms;
    if (!m.stateful())
        return forms;
    forms.push_back(drawForm | 2u);
    if (m.hasScatter())
        forms.push_back(drawForm | 4u);
    return forms;
}

bool EffectManifestParser::checkReflection(const EffectManifest& m, ShaderData& shader, std::vector<std::string>& problems, uint64_t properties){
    const size_t before = problems.size();
    const bool passForm = (properties & 6u) != 0;
    unsigned int frameSize = 0;
    unsigned int stepSize = 0;
    if (passForm){
        // a step or scatter form: the step block (the frame block only when the passes read it)
        if (!shader.getUniformBlockMembers("u_fs_effectStep", stepSize))
            problems.push_back("the step variant has no u_fs_effectStep block (does the effect include effects/lib/main_step.glsl last, "
                               "and does jfx_step read its JfxStep?)");
    }else if (!shader.getUniformBlockMembers("u_fs_effectFrame", frameSize)){
        problems.push_back("the variant has no u_fs_effectFrame block (does the effect include effects/lib/jfx_common.glsl?)");
    }
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
        if (!expected.empty() && !passForm)
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

