// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Helpers shared by the W4 tool modules (ScriptTools.cpp, AssetTools.cpp, DocsTools.cpp):
// file IO, project-history file writes, and making the editor notice changed files.
#pragma once

#include "mcp/McpCall.h"
#include "mcp/McpJson.h"

#include "command/Command.h"
#include "util/ProjectChecks.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::mcp {

    class McpContext;

    namespace w4 {

        namespace fs = std::filesystem;

        bool writeFile(const fs::path& path, const std::string& content);
        // Project-relative generic path ("assets/a.png"); the absolute path when outside the project
        std::string projectRelative(Project* project, const fs::path& absolute);
        // texture, model, audio, font, script, scene, prefab, material, shader, folder or other
        std::string assetType(const fs::path& path);
        // Lowercase extension with the dot
        std::string extensionOf(const fs::path& path);
        // A text file the tools may read/write (scripts, shaders, materials, configs, docs)
        bool isTextFile(const fs::path& path);
        // Files only editor tools may write: project.yaml, scenes, prefabs, generated build files
        bool isEditorOwnedFile(Project* project, const fs::path& absolute, std::string& why);

        // Writes (creates or replaces) files as one step of the project command history; undo
        // puts back the previous contents (or deletes files that did not exist, and the
        // directories it created). After execute and undo it runs `afterChange` with the paths.
        // All or nothing: when a file can't be written (a path component that is a file, a
        // read-only directory, a full disk), the files written before it get their previous
        // content back and what it created is removed, so a failed execute changes nothing
        // (CommandHistory::addCommand then drops it without an undo step); reportFailureTo()
        // receives which file failed and why.
        class WriteFilesCmd : public Command {
        public:
            struct Item {
                fs::path path;          // absolute
                std::string content;
            };
            WriteFilesCmd(std::vector<Item> items, std::function<void(const std::vector<fs::path>&)> afterChange);
            bool execute() override;
            void undo() override;
            bool affectsStructure() const override { return false; }
            // Where execute() writes its failure ("can't write <path>: <reason>; ..."): the
            // command is deleted by the history when it fails, the sink outlives it
            void reportFailureTo(std::shared_ptr<std::string> sink) { failureSink = std::move(sink); }

            // "Write File player.lua", "Create File player.lua", "Write 3 Files"
            std::string describe() const override;
            const char* typeName() const override { return "WriteFilesCmd"; }
            // Every file already held that content
            bool isNoOp() const override;
            size_t memoryBytes() const override;
            // Off-thread release: the destructor only frees file contents, paths and the
            // afterChange callback (it captures a context pointer); no editor/engine/GPU state (S24 R8).
            bool releaseOffThread() const override { return true; }

        private:
            struct Previous {
                bool existed = false;
                bool metaExisted = false;   // its S04 sidecar: one the asset database adds to a file it created goes on undo
                std::string content;
            };
            std::vector<Item> items;
            std::vector<Previous> previous;
            std::vector<fs::path> createdDirs;   // every directory execute() created
            bool captured = false;
            std::function<void(const std::vector<fs::path>&)> afterChange;
            std::shared_ptr<std::string> failureSink;
            std::vector<fs::path> paths() const;
            // items[i] back to what it was before execute (false when that failed)
            bool restore(size_t i);
            // the created directories that are empty, deepest first
            void removeCreatedDirs();
            void fail(const std::string& message);
        };

        // Runs a WriteFilesCmd on the project history (one undo step). False + error on failure.
        // alsoSync: files written before it (an import's downloads) that the same reload takes,
        // so the documents reading them all reload once
        bool writeFilesUndoable(McpContext& ctx, std::vector<WriteFilesCmd::Item> items, std::string& error,
                                std::vector<std::filesystem::path> alsoSync = {});

        // The MCP form of the hot reload (HotReloadService::apply, S05): what reads files that
        // changed on disk (written by a tool or by an external agent) follows them: code
        // editor buffers, script properties, linked materials, shader forks, textures, models,
        // audio, fonts, HTML UI documents, the asset database and the Project window.
        // `changed` = absolute paths known to have changed (may be empty); `full` also scans the
        // project for changes since the last sync; `force` reloads even what is current.
        // Returns {changed, code_editor, script_properties, problems, notes, reloaded, pending}:
        // `pending` are still reloading on a worker (HotReloadService::idleFor).
        struct SyncOptions {
            bool force = false;          // reload even what is current (sync_files paths)
            bool rebuildModels = false;  // sync_files rebuild_models
            bool external = false;       // changes another program made (Console lines); else a tool's own writes
            // the file watcher's journal change of a path (McpAutoSync's pick-up): what the
            // watcher reloaded for that change is current, whatever the stat says
            std::map<fs::path, uint64_t> sequences;
        };
        Json syncFiles(McpContext& ctx, const std::vector<fs::path>& changed, bool full, const SyncOptions& options = {});
        // HTML UI documents using changed files (.html/.htm/.css and the images, svg, fonts,
        // json/lottie they read). Every open scene, prefab stage and running Play scene:
        // UIDocumentSystem::reloadChangedFiles (its stamps make a change reload once, whoever
        // sees it first). Changed .html/.htm/.css that no live document depends on are checked
        // with ProjectChecks::checkUiFiles. Returns
        // {reloaded: [{scene, entity: {id,name,path}, html, stylesheet_only}], current: <documents
        //  depending on a changed file that were already reloaded>,
        //  problems: [{path, line, col, severity, code, message, suggestion}]}
        // problems: the diagnostics located in a changed file, from the documents' (re)load or the
        // check (Info dropped), and a live document whose file is gone. Paths project-relative.
        Json syncHtmlUi(McpContext& ctx, const std::vector<fs::path>& changed);
        // A file syncHtmlUi looks at: .html/.htm/.css, images (.svg included), fonts (.ttf/.otf),
        // .json/.lottie
        bool isHtmlUiFile(const std::string& path);
        // The project's files as syncFiles' next full sync compares them: taken when the project is
        // opened (McpAutoSync's baseline), so edits made before the first sync are listed by it
        void takeSyncSnapshot(Project* project, const std::set<std::string>& changedAlready = {});

        // Compiles each fork now on a worker thread; calls `done` on the main thread with
        // [{shader, type, ok, errors:[{line, message}], log}].
        void checkShadersAsync(const CallPtr& call, std::vector<ProjectChecks::ShaderUse> forks, std::function<void(Json results)> done);

    }

}
