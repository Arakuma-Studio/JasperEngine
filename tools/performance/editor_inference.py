#!/usr/bin/env python3
"""Measure exact editor variable-type inference methods against a revision.

Isolates autocomplete type inference, including regex compilation and backward
declaration search. Reports process CPU time and allocations, not frame time.
"""
import argparse
import os
from pathlib import Path
import subprocess

from run_core import ROOT, definition, source

FIXTURE = r'''
#include <algorithm>
#include <array>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <new>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>
static size_t allocations = 0;
void* operator new(size_t size) {
    ++allocations;
    if (void* result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc();
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, size_t) noexcept { std::free(value); }
LANGUAGE;
template<int Variant> struct Editor {
    SyntaxLanguage language;
    std::vector<std::string> lines;
    std::string inferTypeOfVariable(const std::string&, int) const;
};
METHODS
static void require(bool value) {
    if (!value) throw std::runtime_error("autocomplete type inference mismatch");
}
static void check() {
    const std::vector<std::string> declarations = {
        "", "Player target;", "const Player* target = nullptr;", "Player& target = other;",
        "std::vector<Player> target;", "auto target = new Player();", "auto target = Player{};",
        "auto target = Player();", "return target;", "delete target;", "auto target = other;",
        "local target = Player.new()", "target = Player.create()", "local target = Player:new()",
        "local target = Player:create()", "local target = Player.other()", "target = Player{}",
        "unrelatedIdentifier = x", "// Player target;", "local target2 = Other.new()"
    };
    for (auto language : {SyntaxLanguage::None, SyntaxLanguage::Cpp, SyntaxLanguage::Lua,
                         SyntaxLanguage::CMake, SyntaxLanguage::Html, SyntaxLanguage::Css}) {
        Editor<0> before{language, {}};
        Editor<1> after{language, {}};
        for (const auto& declaration : declarations) {
            before.lines = after.lines = {"Player target;", "local target = First.new()", declaration, "target.member"};
            for (int current : {-1, 0, 1, 2, 3, 4, 6})
                for (const char* name : {"", "target", "missing", "target2"})
                    require(before.inferTypeOfVariable(name, current) == after.inferTypeOfVariable(name, current));
        }
        // The private caller supplies identifier-only names. Probe malformed
        // names too: both variants must throw regex_error or return the same
        // value. Error codes can differ as the first compiled pattern changes.
        if (language == SyntaxLanguage::Cpp || language == SyntaxLanguage::Lua) {
            for (const char* name : {"[", "(", "\\"}) {
                int oldError = -1, newError = -1;
                std::string oldResult, newResult;
                try { oldResult = before.inferTypeOfVariable(name, 0); }
                catch (const std::regex_error& error) { oldError = error.code(); }
                try { newResult = after.inferTypeOfVariable(name, 0); }
                catch (const std::regex_error& error) { newError = error.code(); }
                require((oldError == -1) == (newError == -1) && oldResult == newResult);
            }
        }
    }
    std::cout << "Behavior: identical inferred types for six languages, constructors, pointers, templates, shadowing, missing names and line bounds\n";
}
static volatile size_t sink = 0;
template<int Variant> static std::array<double, 2> measure(Editor<Variant>& editor, const std::string& name) {
    constexpr int iterations = 25;
    const size_t initial = allocations;
    const auto start = std::clock();
    for (int i = 0; i < iterations; ++i)
        sink += editor.inferTypeOfVariable(name, static_cast<int>(editor.lines.size()) - 1).size();
    return {1.0e6 * (std::clock() - start) / CLOCKS_PER_SEC / iterations,
            static_cast<double>(allocations - initial) / iterations};
}
int main(int argc, char**) {
    std::cout << std::unitbuf;
    check();
    if (argc > 1) return 0;
    const char* samplesPath = std::getenv("JASPER_PERF_SAMPLES");
    std::ofstream raw(samplesPath ? samplesPath : "samples.csv");
    raw << "language,lines,query,repeat,before_cpu_us,after_cpu_us,before_allocations,after_allocations\n";
    std::cout << "language,lines,query,before_cpu_us,after_cpu_us,speedup,before_allocations,after_allocations\n";
    for (auto language : {SyntaxLanguage::Cpp, SyntaxLanguage::Lua}) {
        const char* lang = language == SyntaxLanguage::Cpp ? "Cpp" : "Lua";
        for (size_t count : {100u, 10000u}) {
            Editor<0> before{language, std::vector<std::string>(count, "    unrelatedValue = 5;")};
            before.lines.front() = language == SyntaxLanguage::Cpp ? "auto target = new Player();" : "local target = Player.new()";
            Editor<1> after{language, before.lines};
            for (const char* name : {"target", "missing"}) {
                std::vector<double> oldTime, newTime, oldAlloc, newAlloc;
                for (int repeat = 0; repeat < 7; ++repeat) {
                    std::array<double, 2> a, b;
                    if (repeat % 2) { b = measure(after, name); a = measure(before, name); }
                    else { a = measure(before, name); b = measure(after, name); }
                    raw << lang << ',' << count << ',' << name << ',' << repeat << ',' << a[0] << ',' << b[0] << ',' << a[1] << ',' << b[1] << '\n';
                    oldTime.push_back(a[0]); newTime.push_back(b[0]);
                    oldAlloc.push_back(a[1]); newAlloc.push_back(b[1]);
                }
                for (auto* values : {&oldTime, &newTime, &oldAlloc, &newAlloc}) std::sort(values->begin(), values->end());
                std::cout << lang << ',' << count << ',' << name << ',' << oldTime[3] << ',' << newTime[3] << ','
                          << oldTime[3] / newTime[3] << ',' << oldAlloc[3] << ',' << newAlloc[3] << '\n';
            }
        }
    }
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-ref", required=True)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--check-only", action="store_true")
    parser.add_argument("--output", type=Path, default=ROOT / "build/performance-editor/inference")
    args = parser.parse_args()
    methods = []
    for variant, revision in enumerate((args.baseline_ref, None)):
        cpp = source("editor/window/widget/CustomTextEditor.cpp", revision)
        start = cpp.index("std::string CustomTextEditor::inferTypeOfVariable")
        # The regex literals contain unbalanced literal braces, so the generic
        # brace counter cannot delimit this method.
        end = cpp.index("\nstd::string CustomTextEditor::inferTypeBefore", start)
        methods.append("template<> " + cpp[start:end].rstrip()
                       .replace("CustomTextEditor::", f"Editor<{variant}>::"))
    language = definition(source("editor/window/widget/CustomTextEditor.h"), "enum class SyntaxLanguage")
    args.output.mkdir(parents=True, exist_ok=True)
    cpp, binary = args.output / "inference.cpp", args.output / "inference"
    cpp.write_text(FIXTURE.replace("LANGUAGE", language).replace("METHODS", "\n".join(methods)))
    flags = ["-std=c++17", "-O3", "-Wall", "-Wextra", "-Wno-mismatched-new-delete"]
    if args.sanitize:
        flags = ["-std=c++17", "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    subprocess.run([os.environ.get("CXX", "c++"), *flags, str(cpp), "-o", str(binary)], check=True)
    environment = dict(os.environ, JASPER_PERF_SAMPLES=str((args.output / "samples.csv").resolve()))
    subprocess.run([str(binary)] + (["--check-only"] if args.sanitize or args.check_only else []), check=True, env=environment)


if __name__ == "__main__":
    main()
