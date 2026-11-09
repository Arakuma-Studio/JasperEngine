// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The registry every MCP front-end shares: the server at start, and the CLI offline
#include "McpTools.h"
#include "FontTools.h"

#include "mcp/McpCall.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpToolGroups.h"

#include <string>

namespace jasperengine::editor::mcp {

namespace {

struct ToolModule {
    void (*registerTools)(McpRegistry&);
    const char* module;   // the docs/mcp/TOOLS.md section of its tools
    const char* group;    // the group of its tools that set none ("" = core)
};

// Fixed order: tools/list and docs/mcp/TOOLS.md follow it. A spec's new module is one row,
// with its group (docs/mcp/conventions.md); a tool may still set its own ToolDef::group.
const ToolModule TOOL_MODULES[] = {
    {registerCoreTools, "Core", ""},
    {registerProjectTools, "Project", ""},
    {registerHubTools, "Project", ""},   // S27: templates and imports
    {registerSceneTools, "Scenes", ""},
    {registerEntityTools, "GameObjects and selection", ""},
    {registerComponentTools, "Components and properties", ""},
    {registerPrefabTools, "Prefabs", ""},
    {registerPlayTools, "Play mode and input", ""},
    {registerViewTools, "Views and screenshots", ""},
    {registerSceneViewTools, "Scene view: picking, bookmarks, layouts, measures, filters", "editor_ux"},   // S26
    {registerShadowTools, "Shadows", "rendering_fx"},   // S10
    {registerConsoleTools, "Console", ""},
    {registerDataTools, "Saved data (PlayerPrefs)", "scripting"},   // upstream (script-data: UserSettings / PlayerPrefs)
    {registerSoundEventTools, "Sound Events", "audio"},
    {registerAudioTools, "Audio Mixer", "audio"},   // upstream (audio-mixer): out of core to fit the budget (S31)
    {registerSoundTools, "Sound files", "audio"},   // S36
    {registerAudioSessionTools, "Audio captures", "audio"},   // S35
    {registerScriptTools, "Scripts", ""},
    {registerAssetTools, "Assets, materials and shaders", ""},
    {registerSynthesisTools, "Texture synthesis", "assets_pipeline"},   // upstream (texture synthesis)
    {registerSpriteNormalsTools, "Sprite normal maps", "assets_pipeline"},   // upstream (sprite normals)
    {registerMaterialsLightingTools, "Assets, materials and shaders", "rendering_fx"},   // S11
    {registerAssetIdentityTools, "Asset identity: GUIDs, references, missing assets", "assets_pipeline"},   // S04
    {registerTextureTools, "Textures: import settings and cooks", "assets_pipeline"},   // S14
    {registerMeshTools, "Meshes: model import, cooks and render budgets", "meshes"},   // S53
    {registerDocsTools, "Docs", ""},
    {registerBuildTools, "Build and export", ""},
    {registerPublishTools, "Shipping: player runtimes and releases", "shipping"},   // S30
    {registerWorldTools, "World: terrain, tilemaps, sprites, animation, particles, physics, UI, models", ""},
    {registerLatticeTools, "Lattices (free-form deformation)", "animation"},   // upstream (lattice deformer)
    {registerPoseTools, "Pose animation", "animation"},   // upstream (Pose Animation)
    {registerVatTools, "Vertex animation", "animation"},   // upstream (Vertex Animation Textures)
    {registerAnimationQualityTools, "Clip quality: reports and key reduction", "animation"},   // S77
    {registerAnimatorTools, "Animator: controllers, masks, montages, live control", "animation"},   // S21
    {registerPlacementTools, "Physics placement", "editor_ux"},   // upstream (Physics Placement)
    {registerHtmlUiTools, "HTML/CSS UI", ""},
    {registerUiEffectTools, "HTML/CSS UI", ""},
    {registerEcsTools, "ECS: statistics, system schedule and queries", "diagnostics"},   // S07
    {registerDiagnosticsTools, "Crash and hang diagnostics", "diagnostics"},   // S28
    {registerAtmosphereTools, "Atmosphere: fog and clouds", "rendering_fx"},   // S12
    {registerProfilerTools, "Profiling and render diagnostics", "profiling"},   // S15
    {registerMeshLodTools, "Meshes: import, cook and detail levels", "meshes"},   // S53, S54
    {registerDeviceTools, "Device tiers: GPU, thermal and memory state, simulation", "device"},   // S56
    {registerScriptingTools, "Scripting: hot reload, script status, editor menus, expressions", "scripting"},   // S23
    {registerGraphTools, "Graphs", "graphs"},   // S23; S90 R32 moved them out of scripting
    {registerProceduralMaterialTools, "Procedural Materials: preview, checks and bake", "materials"},   // S90
    {registerTerrainGenerationTools, "Terrain generation: Noise Graphs", "terrain"},   // T2
    {registerGameplayTools, "Gameplay: input, character controller, water", "gameplay"},   // S22
    {registerSplineTools, "Splines: roads, walls, placement, terrain and water", "splines"},   // S18
    {registerVoxelTools, "Voxel Volumes", "voxels"},   // T6
    {registerTerrainTools, "Terrain: declarative edits and heightmap files", "terrain"},   // T1
    {registerEditorToolsTools, "Editor tools: recorder, colour sampling, swatches, curves", "editor_tools"},   // S27
    {registerParticleTools, "Particles: effects and playback", "particles"},   // S20
    {registerPaletteTools, "Pixel art: palettes", "pixel_art"},   // S43
    {registerPixelArtTools, "Pixel art: the validator", "pixel_art"},   // S42
    {registerOrganisationTools, "Search, shortcuts, layouts, labels, saved searches and bookmarks", "organisation"},   // S25
    {registerNavigationTools, "Navigation: navmeshes, agents, crowds and grids", "navigation"},   // S19
    {registerVcsTools, "Git/SVN status, diffs, commits, locks", "vcs"},   // S29
    {registerSpriteTools, "Sprites: Aseprite import, atlases, drawing", "sprites"},   // S44 (S45, S47)
    {registerPixelFontTools, "Pixel art: font previews", "pixel_art"},   // S51
};

// JASPER_MCP_SELFTEST_GROUP: one tool in the test-only group "selftest", for the tests of
// tool groups and of the budget gate (a value n > 1 pads its description to n characters)
void registerSelftestTools(McpRegistry& registry) {
    using namespace schema;
    const int mode = selftestGroupMode();
    if (mode <= 0) return;
    ToolDef tool;
    tool.name = "selftest_echo";
    tool.title = "Selftest Echo";
    tool.group = "selftest";
    tool.module = "Selftest";
    tool.description = "Test-only: returns text unchanged.";
    if (mode > 1 && static_cast<int>(tool.description.size()) < mode) {
        tool.description += std::string(static_cast<size_t>(mode) - tool.description.size(), '.');
    }
    tool.inputSchema = object({{"text", withDefault(string(""), "")}});
    tool.annotations.readOnly = true;
    tool.annotations.idempotent = true;
    tool.handler = [](CallPtr call) {
        const std::string text = call->args().value("text", std::string());
        call->ok(text, Json{{"text", text}});
    };
    registry.addTool(std::move(tool));
}

// The tools a module just added (or replaced: same name, same position) have no module yet
void stampModule(McpRegistry& registry, const ToolModule& entry) {
    for (ToolDef& tool : registry.mutableTools()) {
        if (!tool.module.empty()) continue;
        tool.module = entry.module;
        if (tool.group.empty()) tool.group = entry.group;
    }
}

} // namespace

void buildToolRegistry(McpRegistry& registry) {
    for (const ToolModule& entry : TOOL_MODULES) {
        entry.registerTools(registry);
        stampModule(registry, entry);
    }
    registerSelftestTools(registry);
}

}
