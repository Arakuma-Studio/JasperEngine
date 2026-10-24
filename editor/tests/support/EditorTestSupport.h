// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Shared by the editor unit tests: the engine initialised once without a GPU, fixture paths,
// and scene documents decoded and encoded without a Project (no scene render)

#include "Project.h"
#include "LoadReport.h"

#include "yaml-cpp/yaml.h"

#include <filesystem>
#include <memory>
#include <string>

namespace jasperengine::editor::test {

    // the engine is running (TestMain started it with the support library's TestSystem)
    void ensureEngine();

    // editor/tests/fixtures/<relative>
    std::filesystem::path fixture(const std::string& relative);

    std::string readText(const std::filesystem::path& path);

    // The factor wall-clock budgets are enforced with: JASPER_TEST_PERF_SLACK, set by the
    // benchmark run on the benchmark server (docs/testing.md). 0 when it is not set, and under a
    // sanitizer: the budgets are only printed, since a shared builder's load alone can miss them.
    double perfSlack();

    // A scene file decoded the way Project::loadScene decodes it (report installed), into a
    // SceneProject of its own
    struct DecodedScene {
        std::unique_ptr<SceneProject> scene;
        LoadReport report;
        std::string sha1;

        DecodedScene() = default;
        DecodedScene(DecodedScene&&) = default;
        DecodedScene& operator=(DecodedScene&&) = default;
        ~DecodedScene();
    };
    DecodedScene decodeSceneFile(const std::filesystem::path& path);
    DecodedScene decodeSceneText(const std::string& text, const std::string& name = "test.scene");
    // What saving it would write
    YAML::Node encodeScene(const DecodedScene& decoded);
    std::string encodeSceneText(const DecodedScene& decoded);

    // Maps by keys (any order), sequences in order, scalars by text; why the first difference
    // when not equal
    bool structurallyEqual(const YAML::Node& a, const YAML::Node& b, std::string* why = nullptr);
    // A copy of a map without one key
    YAML::Node withoutKey(const YAML::Node& map, const std::string& key);

}
