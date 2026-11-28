// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Build Settings > Build Profiles (docs/specs/build-profiles.md R19): the list under the Platform
// list (Add Build Profile, and Duplicate, Rename, Delete, Switch Profile on a row) and the selected
// profile's pane. Every edit is one project undo step (EditBuildProfileCmd), merged while typing.

#include "buildprofile/BuildProfile.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor {

    class Project;

    class BuildProfilesPane {
    public:
        void reset();
        // under the Platform list; a click selects a profile (selecting a platform clears it)
        void drawList(Project* project, float height);
        // the selected profile's pane; true when it can be built now
        bool drawPane(Project* project);
        bool hasSelection() const { return !selected.empty(); }
        const std::string& selectedName() const { return selected; }
        void clearSelection() { selected.clear(); }
        void selectName(const std::string& name) { selected = name; }
        // where a build of the selected profile goes (its Output Folder, else Build Settings' folder
        // for its platform)
        std::filesystem::path outputDir(Project* project) const;
        // the selected profile's warning this frame (a Store Title over its store's limit), "" none
        const std::string& agentNotice() const { return notice; }

    private:
        std::string selected;
        // Add Build Profile
        char newName[65] = {};
        int newPlatform = 1;   // linux
        // Rename / Delete on a row
        std::string renaming;
        char renameBuffer[65] = {};
        std::string confirmDelete;
        // the pane's text fields, read again when the file changes (unless one is being typed in)
        uint64_t loadedRevision = 0;
        std::string loadedName;
        BuildProfile working;
        bool fileChangedWhileTyping = false;
        char productName[256] = {};
        char identifier[256] = {};
        char versionSuffix[64] = {};
        char icon[512] = {};
        char storeTitle[256] = {};
        char outputFolder[1024] = {};
        char newDefine[65] = {};
        char newPath[512] = {};
        char overrideSearch[128] = {};
        std::string message;
        std::string notice;

        void load(const BuildProfile& profile);
        // one undo step (merged with the step before for the same `mergeKey` while typing)
        bool commit(Project* project, const BuildProfile& next, const std::string& mergeKey);
        bool writeFiles(Project* project, const std::string& step, std::vector<std::pair<std::filesystem::path, std::string>> writes,
                        std::vector<std::filesystem::path> removes, const std::string& mergeKey = std::string(),
                        const std::optional<std::pair<std::string, std::string>>& activeNameChange = std::nullopt);
        void drawIdentity(Project* project, BuildProfile& next, bool& changed, std::string& key);
        void drawStore(BuildProfile& next, bool& changed, std::string& key);
        void drawBuild(Project* project, BuildProfile& next, bool& changed, std::string& key);
        void drawSceneList(Project* project, BuildProfile& next, bool& changed, std::string& key);
        void drawDefines(BuildProfile& next, bool& changed, std::string& key);
        void drawContent(Project* project, BuildProfile& next, bool& changed, std::string& key);
        void drawOverrides(Project* project, BuildProfile& next, bool& changed, std::string& key);
    };

}
