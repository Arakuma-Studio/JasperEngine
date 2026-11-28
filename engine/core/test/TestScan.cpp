// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "test/TestScan.h"
#include "test/TestOptions.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

using namespace jasperengine;

namespace jasperengine::gametest {

namespace fs = std::filesystem;

namespace {

    struct Token {
        enum Kind { Name, String, Number, Symbol } kind;
        std::string text;
        int line;
    };

    // [[ ... ]], [==[ ... ]==]: the level at code[i] ('[' then '='s then '['), -1 when none
    int longBracketLevel(const std::string& code, size_t i) {
        if (i >= code.size() || code[i] != '[') return -1;
        size_t j = i + 1;
        int level = 0;
        while (j < code.size() && code[j] == '=') {
            level++;
            j++;
        }
        return (j < code.size() && code[j] == '[') ? level : -1;
    }

    // skips a long bracket starting at i; returns the index after it and its content
    size_t skipLongBracket(const std::string& code, size_t i, int level, int& line, std::string* content) {
        const std::string close = "]" + std::string(static_cast<size_t>(level), '=') + "]";
        const size_t start = i + 2 + static_cast<size_t>(level);
        const size_t end = code.find(close, start);
        const size_t stop = end == std::string::npos ? code.size() : end;
        for (size_t k = start; k < stop; k++)
            if (code[k] == '\n') line++;
        if (content) *content = code.substr(start, stop - start);
        return end == std::string::npos ? code.size() : end + close.size();
    }

    std::vector<Token> tokenize(const std::string& code) {
        std::vector<Token> tokens;
        int line = 1;
        size_t i = 0;
        const size_t n = code.size();
        while (i < n) {
            const char c = code[i];
            if (c == '\n') {
                line++;
                i++;
            } else if (std::isspace(static_cast<unsigned char>(c))) {
                i++;
            } else if (c == '-' && i + 1 < n && code[i + 1] == '-') {
                const int level = longBracketLevel(code, i + 2);
                if (level >= 0) {
                    i = skipLongBracket(code, i + 2, level, line, nullptr);
                } else {
                    while (i < n && code[i] != '\n') i++;
                }
            } else if (c == '"' || c == '\'') {
                const int startLine = line;
                std::string text;
                i++;
                while (i < n && code[i] != c) {
                    if (code[i] == '\\' && i + 1 < n) {
                        const char e = code[i + 1];
                        if (e == 'n') text += '\n';
                        else if (e == 't') text += '\t';
                        else if (e == '\n') {
                            text += '\n';
                            line++;
                        } else text += e;
                        i += 2;
                    } else {
                        if (code[i] == '\n') line++;
                        text += code[i++];
                    }
                }
                i++;
                tokens.push_back({Token::String, text, startLine});
            } else if (c == '[' && longBracketLevel(code, i) >= 0) {
                const int startLine = line;
                std::string text;
                i = skipLongBracket(code, i, longBracketLevel(code, i), line, &text);
                if (!text.empty() && text[0] == '\n') text.erase(0, 1);
                tokens.push_back({Token::String, text, startLine});
            } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                const size_t start = i;
                while (i < n && (std::isalnum(static_cast<unsigned char>(code[i])) || code[i] == '_')) i++;
                tokens.push_back({Token::Name, code.substr(start, i - start), line});
            } else if (std::isdigit(static_cast<unsigned char>(c))) {
                const size_t start = i;
                while (i < n && (std::isalnum(static_cast<unsigned char>(code[i])) || code[i] == '.' ||
                                 ((code[i] == '-' || code[i] == '+') && (code[i - 1] == 'e' || code[i - 1] == 'E'))))
                    i++;
                tokens.push_back({Token::Number, code.substr(start, i - start), line});
            } else {
                tokens.push_back({Token::Symbol, std::string(1, c), line});
                i++;
            }
        }
        return tokens;
    }

