// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vcs/scene/SceneCli.h"

#include "vcs/scene/SceneDiffJson.h"
#include "vcs/scene/SceneMerge.h"

#include "io/AtomicFile.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

namespace jasperengine::editor::vcs::scene {

namespace fs = std::filesystem;

namespace {

    bool readText(const fs::path& path, std::string& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::ostringstream content;
        content << in.rdbuf();
        out = content.str();
        return true;
    }

    fs::path argPath(const char* arg) {
        const std::string text(arg);
        return fs::path(std::u8string(text.begin(), text.end()));
    }

}

fs::path conflictsFileFor(const fs::path& workTree, const std::string& path) {
    const fs::path relative = argPath(path.c_str());
    const fs::path file = (workTree / relative).lexically_normal();
    std::error_code ec;
    for (fs::path dir = file.parent_path(); !dir.empty(); dir = dir.parent_path()) {
        if (fs::exists(dir / "project.yaml", ec)) {
            return dir / ".jasperengine" / "merge" / (file.lexically_relative(dir).string() + ".conflicts.yaml");
        }
        const fs::path parent = dir.parent_path();
        if (parent == dir || dir == workTree.lexically_normal()) break;
    }
    return workTree / ".jasperengine" / "merge" / (relative.string() + ".conflicts.yaml");
}

int runMergeSceneCommand(int argc, char** argv, const char* executableName) {
    if (argc != 5) {
        std::cerr << "Usage: " << executableName << " merge-scene <base> <ours> <theirs> <path>\n"
                  << "A git merge driver for .scene and .prefab files (git's %O %A %B %P): the result goes to <ours>.\n"
                  << "Exit 0 merged, 1 conflicts (recorded in <project>/.jasperengine/merge/<path>.conflicts.yaml), 2 unreadable input.\n";
        return 2;
    }
    const fs::path basePath = argPath(argv[1]);
    const fs::path oursPath = argPath(argv[2]);
    const fs::path theirsPath = argPath(argv[3]);
    const std::string path = argv[4];
    std::string base, ours, theirs;
    if (!readText(basePath, base) || !readText(oursPath, ours) || !readText(theirsPath, theirs)) {
        std::cerr << "merge-scene: could not read the three versions of " << path << "\n";
        return 2;
    }
    const MergeResult result = mergeSceneTexts(base, ours, theirs);
    if (!result.ok) {
        std::cerr << "merge-scene: " << path << ": " << result.error << " (left for git's text merge)\n";
        return 2;
    }
    io::AtomicWriteOptions options;
    options.durability = io::Durability::Document;
    const io::WriteResult written = io::writeFileAtomic(oursPath, result.text, options);
    if (!written.ok()) {
        std::cerr << "merge-scene: could not write the result: " << io::describeWriteError(written) << "\n";
        return 2;
    }
    std::error_code ec;
    const fs::path conflicts = conflictsFileFor(fs::current_path(ec), path);
    if (result.conflicts.empty()) {
        fs::remove(conflicts, ec);
        if (!result.renumbered.empty()) {
            std::cerr << "merge-scene: " << path << ": " << result.renumbered.size() << " GameObject(s) of theirs renumbered\n";
        }
        return 0;
    }
    const io::WriteResult recorded = io::writeFileAtomic(conflicts, conflictsText(path, result.conflicts), options);
    std::cerr << "merge-scene: " << path << ": " << result.conflicts.size() << " conflict(s), ours kept for them";
    if (recorded.ok()) std::cerr << " (" << conflicts.string() << ")";
    std::cerr << "\n";
    for (const MergeConflict& conflict : result.conflicts) {
        std::cerr << "  " << conflict.name << " " << (conflict.path.empty() ? std::string("(GameObject)") : conflict.path) << ": "
                  << conflict.reason << "\n";
    }
    return 1;
}

int runSceneDiffCommand(int argc, char** argv, const char* executableName) {
    bool json = false;
    std::vector<fs::path> files;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--json") json = true;
        else files.push_back(argPath(argv[i]));
    }
    if (files.size() != 2) {
        std::cerr << "Usage: " << executableName << " scene-diff <old> <new> [--json]\n"
                  << "The semantic diff of two .scene or .prefab files: GameObjects, components and properties\n"
                  << "(as `git difftool -x \"" << executableName << " scene-diff\"`).\n";
        return 2;
    }
    SceneYamlModel models[2];
    for (int side = 0; side < 2; side++) {
        std::string text, error;
        // /dev/null (an added or deleted file): an empty document
        if (!readText(files[side], text)) {
            std::cerr << "scene-diff: could not read " << files[side].string() << "\n";
            return 2;
        }
        if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
            models[side].objects.emplace(kSceneSettingsId, SceneObject{kSceneSettingsId, "Scene Settings", 0, 0, "Entity", true, {}});
            continue;
        }
        if (!SceneYamlModel::parse(text, models[side], &error)) {
            std::cerr << "scene-diff: " << files[side].string() << ": " << error << "\n";
            return 2;
        }
    }
    const SceneDiffResult diff = diffScenes(models[0], models[1]);
    if (json) std::cout << sceneDiffJson(diff).dump(2) << "\n";
    else std::cout << sceneDiffText(diff);
    return 0;
}

}
