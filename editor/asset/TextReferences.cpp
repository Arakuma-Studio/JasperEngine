// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TextReferences.h"

#include <algorithm>
#include <cctype>

using namespace jasperengine::editor;

namespace {

    uint32_t lineAt(const std::string& text, size_t offset, size_t& lastOffset, uint32_t& lastLine) {
        for (size_t i = lastOffset; i < offset && i < text.size(); i++) {
            if (text[i] == '\n') lastLine++;
        }
        lastOffset = offset;
        return lastLine;
    }

    std::string stripQuery(std::string target) {
        const size_t cut = target.find_first_of("?#");
        if (cut != std::string::npos) {
            target.erase(cut);
        }
        return target;
    }

    bool startsWithNoCase(const std::string& text, size_t at, const char* word) {
        for (size_t i = 0; word[i]; i++) {
            if (at + i >= text.size() || std::tolower(static_cast<unsigned char>(text[at + i])) != word[i]) {
                return false;
            }
        }
        return true;
    }

    // The quoted (or, for CSS url(), unquoted) value starting at text[at]
    bool readValue(const std::string& text, size_t at, std::string& value, size_t& end, bool allowUnquoted) {
        while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at]))) at++;
        if (at >= text.size()) return false;
        const char quote = text[at];
        if (quote == '"' || quote == '\'') {
            const size_t close = text.find(quote, at + 1);
            if (close == std::string::npos) return false;
            value = text.substr(at + 1, close - at - 1);
            end = close + 1;
            return true;
        }
        if (!allowUnquoted) return false;
        size_t close = at;
        while (close < text.size() && text[close] != ')' && !std::isspace(static_cast<unsigned char>(text[close]))) close++;
        value = text.substr(at, close - at);
        end = close;
        return !value.empty();
    }

}

bool TextReferences::isLocalTarget(const std::string& target) {
    if (target.empty() || target[0] == '#' || target[0] == '/' || target[0] == '\\') {
        return false;
    }
    const size_t colon = target.find(':');
    const size_t slash = target.find('/');
    if (colon != std::string::npos && (slash == std::string::npos || colon < slash)) {
        return false; // a scheme: http:, https:, data:, file:, mailto:, javascript:
    }
    return target.find("{{") == std::string::npos && target.find("${") == std::string::npos;
}

std::string TextReferences::luaModulePath(const std::string& module) {
    std::string path = module;
    std::replace(path.begin(), path.end(), '.', '/');
    return path + ".lua";
}

std::vector<TextReference> TextReferences::scanHtml(const std::string& text) {
    std::vector<TextReference> found;
    size_t lastOffset = 0;
    uint32_t lastLine = 1;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] != '<') continue;
        if (text.compare(i, 4, "<!--") == 0) {
            const size_t close = text.find("-->", i + 4);
            if (close == std::string::npos) break;
            i = close + 2;
            continue;
        }
        const size_t tagEnd = text.find('>', i);
        if (tagEnd == std::string::npos) break;
        const std::string tag = text.substr(i, tagEnd - i);
        // a stylesheet link, else any src / href is an image or another document
        std::string lowerTag = tag;
        std::transform(lowerTag.begin(), lowerTag.end(), lowerTag.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool stylesheet = lowerTag.compare(0, 5, "<link") == 0 && lowerTag.find("stylesheet") != std::string::npos;
        for (const char* attribute : {"href", "src"}) {
            for (size_t at = lowerTag.find(attribute); at != std::string::npos; at = lowerTag.find(attribute, at + 1)) {
                if (at > 0 && !std::isspace(static_cast<unsigned char>(lowerTag[at - 1]))) continue;
                size_t eq = at + std::char_traits<char>::length(attribute);
                while (eq < tag.size() && std::isspace(static_cast<unsigned char>(tag[eq]))) eq++;
                if (eq >= tag.size() || tag[eq] != '=') continue;
                std::string value;
                size_t end = 0;
                if (!readValue(tag, eq + 1, value, end, false)) continue;
                value = stripQuery(value);
                if (!isLocalTarget(value)) continue;
                const bool isLinkHref = stylesheet && std::string(attribute) == "href";
                found.push_back({value, lineAt(text, i + at, lastOffset, lastLine),
                                 isLinkHref ? DepEdgeKind::UIStylesheet : DepEdgeKind::UIImage});
            }
        }
        // inline styles and <style> blocks reference files through CSS
        if (lowerTag.compare(0, 6, "<style") == 0) {
            const size_t close = text.find("</style", tagEnd);
            const std::string css = text.substr(tagEnd + 1, close == std::string::npos ? std::string::npos : close - tagEnd - 1);
            const uint32_t baseLine = lineAt(text, tagEnd, lastOffset, lastLine);
            for (TextReference reference : scanCss(css)) {
                reference.line += baseLine - 1;
                found.push_back(reference);
            }
        }
        i = tagEnd;
    }
    return found;
}