    // A table constructor's literal fields: key = "string" | number | true/false | {"a", "b"}.
    // `i` is at "{"; it ends after the matching "}". Computed values are left out.
    Json literalTable(const std::vector<Token>& t, size_t& i) {
        Json out = Json::object();
        if (i >= t.size() || t[i].text != "{") return out;
        i++;
        while (i < t.size() && t[i].text != "}") {
            if (t[i].kind == Token::Name && i + 1 < t.size() && t[i + 1].text == "=") {
                const std::string key = t[i].text;
                i += 2;
                if (i >= t.size()) break;
                const Token& v = t[i];
                bool literal = false;
                if (v.kind == Token::String) {
                    out[key] = v.text;
                    literal = true;
                    i++;
                } else if (v.kind == Token::Number) {
                    char* end = nullptr;
                    const double number = std::strtod(v.text.c_str(), &end);
                    if (end && *end == '\0') {
                        if (number == static_cast<double>(static_cast<int64_t>(number))) out[key] = static_cast<int64_t>(number);
                        else out[key] = number;
                        literal = true;
                    }
                    i++;
                } else if (v.kind == Token::Name && (v.text == "true" || v.text == "false")) {
                    out[key] = v.text == "true";
                    literal = true;
                    i++;
                } else if (v.text == "{") {
                    Json list = Json::array();
                    size_t j = i + 1;
                    bool strings = true;
                    while (j < t.size() && t[j].text != "}") {
                        if (t[j].kind == Token::String) list.push_back(t[j].text);
                        else if (t[j].text != "," && t[j].text != ";") strings = false;
                        if (t[j].text == "{") break;
                        j++;
                    }
                    if (strings && j < t.size() && t[j].text == "}") {
                        out[key] = list;
                        literal = true;
                        i = j + 1;
                    }
                }
                // a computed value: on to the end of the field
                if (!literal || (i < t.size() && t[i].text != "," && t[i].text != ";" && t[i].text != "}")) {
                    int depth = 0;
                    while (i < t.size()) {
                        const std::string& s = t[i].text;
                        if (t[i].kind == Token::Symbol && (s == "{" || s == "(" || s == "[")) depth++;
                        else if (t[i].kind == Token::Symbol && (s == "}" || s == ")" || s == "]")) {
                            if (depth == 0) break;
                            depth--;
                        } else if (depth == 0 && t[i].kind == Token::Symbol && (s == "," || s == ";")) break;
                        i++;
                    }
                    if (!literal) out.erase(key);
                }
            } else if (t[i].text == "{" || t[i].text == "(") {
                int depth = 0;
                while (i < t.size()) {
                    const std::string& s = t[i].text;
                    if (t[i].kind == Token::Symbol && (s == "{" || s == "(")) depth++;
                    else if (t[i].kind == Token::Symbol && (s == "}" || s == ")")) {
                        depth--;
                        if (depth == 0) {
                            i++;
                            break;
                        }
                    }
                    i++;
                }
                continue;
            }
            if (i < t.size() && t[i].text != "}") i++;
        }
        if (i < t.size()) i++;
        return out;
    }

    bool isTestCall(const std::vector<Token>& t, size_t i, const char* function) {
        return t[i].kind == Token::Name && t[i].text == "Test" && i + 2 < t.size() && t[i + 1].text == "." &&
               t[i + 2].kind == Token::Name && t[i + 2].text == function;
    }

}

std::vector<std::string> TestScan::listFiles(const fs::path& root) {
    std::vector<std::string> files;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return files;
    const std::string suffix = ".test.lua";
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        const fs::path relative = it->path().lexically_relative(root);
        const std::string first = relative.begin() != relative.end() ? relative.begin()->generic_string() : std::string();
        const std::string name = it->path().filename().generic_string();
        if (it->is_directory(ec)) {
            if ((it.depth() == 0 && (first == "Golden" || first == "Data")) || (!name.empty() && name[0] == '.'))
                it.disable_recursion_pending();
            continue;
        }
        if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            files.push_back(relative.generic_string());
    }
    std::sort(files.begin(), files.end());
    return files;
}

ScannedFile TestScan::scanText(const std::string& code, const std::string& file, const Settings& settings) {
    ScannedFile out;
    out.file = file;
    const std::vector<Token> t = tokenize(code);
    Json config = Json::object();
    std::map<std::string, int> seen;
    for (size_t i = 0; i < t.size(); i++) {
        if (isTestCall(t, i, "config")) {
            size_t j = i + 3;
            if (j < t.size() && t[j].text == "(") j++;
            if (j < t.size() && t[j].text == "{") {
                const Json literal = literalTable(t, j);
                for (auto it = literal.begin(); it != literal.end(); ++it) config[it.key()] = it.value();
            }
            continue;
        }
        if (!isTestCall(t, i, "case")) continue;
        size_t j = i + 3;
        if (j >= t.size() || t[j].text != "(") continue;
        j++;
        if (j >= t.size() || t[j].kind != Token::String) continue;
        CaseSpec spec;
        std::string unused;
        Json options = Json::object();
        const std::string name = t[j].text;
        const int line = t[i].line;
        j++;
        if (j + 1 < t.size() && t[j].text == "," && t[j + 1].text == "{") {
            j++;
            options = literalTable(t, j);
        }
        applyOptions(spec, settings, config, options, unused);
        spec.file = file;
        spec.name = name;
        spec.id = file + "#" + name;
        spec.line = line;
        seen[name]++;
        out.cases.push_back(std::move(spec));
    }
    for (CaseSpec& spec : out.cases)
        if (seen[spec.name] > 1) spec.duplicate = true;
    return out;
}

std::vector<ScannedFile> TestScan::scan(const fs::path& root, const Settings& settings) {
    std::vector<ScannedFile> out;
    for (const std::string& file : listFiles(root)) {
        std::ifstream in(root / fs::path(file), std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        out.push_back(scanText(text.str(), file, settings));
    }
    return out;
}

std::vector<CaseSpec> TestScan::cases(const std::vector<ScannedFile>& files) {
    std::vector<CaseSpec> out;
    for (const ScannedFile& f : files)
        out.insert(out.end(), f.cases.begin(), f.cases.end());
    return out;
}

}  // namespace jasperengine::gametest
