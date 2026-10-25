// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (animation clips spec 9.R4)
// - Purpose: the one scene-history step of every action that writes clip files AND changes a
//   scene: Create New Clip (Animation window, Inspector, empty state), a clip dropped onto an
//   object, Convert to Clip, Bake to Clip, Use Animation Player, Extract with "add to the
//   player", MCP edit_animation {clip, create: {from: entity}}. CreatePrefabCmd pattern: it
//   writes the files itself and runs the scene commands the caller built (AddComponentCmd,
//   PropertyCmd / MultiPropertyCmd on clips[], DeleteEntityCmd of converted tracks,
//   ModelLoadCmd for a layout reload...).
// - Owner: T0 (API); T2d implements it (T2b/T2c/T3b/T4 build it).
// - Thread-safety: main thread.
// - Invariants:
//   * execute/redo writes every file (all or nothing: a failure restores the files written so
//     far and runs no scene command), then executes the scene commands in order; a scene
//     command failing undoes the ones before it and the files, and execute returns false.
//   * undo undoes the scene commands in reverse order, then for each file: deleted (it did not
//     exist) or given its previous content back (it existed), but only when its bytes are still
//     the ones this command wrote; otherwise it is kept and the mismatch logged, and a redo then
//     leaves it as it is too (logged).
//   * the scene's modified flag is set on execute and put back on undo (like CreatePrefabCmd);
//     after execute, undo and redo the files go through AnimationClipDocument::filesChanged and
//     the same files-written hook as document commits (AnimationClipDocument::
//     setFilesWrittenHook: AssetDatabase GUID registration, Project window refresh, McpAutoSync
//     stamps).

#pragma once

#include "animation/PlayerModelLayout.h"
#include "command/Command.h"
#include "ecs/Entity.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor{

    class Project;

    class CreateClipCmd: public Command{
    public:
        struct File{
            std::filesystem::path path;     // absolute
            std::string content;
        };

        // takes ownership of `sceneCommands`
        CreateClipCmd(Project* project, uint32_t sceneId, std::vector<File> files, std::vector<Command*> sceneCommands);
        ~CreateClipCmd() override;

        CreateClipCmd(const CreateClipCmd&) = delete;
        CreateClipCmd& operator=(const CreateClipCmd&) = delete;

        bool execute() override;
        void undo() override;
        bool affectsStructure() const override;

        // why the last execute failed ("can't write <path>: <reason>", a scene command's)
        const std::string& getFailure() const { return failure; }
        // where execute() also writes its failure (the history deletes a command that fails)
        void reportFailureTo(std::shared_ptr<std::string> sink) { failureSink = std::move(sink); }

        // The entity whose Animation Player this step adds or uses: after the scene commands,
        // models on or under it still in the legacy layout are reloaded in the node hierarchy
        // (PlayerModelLayout::ensureNodeHierarchyForPlayer, R3), undone first on undo
        void setPlayerRoot(Entity root) { playerRoot = root; }
        // how many models the last execute reloaded in the node hierarchy
        size_t getReloadedModels() const { return layoutReloads.size(); }
        // the files left as they are because they changed since this command wrote them: by the
        // last undo (not taken back) or redo (not written over, and so not taken back either)
        const std::vector<std::filesystem::path>& getKeptFiles() const { return keptFiles; }
        const std::vector<File>& getFiles() const { return files; }

    private:
        struct Written{
            bool existed = false;
            std::string previous;
            bool done = false;          // written by the last execute
        };

        Project* project = nullptr;
        uint32_t sceneId = 0;
        std::vector<File> files;
        std::vector<Written> written;
        std::vector<std::unique_ptr<Command>> sceneCommands;
        bool wasModified = false;
        std::string failure;
        std::shared_ptr<std::string> failureSink;
        Entity playerRoot = NULL_ENTITY;
        PlayerModelLayout::Reloads layoutReloads;
        bool reloadsTaken = false;              // the first execute ran the helper
        std::vector<std::filesystem::path> createdDirs;
        std::vector<std::filesystem::path> keptFiles;

        // files [0, count) back to what was there before execute
        void restoreFiles(size_t count, bool checkBytes);
        void notifyFiles();
        bool fail(const std::string& message);
    };

}
