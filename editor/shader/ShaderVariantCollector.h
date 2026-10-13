// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Scene.h"
#include "shader/ShaderBuildTypes.h"

#include <set>
#include <vector>

namespace jasperengine::editor {

    // GameObjects whose shader variants are collected: a scene's, or a prefab's registry
    struct ShaderVariantContent {
        EntityRegistry* registry = nullptr;
        std::vector<Entity> entities;
    };

    // The shader variants a build needs, derived from content the way RenderSystem picks
    // them when it loads what draws (RenderSystem::getSubmeshShaderVariant), without loading
    // anything on the GPU or needing the editor to have drawn it. Model files are read for
    // their vertex attributes and materials. Generated meshes (sprites, tilemaps, polygons,
    // terrains) take the vertex layout their generator gives them.
    //
    // A scene's lighting decides the variants (lights, shadows, 2D lights, fog, IBL), and a
    // prefab a script spawns can bring its own: every combination the scene's content and the
    // build's prefabs can reach is collected, for the scene's own GameObjects and for every
    // prefab spawned into it.
    //
    // Lit variants are keyed by the per-object light capacity tier (4, 8 or 16 lights, from
    // the scene's maxLightsPerObject). Only a script can change it at runtime: with
    // allLightCapacities (the project's scripts set maxLightsPerObject) every tier is
    // collected, else only the scene's own.
    //
    // The post-processing colour (phase 4, one global variant bit of mesh, points, lines and
    // sky variants) follows the scene's PostProcessingSettings; with scriptsSetPostProcessing
    // (a project script mentions postProcessing) both states are collected. The uber pass's
    // variants (BLIT features) and the bloom chain's programs: the scene settings', and with
    // scripts setting post-processing the plausible ones (plausibleUberFeatures); any other a
    // script reaches compiles at runtime.
    class ShaderVariantCollector {
    public:
        static void collectScene(Scene* scene, const ShaderVariantContent& sceneContent,
                                 const std::vector<ShaderVariantContent>& prefabContents,
                                 std::set<ShaderKey>& keys, bool allLightCapacities = false,
                                 bool scriptsSetPostProcessing = false,
                                 const std::vector<std::string>& scriptPostProcessingFields = {});

        // The PostProcessingSettings switches a script may flip (their camelCase names): the
        // effects of the uber pass, the bloom and blur, and the grade, vignette and dither the
        // uber pass takes from the shaders
        static const std::vector<std::string>& getScriptableEffectFields();
        // The uber variants a scene may switch to at runtime (phase 4): its settings', and when
        // scripts set post-processing the combinations of the switches they name (every
        // combination when they name up to 4, else each flipped alone over the scene's), or each
        // switch flipped alone when they name none. Returns non-zero BLIT feature sets.
        static std::set<uint32_t> plausibleUberFeatures(const PostProcessingSettings& settings, bool scriptsSetPostProcessing,
                                                        const std::vector<std::string>& namedFields);
    };

}
