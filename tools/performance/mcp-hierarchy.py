#!/usr/bin/env python3
"""Compile and compare the real MCP tree-building/traversal code against a git ref.

Usage: python3 tools/performance/mcp-hierarchy.py --baseline-ref <revision>
No editor or graphics context needed: Scene only supplies fixture data. The source
functions are compiled unchanged in separate namespaces, checked for ordered
output equality, then timed with median process CPU time.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = "editor/mcp/tools/EntityTools.cpp"


def definition(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def methods(source):
    signatures = ["struct Hierarchy", "Hierarchy buildHierarchy", "struct TreeState",
                  "std::string flagsText", "void emitText", "Json emitJson"]
    return "\n".join(definition(source, s) + (";" if s.startswith("struct") else "")
                     for s in signatures)


FIXTURE = r'''
#include <algorithm>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "json.hpp"
using Json = nlohmann::json;
using Entity = uint32_t;
constexpr Entity NULL_ENTITY = 0;
enum class ScenePlayState { STOPPED, PLAYING };
struct Transform { Entity parent = 0; };
struct Components {
    std::vector<Entity> entities;
    std::vector<Transform> transforms;
    size_t size() const { return entities.size(); }
    Entity getEntity(size_t i) const { return entities[i]; }
    const Transform& getComponentFromIndex(size_t i) const { return transforms[i]; }
};
struct Scene {
    Components components;
    std::unordered_map<Entity, size_t> index;
    std::unordered_set<Entity> created;
    template<class T> Components* getComponentArray() { return &components; }
    template<class T> Transform* findComponent(Entity e) {
        auto it = index.find(e);
        return it == index.end() ? nullptr : &components.transforms[it->second];
    }
    bool isEntityCreated(Entity e) const { return created.count(e); }
    bool isEntityActive(Entity e) const { return e % 7 != 0; }
    std::string getEntityName(Entity e) const { return "Entity " + std::to_string(e); }
};
struct SceneProject {
    Scene* scene;
    std::vector<Entity> entities;
    ScenePlayState playState = ScenePlayState::STOPPED;
};
std::string componentsText(Scene*, Entity e) { return e % 3 == 0 ? "mesh" : ""; }
Json componentsJson(Scene*, Entity e) { return e % 3 == 0 ? Json::array({"mesh"}) : Json::array(); }
namespace before { BEFORE }
namespace after { AFTER }
void require(bool good) { if (!good) throw std::runtime_error("MCP tree regression"); }
SceneProject makeScene(Scene& scene, size_t count, bool flat, bool playing, unsigned seed) {
    SceneProject project{&scene};
    project.playState = playing ? ScenePlayState::PLAYING : ScenePlayState::STOPPED;
    std::mt19937 random(seed);
    for (Entity e = 1; e <= count; ++e) {
        // Include missing authored entities, bare entities, runtime roots/children,
        // and children of absent/non-authored parents in deterministic fixtures.
        if (e % 13 != 0) project.entities.push_back(e);
        if (e % 19 == 0) continue;
        scene.created.insert(e);
        if (e % 17 == 0) continue;
        Entity parent = !flat && e > 1 && e % 5 != 0 ? 1 + random() % (e - 1) : 0;
        scene.index.emplace(e, scene.components.size());
        scene.components.entities.push_back(e);
        scene.components.transforms.push_back({parent});
    }
    std::shuffle(project.entities.begin(), project.entities.end(), random);
    return project;
}
void check(SceneProject& project) {
    auto a = before::buildHierarchy(&project);
    auto b = after::buildHierarchy(&project);
    require(a.roots == b.roots);
    require(a.children.size() == b.children.size());
    for (const auto& item : a.children) require(item.second == b.children.at(item.first));
    for (int depth : {0, 1, 3, 64}) for (size_t limit : {1u, 2u, 40u, 100000u}) {
        before::TreeState x{&project, &a, true, depth, limit};
        after::TreeState y{&project, &b, true, depth, limit};
        x.mainCamera = y.mainCamera = 3;
        x.selected = y.selected = {2, 10, 80};
        for (Entity e : a.roots) { before::emitText(x, e, 0); if (x.truncated) break; }
        for (Entity e : b.roots) { after::emitText(y, e, 0); if (y.truncated) break; }
        require(x.text == y.text && x.emitted == y.emitted && x.truncated == y.truncated);
        x.emitted = y.emitted = 0;
        x.truncated = y.truncated = false;
        Json left = Json::array(), right = Json::array();
        for (Entity e : a.roots) {
            Json node = before::emitJson(x, e, 0);
            if (node.is_null()) break;
            left.push_back(node);
        }
        for (Entity e : b.roots) {
            Json node = after::emitJson(y, e, 0);
            if (node.is_null()) break;
            right.push_back(node);
        }
        require(left == right && x.emitted == y.emitted && x.truncated == y.truncated);
    }
}
volatile size_t sink = 0;
template<class Function> double timeUs(Function function) {
    std::vector<double> samples;
    for (int i = 0; i < 9; ++i) {
        const auto start = std::clock();
        function();
        samples.push_back(1e6 * (std::clock() - start) / CLOCKS_PER_SEC);
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}
int main() {
    for (unsigned seed = 0; seed < 80; ++seed) {
        Scene scene;
        auto project = makeScene(scene, seed * 13, seed % 3 == 0, seed % 2 == 0, seed);
        check(project);
    }
    std::cout << "80 randomized fixtures: identical root/child order, text/JSON, limits and flags\n";
    if (SANITIZE) return 0;
    for (size_t count : {10000u, 100000u}) for (bool flat : {true, false}) for (bool playing : {false, true}) {
        Scene scene;
        auto project = makeScene(scene, count, flat, playing, 42);
        const double oldUs = timeUs([&] { auto h = before::buildHierarchy(&project); sink = h.roots.size() + h.children.size(); });
        const double newUs = timeUs([&] { auto h = after::buildHierarchy(&project); sink = h.roots.size() + h.children.size(); });
        std::cout << count << (flat ? " flat" : " nested") << (playing ? " runtime" : " authored")
                  << ": before_us=" << oldUs << " after_us=" << newUs << " speedup=" << oldUs / newUs << "x\n";
    }
    // Wide subtree: the text limit must avoid visiting the remaining siblings.
    Scene scene;
    SceneProject project{&scene};
    project.entities.push_back(1);
    scene.created.insert(1);
    for (Entity e = 1; e <= 100000; ++e) {
        scene.index.emplace(e, scene.components.size());
        scene.components.entities.push_back(e);
        scene.components.transforms.push_back({e == 1 ? 0u : 1u});
        scene.created.insert(e);
        if (e != 1) project.entities.push_back(e);
    }
    auto a = before::buildHierarchy(&project);
    auto b = after::buildHierarchy(&project);
    const auto oldUs = timeUs([&] { for (int i = 0; i < 100; ++i) { before::TreeState s{&project, &a, false, 64, 100}; s.mainCamera = 0; before::emitText(s, 1, 0); sink = s.text.size(); } });
    const auto newUs = timeUs([&] { for (int i = 0; i < 100; ++i) { after::TreeState s{&project, &b, false, 64, 100}; s.mainCamera = 0; after::emitText(s, 1, 0); sink = s.text.size(); } });
    const auto oldJsonUs = timeUs([&] { before::TreeState s{&project, &a, true, 64, 100000}; s.mainCamera = 0; auto j = before::emitJson(s, 1, 0); sink = j.size(); });
    const auto newJsonUs = timeUs([&] { after::TreeState s{&project, &b, true, 64, 100000}; s.mainCamera = 0; auto j = after::emitJson(s, 1, 0); sink = j.size(); });
    std::cout << "100k wide-tree JSON traversal: before_us=" << oldJsonUs << " after_us=" << newJsonUs << " speedup=" << oldJsonUs / newJsonUs << "x\n";
    std::cout << "100 truncated wide-tree traversals: before_us=" << oldUs << " after_us=" << newUs << " speedup=" << oldUs / newUs << "x\n";
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-ref", required=True)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    before = subprocess.check_output(["git", "show", f"{args.baseline_ref}:{SOURCE}"], cwd=ROOT, text=True)
    after = (ROOT / SOURCE).read_text()
    fixture = FIXTURE.replace("BEFORE", methods(before)).replace("AFTER", methods(after)).replace("SANITIZE", "true" if args.sanitize else "false")
    with tempfile.TemporaryDirectory(prefix="mcp-hierarchy-") as temp:
        path = Path(temp)
        (path / "test.cpp").write_text(fixture)
        flags = ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else ["-O2"]
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-I", str(ROOT / "engine/libs/json"), *flags, str(path / "test.cpp"), "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    main()
