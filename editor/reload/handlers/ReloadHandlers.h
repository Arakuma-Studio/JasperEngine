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
    struct SceneProject;
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
    // .aseprite/.ase: imported again on a worker; the sheet, pages, clips and previews swap in
    // once the new pages are loaded (S44 R22)
    void reloadAseprite(Context& ctx, const std::vector<std::filesystem::path>& files);
    // gltf/glb/obj/fbx: meshes, skins, morphs, animations of an unchanged hierarchy
    void reloadModels(Context& ctx, const std::vector<std::filesystem::path>& files);
    // .particles effects: read again; every Particle System playing one rebinds (S20 R1)
    void reloadParticleEffects(Context& ctx, const std::vector<std::filesystem::path>& files);
    // .voxelvolume files another program changed: the volumes read them again (T6 R77)
    void reloadVoxelVolumes(Context& ctx, const std::vector<std::filesystem::path>& files);
    // What a .particles file says wrong as it is now: errors and unknown fields
    void addParticleEffectFileProblems(Project& project, const std::filesystem::path& path, Json& problems);
    // graph files: another program's checked (their problems reported); open documents (the Graph
    // Editor's) reload for those and for `documents` (the editor's own writes: write_file; removed
    // files, whoever removed them); a full reload has every open document look at its file
    void reloadGraphs(Context& ctx, const std::vector<std::filesystem::path>& external, const std::vector<std::filesystem::path>& documents);
    void addGraphFileProblems(Project& project, const std::filesystem::path& path, Json& problems);
    // .palette files (S43 R3): Palette Swap, Palette Quantize and the ramp strips made from them
    void reloadPalettes(Context& ctx, const std::vector<std::filesystem::path>& files);
    // Audio: a new sample for the next plays
    void reloadAudio(Context& ctx, const std::vector<std::filesystem::path>& files);
    // Fonts: a new pool entry; the texts using it rebuild their atlas
    void reloadFonts(Context& ctx, const std::vector<std::filesystem::path>& files);
    // bitmap fonts (S51 AC23): a changed .bitmapfont, .fnt or page PNG drops the pooled fonts that
    // read it; the Texts using them load again on their next update
    void reloadBitmapFonts(Context& ctx, const std::vector<std::filesystem::path>& files);
    // .poseanim, .humanoid, .anim and .vat files (and models, for their sub-clips and a .vat's
    // mesh): the open pose, clip and Vertex Animation documents reload, the clip and Vertex
    // Animation pools drop what the players and Vertex Animators use; the load errors and warnings
    // are problems (`all`: every open document checks its file, a full sync)
    void reloadAnimations(Context& ctx, const std::vector<std::filesystem::path>& files,
                          const std::vector<std::filesystem::path>& models, bool all);
    // The load error or warnings of an .anim or .poseanim file as it is now (nothing for other
    // files): what reloadAnimations reports for it
    void addAnimationFileProblems(Project& project, const std::filesystem::path& path, Json& problems);
    // .titleart documents and the fonts, images and SVGs they read (`assets`: changed files of
    // any kind; the documents naming them by the dependency index re-render and rebake)
    void reloadTitleArt(Context& ctx, const std::vector<std::filesystem::path>& documents, const std::vector<std::filesystem::path>& assets);
    // Lua and C++ scripts: script properties, Lua syntax, lua-language-server stubs
    // (`all`: every attached script, a full sync)
    void reloadScripts(Context& ctx, const std::vector<std::filesystem::path>& files, bool all);
    // Lua scripts deleted or renamed away: their lua-language-server classes go (a class of a
    // script that is gone would keep its name valid for the language server)
    void removeScriptClasses(Context& ctx, const std::vector<std::filesystem::path>& gone);
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
