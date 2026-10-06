// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Helpers shared by the W4 tool modules (ScriptTools.cpp, AssetTools.cpp, DocsTools.cpp):
// file IO, project-history file writes, and making the editor notice changed files.
#pragma once

#include "mcp/McpCall.h"
#include "mcp/McpJson.h"

#include "command/Command.h"

#include <filesystem>
#include <functional>
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

        bool readFile(const fs::path& path, std::string& content);
        bool writeFile(const fs::path& path, const std::string& content);
        // Project-relative generic path ("assets/a.png"); the absolute path when outside the project
        std::string projectRelative(Project* project, const fs::path& absolute);
        // texture, model, audio, font, script, scene, prefab, material, shader, folder or other
        std::string assetType(const fs::path& path);
        // Lowercase extension with the dot
        std::string extensionOf(const fs::path& path);
        // Hidden entries, build dirs and the editor's internal dir, skipped by listings and scans
        bool isSkippedDir(const fs::path& dir);
        // A text file the tools may read/write (scripts, shaders, materials, configs, docs)
        bool isTextFile(const fs::path& path);
        // Files only editor tools may write: project.yaml, scenes, prefabs, generated build files
        bool isEditorOwnedFile(Project* project, const fs::path& absolute, std::string& why);

        // Writes (creates or replaces) files as one step of the project command history; undo
        // puts back the previous contents (or deletes files that did not exist, and the
        // directories it created). After execute and undo it runs `afterChange` with the paths.
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
            const std::string& error() const { return lastError; }

        private:
            struct Previous {
                bool existed = false;
                std::string content;
            };
            std::vector<Item> items;
            std::vector<Previous> previous;
            std::vector<fs::path> createdDirs;
            bool captured = false;
            std::string lastError;
            std::function<void(const std::vector<fs::path>&)> afterChange;
            std::vector<fs::path> paths() const;
        };

        // Runs a WriteFilesCmd on the project history (one undo step). False + error on failure.
        bool writeFilesUndoable(McpContext& ctx, std::vector<WriteFilesCmd::Item> items, std::string& error);

        // What the editor must do after files changed on disk (written by a tool or by an
        // external agent): reload code editor buffers, re-parse script properties of the
        // GameObjects using changed scripts, refresh linked materials, invalidate shader forks,
        // reload changed textures, refresh the asset database and the Project window.
        // `changed` = absolute paths known to have changed (may be empty); `full` also re-parses
        // every attached script and scans the project for changes since the last sync.
        // Returns {changed, code_editor, script_properties, problems, notes}.
        Json syncFiles(McpContext& ctx, const std::vector<fs::path>& changed, bool full);
        // The project's files as syncFiles' next full sync compares them: taken when the project is
        // opened (McpAutoSync's baseline), so edits made before the first sync are listed by it
        void takeSyncSnapshot(Project* project, const std::set<std::string>& changedAlready = {});

        // Lua syntax check without running the chunk (luaL_loadbuffer). line = 0 when ok.
        struct LuaSyntax {
            bool ok = true;
            int line = 0;
            std::string message;
        };
        LuaSyntax checkLuaSyntax(const std::string& code, const std::string& chunkName);

        // Shader forks (customShader values) a set of shader source files belongs to, with the
        // shader type they are used as (from the components / scene settings using them).
        struct ShaderUse {
            std::string base;        // customShader value
            int shaderType = -1;     // ShaderType, -1 when unknown
        };
        std::vector<ShaderUse> shaderForksUsingFiles(Project* project, const std::vector<std::string>& relativeFiles);
        // Compiles each fork now on a worker thread; calls `done` on the main thread with
        // [{shader, type, ok, errors:[{line, message}], log}].
        void checkShadersAsync(const CallPtr& call, std::vector<ShaderUse> forks, std::function<void(Json results)> done);

    }

}
