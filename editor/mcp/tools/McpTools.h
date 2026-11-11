// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "mcp/McpJson.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {
    class Scene;
}

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::texsynth {
    struct Image;
}

namespace jasperengine::editor::mcp {

    class McpRegistry;
    class McpContext;
    class McpCall;

    // A search result's next step when its query matches editor tools this session does not list
    // (a group it has not enabled): adds "editor_tools" [{tool, group, enable}] and "next" to data,
    // and the next step to text (DocsTools.cpp). The searches agents look in call it.
    void addUnlistedTools(const std::shared_ptr<McpCall>& call, const std::string& query, std::string& text, Json& data);

    // The `next` of a Light 2D made (create_entity) or edited (set_properties) in a scene whose
    // 2D ambient is bright (util/Ambient2D.h), naming the fix; "" otherwise (ComponentTools.cpp)
    std::string ambientLight2DNext(const Scene& scene);
    // Grid tilemaps in edit_tilemap and get_tilemap (S50, TilemapGridTools.cpp): true when the call
    // was a Grid, a Tilemap Layer or a new grid tilemap and has been answered
    namespace tilemapgridtools {
        bool editTilemapGrid(const std::shared_ptr<McpCall>& call);
        bool getTilemapGrid(const std::shared_ptr<McpCall>& call);
    }

    // Every tool, resource and prompt of the editor, in tools/list order (McpTools.cpp).
    // Side-effect free: needs no App and does no I/O, so the CLI builds the same registry
    // offline (`jasperengine-editor call --help`, list-tools) as the server does at start.
    void buildToolRegistry(McpRegistry& registry);

    // Tool registrations grouped by editor feature. A new one is also added to the module
    // table of buildToolRegistry (McpTools.cpp), which names its docs section and group.
    void registerCoreTools(McpRegistry& registry);
    void registerProjectTools(McpRegistry& registry);
    void registerSceneTools(McpRegistry& registry);
    void registerEntityTools(McpRegistry& registry);
    void registerComponentTools(McpRegistry& registry);
    void registerPrefabTools(McpRegistry& registry);
    void registerPlayTools(McpRegistry& registry);
    void registerViewTools(McpRegistry& registry);
    void registerSceneViewTools(McpRegistry& registry);   // S26
    void registerShadowTools(McpRegistry& registry);
    void registerConsoleTools(McpRegistry& registry);
    // DataTools.cpp: user_settings (the UserSettings / PlayerPrefs store)
    void registerDataTools(McpRegistry& registry);
    // AudioTools.cpp: audio_mixer (the project's Audio Mixer asset)
    void registerAudioTools(McpRegistry& registry);
    void registerAudioSessionTools(McpRegistry& registry);
    void registerSoundTools(McpRegistry& registry);
    // SoundEventTools.cpp: edit_sound_event (Sound Events, docs/specs/S37-sound-events.md)
    void registerSoundEventTools(McpRegistry& registry);
    // get_asset_info's block of a .soundevent
    nlohmann::json soundEventInfo(Project* project, const std::filesystem::path& file);
    void registerScriptTools(McpRegistry& registry);
    void registerAssetTools(McpRegistry& registry);
    void registerMaterialsLightingTools(McpRegistry& registry);
    void registerAssetIdentityTools(McpRegistry& registry);
    // S14: texture import settings and cooks (in AssetTools.cpp)
    void registerTextureTools(McpRegistry& registry);
    void registerMeshTools(McpRegistry& registry);
    void registerDocsTools(McpRegistry& registry);
    void registerBuildTools(McpRegistry& registry);
    void registerPublishTools(McpRegistry& registry);   // S30
    void registerWorldTools(McpRegistry& registry);
    // AnimatorTools.cpp: edit_animator (Animator Controllers, anim layers spec 11.5)
    void registerAnimatorTools(McpRegistry& registry);
    void registerLatticeTools(McpRegistry& registry);
    void registerHtmlUiTools(McpRegistry& registry);
    void registerUiEffectTools(McpRegistry& registry);
    void registerSynthesisTools(McpRegistry& registry);
    void registerPlacementTools(McpRegistry& registry);
    void registerVatTools(McpRegistry& registry);
    void registerAnimationQualityTools(McpRegistry& registry);   // S77: check_animation, optimize_animation
    void registerSpriteNormalsTools(McpRegistry& registry);
    // SpriteTools.cpp: the sprites group (S44's Aseprite import; S45 atlases, S47 drawing)
    void registerSpriteTools(McpRegistry& registry);
    void registerSpriteAtlasTools(McpRegistry& registry);
    void registerSpriteDrawTools(McpRegistry& registry);   // S47

    // PlacementTools.cpp: puts back the preview poses a place_with_physics screenshot shows for a
    // few frames (App::stopTransientPreviews: before a save or Play)
    void stopPlacementPreviews(Project* project);
    // control_animator's edit-mode previews end (the scene goes back): before a save or Play
    void stopAnimatorPreviews(Project* project);

