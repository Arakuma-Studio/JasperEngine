// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Where vertex colors meet the rest of the editor (docs/specs/V1-vertex-colors.md R8-R10, R92): the
// scene's save, the frame's check for stale targets, the model cook's painted set, and the export.
// Main thread unless said.

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;
    struct SceneProject;

    // R8, R9: the scene's save writes the files its components show that are not on disk as they are,
    // with the editor-only snapshot of each target's mesh as it is now. False (reported) when one
    // could not be written: its values stay in memory and the next save tries again.
    bool saveSceneVertexColors(Project& project, SceneProject& sceneProject);
    // the file's values differ from the disk's (tests, the Inspector)
    bool vertexColorFileDirty(Project& project, const std::string& assetPath);
    // a project closing: queued writes are written, the live values forgotten
    void releaseVertexColorFiles();
    // the editor quitting: queued writes are written on this thread (the job system cancels a
    // pending drain at its shutdown). False when one could not be written
    bool flushVertexColorFiles();
    // values no component shows and the disk holds already go
    void forgetUnusedVertexColorFiles(Project& project);

    // R10, once a frame: a painted target whose mesh no longer matches its fingerprint (a reimport
    // changed the source's topology) is transferred from its snapshot; the scene becomes modified and
    // the issue is logged once (remapIssues lists it). Nothing to do without a component (R95).
    void tickVertexColors(Project* project);

    // R9, D11: the models (assets-relative) a GameObject of an open scene paints: their cook keeps
    // every primitive's vertex and triangle order and welds nothing
    std::set<std::string> livePaintedModels(Project& project);
    // the model of the file was just painted for the first time: its cooks follow
    void paintedModelChanged(Project& project, const std::string& modelAssetPath);

    // R92: with every scene of the build loaded, a target whose topology fingerprint differs from its
    // mesh's stops the export: one problem per target, naming the GameObject and the fix
    std::vector<std::string> vertexColorExportProblems(Project& project);
    bool isVertexColorsFile(const std::filesystem::path& path);
    // the file without its editor-only section (R92), written through AtomicFile. Any thread.
    bool cookVertexColorsFile(const std::filesystem::path& source, const std::filesystem::path& destination, std::string& error);

}
