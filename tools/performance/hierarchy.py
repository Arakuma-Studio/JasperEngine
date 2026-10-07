#!/usr/bin/env python3
"""Benchmark actual hierarchy filtering methods against a git revision.

Usage: python3 tools/performance/hierarchy.py --baseline-ref 6479bcb0
Requires a C++17 compiler. No editor, graphics context or third-party libraries needed.
The fixture compiles methods and TreeNode directly from the production source.
Reports median process CPU microseconds, with one grouping node per 100 leaves.
Each 120-frame sample includes four cache invalidations (the existing 0.5s timer).
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from run_core import ROOT, definition


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-ref", required=True, help="Revision before the hierarchy filter cache change")
    args = parser.parse_args()
    source_path = "editor/window/HierarchyWindow.cpp"
    source = (ROOT / source_path).read_text()
    header = (ROOT / "editor/window/HierarchyWindow.h").read_text()
    baseline = subprocess.check_output(
        ["git", "show", f"{args.baseline_ref}:{source_path}"], cwd=ROOT, text=True)
    start = baseline.index("    // Apply search filtering if")
    end = baseline.index("    revealOpenNodes.clear();", start)
    baseline_method = "void editor::HierarchyWindow::baselineUpdate(TreeNode& root, bool) {\n" + baseline[start:end] + "}\n"
    signatures = [
        "uint32_t editor::HierarchyWindow::getNodeSceneId",
        "void editor::HierarchyWindow::collectVisibleEntitySelectionOrder",
        "bool editor::HierarchyWindow::nodeMatchesSearch",
        "void editor::HierarchyWindow::markMatchingNodes",
    ]
    methods = "\n".join(definition(source, signature) for signature in signatures)
    update_signature = "void editor::HierarchyWindow::updateSearchAndSelectionOrder"
    if update_signature in source:
        methods += "\n" + definition(source, update_signature)
    else:
        methods += "\n" + baseline_method.replace("baselineUpdate", "updateSearchAndSelectionOrder")
    fixture = r'''
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <list>
#include <stdexcept>
#include <string>
#include <vector>
using Entity = uint32_t;
constexpr Entity NULL_ENTITY = 0;
constexpr uint32_t NULL_PROJECT_SCENE = 0;
namespace jasperengine { namespace editor {
TREE_NODE;
SELECTION_ENTRY;
struct Project { uint32_t getSelectedSceneId() const { return 7; } };
struct HierarchyWindow {
    Project storage;
    Project* project = &storage;
    bool matchCase = false;
    char searchBuffer[256] = "";
    std::string cachedSearch;
    bool cachedMatchCase = false;
    std::vector<EntitySelectionEntry> visibleEntitySelectionOrder;
    uint32_t getNodeSceneId(const TreeNode&) const;
    void collectVisibleEntitySelectionOrder(const TreeNode&, bool);
    bool nodeMatchesSearch(const TreeNode&, const std::string&);
    void markMatchingNodes(TreeNode&, const std::string&);
    void updateSearchAndSelectionOrder(TreeNode&, bool);
    void baselineUpdate(TreeNode&, bool);
};
}}
using namespace jasperengine;
METHODS
BASELINE_METHOD
using namespace jasperengine::editor;
static TreeNode makeTree(size_t count) {
    TreeNode root;
    root.isScene = true;
    root.id = 7;
    root.name = "Scene";
    for (size_t i = 0; i < count; ++i) {
        if (i % 100 == 0) {
            root.children.emplace_back();
            auto& group = root.children.back();
            group.id = static_cast<Entity>(count + i + 1);
            group.name = "Group " + std::to_string(i);
            group.entitySceneId = i % 200 == 0 ? 7 : 9;
        }
        auto& group = root.children.back();
        group.children.emplace_back();
        auto& node = group.children.back();
        node.id = static_cast<Entity>(i + 1);
        node.entitySceneId = group.entitySceneId;
        node.name = "Long Hierarchy Object Name " + std::to_string(i);
    }
    return root;
}
static void require(bool condition) {
    if (!condition) throw std::runtime_error("hierarchy filtering regression");
}
static void compareNodes(const TreeNode& a, const TreeNode& b) {
    require(a.matchesSearch == b.matchesSearch);
    require(a.hasMatchingDescendant == b.hasMatchingDescendant);
    require(a.children.size() == b.children.size());
    auto it = b.children.begin();
    for (const auto& child : a.children) compareNodes(child, *it++);
}
static void check() {
    HierarchyWindow before, after;
    auto a = makeTree(320), b = a;
    auto step = [&](const char* search, bool matchCase, bool rebuild) {
        std::strcpy(before.searchBuffer, search);
        std::strcpy(after.searchBuffer, search);
        before.matchCase = after.matchCase = matchCase;
        before.baselineUpdate(a, rebuild);
        after.updateSearchAndSelectionOrder(b, rebuild);
        compareNodes(a, b);
        const auto& oldOrder = before.visibleEntitySelectionOrder;
        const auto& newOrder = after.visibleEntitySelectionOrder;
        require(oldOrder.size() == newOrder.size());
        for (size_t i = 0; i < oldOrder.size(); ++i) {
            require(oldOrder[i].sceneId == newOrder[i].sceneId);
            require(oldOrder[i].entity == newOrder[i].entity);
        }
    };
    step("", false, true);
    step("", false, false);
    step("object", false, false);
    step("object", false, false);
    step("object", true, false);
    step("Object", true, false);
    step("does not exist", false, false);
    step("Group", false, false);
    step("", false, false);
    a.children.front().children.front().name = "Renamed object";
    b.children.front().children.front().name = "Renamed object";
    step("Renamed", false, true);
    a.children.pop_back(); b.children.pop_back();
    step("Renamed", false, true);
    a = makeTree(5); b = a;
    step("Renamed", false, true);
    step("", false, false);
    std::cout << "Correctness: search changes, case changes, clearing, renaming, deletion, tree replacement and scene order passed\n";
}
static volatile size_t sink = 0;
static double measure(TreeNode& tree, const char* search, bool optimized) {
    constexpr int frames = 120;
    std::vector<double> samples;
    for (int repetition = 0; repetition < 3; ++repetition) {
        HierarchyWindow window;
        std::strcpy(window.searchBuffer, search);
        const auto start = std::clock();
        for (int frame = 0; frame < frames; ++frame) {
            // Include the production tree cache's periodic 0.5-second refresh at 60 Hz.
            const bool rebuild = frame % 31 == 0;
            if (optimized) window.updateSearchAndSelectionOrder(tree, rebuild);
            else window.baselineUpdate(tree, rebuild);
            sink += window.visibleEntitySelectionOrder.size();
        }
        samples.push_back(1.0e6 * (std::clock() - start) / CLOCKS_PER_SEC / frames);
    }
    std::sort(samples.begin(), samples.end());
    return samples[1];
}
int main() {
    std::cout << std::unitbuf;
    check();
    std::cout << "rows,query,before_cpu_us_per_frame,after_cpu_us_per_frame,speedup\n";
    for (size_t count : {1000u, 10000u, 100000u}) {
        auto tree = makeTree(count);
        for (const char* search : {"", "object", "not present"}) {
            const auto before = measure(tree, search, false);
            const auto after = measure(tree, search, true);
            std::cout << count << ',' << (search[0] ? search : "<empty>") << ','
                      << before << ',' << after << ',' << before / after << '\n';
        }
    }
}
'''
    fixture = fixture.replace("TREE_NODE", definition(header, "struct TreeNode"))
    fixture = fixture.replace("SELECTION_ENTRY", definition(header, "struct EntitySelectionEntry"))
    fixture = fixture.replace("METHODS", methods).replace("BASELINE_METHOD", baseline_method)
    with tempfile.TemporaryDirectory(prefix="jasper-hierarchy-perf-") as directory:
        directory = Path(directory)
        cpp = directory / "benchmark.cpp"
        binary = directory / "benchmark"
        cpp.write_text(fixture)
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-Wall", "-Wextra", str(cpp), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
