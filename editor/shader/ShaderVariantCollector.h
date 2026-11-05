// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Scene.h"
#include "shader/ShaderBuildTypes.h"
#include "texture/MaterialPbr.h"

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor {

    // GameObjects whose shader variants are collected: a scene's, or a prefab's registry
    struct ShaderVariantContent {
        EntityRegistry* registry = nullptr;
        std::vector<Entity> entities;
    };

    // The UI effects (phase 5) an HTML/CSS UI file or a Lua script names, by form
    struct UiEffectUses {
        std::set<std::string> paint;    // effect names used in the paint form (background, mask)
        std::set<std::string> filter;   // used in the filter form (filter, backdrop-filter, gradient text)
        std::set<std::string> any;      // the form is decided where it is used (a custom property, Lua)
    };

    // The shader variants a build needs, derived from content the way RenderSystem picks
    // them when it loads what draws (RenderSystem::getSubmeshShaderVariant), without loading
    // anything on the GPU or needing the editor to have drawn it. Model files are read for
    // their vertex attributes and materials. Generated meshes (sprites, tilemaps, polygons,
    // terrains) take the vertex layout their generator gives them.
    //
    // A mesh bound to a lattice (its own Lattice Modifier or an ancestor's with
    // includeChildren, LatticeRender::isLatticeTarget) adds its lattice variants ('Ltc') to
    // the ones it draws with unbound. A mesh with a Vertex Animator adds its Vertex Animation
    // variants ('Vat' or 'Vab', by its asset's mode; both when the asset cannot be read) to the
    // ones it draws with at rest.
    //
    // A scene's lighting decides the variants (lights, shadows, 2D lights, fog, IBL), and a
    // prefab a script spawns can bring its own: every combination the scene's content and the
    // build's prefabs can reach is collected, for the scene's own GameObjects and for every
    // prefab spawned into it.
    //
    // HTML UI documents (UI Document components) also bring the UI effect variants they use
    // (phase-5 spec 3.8), read from projectRoot (empty: no effects): every document is
    // converted by the compatibility layer as it loads (its HTML, linked and inline CSS, style
    // attributes, @effect declarations), and its files are also read as written, so an effect
    // only a @media block the conversion leaves out uses is there too; an effect in a paint
    // property adds its paint form, in a filter property or as gradient text its filter form.
    // The project's Lua scripts (and the documents' own) are scanned for effect(<name>): a
    // paint effect named there adds both forms, a filter effect its filter form. An effect
    // whose blur prepass has a threshold adds the internal bright pass.
    //
    // Lit variants are keyed by the per-object light capacity tier (4, 8 or 16 lights, from
    // the scene's maxLightsPerObject). Only a script can change it at runtime: with
    // allLightCapacities (the project's scripts set maxLightsPerObject) every tier is
    // collected, else only the scene's own.
    //
    // The built-in Lit's variants follow the scene's Lit Quality (both with scriptsSetLitQuality:
    // a script sets scene.litQuality) and the material lobes for desktop and, with a mobile
    // tier other than FULL, the lobes Android, iOS and HTML5 draw (MaterialPbr::resolveTier).
    //
    // The post-processing colour (phase 4, one global variant bit of mesh, points, lines and
    // sky variants) follows the scene's PostProcessingSettings; with scriptsSetPostProcessing
    // (a project script mentions postProcessing) both states are collected. The uber pass's
    // variants (BLIT features) and the bloom chain's programs: the scene settings', and with
    // scripts setting post-processing the plausible ones (plausibleUberFeatures); any other a
    // script reaches compiles at runtime.
    //
    // The High shadow filters (S10, the 'Shq' bit of variants receiving shadows) follow the
    // scene's shadow Quality: High and Ultra have them, Low, Medium and Hard Only not, Auto
    // (the platform's tier) both, and so does a scene whose scripts set the quality
    // (scriptsSetShadowQuality). An export drops them for the backends no platform of which
    // has a High tier (ShadowTiers::cooksHighFilters).
    //
    // Every Device Tier the project's settings let a player apply (spec S56), with the thermal
    // steps of the families whose Thermal Response is on, is collected too: a tier's render scale
    // below 1 needs the uber pass's upscale ('Scl'), and its SSAO, bloom, Lit Quality, shadow,
    // light and volumetrics cells other mesh and uber variants.
    class ShaderVariantCollector {
    public:
        // The variants every build ships whatever its scenes hold (added by the Exporter, never
        // to a scene's saved shaderKeys): the FLATTEN Canvas Group composite (the HTML UI
        // passthrough, a few KB), since a script can add a group or switch one to FLATTEN in any scene
        static void addBuildRequiredKeys(std::set<ShaderKey>& keys);

        static void collectScene(Scene* scene, const ShaderVariantContent& sceneContent,
                                 const std::vector<ShaderVariantContent>& prefabContents,
                                 std::set<ShaderKey>& keys, const std::filesystem::path& projectRoot = {},
                                 bool allLightCapacities = false, bool scriptsSetPostProcessing = false,
                                 const std::vector<std::string>& scriptPostProcessingFields = {},
                                 bool scriptsSetShadowQuality = false, bool scriptsSetLitQuality = false,
                                 PbrTier mobilePbrTier = PbrTier::FULL);

        // the UI effect variants of documents (asset-relative .html/.htm/.rml paths) of a project
        static void collectUiEffects(const std::vector<std::string>& documents, const std::filesystem::path& projectRoot,
                                     std::set<ShaderKey>& keys);
        // the effects a text names: CSS (or the typed RCSS the compatibility layer writes, an
        // .rml document's), or Lua (lua = true: every use is `any`)
        static void scanUiEffects(const std::string& text, bool lua, UiEffectUses& uses);
        // the variants of the uses (unknown names are skipped)
        static void addUiEffectKeys(const UiEffectUses& uses, std::set<ShaderKey>& keys);
        // the fluid simulation's programs (FluidSimulator, ShaderType::FLUID: the solver, the
        // look, the solver of devices that cannot filter half floats): a scene or prefab with a
        // FluidSimulationComponent, or scripts naming FluidSimulation (Project), ship them
        static void addFluidKeys(std::set<ShaderKey>& keys);

        // The PostProcessingSettings switches a script may flip (their camelCase names): the
        // effects of the uber pass, the bloom and blur, the grade (monochrome is part of its
        // LUT), vignette and dither the uber pass takes from the shaders, and the render scale
        // (below 1 alone: the upscale variant)
        static const std::vector<std::string>& getScriptableEffectFields();
        // The uber variants a scene may switch to at runtime (phase 4): its settings', and when
        // scripts set post-processing the combinations of the switches they name (every
        // combination when they name up to 4, else each flipped alone over the scene's), or each
        // switch flipped alone when they name none. Returns non-zero BLIT feature sets.
        static std::set<uint32_t> plausibleUberFeatures(const PostProcessingSettings& settings, bool scriptsSetPostProcessing,
                                                        const std::vector<std::string>& namedFields);

    private:
        // collectScene's work under the Device Tier QualityResolver holds now
        static void collectSceneUnderTier(Scene* scene, const ShaderVariantContent& sceneContent,
                                          const std::vector<ShaderVariantContent>& prefabContents, std::set<ShaderKey>& keys,
                                          const std::filesystem::path& projectRoot, bool scriptsSetPostProcessing,
                                          const std::vector<std::string>& scriptPostProcessingFields, bool allLightCapacities,
                                          bool scriptsSetShadowQuality, bool scriptsSetLitQuality, PbrTier mobilePbrTier);
    };

}