std::vector<TextReference> TextReferences::scanCss(const std::string& text) {
    std::vector<TextReference> found;
    size_t lastOffset = 0;
    uint32_t lastLine = 1;
    for (size_t i = 0; i < text.size(); i++) {
        if (text.compare(i, 2, "/*") == 0) {
            const size_t close = text.find("*/", i + 2);
            if (close == std::string::npos) break;
            i = close + 1;
            continue;
        }
        if (text[i] == '@' && startsWithNoCase(text, i + 1, "import")) {
            size_t at = i + 7;
            while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at]))) at++;
            std::string value;
            size_t end = 0;
            if (startsWithNoCase(text, at, "url(")) {
                if (!readValue(text, at + 4, value, end, true)) continue;
            } else if (!readValue(text, at, value, end, false)) {
                continue;
            }
            value = stripQuery(value);
            if (isLocalTarget(value)) {
                found.push_back({value, lineAt(text, i, lastOffset, lastLine), DepEdgeKind::UIStylesheet});
            }
            i = end;
            continue;
        }
        if (startsWithNoCase(text, i, "url(") && (i == 0 || !std::isalnum(static_cast<unsigned char>(text[i - 1])))) {
            std::string value;
            size_t end = 0;
            if (!readValue(text, i + 4, value, end, true)) continue;
            value = stripQuery(value);
            if (isLocalTarget(value)) {
                found.push_back({value, lineAt(text, i, lastOffset, lastLine), DepEdgeKind::UIImage});
            }
            i = end;
        }
    }
    return found;
}

std::vector<TextReference> TextReferences::scanLua(const std::string& text) {
    std::vector<TextReference> found;
    uint32_t line = 1;
    for (size_t i = 0; i < text.size(); i++) {
        const char c = text[i];
        if (c == '\n') {
            line++;
            continue;
        }
        if (c == '-' && text.compare(i, 2, "--") == 0) {
            // a comment to the end of the line, or a --[[ ]] block
            if (text.compare(i, 4, "--[[") == 0) {
                const size_t close = text.find("]]", i + 4);
                const size_t stop = close == std::string::npos ? text.size() : close + 2;
                line += static_cast<uint32_t>(std::count(text.begin() + static_cast<std::ptrdiff_t>(i), text.begin() + static_cast<std::ptrdiff_t>(stop), '\n'));
                i = stop - 1;
            } else {
                const size_t newline = text.find('\n', i);
                i = (newline == std::string::npos ? text.size() : newline) - 1;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            // skip string literals that are not require arguments
            const size_t close = text.find(c, i + 1);
            if (close == std::string::npos) break;
            i = close;
            continue;
        }
        if (text.compare(i, 7, "require") != 0 || (i > 0 && (std::isalnum(static_cast<unsigned char>(text[i - 1])) || text[i - 1] == '_' || text[i - 1] == '.'))) {
            continue;
        }
        size_t at = i + 7;
        if (at < text.size() && (std::isalnum(static_cast<unsigned char>(text[at])) || text[at] == '_')) continue;
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) at++;
        if (at < text.size() && text[at] == '(') at++;
        std::string value;
        size_t end = 0;
        if (readValue(text, at, value, end, false) && !value.empty()) {
            found.push_back({value, line, DepEdgeKind::LuaRequire});
            i = end - 1;
        }
    }
    return found;
}

std::vector<TextReference> TextReferences::scanShader(const std::string& text) {
    std::vector<TextReference> found;
    uint32_t line = 1;
    for (size_t begin = 0; begin < text.size();) {
        size_t end = text.find('\n', begin);
        if (end == std::string::npos) end = text.size();
        size_t at = begin;
        while (at < end && (text[at] == ' ' || text[at] == '\t')) at++;
        if (at < end && text[at] == '#') {
            at++;
            while (at < end && (text[at] == ' ' || text[at] == '\t')) at++;
            if (text.compare(at, 7, "include") == 0) {
                at += 7;
                while (at < end && (text[at] == ' ' || text[at] == '\t')) at++;
                if (at < end && (text[at] == '"' || text[at] == '<')) {
                    const char close = text[at] == '"' ? '"' : '>';
                    const size_t stop = text.find(close, at + 1);
                    if (stop != std::string::npos && stop < end) {
                        found.push_back({text.substr(at + 1, stop - at - 1), line, DepEdgeKind::ShaderInclude});
                    }
                }
            }
        }
        line++;
        begin = end + 1;
    }
    return found;
}
