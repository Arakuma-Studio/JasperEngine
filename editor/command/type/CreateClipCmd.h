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
//     the ones this command wrote; otherwise it is kept and the mismatch logged.
//   * the scene's modified flag is set on execute and put back on undo (like CreatePrefabCmd);
//     AnimationClipDocument::filesChanged runs for the files after execute, undo and redo.

#pragma once

#include "command/Command.h"

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

    private:
        struct Written{
            bool existed = false;
            std::string previous;
        };

        Project* project = nullptr;
        uint32_t sceneId = 0;
        std::vector<File> files;
        std::vector<Written> written;
        std::vector<std::unique_ptr<Command>> sceneCommands;
        bool wasModified = false;
        std::string failure;
    };

}
