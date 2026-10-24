// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Additions, removals and renames (S05 R14): the asset database follows them (a renamed
// asset keeps its GUID and records a redirect, S04's refresh), the loaded prefabs re-read
// edited files, and the Project window lists the touched folder again. While a scene plays
// the database waits for Play to stop. Open scenes, prefab stages and project.yaml changed
// by another program are never reloaded on their own: the Scene view shows a bar (Reload /
// Keep Mine, D5).

#include "ReloadHandlers.h"

#include "EditorHost.h"
#include "Project.h"
#include "util/Util.h"
#include "window/ProjectWindow.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

void reload::applyAssetChanges(Context& ctx, const std::vector<FileChange>& changes) {
    std::vector<FileChange> external;
    for (const FileChange& change : changes) {
        // the editor's own writes are in its memory already
        if (change.origin == ChangeOrigin::External) external.push_back(change);
    }
    if (ctx.options.full) {
        if (ctx.playing) ctx.service.refreshAssetsAfterPlay();
        else ctx.project.refreshAssetDatabase(true);
    } else if (!external.empty()) {
        // an MCP call's pick-up answers from the database next: it looks at the disk now
        if (!ctx.project.applyFileChanges(external, ctx.options.fromMcp)) ctx.service.refreshAssetsAfterPlay();
        for (const FileChange& change : external) {
            if (change.kind == FileChangeKind::Modified && !change.directory && Util::isPrefabFile(watch::pathToUtf8(change.path))) {
                ctx.service.recordDone("prefab", change.path, 0.0);
            }
        }
    }

    ProjectWindow* window = getEditorHost().getProjectWindow();
    if (!window) return;
    if (ctx.options.full) {
        window->refreshCurrentDirectory();
        for (const FileChange& change : changes) {
            if (change.kind != FileChangeKind::Removed) window->notifyResourceFileChanged(change.path);
        }
        return;
    }
    if (!changes.empty()) window->onFilesChanged(changes);
}

void reload::noteOpenDocuments(Context& ctx, const std::vector<fs::path>& files, std::set<uint32_t>& scenes, bool& projectFile) {
    for (const fs::path& path : files) {
        const std::string relative = rel(ctx.project, path);
        if (relative == "project.yaml") {
            projectFile = true;
            ctx.result.notes.push_back("project.yaml changed on disk: the editor keeps its settings (the Scene view offers Reload / Keep "
                                       "Mine) and writes them back on the next save. Edit settings with the project tools.");
            continue;
        }
        for (SceneProject* sceneProject : ctx.project.getScenesAndPrefabStages()) {
            if (!sceneProject->scene || sceneProject->filepath.empty()) continue;
            if (rel(ctx.project, sceneProject->filepath) != relative) continue;
            scenes.insert(sceneProject->id);
            ctx.result.notes.push_back(relative + " changed on disk but the editor has it open: its copy wins and is written back on "
                                       "the next save unless the Scene view's Reload takes the file. Edit scenes and prefabs with the "
                                       "editor tools instead.");
        }
    }
}
