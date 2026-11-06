// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Every asset-reference field the editor knows, in one table: component, YAML key path,
// expected asset type, path root and edge kind. The component rows are generated from the
// reflection tables (editor/reflect), and so is the walk that reaches their fields in memory;
// materials, scene settings, scene file lists and project.yaml are small explicit lists here.
// A new reference field of a component is only its field in the component's table: its type
// (Texture, ShaderRef, AssetRef, FontArray, a list of them) or its .asset says the rest, and its
// updateFlags are what a moved file reloads. AssetRefSchemaFrozen holds the rows of the hand
// table this replaced. Two consumers share it:
//   - forEachAssetRef walks references in memory (loaded scenes, prefab registries,
//     materials) for resolution, rename hint refreshes, Find References of unsaved edits,
//     the Missing Assets tracker and export;
//   - the closed-file extractor walks the same fields in a scene, prefab, material or
//     project.yaml file's YAML, so the dependency index covers files nobody has open.

#pragma once

#include "asset/AssetRef.h"
#include "asset/AssetType.h"
#include "asset/AssetTypes.h"
#include "asset/DepEdge.h"
#include "Catalog.h"

#include "yaml-cpp/yaml.h"

#include <functional>
#include <string>
#include <vector>

namespace jasperengine {
    class EntityRegistry;
    class Scene;
    struct Material;
}

namespace jasperengine::editor {

    // How a field holds its reference
    enum class AssetRefShape {
        Asset,       // AssetRef: "<key>: path" + "<key>Guid: guid"
        Shader,      // ShaderRef: "<key>: base|vert|frag" + "<key>Guid: vert|frag"; two files
        Texture,     // Texture: a texture node {path, guid} (cube faces: {paths, guids})
        AssetList,   // an array of AssetRef: "<key>: [..]" + "<key>Guid: [..]"
        PrefabRef,   // a map {path, guid} (PrefabComponent, the scene prefab list)
        ScriptProperty, // a script property value {guid, path} (Prefab and Asset types)
        ParticleEffect, // an inline particle effect (its .particles JSON in a string): the textures,
                        // models and child effects its modules name ("<key>.renderer.texture")
        ClipEntry,      // an Animation Player clip entry {clip, guid} (or a bare path): an .anim by
                        // GUID and path hint, or a model's sub-clip "<model>#<clip>" (the model,
                        // by path; the clip part and the empty GUID kept)
        GuidSibling     // "<key>: path" + "guid: guid" in the same map (a Vertex Animator's .vat)
    };

    // Where a row's field lives
    enum class AssetRefScope {
        Component,      // a component of a GameObject
        Material,       // a material: a submesh's own, or a .material file
        SceneSettings,  // the scene settings (the `scene:` map of a scene file)
        SceneFile,      // the lists at the root of a scene file
        Project         // project.yaml
    };

    struct AssetRefField {
        AssetRefScope scope;
        ComponentType component;      // Component rows: the owner component
        std::string path;             // the field: "submeshes[].material.baseColorTexture" ("[]" an array, "{}" a map)
        AssetRefShape shape;
        AssetType expected;           // Unknown: any (script properties take their own type)
        AssetPathBase base;
        DepEdgeKind kind;             // SceneComponent for plain component fields (PrefabComponent inside prefabs)
        bool legacy = false;          // a key only old files have: read, never written
        bool modelOrPrefab = false;   // expected by the path's extension (a spline pool's entry)
    };

    // One reference, as a visitor sees it
    struct AssetRefSite {
        const AssetRefField* row = nullptr;
        Entity entity = NULL_ENTITY;  // the owner GameObject (NULL_ENTITY outside components)
        std::string component;        // "MeshComponent" (empty outside components)
        std::string field;            // the field with indices: "submeshes[0].material.baseColorTexture"
        uint32_t ordinal = 0;         // cube face, 0 vert / 1 frag of a shader
        AssetType expected = AssetType::Unknown;
        AssetPathBase base = AssetPathBase::Project;
        DepEdgeKind kind = DepEdgeKind::Other;

        // "entity:<id>/<component>.<field>" (or "<scope>/<field>" outside components)
        std::string locator(const std::string& scope = std::string()) const;
    };

    // The visitor gets a copy of the reference; what it changes is written back to the field
    // (a new path reloads what the field feeds: a texture, a model, a shader...)
    using AssetRefVisitor = std::function<void(AssetRef& ref, const AssetRefSite& site)>;

    // A reference found in a file's YAML
    struct AssetRefFound {
        AssetRef ref;
        AssetRefSite site;
        uint64_t fileEntity = 0;      // the `entity:` id of the GameObject (0 outside entities)
        uint32_t sceneId = 0;         // a child scene reference: the scene's project id (no path, no GUID)
        std::string entityPath;       // the GameObject's name path ("Root/Child")
        std::string scope;            // the locator scope
    };
    using AssetRefEmitter = std::function<void(const AssetRefFound& found)>;

    class AssetRefSchema {
    public:
        static const std::vector<AssetRefField>& fields();
        // The rows of a material, relative to the material node (submesh materials and .material files)
        static const std::vector<AssetRefField>& materialFields();
        // Scene settings, scene file lists and project.yaml rows
        static const std::vector<AssetRefField>& documentFields();

        // ---- memory

        // Every reference of every component in the registry (or of one entity); true when any
        // field changed
        static bool forEachAssetRef(EntityRegistry& registry, const AssetRefVisitor& visitor);
        static bool forEachAssetRef(EntityRegistry& registry, Entity entity, const AssetRefVisitor& visitor);
        // A material's textures, shader, extra pass and linked file; fieldPrefix goes before
        // each field ("submeshes[2].material." for a submesh, "" for a .material file)
        static bool forEachMaterialRef(Material& material, const std::string& fieldPrefix, const AssetRefVisitor& visitor,
                                       Entity entity = NULL_ENTITY, const std::string& component = std::string());
        // The scene's default shaders, post-process passes and LUT
        static bool forEachSceneSettingsRef(Scene& scene, const AssetRefVisitor& visitor);

        // ---- files

        // A scene or prefab file: its GameObjects (prefab instance overrides and added
        // GameObjects included), its settings and lists. inPrefab turns component edges into
        // PrefabComponent ones.
        static void extractDocumentRefs(const YAML::Node& root, bool inPrefab, const AssetRefEmitter& emit);
        // A .material file
        static void extractMaterialFileRefs(const YAML::Node& root, const AssetRefEmitter& emit);
        // project.yaml: the scene list and the standalone prefabs
        static void extractProjectRefs(const YAML::Node& root, const AssetRefEmitter& emit);

        // The row a field with indices belongs to ("submeshes[2].material.shaderTextures.tex"
        // -> "submeshes[].material.shaderTextures{}"); nullptr when none
        static const AssetRefField* findRow(AssetRefScope scope, ComponentType component, const std::string& field);
        // A component of the table by its class name ("MeshComponent"); false when none has references
        static bool componentByName(const std::string& name, ComponentType& component);

        // The two files of a custom shader value ("" for a library shader)
        static void shaderFiles(const std::string& value, std::string& vert, std::string& frag);
    };

}
