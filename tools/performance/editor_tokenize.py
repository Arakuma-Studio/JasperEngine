#!/usr/bin/env python3
"""Compare the exact production editor tokenizer against a revision.

Measures CPU time and allocations for full-document retokenization after an edit,
not editor frame time. All tokenizer methods and their types are extracted from
each revision; the actual HtmlCssLexer is linked. No graphics context is needed.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys

from run_core import ROOT, definition


FIXTURE = r'''
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>
#include "HtmlCssLexer.h"
static size_t allocations = 0;
void* operator new(size_t size) {
    ++allocations;
    if (void* result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc();
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, size_t) noexcept { std::free(value); }
using jasperengine::editor::HtmlCssLexer;
TYPES
ENGINE_NAMES
template<int Variant> struct Editor {
    std::vector<std::string> lines;
    std::vector<std::vector<Token>> lineTokens;
    std::vector<HtmlCssLexer::State> lineEndStates;
    SyntaxLanguage language = SyntaxLanguage::Cpp;
    LanguageDefinition languageDef;
    bool maxLineWidthDirty = false;
    bool isHtmlOrCss() const { return language == SyntaxLanguage::Html || language == SyntaxLanguage::Css; }
    HtmlCssLexer::Language htmlCssLanguage() const;
    void tokenizeText(const std::string&, OpenToken&, std::vector<Token>&) const;
    int multiLineTokenAt(const std::string&, int, OpenToken&) const;
    int scanOpenToken(const std::string&, int, OpenToken&) const;
    TokenType classifyWord(const std::string&) const;
    void tokenizeAll();
    void initializeLanguage();
    void configure(SyntaxLanguage value) {
        language = value;
        initializeLanguage();
    }
};
METHODS
static void require(bool value) {
    if (!value) throw std::runtime_error("tokenizer behavior mismatch");
}
static void compare(Editor<0>& before, Editor<1>& after) {
    before.tokenizeAll(); after.tokenizeAll();
    require(before.maxLineWidthDirty && after.maxLineWidthDirty);
    require(before.lineTokens.size() == after.lineTokens.size());
    require(before.lineEndStates.size() == after.lineEndStates.size());
    for (size_t line = 0; line < before.lineTokens.size(); ++line) {
        const auto& a = before.lineTokens[line];
        const auto& b = after.lineTokens[line];
        require(a.size() == b.size());
        for (size_t i = 0; i < a.size(); ++i)
            require(a[i].start == b[i].start && a[i].length == b[i].length && a[i].type == b[i].type);
    }
    // Compare lexing of a continuation with each cached end state, avoiding
    // padding-byte comparisons of the State aggregate.
    for (size_t line = 0; line < before.lineEndStates.size(); ++line) {
        auto a = before.lineEndStates[line], b = after.lineEndStates[line];
        auto x = HtmlCssLexer::lexLine(before.htmlCssLanguage(), "\" > /* */ color: red; } </style>", a);
        auto y = HtmlCssLexer::lexLine(after.htmlCssLanguage(), "\" > /* */ color: red; } </style>", b);
        require(x.size() == y.size());
        for (size_t i = 0; i < x.size(); ++i)
            require(x[i].start == y[i].start && x[i].length == y[i].length && x[i].kind == y[i].kind);
    }
}
static void check() {
    Editor<0> before;
    Editor<1> after;
    const std::vector<std::string> cases = {
        "", "float x = 1.5f; // comment", "/* open", "inside", "*/ int y;",
        "auto raw = R\"tag(one", "two)tag\";", "--[==[ comment", "more ]==] local x = 2",
        "s = [=[ first", "second ]=]", "#[[", "set(X 4)", "]]",
        "<!-- comment", "end --> <style>", ".foo { color: red;", "/* comment", "end */ } </style>",
        "<div style=\"color: red;", "width: 2px\">text</div>", "u8\"UTF-8: Жук العربية\""
    };
    for (auto language : {SyntaxLanguage::None, SyntaxLanguage::Cpp, SyntaxLanguage::Lua,
                         SyntaxLanguage::CMake, SyntaxLanguage::Html, SyntaxLanguage::Css}) {
        before.configure(language); after.configure(language);
        before.lines = after.lines = cases;
        compare(before, after);
        // Same document, replacement, growth, shrinkage, clearing and reloading
        // exercise all paths where retained vectors might otherwise hold old tokens.
        for (int edit = 0; edit < 80; ++edit) {
            const size_t line = static_cast<size_t>(edit) % before.lines.size();
            before.lines[line] = after.lines[line] = cases[static_cast<size_t>(edit * 7) % cases.size()];
            if (edit % 4 == 0) {
                before.lines.insert(before.lines.begin() + line, "");
                after.lines.insert(after.lines.begin() + line, "");
            } else if (edit % 7 == 0 && before.lines.size() > 1) {
                before.lines.erase(before.lines.begin() + line);
                after.lines.erase(after.lines.begin() + line);
            }
            compare(before, after);
        }
        before.lines.clear(); after.lines.clear(); compare(before, after);
        before.lines = after.lines = {"", "new"}; compare(before, after);
    }
    std::cout << "Behavior: identical tokens for six languages, multiline state, edits, insertion, deletion, language changes and empty documents\n";
}
static volatile size_t sink = 0;
template<int Variant> static std::array<double, 2> measure(Editor<Variant>& editor, int iterations) {
    const size_t initial = allocations;
    const auto start = std::clock();
    for (int i = 0; i < iterations; ++i) {
        editor.tokenizeAll();
        sink += editor.lineTokens[static_cast<size_t>(i) % editor.lines.size()].size();
    }
    const double micros = 1.0e6 * (std::clock() - start) / CLOCKS_PER_SEC / iterations;
    return {micros, static_cast<double>(allocations - initial) / iterations};
}
int main(int argc, char**) {
    std::cout << std::unitbuf;
    check();
    if (argc > 1) return 0;
    const char* samplesPath = std::getenv("JASPER_PERF_SAMPLES");
    std::ofstream raw(samplesPath ? samplesPath : "samples.csv");
    raw << "language,lines,repeat,before_cpu_us,after_cpu_us,before_allocations,after_allocations\n";
    std::cout << "language,lines,before_cpu_us,after_cpu_us,speedup,before_allocations,after_allocations\n";
    for (auto language : {SyntaxLanguage::Cpp, SyntaxLanguage::Lua, SyntaxLanguage::Css}) {
        const char* name = language == SyntaxLanguage::Cpp ? "Cpp" : language == SyntaxLanguage::Lua ? "Lua" : "Css";
        for (size_t count : {100u, 1000u, 10000u}) {
            Editor<0> before;
            Editor<1> after;
            before.configure(language); after.configure(language);
            const char* text = language == SyntaxLanguage::Cpp ? "    float value = sin(x * 1.25f) + 3.0f;" :
                               language == SyntaxLanguage::Lua ? "    local value = sin(x * 1.25) + 3.0" :
                               ".button { color: #ffffff; width: 10px; }";
            before.lines.assign(count, text); after.lines = before.lines;
            before.tokenizeAll(); after.tokenizeAll();
            std::vector<double> oldTime, newTime, oldAlloc, newAlloc;
            for (int repetition = 0; repetition < 7; ++repetition) {
                std::array<double, 2> a, b;
                const int iterations = count == 10000 ? 10 : 60;
                if (repetition % 2) { b = measure(after, iterations); a = measure(before, iterations); }
                else { a = measure(before, iterations); b = measure(after, iterations); }
                raw << name << ',' << count << ',' << repetition << ',' << a[0] << ',' << b[0] << ',' << a[1] << ',' << b[1] << '\n';
                oldTime.push_back(a[0]); newTime.push_back(b[0]);
                oldAlloc.push_back(a[1]); newAlloc.push_back(b[1]);
            }
            for (auto* values : {&oldTime, &newTime, &oldAlloc, &newAlloc}) std::sort(values->begin(), values->end());
            std::cout << name << ',' << count << ',' << oldTime[3] << ',' << newTime[3] << ','
                      << oldTime[3] / newTime[3] << ',' << oldAlloc[3] << ',' << newAlloc[3] << '\n';
        }
    }
}
'''


def source(path, revision=None):
    if revision:
        return subprocess.check_output(["git", "show", f"{revision}:{path}"], cwd=ROOT, text=True)
    return (ROOT / path).read_text()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-ref", required=True)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--check-only", action="store_true")
    parser.add_argument("--output", type=Path, default=ROOT / "build/performance-editor/tokenize")
    args = parser.parse_args()
    path = "editor/window/widget/CustomTextEditor.cpp"
    for lexer in ("HtmlCssLexer.h", "HtmlCssLexer.cpp"):
        lexer_path = f"editor/window/widget/{lexer}"
        if source(lexer_path, args.baseline_ref) != source(lexer_path):
            parser.error(f"{lexer} differs from baseline; this runner requires a common unchanged lexer")
    header = source("editor/window/widget/CustomTextEditor.h", args.baseline_ref)
    types = "\n".join(definition(header, marker) + ";" for marker in (
        "enum class SyntaxLanguage", "enum class TokenType", "struct Token {", "struct OpenToken", "struct LanguageDefinition"))
    methods = []
    for variant, revision in enumerate((args.baseline_ref, None)):
        text = source(path, revision)
        for marker in ("int CustomTextEditor::multiLineTokenAt", "int CustomTextEditor::scanOpenToken",
                       "TokenType CustomTextEditor::classifyWord", "HtmlCssLexer::Language CustomTextEditor::htmlCssLanguage",
                       "void CustomTextEditor::tokenizeText", "void CustomTextEditor::tokenizeAll",
                       "void CustomTextEditor::initializeLanguage"):
            method = definition(text, marker).replace("CustomTextEditor::", f"Editor<{variant}>::")
            methods.append("template<> " + method)
    args.output.mkdir(parents=True, exist_ok=True)
    generated = args.output / "engine_api_suggestions.h"
    with (args.output / "generate_api_suggestions.log").open("w") as log:
        subprocess.run([sys.executable, str(ROOT / "generate_api_suggestions.py"),
                        str(ROOT / "engine/core/script/binding"), str(generated),
                        str(ROOT / "engine/core")], stdout=log, check=True)
    engine_names = "\n".join(definition(generated.read_text(), marker) for marker in (
        "inline std::unordered_set<std::string> getEngineTypeNames",
        "inline std::unordered_set<std::string> getEngineBuiltinNames",
        "inline std::unordered_set<std::string> getEngineEnumNames"))
    cpp, binary = args.output / "tokenize.cpp", args.output / "tokenize"
    cpp.write_text(FIXTURE.replace("TYPES", types).replace("ENGINE_NAMES", engine_names).replace("METHODS", "\n".join(methods)))
    flags = ["-std=c++17", "-O3", "-Wall", "-Wextra", "-Wno-mismatched-new-delete"]
    if args.sanitize:
        flags = ["-std=c++17", "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    subprocess.run([os.environ.get("CXX", "c++"), *flags, "-I", str(ROOT / "editor/window/widget"),
                    str(cpp), str(ROOT / "editor/window/widget/HtmlCssLexer.cpp"), "-o", str(binary)], check=True)
    environment = dict(os.environ, JASPER_PERF_SAMPLES=str((args.output / "samples.csv").resolve()))
    subprocess.run([str(binary)] + (["--check-only"] if args.check_only or args.sanitize else []), check=True, env=environment)


if __name__ == "__main__":
    main()