    // SynthesisTools.cpp: the jasper.synthesis recipe of a PNG for get_asset_info / view_asset:
    // {role, mode, quality, seed, size, sources: [{role, path, state}], sources_changed,
    // sources_missing} plus, with full, the raw recipe and `regenerate` (synthesize_texture
    // arguments reproducing it); null when the file has no recipe
    Json synthesisAssetInfo(Project* project, const std::filesystem::path& file, bool full);
    // " Synthesized (tileable, balanced, seed 7, role primary); sources unchanged." or ""
    std::string synthesisAssetText(const Json& info);
    void registerEcsTools(McpRegistry& registry);
    void registerDiagnosticsTools(McpRegistry& registry);
    void registerAtmosphereTools(McpRegistry& registry);
    void registerProfilerTools(McpRegistry& registry);
    // S54: capture_model_lods (group meshes, shared with S53's model import tools)
    void registerMeshLodTools(McpRegistry& registry);
    void registerDeviceTools(McpRegistry& registry);
    void registerScriptingTools(McpRegistry& registry);
    void registerGraphTools(McpRegistry& registry);
    void registerTerrainGenerationTools(McpRegistry& registry);
    void registerProceduralMaterialTools(McpRegistry& registry);
    void registerGameplayTools(McpRegistry& registry);
    void registerSplineTools(McpRegistry& registry);
    void registerVoxelTools(McpRegistry& registry);
    void registerTerrainTools(McpRegistry& registry);
    // T4: edit_terrain_world, registered by registerTerrainTools (its module and group)
    void registerTerrainWorldTools(McpRegistry& registry);
    void registerParticleTools(McpRegistry& registry);
    void registerAnimatorTools(McpRegistry& registry);
    // S27: the editor tools (group editor_tools) and the Project Hub's core tools
    void registerEditorToolsTools(McpRegistry& registry);
    // S80: diff_images (editor_tools) and the title_art group
    void registerImageDiffTools(McpRegistry& registry);
    void registerTitleArtTools(McpRegistry& registry);
    void registerHubTools(McpRegistry& registry);

    void registerPoseTools(McpRegistry& registry);

    // VatTools.cpp (vertex animation spec 6): the .vat parts of the asset tools.
    // get_asset_info of a .vat: data.vertex_animation = {guid, mode, mesh, layout, bounds, clips,
    // sockets, stats, recipe, warnings, staleness, stale, rebake?, users}; `text` gets a summary.
    // False (data.error, data.fix) when it doesn't parse.
    bool vatAssetInfo(Project* project, const std::filesystem::path& file, Json& data, std::string& text);
    // a .titleart's part of get_asset_info (canvas, layer counts, fonts with missing ones, outputs
    // with freshness, problems) and what view_asset shows of it (its fresh main output, else a
    // render); false with `error`
    bool titleArtAssetInfo(Project* project, const std::filesystem::path& file, Json& data, std::string& text);
    // a baked PNG's Title Art recipe: {source, guid, output, status, frame, scale}; null when it has none
    Json titleArtBakedPngInfo(Project* project, const std::filesystem::path& file);
    bool titleArtViewImage(Project* project, const std::filesystem::path& file, int maxSize, texsynth::Image& out, std::string& label,
                           std::string& error);
    // the Vertex Animators of the open scenes and prefab stages using a .vat: [{scene, entity}]
    Json vatUsers(Project* project, const std::string& vatRelative);
    // write_file of a .vat: false + error when it doesn't parse, VatDocument::normalize refuses it,
    // or it changes a generated field of the existing file (VatDocument::generatedChange: the
    // rebake message); `warnings` the lenient reader's
    bool checkVatText(Project* project, const std::filesystem::path& file, const std::string& content,
                      std::vector<std::string>& warnings, std::string& error);

    // SpriteNormalsTools.cpp (sprite normals Phase 3, 3.9): the sprite normal map parts of the asset
    // tools. A PNG with a jasper.spritenormals recipe: {sprite_normals: {role, source: {path, kind,
    // entity?, state: fresh|changed|missing|invalid}, preset, frames: {from, count}, outputs, stale}}
    // plus, with full, the raw recipe and `regenerate` (generate_sprite_normals arguments writing it
    // again); a garbled recipe {sprite_normals: {error}}. Another image whose default-name normal map
    // (<stem>_normal.png, <stem>_sheet_normal.png) was made from it: {normal_map: {path, stale}}.
    // Null otherwise.
    Json spriteNormalsAssetInfo(Project* project, const std::filesystem::path& file, bool full);
    // " Sprite normal map (normal, preset default) of assets/hero.png; source unchanged.",
    // " Its normal map: assets/hero_normal.png (up to date)." or ""
    std::string spriteNormalsAssetText(const Json& info);
    // a changed image (file sync, auto-sync): the normal maps made from it (the default-name probe
    // and the check cache's index, no scan) whose source changed or went missing go to `problems`
    // ({path: <normal map>, message})
    void spriteNormalsFileChanged(Project* project, const std::filesystem::path& path, Json& problems);

    // the pose documents' files-written hook (their writes reach the asset database and the
    // Project window like any MCP file change): installed once, when the MCP context exists
    void installPoseSyncHook(McpContext& context);

    void registerOrganisationTools(McpRegistry& registry);
    void registerNavigationTools(McpRegistry& registry);
    void registerVcsTools(McpRegistry& registry);   // S29 (VcsTools.cpp)
    void registerTilesetTools(McpRegistry& registry);   // S50 (TilesetTools.cpp): edit_tileset, import_tilemap
    void registerPaletteTools(McpRegistry& registry);   // S43 (PaletteTools.cpp): edit_palette, apply_palette
    void registerPixelArtTools(McpRegistry& registry);   // S42 (PixelArtTools.cpp): check_pixel_art
    void registerPixelFxTools(McpRegistry& registry);   // S48 (PixelFxTools.cpp): capture_contact_sheet
    void registerPixelArtCleanupTools(McpRegistry& registry);   // S46 (PixelArtCleanupTools.cpp): fix_pixel_art, lint_pixel_art

    // list_assets search and saved searches (OrganisationTools.cpp): an R20 search over the asset
    // index, as {entries: [{path, type, guid?, labels?}], total, truncated, query}; *error for a bad re:
    Json searchAssetsJson(Project* project, const std::string& query, const std::string& folder, size_t limit, std::string& error);

}
