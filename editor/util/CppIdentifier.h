// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cctype>
#include <string>
#include <unordered_set>

namespace jasperengine::editor {

    // A name as a C++ identifier (a script's class, the C++ scripts library, an executable): runs
    // of other characters become one '_', a keyword or a leading digit gets an '_'
    inline std::string toIdentifier(const std::string& name) {
        std::string out;
        out.reserve(name.size());

        auto isIdentStart = [](unsigned char c) {
            return std::isalpha(c) || c == '_';
        };
        auto isIdentChar = [](unsigned char c) {
            return std::isalnum(c) || c == '_';
        };

        bool lastWasUnderscore = false;
        for (unsigned char c : name) {
            if (isIdentChar(c)) {
                out.push_back(static_cast<char>(c));
                lastWasUnderscore = (c == '_');
            } else {
                if (!out.empty() && !lastWasUnderscore) {
                    out.push_back('_');
                    lastWasUnderscore = true;
                }
            }
        }

        while (!out.empty() && out.front() == '_') {
            out.erase(out.begin());
        }
        while (!out.empty() && out.back() == '_') {
            out.pop_back();
        }

        if (out.empty()) {
            out = "var";
        }

        if (!isIdentStart(static_cast<unsigned char>(out[0]))) {
            out.insert(out.begin(), '_');
        }

        static const std::unordered_set<std::string> kCppKeywords = {
            "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor",
            "bool", "break", "case", "catch", "char", "char8_t", "char16_t", "char32_t",
            "class", "compl", "concept", "const", "consteval", "constexpr", "constinit",
            "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype",
            "default", "delete", "do", "double", "dynamic_cast", "else", "enum", "explicit",
            "export", "extern", "false", "float", "for", "friend", "goto", "if", "inline",
            "int", "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq",
            "nullptr", "operator", "or", "or_eq", "private", "protected", "public", "register",
            "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static",
            "static_assert", "static_cast", "struct", "switch", "template", "this", "thread_local",
            "throw", "true", "try", "typedef", "typeid", "typename", "union", "unsigned",
            "using", "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq"
        };

        if (kCppKeywords.find(out) != kCppKeywords.end()) {
            out.push_back('_');
        }

        return out;
    }

}
