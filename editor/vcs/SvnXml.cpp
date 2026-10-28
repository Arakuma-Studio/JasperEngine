// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vcs/SvnXml.h"

#include <cstdint>

namespace jasperengine::editor::vcs {

namespace {

    const std::string kEmpty;

    void appendUtf8(std::string& out, uint32_t code) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x110000) {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    bool decodeEntities(std::string_view text, std::string& out) {
        out.clear();
        out.reserve(text.size());
        for (size_t i = 0; i < text.size(); i++) {
            if (text[i] != '&') {
                out.push_back(text[i]);
                continue;
            }
            const size_t end = text.find(';', i);
            if (end == std::string_view::npos) return false;
            const std::string_view entity = text.substr(i + 1, end - i - 1);
            if (entity == "lt") out.push_back('<');
            else if (entity == "gt") out.push_back('>');
            else if (entity == "amp") out.push_back('&');
            else if (entity == "quot") out.push_back('"');
            else if (entity == "apos") out.push_back('\'');
            else if (entity.size() > 1 && entity[0] == '#') {
                uint32_t code = 0;
                const bool hex = entity[1] == 'x' || entity[1] == 'X';
                for (size_t k = hex ? 2 : 1; k < entity.size(); k++) {
                    const char c = entity[k];
                    uint32_t digit = 0;
                    if (c >= '0' && c <= '9') digit = static_cast<uint32_t>(c - '0');
                    else if (hex && c >= 'a' && c <= 'f') digit = static_cast<uint32_t>(c - 'a' + 10);
                    else if (hex && c >= 'A' && c <= 'F') digit = static_cast<uint32_t>(c - 'A' + 10);
                    else return false;
                    code = code * (hex ? 16 : 10) + digit;
                    if (code > 0x10FFFF) return false;
                }
                appendUtf8(out, code);
            } else {
                return false;
            }
            i = end;
        }
        return true;
    }

    struct Parser {
        std::string_view text;
        size_t pos = 0;
        std::string error;

        bool fail(const std::string& what) {
            if (error.empty()) error = what + " at offset " + std::to_string(pos);
            return false;
        }

        void skipSpace() {
            while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r')) pos++;
        }

        bool startsWith(std::string_view prefix) const { return text.substr(pos, prefix.size()) == prefix; }

        // <?...?>, <!--...-->, <!DOCTYPE ...>
        bool skipMisc() {
            while (true) {
                skipSpace();
                if (startsWith("<?")) {
                    const size_t end = text.find("?>", pos);
                    if (end == std::string_view::npos) return fail("unterminated processing instruction");
                    pos = end + 2;
                } else if (startsWith("<!--")) {
                    const size_t end = text.find("-->", pos);
                    if (end == std::string_view::npos) return fail("unterminated comment");
                    pos = end + 3;
                } else if (startsWith("<!")) {
                    const size_t end = text.find('>', pos);
                    if (end == std::string_view::npos) return fail("unterminated declaration");
                    pos = end + 1;
                } else {
                    return true;
                }
            }
        }

        static bool nameChar(char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ':' || c == '.' ||
                   static_cast<unsigned char>(c) >= 0x80;
        }

        bool readName(std::string& name) {
            const size_t start = pos;
            while (pos < text.size() && nameChar(text[pos])) pos++;
            if (pos == start) return fail("expected a name");
            name.assign(text.substr(start, pos - start));
            return true;
        }

        bool element(XmlElement& out, int depth) {
            if (depth > 256) return fail("nested too deeply");
            if (pos >= text.size() || text[pos] != '<') return fail("expected an element");
            pos++;
            if (!readName(out.name)) return false;
            while (true) {
                skipSpace();
                if (pos >= text.size()) return fail("unterminated tag");
                if (text[pos] == '/') {
                    if (!startsWith("/>")) return fail("expected />");
                    pos += 2;
                    return true;
                }
                if (text[pos] == '>') {
                    pos++;
                    break;
                }
                std::string key;
                if (!readName(key)) return false;
                skipSpace();
                if (pos >= text.size() || text[pos] != '=') return fail("expected =");
                pos++;
                skipSpace();
                if (pos >= text.size() || (text[pos] != '"' && text[pos] != '\'')) return fail("expected a quoted value");
                const char quote = text[pos++];
                const size_t end = text.find(quote, pos);
                if (end == std::string_view::npos) return fail("unterminated attribute");
                std::string value;
                if (!decodeEntities(text.substr(pos, end - pos), value)) return fail("bad entity");
                out.attributes.emplace_back(std::move(key), std::move(value));
                pos = end + 1;
            }
            // content
            while (true) {
                const size_t lt = text.find('<', pos);
                if (lt == std::string_view::npos) return fail("unterminated element " + out.name);
                if (lt > pos) {
                    std::string chunk;
                    if (!decodeEntities(text.substr(pos, lt - pos), chunk)) return fail("bad entity");
                    out.text += chunk;
                    pos = lt;
                }
                if (startsWith("</")) {
                    pos += 2;
                    std::string closing;
                    if (!readName(closing)) return false;
                    if (closing != out.name) return fail("</" + closing + "> closes <" + out.name + ">");
                    skipSpace();
                    if (pos >= text.size() || text[pos] != '>') return fail("expected >");
                    pos++;
                    return true;
                }
                if (startsWith("<!--")) {
                    const size_t end = text.find("-->", pos);
                    if (end == std::string_view::npos) return fail("unterminated comment");
                    pos = end + 3;
                    continue;
                }
                XmlElement child;
                if (!element(child, depth + 1)) return false;
                out.children.push_back(std::move(child));
            }
        }
    };

}

const std::string& XmlElement::attribute(std::string_view key) const {
    for (const auto& [name, value] : attributes) {
        if (name == key) return value;
    }
    return kEmpty;
}

const XmlElement* XmlElement::child(std::string_view childName) const {
    for (const XmlElement& element : children) {
        if (element.name == childName) return &element;
    }
    return nullptr;
}

const std::string& XmlElement::childText(std::string_view childName) const {
    const XmlElement* element = child(childName);
    return element ? element->text : kEmpty;
}

bool parseXml(std::string_view text, XmlElement& root, std::string* error) {
    Parser parser{text};
    root = XmlElement();
    if (!parser.skipMisc() || !parser.element(root, 0)) {
        if (error) *error = parser.error;
        return false;
    }
    return true;
}

}
