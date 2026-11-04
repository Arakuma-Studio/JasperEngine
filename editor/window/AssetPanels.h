// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The asset identity panels (S04): Window > Missing Assets (every unresolved reference of the
// project, the references resolved by path, the orphan metas, with Retarget / Clear /
// Locate), and the References panel the Project window's Find References opens.

#pragma once

#include "asset/AssetIndex.h"
#include "asset/MissingAssetTracker.h"

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    class MissingAssetsWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Missing Assets";

        explicit MissingAssetsWindow(Project* project);
        void show();
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }

    private:
        Project* project;
        bool windowOpen = false;
        bool focusRequested = false;
        // referrers chosen for Retarget Selected / Clear Selected: "<owner>\n<locator>\n<ordinal>"
        std::set<std::string> selected;
        // the asset picker of a Retarget
        bool pickerOpen = false;
        AssetType pickerType = AssetType::Unknown;
        std::string pickerFilter;
       std::string pickerMiss;      // an agent's path that is no asset of the list
        std::function<void(const AssetDatabase::AssetInfo&)> pickerDone;

        void drawGroup(const MissingAssetTracker::Group& group, size_t index);
        void drawPicker();
        void openPicker(AssetType type, std::function<void(const AssetDatabase::AssetInfo&)> done);
    };

    class AssetReferencesWindow {
    public:
        static constexpr const char* WINDOW_NAME = "References";

        explicit AssetReferencesWindow(Project* project);
        void show();
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }
        // Find References of a file of the project (an absolute or project-relative path)
        void findReferences(const std::string& path);
        // What the panel lists (editor_status reports it)
        const std::string& getTargetPath() const { return targetPath; }
        const Guid& getTargetGuid() const { return targetGuid; }
        const std::vector<DepEdge>& getResults() const { return results; }

    private:
        Project* project;
        bool windowOpen = false;
        bool focusRequested = false;
        std::string targetPath;
        Guid targetGuid;
        bool transitive = false;
        std::vector<DepEdge> results;
        uint64_t resultsVersion = UINT64_MAX;
        bool resultsTransitive = false;

        void refresh();
    };

    // Locate: opens a referrer (a scene, a prefab in Prefab Mode, a material in the Project
    // window) and selects the GameObject when there is one
    void locateAssetReferrer(Project* project, const std::string& ownerPath, uint64_t entity);

}
