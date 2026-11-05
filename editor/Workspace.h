// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "render/SceneDisplaySettings.h"
#include "Project.h"
#include "window/GameResolutions.h"

#include "yaml-cpp/yaml.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor {

// One developer's view of a project: open tabs and their order, each scene's viewport
// camera and guides, the terrain brush and the Physics Placement settings. None of it
// describes the game, so keeping it in project.yaml means a diff every time anyone opens
// the editor and a conflict every time two people do. It lives in .jasperengine/user/
// instead, which is not committed.
//
// Projects written before that split still carry these keys in project.yaml;
// adoptLegacyProjectState() takes over whatever the workspace does not already define.
class Workspace {
public:
    struct SceneState {
        SceneDisplaySettings displaySettings;
        YAML::Node editorCamera;
        bool hasDisplaySettings = false;
        // the Scene view's camera settings, layout, bookmarks, measures and filters (S26), and
        // the entry's keys this version does not know
        SceneViewState viewState;
    };

    // .jasperengine/user, created on demand
    static std::filesystem::path getUserDirectory(const std::filesystem::path& projectPath);

    static bool save(const Project* project);

    // A missing file leaves the workspace empty, which is what a fresh clone starts from
    void load(const std::filesystem::path& projectPath);

    // Fills in only what load() did not find, so the workspace always wins over the
    // stale copy in project.yaml
    void adoptLegacyProjectState(const YAML::Node& projectNode);
    bool hasAdoptedLegacyState() const { return adoptedLegacy; }

    // Entries for scenes the project lists but could not load, encoded as they were
    // read. Without them a checkout missing those files would save the workspace and
    // discard the settings of whoever has them.
    std::map<std::string, YAML::Node> statesForUnresolvedScenes(const Project* project) const;

    bool hasTabs() const { return tabsDefined; }
    const std::vector<TabEntry>& getTabs() const { return tabs; }

    bool hasSelectedScene() const { return selectedSceneDefined; }
    uint32_t getSelectedScene() const { return selectedScene; }

    bool hasTerrainEditorSettings() const { return terrainDefined; }
    const TerrainEditorSettings& getTerrainEditorSettings() const { return terrainEditorSettings; }

    // Defaults when the file has none (it never was in project.yaml)
    const PhysicsPlacementSettings& getPhysicsPlacementSettings() const { return physicsPlacementSettings; }
    const SpriteNormalsWindowSettings& getSpriteNormalsSettings() const { return spriteNormalsSettings; }

    bool isSimulateTouch() const { return simulateTouch; }
    // the Game view's "Add custom..." resolutions
    const std::vector<GameResolution>& getCustomResolutions() const { return customResolutions; }

    // Keyed by project-relative path, so reordering scenes never rewrites this file
    const SceneState* getSceneState(const std::filesystem::path& sceneFilepath) const;

    // A scene's workspace entry (tests: what save() writes and load() reads back)
    static YAML::Node encodeSceneEntry(const SceneProject& sceneProject);
    static SceneState decodeSceneEntry(const YAML::Node& node);

private:
    static std::filesystem::path getWorkspaceFile(const std::filesystem::path& projectPath);
    static std::string sceneKey(const std::filesystem::path& sceneFilepath);

    void clear();

    std::vector<TabEntry> tabs;
    bool tabsDefined = false;

    uint32_t selectedScene = NULL_PROJECT_SCENE;
    bool selectedSceneDefined = false;

    TerrainEditorSettings terrainEditorSettings;
    bool terrainDefined = false;

    PhysicsPlacementSettings physicsPlacementSettings;
    SpriteNormalsWindowSettings spriteNormalsSettings;

    bool simulateTouch = false;
    std::vector<GameResolution> customResolutions;

    std::map<std::string, SceneState> sceneStates;
    bool adoptedLegacy = false;
};

} // namespace jasperengine::editor
