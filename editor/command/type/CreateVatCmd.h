// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (vertex animation spec 4.4)
// - Purpose: the one scene-history step of a Vertex Animation bake that writes its files AND
//   places the result in a scene (place beside / replace / crowd). The CreateClipCmd pattern:
//   it writes the files itself (.vat, .vat.glb), then runs the scene
//   commands it builds on its first execute (the new GameObject's id is only known then; redo
//   recreates the same id): ModelLoadCmd of a new GameObject "<name> (VAT)" with Model = the
//   .vat.glb at the source's transform, AddComponentCmd + PropertyCmds of its Vertex Animator
//   (animation = the .vat, its GUID), for replace an EntityStateCmd disabling the source, for
//   crowd the crowd commands VatCrowd gives. The new GameObject is selected.
//   A bake that only writes files uses WriteFilesCmd on the project history instead.
// - Thread-safety: main thread.
// - Invariants:
//   * execute/redo writes every file (all or nothing: a failure puts back the ones written and
//     runs no scene command), then the scene commands in order; one failing undoes the ones
//     before it and the files, and execute returns false (getFailure says why). The command owns
//     every scene command it runs, a failed one included (deleted once, by it); crowd commands
//     that never ran are deleted too.
//   * undo undoes the scene commands in reverse order, then each file is deleted (it did not
//     exist) or given its previous bytes back, only when its bytes are still the ones written;
//     a changed file is kept (getKeptFiles, logged) and a redo leaves it as it is too.
//   * after execute, undo and redo: the asset database, documents and MCP sync learn the files
//     (AnimationClipDocument::filesWritten) and VertexAnimationPool drops the .vat (invalidate).
//   * the scene's modified flag is set on execute and put back on undo.

#pragma once

#include "command/Command.h"
#include "ecs/Entity.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor{

    class Project;

    // where and how the baked asset is placed
    struct VatPlacement{
        enum class Mode{ None, Beside, Replace, Crowd };
        Mode mode = Mode::None;
        std::string name;                   // the new GameObject's ("Hero (VAT)")
        Entity parent = NULL_ENTITY;        // its parent (the source's)
        Vector3 position = Vector3(0, 0, 0);
        Quaternion rotation;
        Vector3 scale = Vector3(1, 1, 1);
        std::string model;                  // the .vat.glb, asset-relative (the Model's filename)
        std::string animation;              // the .vat, asset-relative
        std::string guid;                   // the .vat's GUID
        Entity source = NULL_ENTITY;        // Replace: disabled
        // Crowd: the scene commands that make the placed GameObject a crowd (Instanced Mesh,
        // instances, Vertex Animator settings), built for the entity once it exists
        std::function<bool(Project* project, uint32_t sceneId, Entity entity, std::vector<Command*>& commands, std::string& error)> crowd;
    };

    class CreateVatCmd: public Command{
    public:
        struct File{
            std::filesystem::path path;     // absolute
            std::string content;
        };

        CreateVatCmd(Project* project, uint32_t sceneId, std::vector<File> files, VatPlacement placement);
        ~CreateVatCmd() override;

        CreateVatCmd(const CreateVatCmd&) = delete;
        CreateVatCmd& operator=(const CreateVatCmd&) = delete;

        bool execute() override;

        std::vector<Touched> touchedPaths(bool undo) const override;
        std::string failure() const override { return failureText; }
        void undo() override;
        bool affectsStructure() const override;

        const std::string& getFailure() const{ return failureText; }
        void reportFailureTo(std::shared_ptr<std::string> sink){ failureSink = std::move(sink); }
        // the placed GameObject (NULL_ENTITY before the first execute, or with Mode::None)
        Entity getEntity() const{ return entity; }
        const std::vector<std::filesystem::path>& getKeptFiles() const{ return keptFiles; }
        const std::vector<File>& getFiles() const{ return files; }

        // what every write of baked files is followed by: the editor learns the files, the pool
        // drops the .vat (also used after WriteFilesCmd steps)
        static void notifyFiles(Project* project, const std::vector<std::filesystem::path>& paths);

    private:
        struct Written{
            bool existed = false;
            std::string previous;
            bool done = false;
        };

        Project* project = nullptr;
        uint32_t sceneId = 0;
        std::vector<File> files;
        std::vector<Written> written;
        VatPlacement placement;
        std::vector<std::unique_ptr<Command>> sceneCommands;
        bool built = false;
        Entity entity = NULL_ENTITY;
        bool wasModified = false;
        std::vector<Entity> previousSelection;
        std::string failureText;
        std::shared_ptr<std::string> failureSink;
        bool executedOnce = false;
        std::vector<bool> existedFirst;
        std::vector<std::filesystem::path> createdDirs;
        std::vector<std::filesystem::path> keptFiles;

        bool writeFiles();
        void restoreFiles(size_t count, bool checkBytes);
        // first execute: the scene commands, executed as they are built
        bool buildAndRun();
        void notify();
        bool fail(const std::string& message);
    };

}
