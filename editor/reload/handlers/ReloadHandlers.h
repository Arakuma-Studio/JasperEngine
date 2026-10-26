// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The reload of each kind (S05 R11), called by HotReloadService::apply. Editor-internal.

#pragma once

#include "reload/HotReloadService.h"

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class SceneProject;
}

namespace jasperengine::editor::reload {

    struct Context {
        HotReloadService& service;
        Project& project;
        const ApplyOptions& options;
        ApplyResult& result;
        bool playing = false;
    };

    // The kind apply() files a path under ("texture", "model", ... "" for none)
    std::string kindOf(const std::filesystem::path& path);
    // project-relative, '/' separators
    std::string rel(Project& project, const std::filesystem::path& path);
    // terrain_maps/terrain_edit_*.png: written from the editor's terrain, never reloaded
    bool isEditorOwnedFile(const std::string& relativePath);

    // Textures (images, .svg): a new pool entry with the same settings, swapped in (R13)
    void reloadTextures(Context& ctx, const std::vector<std::filesystem::path>& files);
    // .material: parsed on a worker, applied to every submesh linked to the file
    void reloadMaterials(Context& ctx, const std::vector<std::filesystem::path>& files);
    // Shader forks whose sources or includes are among `files`, compiled before they swap
    void reloadShaders(Context& ctx, const std::vector<std::filesystem::path>& files);
    // The problems of fork checks (ProjectChecks::toJson results): {path, line, message, code?}
    // at the file the compiler names, else the changed file of the fork (UI effect forms share
    // their errors: reported once)
    Json shaderProblems(Project* project, const Json& results, const std::vector<std::string>& changedFiles);
    // Whether a fork's entry points or #include closure hold one of `changed` (project- or
    // assets-relative); a fork whose sources no longer resolve counts as reached
    bool shaderSourcesReach(Project* project, const std::string& fork, int shaderType, const std::set<std::string>& changed);
    // gltf/glb/obj/fbx: meshes, skins, morphs, animations of an unchanged hierarchy
    void reloadModels(Context& ctx, const std::vector<std::filesystem::path>& files);
    // Audio: a new sample for the next plays
    void reloadAudio(Context& ctx, const std::vector<std::filesystem::path>& files);
    // Fonts: a new pool entry; the texts using it rebuild their atlas
    void reloadFonts(Context& ctx, const std::vector<std::filesystem::path>& files);
    // .poseanim, .humanoid and .anim files (and models, for their sub-clips): the open pose and
    // clip documents reload, the clip pool drops what the Animation Players use; the load errors
    // and warnings are problems (`all`: every open document checks its file, a full sync)
    void reloadAnimations(Context& ctx, const std::vector<std::filesystem::path>& files,
                          const std::vector<std::filesystem::path>& models, bool all);
    // Lua and C++ scripts: script properties, Lua syntax, lua-language-server stubs
    // (`all`: every attached script, a full sync)
    void reloadScripts(Context& ctx, const std::vector<std::filesystem::path>& files, bool all);
    // Whether the code editor has a buffer of `path`
    bool isOpenInCodeEditor(const std::filesystem::path& path);
    // Open code editor buffers of `files` (all buffers when `all`)
    void reloadCodeEditorBuffers(Context& ctx, const std::vector<std::filesystem::path>& files, bool all);
    // HTML UI documents reading a changed file (html/css, images, fonts, json/lottie, effects)
    // `changedSince`: files of `files` known to have changed since the documents read them
    // (another program's change the service decided to reload): their stamps are not trusted
    void reloadHtmlUi(Context& ctx, const std::vector<std::filesystem::path>& files,
                      const std::vector<std::filesystem::path>& changedSince = {});
    // The documents of every open scene reading `changed`: {reloaded, current, problems}
    Json syncHtmlUi(Project* project, const std::vector<std::filesystem::path>& changed,
                    const std::vector<std::filesystem::path>& changedSince = {});
    bool isHtmlUiFile(const std::string& path);

    // Added / removed / renamed files and changed prefabs: the asset database (prefab GUIDs
    // kept across renames), the loaded prefabs and the Project window (R14)
    void applyAssetChanges(Context& ctx, const std::vector<FileChange>& changes);
    // Open scenes, prefab stages and project.yaml changed by another program: their bar
    void noteOpenDocuments(Context& ctx, const std::vector<std::filesystem::path>& files, std::set<uint32_t>& scenes, bool& projectFile);

}
