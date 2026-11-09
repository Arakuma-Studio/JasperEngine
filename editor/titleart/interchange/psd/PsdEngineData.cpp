// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PsdEngineData.h"

#include "PsdBytes.h"

#include <cctype>
#include <cstdlib>

using namespace jasperengine::editor;
using namespace jasperengine::editor::psd;
using jasperengine::titleart::Json;

namespace {

    constexpr int MAX_DEPTH = 64;

    struct Parser {
        const std::vector<uint8_t>& b;
        size_t pos = 0;
        std::string error;

        bool fail(const std::string& what) {
            if (error.empty()) error = "EngineData: " + what + " at byte " + std::to_string(pos);
            return false;
        }
        void space() {
            while (pos < b.size() && (std::isspace(b[pos]) || b[pos] == 0)) pos++;
        }
        bool at(const char* s) const {
            for (size_t i = 0; s[i]; i++) {
                if (pos + i >= b.size() || b[pos + i] != (uint8_t)s[i]) return false;
            }
            return true;
        }

        bool value(Json& out, int depth) {
            if (depth > MAX_DEPTH) return fail("nesting deeper than 64");
            space();
            if (pos >= b.size()) return fail("the text ends early");
            if (at("<<")) {
                pos += 2;
                out = Json::object();
                for (;;) {
                    space();
                    if (at(">>")) {
                        pos += 2;
                        return true;
                    }
                    if (pos >= b.size()) return fail("an unclosed dictionary");
                    if (b[pos] != '/') return fail("a dictionary key without '/'");
                    pos++;
                    std::string key;
                    while (pos < b.size() && !std::isspace(b[pos]) && b[pos] != '/' && b[pos] != '<' && b[pos] != '[' && b[pos] != '(') key += (char)b[pos++];
                    Json v;
                    if (!value(v, depth + 1)) return false;
                    out[key] = std::move(v);
                }
            }
            if (b[pos] == '[') {
                pos++;
                out = Json::array();
                for (;;) {
                    space();
                    if (pos < b.size() && b[pos] == ']') {
                        pos++;
                        return true;
                    }
                    if (pos >= b.size()) return fail("an unclosed list");
                    Json v;
                    if (!value(v, depth + 1)) return false;
                    out.push_back(std::move(v));
                }
            }
            if (b[pos] == '(') return string(out);
            if (b[pos] == '/') {
                pos++;
                std::string name;
                while (pos < b.size() && !std::isspace(b[pos]) && b[pos] != '/' && b[pos] != ']' && b[pos] != '>') name += (char)b[pos++];
                out = "/" + name;
                return true;
            }
            std::string word;
            while (pos < b.size() && !std::isspace(b[pos]) && b[pos] != ']' && b[pos] != '>' && b[pos] != '/' && b[pos] != '[' && b[pos] != '<') {
                word += (char)b[pos++];
            }
            if (word == "true") out = true;
            else if (word == "false") out = false;
            else if (!word.empty()) {
                char* end = nullptr;
                const double v = std::strtod(word.c_str(), &end);
                if (!end || *end) return fail("'" + word + "' is not a value");
                out = v;
            } else {
                return fail("an unexpected character");
            }
            return true;
        }

        // (...) with \ escapes; UTF-16BE after a FE FF mark, else bytes as Latin-1
        bool string(Json& out) {
            pos++;
            std::vector<uint8_t> raw;
            for (;;) {
                if (pos >= b.size()) return fail("an unclosed string");
                uint8_t c = b[pos++];
                if (c == '\\' && pos < b.size()) {
                    c = b[pos++];
                } else if (c == ')') {
                    break;
                }
                raw.push_back(c);
            }
            std::string text;
            if (raw.size() >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) {
                for (size_t i = 2; i + 1 < raw.size(); i += 2) {
                    uint32_t u = (uint32_t)(raw[i] << 8 | raw[i + 1]);
                    if (u >= 0xD800 && u <= 0xDBFF && i + 3 < raw.size()) {
                        const uint32_t low = (uint32_t)(raw[i + 2] << 8 | raw[i + 3]);
                        u = 0x10000 + ((u - 0xD800) << 10) + (low - 0xDC00);
                        i += 2;
                    }
                    if (u) ByteReader::appendUtf8(text, u);
                }
            } else {
                for (uint8_t c : raw) ByteReader::appendUtf8(text, c);
            }
            out = text;
            return true;
        }
    };

    const Json* path(const Json& root, std::initializer_list<const char*> keys) {
        const Json* at = &root;
        for (const char* k : keys) {
            if (!at->is_object() || !at->contains(k)) return nullptr;
            at = &(*at)[k];
        }
        return at;
    }

    double num(const Json* j, double fallback) {
        return j && j->is_number() ? j->get<double>() : fallback;
    }

}

bool psd::parseEngineData(const std::vector<uint8_t>& bytes, Json& out, std::string& error) {
    Parser p{bytes};
    if (!p.value(out, 0) || !out.is_object()) {
        error = p.error.empty() ? "EngineData is not a dictionary" : p.error;
        return false;
    }
    return true;
}

bool psd::firstTextStyle(const Json& engineData, PsTextStyle& out) {
    const Json* text = path(engineData, {"EngineDict", "Editor", "Text"});
    if (!text || !text->is_string()) return false;
    out = PsTextStyle();
    std::string t = text->get<std::string>();
    for (char& c : t) if (c == '\r') c = '\n';
    // EngineData ends the text with a paragraph break
    while (!t.empty() && t.back() == '\n') t.pop_back();
    out.text = t;
    const Json* runs = path(engineData, {"EngineDict", "StyleRun", "RunArray"});
    if (runs && runs->is_array() && !runs->empty()) {
        out.runs = runs->size();
        const Json* data = path((*runs)[0], {"StyleSheet", "StyleSheetData"});
        if (data) {
            out.fontSize = num(path(*data, {"FontSize"}), 0);
            out.tracking = num(path(*data, {"Tracking"}), 0);
            const Json* autoLeading = path(*data, {"AutoLeading"});
            const bool isAuto = !autoLeading || !autoLeading->is_boolean() || autoLeading->get<bool>();
            out.leading = isAuto ? 0 : num(path(*data, {"Leading"}), 0);
            const Json* values = path(*data, {"FillColor", "Values"});
            if (values && values->is_array() && values->size() == 4) {
                out.hasColor = true;
                for (int i = 0; i < 3; i++) out.color[i] = num(&(*values)[(size_t)i + 1], 0);
            }
            const int font = (int)num(path(*data, {"Font"}), 0);
            const Json* fonts = path(engineData, {"ResourceDict", "FontSet"});
            if (!fonts) fonts = path(engineData, {"DocumentResources", "FontSet"});
            if (fonts && fonts->is_array() && font >= 0 && (size_t)font < fonts->size()) {
                const Json* name = path((*fonts)[(size_t)font], {"Name"});
                if (name && name->is_string()) out.font = name->get<std::string>();
            }
        }
    }
    const Json* paragraphs = path(engineData, {"EngineDict", "ParagraphRun", "RunArray"});
    if (paragraphs && paragraphs->is_array() && !paragraphs->empty()) {
        out.justification = (int)num(path((*paragraphs)[0], {"ParagraphSheet", "Properties", "Justification"}), 0);
    }
    return true;
}
