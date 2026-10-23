// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Helpers shared by the W4 tool modules (ScriptTools.cpp, AssetTools.cpp, DocsTools.cpp):
// file IO, project-history file writes, and making the editor notice changed files.
#pragma once

#include "mcp/McpCall.h"
#include "mcp/McpJson.h"

#include "command/Command.h"
#include "command/type/WriteFilesCmd.h"
#include "util/ProjectChecks.h"

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

        // Writes files as one step of the project command history (editor/command/type)
        using WriteFilesCmd = ::jasperengine::editor::WriteFilesCmd;

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
        // HTML UI documents using changed files (.html/.htm/.css and the images, svg, fonts,
        // json/lottie they read). Every open scene, prefab stage and running Play scene:
        // UIDocumentSystem::reloadChangedFiles (shares its stamps with the editor's poller,
        // HtmlUIWatcher: a file is reloaded once, whoever sees it first). Changed .html/.htm/.css
        // that no live document depends on are checked with ProjectChecks::checkUiFiles. Returns
        // {reloaded: [{scene, entity: {id,name,path}, html, stylesheet_only}], current: <documents
        //  depending on a changed file the poller had already reloaded>,
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
