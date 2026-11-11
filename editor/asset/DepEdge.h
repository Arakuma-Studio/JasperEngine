// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// One reference from an asset (the referrer) to another asset (the target), as the
// dependency index stores it. The shape follows OpenEngine's DepEdge
// (Engine/Modules/AssetCore/Include/AssetCore/DepEdge.h, MIT); the kinds are ours.
//
// The field locator names the field: "<scope>/<Component>.<field>[<i>].<field>", the scope
// being "entity:<file entity id>" (the `entity:` key of scene and prefab files, stable per
// editor/project/PREFABS.md §2), "settings", "material", "project" or "text:<line>":
//   entity:12/MeshComponent.submeshes[0].material.baseColorTexture
//   project/scenes[3]
// S08 generates the same locators from reflection.

#pragma once

#include "asset/Guid.h"
#include "asset/AssetType.h"

#include <cstdint>
#include <string>

namespace jasperengine::editor {

    enum class DepEdgeKind : uint16_t {
        Other,
        SceneComponent,     // a component field of a scene GameObject
        PrefabComponent,    // a component field of a GameObject inside a prefab file
        MaterialTexture,    // a texture of a material (a .material file or a submesh's own)
        MaterialShader,     // a material's shader
        MeshMaterialFile,   // a submesh linked to a .material file
        SceneSettings,      // scene settings: default shaders, post-processing
        SceneList,          // project.yaml scenes
        ChildScene,         // a scene's child scenes
        ScenePrefabList,    // a scene's prefab list (the build's prefabs), project standalone prefabs
        ScriptProperty,     // an Asset or Prefab script property value
        CppScript,          // a scene's C++ script sources
        UIStylesheet,       // HTML <link href> / CSS @import
        UIImage,            // HTML src, CSS url()
        LuaRequire,         // Lua require("...")
        ShaderInclude,      // shader #include
        ModelUri,           // a glTF's external buffer/image URI
        TitleArtAsset,      // a font, image, SVG or map a .titleart reads (field: its JSON path)
        TitleArtOutput      // a .titleart's baked PNG (field: outputs[i])
    };

    const char* depEdgeKindName(DepEdgeKind kind);
    DepEdgeKind depEdgeKindFromName(const std::string& name);

    // Text references (Lua, HTML, CSS, shaders, glTF URIs) are path edges: the editor does
    // not rewrite them on a rename, it lists the ones a rename breaks (S04 R14)
    inline bool isTextEdge(DepEdgeKind kind) {
        return kind == DepEdgeKind::UIStylesheet || kind == DepEdgeKind::UIImage || kind == DepEdgeKind::LuaRequire ||
               kind == DepEdgeKind::ShaderInclude || kind == DepEdgeKind::ModelUri;
    }

    struct DepEdge {
        Guid referrer;
        Guid target;              // null for a path edge whose target is not an asset (yet)
        std::string targetPath;   // project-relative; the hint (GUID edges) or the path (path edges)
        DepEdgeKind kind = DepEdgeKind::Other;
        std::string field;        // the field locator
        uint32_t ordinal = 0;     // order among edges with the same locator (cube faces, vert/frag)
        // The owner of the field, for display (Find References, Missing Assets)
        uint64_t entity = 0;      // the GameObject's file entity id (0 outside GameObjects)
        std::string entityPath;   // the GameObject's name path ("Root/Child")
        std::string component;    // "MeshComponent" (empty outside components)
        uint32_t line = 0;        // text edges: the line of the reference
        AssetType expected = AssetType::Unknown;

        bool operator==(const DepEdge& other) const {
            return referrer == other.referrer && target == other.target && targetPath == other.targetPath &&
                   kind == other.kind && field == other.field && ordinal == other.ordinal;
        }
    };

    // OpenEngine's IsValidDepEdge: a referrer, a target (GUID or path) and a field
    inline bool isValidDepEdge(const DepEdge& edge) {
        return !edge.referrer.isNull() && (!edge.target.isNull() || !edge.targetPath.empty()) && !edge.field.empty();
    }

    // The parts of a field locator: scope ("entity:12", "settings", ...), the entity id when
    // the scope is an entity, the component and the field path below it
    struct FieldLocatorParts {
        std::string scope;
        uint64_t entity = 0;
        bool hasEntity = false;
        std::string component;
        std::string field;
    };
    FieldLocatorParts parseFieldLocator(const std::string& locator);

}
