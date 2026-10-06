#!/usr/bin/env python3
"""Benchmark the production search-highlight traversal with a recording draw list.

Usage: python3 tools/performance/text_search.py --baseline-ref REV
Measures CPU traversal and rectangle generation, not GPU or whole-frame time.
The lightweight drawing fixture also verifies identical ordered rectangles.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from console import definition

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = r'''
#include <algorithm>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
struct ImVec2 { float x, y; ImVec2(float x = 0, float y = 0) : x(x), y(y) {} };
using ImU32 = uint32_t;
namespace ImGui { static ImU32 ColorConvertFloat4ToU32(ImU32 color) { return color; } }
struct Rectangle { ImVec2 start, end; ImU32 color; };
struct ImDrawList {
    std::vector<Rectangle> rectangles;
    void AddRectFilled(ImVec2 start, ImVec2 end, ImU32 color) { rectangles.push_back({start, end, color}); }
};
TEXT_POSITION;
struct CustomTextEditor {
    std::vector<TextPosition> searchResults;
    std::string searchText = "find";
    ImU32 searchHighlightColor = 1234;
    float textStartX = 40, lineHeight = 20;
    float byteOffsetToPixelX(int, int column) const { return column * 8.0f; }
    void before(ImDrawList*, const ImVec2&, int, int);
    void after(ImDrawList*, const ImVec2&, int, int);
};
METHODS
static void require(bool condition) {
    if (!condition) throw std::runtime_error("search-highlight rectangle regression");
}
static void compare(CustomTextEditor& editor, int start, int end) {
    ImDrawList a, b;
    editor.before(&a, ImVec2(10, -1000), start, end);
    editor.after(&b, ImVec2(10, -1000), start, end);
    require(a.rectangles.size() == b.rectangles.size());
    for (size_t i = 0; i < a.rectangles.size(); ++i) {
        const auto& x = a.rectangles[i]; const auto& y = b.rectangles[i];
        require(x.start.x == y.start.x && x.start.y == y.start.y &&
                x.end.x == y.end.x && x.end.y == y.end.y && x.color == y.color);
    }
}
static void check() {
    CustomTextEditor editor;
    compare(editor, 0, 20);
    for (int line = 0; line < 1000; line += 3)
        for (int column : {0, 4, 12}) editor.searchResults.emplace_back(line, column);
    for (int start : {-20, 0, 1, 2, 3, 100, 998, 999, 1000, 5000})
        for (int count : {-1, 0, 1, 20, 10000}) compare(editor, start, start + count);
    editor.searchResults.clear();
    editor.searchResults.emplace_back(99, 8);
    compare(editor, 99, 99); compare(editor, 0, 98); compare(editor, 100, 101);
    std::cout << "Correctness: empty/single/multiple matches, inclusive bounds, gaps, offscreen results and draw order passed\n";
}
static volatile size_t sink = 0;
static double measure(CustomTextEditor& editor, bool after, int firstLine) {
    std::vector<double> samples;
    ImDrawList list;
    for (int repeat = 0; repeat < 5; ++repeat) {
        const auto start = std::clock();
        for (int frame = 0; frame < 1000; ++frame) {
            list.rectangles.clear();
            if (after) editor.after(&list, ImVec2(), firstLine, firstLine + 40);
            else editor.before(&list, ImVec2(), firstLine, firstLine + 40);
            sink += list.rectangles.size();
        }
        samples.push_back(1.0e6 * (std::clock() - start) / CLOCKS_PER_SEC / 1000);
    }
    std::sort(samples.begin(), samples.end());
    return samples[2];
}
int main(int argc, char**) {
    std::cout << std::unitbuf;
    check();
    if (argc > 1) return 0;
    std::cout << "matches,before_cpu_us_per_frame,after_cpu_us_per_frame,speedup\n";
    for (int count : {1000, 10000, 100000, 1000000}) {
        CustomTextEditor editor;
        for (int i = 0; i < count; ++i) editor.searchResults.emplace_back(i / 3, (i % 3) * 10);
        const auto a = measure(editor, false, count / 6);
        const auto b = measure(editor, true, count / 6);
        std::cout << count << ',' << a << ',' << b << ',' << a / b << '\n';
    }
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-ref", required=True)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args()
    path = "editor/window/widget/CustomTextEditor.cpp"
    before = subprocess.check_output(["git", "show", f"{args.baseline_ref}:{path}"], cwd=ROOT, text=True)
    after = (ROOT / path).read_text()
    signature = "void CustomTextEditor::renderSearchHighlights"
    methods = definition(before, signature).replace("::renderSearchHighlights", "::before")
    methods += "\n" + definition(after, signature).replace("::renderSearchHighlights", "::after")
    header = (ROOT / "editor/window/widget/CustomTextEditor.h").read_text()
    fixture = FIXTURE.replace("METHODS", methods).replace("TEXT_POSITION", definition(header, "struct TextPosition"))
    with tempfile.TemporaryDirectory(prefix="jasper-text-search-perf-") as directory:
        cpp, binary = Path(directory) / "benchmark.cpp", Path(directory) / "benchmark"
        cpp.write_text(fixture)
        command = [os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-Wall", "-Wextra", str(cpp), "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)] + (["--check-only"] if args.check_only else []), check=True)


if __name__ == "__main__":
    main()
