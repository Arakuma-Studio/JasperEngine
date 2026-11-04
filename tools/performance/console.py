#!/usr/bin/env python3
"""Compare actual ConsoleWindow storage/filter methods with a git revision.

Usage: python3 tools/performance/console.py --baseline-ref REV
Builds production headers and method definitions with the repository's ImGui.
No window, renderer, or editor build is needed. Reports median process CPU time.
Add --selection to compare the selected-message lookup over monotonic log ids.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from run_core import ROOT, definition


def selection_production(ref, namespace):
    path = "editor/window/ConsoleWindow.cpp"
    source = subprocess.check_output(["git", "show", f"{ref}:{path}"], cwd=ROOT, text=True) if ref else (ROOT / path).read_text()
    method = definition(source, "const LogData* ConsoleWindow::selectedLog").replace("ConsoleWindow::", "Selection::")
    return f"""namespace {namespace} {{
struct Selection {{
    std::deque<LogData> logs;
    uint64_t selectedLogId = 0;
    bool hasSelection = false;
    const LogData* selectedLog() const;
}};
{method}
}}"""


def run_selection(args):
    fixture = (ROOT / "tools/performance/console_selection.cpp").read_text().replace(
        "PRODUCTION", selection_production(args.baseline_ref, "before") + selection_production(None, "after"))
    with tempfile.TemporaryDirectory(prefix="jasper-console-selection-") as directory:
        cpp, binary = Path(directory) / "benchmark.cpp", Path(directory) / "benchmark"
        cpp.write_text(fixture)
        command = [os.environ.get("CXX", "c++"), "-std=c++20", "-O3", "-Wall", "-Wextra", "-Werror",
                   "-I", str(ROOT / "editor"), "-I", str(ROOT / "libs/imgui"), str(cpp), "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)] + (["--check-only"] if args.check_only else []), check=True)


def production(ref, namespace):
    def read(path):
        if ref:
            return subprocess.check_output(["git", "show", f"{ref}:{path}"], cwd=ROOT, text=True)
        return (ROOT / path).read_text()

    header = read("editor/window/ConsoleWindow.h")
    source = read("editor/window/ConsoleWindow.cpp")
    header = header.replace("#pragma once", "").replace("private:", "public:")
    header = header.replace("namespace jasperengine::editor", f"namespace {namespace}")
    signatures = [
        "ConsoleWindow::ConsoleWindow()", "void ConsoleWindow::clear()",
        "ConsoleWindow::Severity ConsoleWindow::severityOf", "size_t ConsoleWindow::countRawLines",
        "size_t ConsoleWindow::trimLeadingRawLines", "void ConsoleWindow::enforceStoredLineLimit",
        "void ConsoleWindow::recountSeverities", "void ConsoleWindow::addLog(LogType type, const std::string&",
        "bool ConsoleWindow::getLatestLog",
        "bool ConsoleWindow::passTextFilter", "void ConsoleWindow::rebuildRows",
        "const LogData* ConsoleWindow::selectedLog",
    ]
    methods = "\n".join(definition(source, signature) for signature in signatures)
    return header + f"\nnamespace {namespace} {{\n" + definition(source, "const char* logTypeName") + "\n" + methods + "\n}\n"


FIXTURE = r'''
#include <algorithm>
#include <cstring>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
PRODUCTION
static void require(bool condition) {
    if (!condition) throw std::runtime_error("Console storage/filter regression");
}
static void compare(before::ConsoleWindow& a, after::ConsoleWindow& b) {
    require(a.logs.size() == b.logs.size());
    require(a.storedLogLineCount == b.storedLogLineCount);
    require(b.storedLogLineCount <= 10000);
    require(a.nextLogId == b.nextLogId);
    for (int i = 0; i < 3; ++i) require(a.severityCounts[i] == b.severityCounts[i]);
    for (size_t i = 0; i < a.logs.size(); ++i) {
        require(a.logs[i].id == b.logs[i].id);
        require(a.logs[i].message == b.logs[i].message);
        require(static_cast<int>(a.logs[i].type) == static_cast<int>(b.logs[i].type));
    }
    a.rebuildRows(); b.rebuildRows();
    require(a.rows.size() == b.rows.size());
    for (size_t i = 0; i < a.rows.size(); ++i) {
        require(a.rows[i].logIndex == b.rows[i].logIndex);
        require(a.rows[i].count == b.rows[i].count);
    }
    before::LogType at; after::LogType bt;
    std::string am, bm;
    require(a.getLatestLog(at, am) == b.getLatestLog(bt, bm));
    if (!a.logs.empty()) require(static_cast<int>(at) == static_cast<int>(bt) && am == bm);
    const auto* as = a.selectedLog(); const auto* bs = b.selectedLog();
    require((as == nullptr) == (bs == nullptr));
    if (as) require(as->id == bs->id && as->message == bs->message);
}
static void check() {
    before::ConsoleWindow a; after::ConsoleWindow b;
    auto add = [&](int type, const std::string& message) {
        a.addLog(static_cast<before::LogType>(type), message);
        b.addLog(static_cast<after::LogType>(type), message);
    };
    compare(a, b);
    add(1, "selected\nsecond\n");
    a.hasSelection = b.hasSelection = true;
    a.selectedLogId = b.selectedLogId = 1;
    for (int i = 0; i < 10100; ++i) {
        add(i % 5, i % 7 == 0 ? "" : "repeat " + std::to_string(i % 13));
        if (i % 100 == 0 || i > 9990) compare(a, b);
    }
    for (bool collapse : {false, true}) {
        a.collapse = b.collapse = collapse;
        for (bool matchCase : {false, true}) {
            a.searchMatchCase = b.searchMatchCase = matchCase;
            for (const char* filter : {"", "repeat", "Repeat", "repeat,-3", "missing"}) {
                std::strcpy(a.filter.InputBuf, filter); a.filter.Build();
                std::strcpy(b.filter.InputBuf, filter); b.filter.Build();
                compare(a, b);
                a.severityFilters[1] = b.severityFilters[1] = false;
                compare(a, b);
                a.severityFilters[1] = b.severityFilters[1] = true;
            }
        }
    }
    a.clear(); b.clear(); compare(a, b);
    add(2, "one\ntwo\nthree");
    a.hasSelection = b.hasSelection = true;
    a.selectedLogId = b.selectedLogId = a.logs.front().id;
    for (int i = 0; i < 9998; ++i) add(0, "filler");
    compare(a, b); // Partial trim preserves the first entry's severity and id.
    require(b.logs.front().message == "two\nthree");
    std::string oversized;
    for (int i = 0; i < 12000; ++i) oversized += "line\n";
    add(4, oversized); compare(a, b);
    require(b.logs.size() == 1 && b.storedLogLineCount == 10000);
    add(3, "tail\n"); compare(a, b);
    a.clear(); b.clear(); compare(a, b);
    std::cout << "Correctness: rollover, partial/oversized trimming, empty/trailing lines, severity counts, selection, clear, filters and collapse passed\n";
}
static volatile size_t sink = 0;
template<class Window, class Type> static double measure(const std::string& message, int count, int warmup) {
    std::vector<double> samples;
    for (int repetition = 0; repetition < 5; ++repetition) {
        Window window;
        for (int i = 0; i < warmup; ++i) window.addLog(static_cast<Type>(i % 5), message);
        const auto start = std::clock();
        for (int i = 0; i < count; ++i) window.addLog(static_cast<Type>(i % 5), message);
        samples.push_back(1.0e6 * (std::clock() - start) / CLOCKS_PER_SEC / count);
        sink = sink + window.logs.size() + window.severityCounts[0];
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}
int main(int argc, char**) {
    std::cout << std::unitbuf;
    check();
    if (argc > 1) return 0;
    std::cout << "scenario,before_cpu_us_per_append,after_cpu_us_per_append,speedup\n";
    for (const std::string message : {std::string("short"), std::string(160, 'x'), std::string("one\ntwo\nthree\nfour")}) {
        const auto a = measure<before::ConsoleWindow, before::LogType>(message, 20000, 10000);
        const auto b = measure<after::ConsoleWindow, after::LogType>(message, 20000, 10000);
        std::cout << "full-buffer-" << message.size() << "-bytes," << a << ',' << b << ',' << a / b << '\n';
    }
    const auto a = measure<before::ConsoleWindow, before::LogType>("short", 5000, 0);
    const auto b = measure<after::ConsoleWindow, after::LogType>("short", 5000, 0);
    std::cout << "below-capacity," << a << ',' << b << ',' << a / b << '\n';
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-ref", required=True)
    parser.add_argument("--sanitize", action="store_true", help="Enable address/undefined sanitizers")
    parser.add_argument("--check-only", action="store_true", help="Run regression checks without timing")
    parser.add_argument("--selection", action="store_true", help="Compare selected-message lookup instead of append/filter work")
    args = parser.parse_args()
    if args.selection:
        run_selection(args)
        return
    fixture = FIXTURE.replace("PRODUCTION", production(args.baseline_ref, "before") + production(None, "after"))
    with tempfile.TemporaryDirectory(prefix="jasper-console-perf-") as directory:
        cpp, binary = Path(directory) / "benchmark.cpp", Path(directory) / "benchmark"
        cpp.write_text(fixture)
        command = [os.environ.get("CXX", "c++"), "-std=c++20", "-O2", "-ffunction-sections", "-fdata-sections",
                   "-I", str(ROOT / "libs/imgui"), str(cpp), str(ROOT / "libs/imgui/imgui.cpp"),
                   "-Wl,--gc-sections", "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)] + (["--check-only"] if args.check_only else []), check=True)


if __name__ == "__main__":
    main()
