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
    class ShaderVariantCollector {
    public:
        static void collectScene(Scene* scene, const ShaderVariantContent& sceneContent,
                                 const std::vector<ShaderVariantContent>& prefabContents,
                                 std::set<ShaderKey>& keys);
    };

}
