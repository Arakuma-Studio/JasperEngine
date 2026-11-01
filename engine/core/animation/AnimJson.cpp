// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimJson.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <type_traits>

using namespace jasperengine;

namespace{

    using OJson = anim::json::OJson;

    // line and column (1-based, in code points; a UTF-8 BOM is not a column) of the byte at
    // `index` (0-based)
    void textPosition(const std::string& text, size_t index, int& line, int& column){
        if (index > text.size()) index = text.size();
        line = 1;
        size_t lineStart = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? std::min<size_t>(3, index) : 0;
        for (size_t i = lineStart; i < index; i++){
            if (text[i] == '\n'){
                line++;
                lineStart = i + 1;
            }
        }
        column = 1;
        for (size_t i = lineStart; i < index; i++){
            if (((unsigned char)text[i] & 0xC0) != 0x80) column++;
        }
    }

    // nlohmann's DOM builder, recording where and why the parse stopped, without exceptions
    class SaxParser : public nlohmann::detail::json_sax_dom_parser<OJson>{
    public:
        explicit SaxParser(OJson& root): json_sax_dom_parser<OJson>(root, false){}

        template<class Exception>
        bool parse_error(std::size_t position, const std::string& lastToken, const Exception& ex){
            failed = true;
            errorPosition = position;
            errorToken = lastToken;
            message = ex.what();
            overflow = std::is_same<Exception, nlohmann::detail::out_of_range>::value;
            return json_sax_dom_parser<OJson>::parse_error(position, lastToken, ex);
        }

        bool failed = false;
        bool overflow = false;
        std::size_t errorPosition = 0;
        std::string errorToken;
        std::string message;
    };

}

bool anim::json::parseDocument(const std::string& text, OJson& root, ClipParseError& error){
    SaxParser sax(root);
    OJson::sax_parse(text, &sax);
    if (!sax.failed) return true;
    if (sax.overflow){
        size_t end = std::min(sax.errorPosition, text.size());
        size_t start = sax.errorToken.empty() ? std::string::npos : text.rfind(sax.errorToken, end);
        textPosition(text, start != std::string::npos ? start : (end > 0 ? end - 1 : 0), error.line, error.column);
        error.message = "not valid JSON: the number " + sax.errorToken + " is out of range";
    }else{
        textPosition(text, sax.errorPosition > 0 ? sax.errorPosition - 1 : 0, error.line, error.column);
        size_t at = sax.message.find("syntax error");
        error.message = "not valid JSON: " + (at != std::string::npos ? sax.message.substr(at) : sax.message);
    }
    return false;
}

bool anim::json::checkHeader(const OJson& root, const char* format, int version, const char* what, const char* kind, ClipParseError& error){
    if (!root.is_object()){
        error.message = std::string("not ") + what + ": the top level must be a JSON object";
        return false;
    }
    auto formatIt = root.find("format");
    if (formatIt == root.end() || !formatIt->is_string() || formatIt->get<std::string>() != format){
        error.message = std::string("not ") + what + ": \"format\" must be \"" + format + "\"";
        return false;
    }
    auto versionIt = root.find("version");
    if (versionIt == root.end()){
        error.message = "missing \"version\" (" + std::to_string(version) + ")";
        return false;
    }
    if (!versionIt->is_number() || versionIt->get<double>() != std::floor(versionIt->get<double>()) || versionIt->get<double>() < 1.0){
        error.message = "invalid \"version\": expected a whole number from 1";
        return false;
    }
    if (versionIt->get<double>() > version){
        error.message = std::string("unsupported ") + kind + " version " +
                        shortNumber(versionIt->get<double>()) + " (this engine reads versions up to " + std::to_string(version) + ")";
        return false;
    }
    return true;
}

std::string anim::json::quote(const std::string& text){
    return OJson(text).dump(-1, ' ', false, OJson::error_handler_t::replace);
}

std::string anim::json::number(float value){
    if (!std::isfinite(value)) return "0";
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.6g", value);
    std::string text = buffer;
    return text == "-0" ? "0" : text;
}

std::string anim::json::time(float value){
    if (!std::isfinite(value)) return "0";
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.6f", value);
    std::string text = buffer;
    size_t end = text.size();
    while (end > 0 && text[end - 1] == '0') end--;
    if (end > 0 && text[end - 1] == '.') end--;
    text.resize(end);
    return (text == "-0" || text.empty()) ? "0" : text;
}

std::string anim::json::shortNumber(double value){
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.6g", value);
    return buffer;
}

std::string anim::json::compact(const OJson& value){
    return value.dump(-1, ' ', false, OJson::error_handler_t::replace);
}

bool anim::json::toFloat(const OJson& value, float& out){
    if (!value.is_number()) return false;
    double number = value.get<double>();
    if (!std::isfinite(number) || std::fabs(number) > (double)FLT_MAX) return false;
    out = (float)number;
    return true;
}

bool anim::json::toWeight(const OJson& value, float& out){
    if (value.is_boolean()){
        out = value.get<bool>() ? 1.0f : 0.0f;
        return true;
    }
    return toFloat(value, out);
}

void anim::json::warn(std::vector<ClipWarning>* warnings, const std::string& path, const std::string& message){
    if (warnings) warnings->push_back({path, message});
}

std::string anim::json::member(const std::string& path, const std::string& name){
    return path.empty() ? name : path + "." + name;
}

std::string anim::json::indexed(const std::string& path, const std::string& name, size_t index){
    return member(path, name) + "[" + std::to_string(index) + "]";
}

std::string anim::json::unknownKeysText(const std::vector<std::pair<std::string, std::string>>& unknownKeys, const char* const* known, size_t knownCount){
    std::string text;
    for (size_t i = 0; i < unknownKeys.size(); i++){
        const auto& entry = unknownKeys[i];
        bool isKnown = false;
        for (size_t k = 0; k < knownCount; k++){
            if (entry.first == known[k]) isKnown = true;
        }
        if (isKnown) continue;
        bool repeated = false;
        for (size_t j = i + 1; j < unknownKeys.size(); j++){
            if (unknownKeys[j].first == entry.first) repeated = true;
        }
        if (repeated) continue;
        text += "  " + quote(entry.first) + ": " + (entry.second.empty() ? std::string("null") : entry.second) + ",\n";
    }
    return text;
}

bool anim::json::isUnityYaml(const std::string& text, const char* document){
    size_t i = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    while (i < text.size() && std::isspace((unsigned char)text[i])) i++;
    if (text.compare(i, 5, "%YAML") == 0) return true;
    const std::string needle = document;
    size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos){
        if (pos == 0 || text[pos - 1] == '\n') return true;
        pos++;
    }
    return false;
}
