// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Scene.h"
#include "shader/ShaderBuildTypes.h"

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
    class ShaderVariantCollector {
    public:
        static void collectScene(Scene* scene, const ShaderVariantContent& sceneContent,
                                 const std::vector<ShaderVariantContent>& prefabContents,
                                 std::set<ShaderKey>& keys, const std::filesystem::path& projectRoot = {});

        // the UI effect variants of documents (asset-relative .html/.htm/.rml paths) of a project
        static void collectUiEffects(const std::vector<std::string>& documents, const std::filesystem::path& projectRoot,
                                     std::set<ShaderKey>& keys);
        // the effects a text names: CSS (or the typed RCSS the compatibility layer writes, an
        // .rml document's), or Lua (lua = true: every use is `any`)
        static void scanUiEffects(const std::string& text, bool lua, UiEffectUses& uses);
        // the variants of the uses (unknown names are skipped)
        static void addUiEffectKeys(const UiEffectUses& uses, std::set<ShaderKey>& keys);
    };

}
