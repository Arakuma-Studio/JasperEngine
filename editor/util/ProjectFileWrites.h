// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/type/WriteFilesCmd.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // Files the editor writes into its project as one undo step, and what the editor refreshes
    // after files changed on disk. The editor's own, with or without the MCP server: the Sprite
    // Normal Map window's Save (its Save & Assign, paint Save and quick jobs), the Texture
    // Synthesis window's Save and the sprite normals watcher's regeneration write through it.
    // The MCP file tools build on it (mcp::w4::writeFilesUndoable runs writeUndoable with
    // mcp::w4::syncFiles as its after-change; syncFiles runs refresh() and adds its own
    // bookkeeping: the code editor buffers, script properties, HTML UI documents, problems and
    // the auto-sync snapshot).
    //
    // Main thread only.
    class ProjectFileWrites {
    public:
        struct Refreshed {
            int texturesReloaded = 0;
            bool shadersInvalidated = false;
        };

        // The editor's refresh after files changed on disk (`changed`: absolute paths; written,
        // removed or restored): the pose, clip and .vat documents of changed files reload; the
        // linked materials refresh (a changed .material, or `force`); the custom shader forks are
        // invalidated (a changed shader, or `force`); the textures of every open scene and prefab
        // stage reading a changed image are released to load again (properties, materials,
        // shader library textures, post-processing images and LUT); the asset database
        // refreshes; the Project window re-reads its listing and the changed files' thumbnails;
        // the sprite normals watcher reads the changed images again soon.
        static Refreshed refresh(Project* project, const std::vector<std::filesystem::path>& changed, bool force = false);

        // After the editor itself changed files (a write, its undo or its redo): the sync hook
        // when it takes them (the MCP server's file sync while it runs on this project: the same
        // refresh, and its snapshot keeps the editor's own writes from being reported as external
        // changes), else refresh().
        static void filesWritten(Project* project, const std::vector<std::filesystem::path>& changed);

        // Installed by the MCP server when it starts: true when it synced the files (then
        // refresh() is not run again), false to leave them to refresh() (the server stopped, or
        // another project)
        using SyncHook = std::function<bool(Project* project, const std::vector<std::filesystem::path>& changed)>;
        static void setSyncHook(SyncHook hook);

        // Asked before every execute (the first one and each redo) and undo of a write: false with
        // the reason refuses that step (nothing written or removed; a refused redo or undo says so
        // in the console, with the fix "remove the link, then undo / redo again"). A refused redo
        // or undo is not taken (canRedo / canUndo: the history keeps its place, and
        // CommandHistory::lastRefusal has the reason for the MCP undo tool and the Edit menu).
        // The sprite normal maps' containment check.
        using Guard = std::function<bool(std::string& why)>;

        // A WriteFilesCmd on the project's command history: one undo step, all or nothing (a
        // failed write changes nothing and adds no step). After its execute, undo and redo it runs
        // `afterChange` with the paths (default: filesWritten). False + `error` without a
        // project, when an item is a directory, when `guard` refuses, or when the write failed.
        // Every write, removal and restore stays beneath `beneath` when it happens
        // (WriteFilesCmd::writeBeneath, util/SafeWrite.h): a path that resolves outside it is
        // refused, a link planted on the way since is never followed. Without `beneath`: beneath
        // the write scope the step is made in (an MCP call: the project directory), else, with
        // `ownFolders`, each file beneath its own folder once resolved (no new refusal; still no
        // link planted below it followed), else plain writes. `stepName` names the undo step
        // ("Create Lua Script Goblin.lua"; empty: WriteFilesCmd's own name).
        static bool writeUndoable(Project* project, std::vector<WriteFilesCmd::Item> items, std::string& error,
                                  std::function<void(const std::vector<std::filesystem::path>&)> afterChange = nullptr,
                                  Guard guard = nullptr, std::filesystem::path beneath = {}, bool ownFolders = false,
                                  const std::string& stepName = {});
    };

}
