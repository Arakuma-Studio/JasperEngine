// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <cstdint>
#include <algorithm>

#include "Scene.h"     // Entity, EntityRegistry, NULL_ENTITY
#include "Catalog.h"   // ComponentType
#include "LoadReport.h"

#include "yaml-cpp/yaml.h"

namespace jasperengine{
    struct PrefabComponent;
}

namespace jasperengine::editor {

// What one prefab instance changes on one of its prefab's GameObjects (Unity's
// PropertyModifications and friends, per object). See editor/project/PREFABS.md.
struct PrefabObjectOverrides {
    // The name, active state, tag and layer of the GameObject (`state` bits)
    enum State : uint8_t {
        StateName   = 1 << 0,
        StateActive = 1 << 1,
        StateTag    = 1 << 2,
        StateLayer  = 1 << 3
    };
    // A property name standing for every property of the component: what component-level
    // overrides (files written before property overrides, "Override Prefab Component", a
    // reference outside the instance) record
    static constexpr const char* WHOLE_COMPONENT = "*";

    // Overridden properties per component, by the Catalog property names PropertyCmd uses
    // ("position", "color", "submeshes[0].material", ...): whole-property granularity, an
    // indexed name is one element, a compound one (scripts, actions) the whole of it
    std::map<ComponentType, std::set<std::string>> properties;
    uint8_t state = 0;
    // Components the instance added to the GameObject, and prefab components it removed
    std::set<ComponentType> addedComponents;
    std::set<ComponentType> removedComponents;

    bool empty() const {
        return properties.empty() && state == 0 && addedComponents.empty() && removedComponents.empty();
    }
    // Any property of the component
    bool hasComponent(ComponentType type) const {
        return properties.count(type) != 0;
    }
    bool hasProperty(ComponentType type, const std::string& property) const;
    bool isWholeComponent(ComponentType type) const;
    void addProperty(ComponentType type, const std::string& property);
    void addWholeComponent(ComponentType type);
    // Merges `other` in (an added component stays added)
    void merge(const PrefabObjectOverrides& other);
};

// The data an instance keeps relative to its prefab, as files hold it (scene files, the
// Missing Prefab records, and prefab files for the instances nested in them): every key is
// a registry entity of the prefab. Read from the v3 `prefabInstance` block or from the
// older prefabOverrides/prefabLocalEntities/prefabRemovedGameObjects keys
// (Stream::decodePrefabInstanceData). Recursive: `nested` holds the same for each nested
// instance, by the registry entity of its root in this prefab, to any depth.
struct PrefabInstanceData {
    struct Object {
        PrefabObjectOverrides overrides;
        // The values: name/active/tag/layer and `components` (whole components as Stream
        // writes them; only the overridden properties and the added components are read).
        // Null for a root, whose values are its own (the instance root node, or the nested
        // root in the containing prefab's registry).
        YAML::Node values;
    };

    bool present = false;  // read from something: false for an instance without data
    // Written before the real-root model (no `prefabInstance` block): the root's own
    // components and state are the old per-instance wrapper's, made into overrides against
    // the prefab's root when imported
    bool legacy = false;

    std::map<Entity, Object> objects;            // by registry entity, the root included
    std::set<Entity> removedGameObjects;         // removed GameObject overrides
    // Added GameObjects: entity nodes with parentRegistryEntity (the root's, or NULL for it)
    // and childIndex among the parent's children
    std::vector<YAML::Node> addedGameObjects;
    std::map<Entity, Entity> memberIds;          // registry entity -> local entity, members

    // Nested instances: by their root's registry entity in this prefab (nestedRoot), or for
    // the oldest files by the nested prefab's path (nestedRoot NULL_ENTITY)
    Entity nestedRoot = NULL_ENTITY;
    std::string nestedPath;
    std::vector<PrefabInstanceData> nested;

    const PrefabInstanceData* findNested(Entity registryRoot, const std::string& path) const;
    PrefabInstanceData* findNested(Entity registryRoot, const std::string& path);
    // The nested entry for registryRoot, made when missing
    PrefabInstanceData& nestedFor(Entity registryRoot, const std::string& path);
    bool empty() const;
};

class Prefab {
public:
    // Prefab file format, see editor/project/PREFABS.md:
    // {type: Prefab, version: 8, guid, nextEntity, root, members: [...]}
    // 2: header, wrapper-root model; 3: the prefab's root GameObject is `root`; 4: light shadow
    // biases in shadow texels (StreamMigrations, prefab 3 -> 4); 5: particle systems as effects;
    // 6: no derived transform values (S29 R20); 7: Animation Player controllers on Animators (S21
    // upstream absorption, StreamMigrations prefab 6 -> 7); 8: Line / Trail Renderer curves and
    // gradients in S27's form (the S27 upstream absorption, prefab 7 -> 8)
    static constexpr int FILE_VERSION = 8;
    // the first version with the root-GameObject layout of today (`root`, members as written)
    static constexpr int ROOT_LAYOUT_VERSION = 3;

    struct FileHeader {
        int version = 1;                  // 1: a file written before the header
        std::string guid;                 // empty when the file has none
        Entity nextEntity = NULL_ENTITY;  // the id the next new GameObject of the Prefab takes
        Entity root = NULL_ENTITY;        // version 3: the root GameObject
    };
    static FileHeader readHeader(const YAML::Node& node);

    // The one way a prefab file becomes a registry: decodes it into `registry` (made, or
    // cleared first) and `registryEntities`, then raises the registry's id counter to the
    // file's nextEntity, past its highest id and past what the counter already was. A
    // registry entity id is the GameObject's persistent file id: scenes key their overrides
    // by it, so an id is never handed out twice, whatever was deleted since.
    // A file older than version 3 is upgraded on the spot (needsUpgrade): its top-level
    // GameObjects go below a new root GameObject named legacyRootName, with a Transform
    // when one of them has one, taking the id nextEntity (the old per-instance wrapper).
    // The `prefabInstance` blocks of nested prefab roots fill nestedInstances.
    FileHeader loadRegistry(const YAML::Node& node, const std::string& legacyRootName = "Prefab");
    // The id the next GameObject added to the Prefab takes (the file's nextEntity)
    Entity getNextEntity() const;

    using ObjectOverrides = PrefabObjectOverrides;

    struct EntityMember {
        Entity localEntity;     // Entity in the scene
        Entity registryEntity;  // Corresponding entity in the shared registry
    };

    struct Instance {
        uint32_t instanceId;
        Entity rootEntity = NULL_ENTITY;          // Root entity of this instance in the scene
        Entity rootRegistryEntity = NULL_ENTITY;  // the prefab's root GameObject it instantiates
        // The instance's other GameObjects (the root is not one of them)
        std::vector<EntityMember> members;
        // Overrides by local entity, the root's included (see PREFABS.md for what the root
        // keeps: its name, position and rotation are never overrides)
        std::map<Entity, ObjectOverrides> overrides;
        // Unity's removed GameObject overrides: registry entities taken out of this instance
        // only, each with the members its branch had then (whose ids a revert takes back;
        // empty after a reload). A Prefab GameObject is absent from the instance when it or
        // one of its registry ancestors is listed, so what the Prefab adds below a removed
        // GameObject stays out too. Members only ever hold the GameObjects present.
        std::unordered_map<Entity, std::vector<EntityMember>> removed;

        // Root included
        Entity getRegistryEntity(Entity localEntity) const;
        Entity getLocalEntity(Entity registryEntity) const;
        const ObjectOverrides* findOverrides(Entity localEntity) const;
    };

    // sceneId → list of instances
    std::map<uint32_t, std::vector<Instance>> instances;

    std::unique_ptr<EntityRegistry> registry;
    std::vector<Entity> registryEntities;
    // The root GameObject: the only top-level GameObject with a Transform (others, without
    // a Transform, can sit next to it)
    Entity root = NULL_ENTITY;

    // Asset-level data of the instances nested in this prefab, by the registry entity of
    // their root here: what this prefab overrides of them (the layer between the nested
    // prefab and the scene, see Project::importPrefab). Written on the nested root's node.
    std::map<Entity, PrefabInstanceData> nestedInstances;

    // Unity's stripped objects: GameObjects of the instances nested in this prefab (at any
    // depth) that this prefab refers to, each by an id of this prefab (never a registry
    // entity, taken from the same never-reused counter). `source` is the way down: the
    // nested root's registry entity here, then the registry entity of the next nested root
    // in that prefab, ..., then the GameObject's registry entity in the innermost prefab.
    // A reference to such a GameObject (any component's entity reference, a script
    // property, a camera texture link) holds the stripped id in this registry; importPrefab
    // turns it into the nested GameObject's local id. Written as the file's `stripped:` list.
    std::map<Entity, std::vector<Entity>> stripped;

    // GameObjects this prefab adds to its nested instances (Unity's added GameObjects at
    // asset level): registry entities of this prefab whose registry parent is a nested root.
    // The top of each such branch may sit deeper in the nested instance than its root: its
    // entry says below which nested GameObject (a stripped id; NULL_ENTITY for the nested
    // root itself) and where among that GameObject's children (-1: last). Written on the
    // branch's node as `addedTo` / `childIndex`.
    struct AddedObject {
        Entity parent = NULL_ENTITY;
        int childIndex = -1;
    };
    std::map<Entity, AddedObject> addedObjects;

    bool isModified = false;
    // CommandHistory::stampSave when its file was last written or read back (0: since it loaded):
    // an undo that captured another one gives back a state the file does not hold
    uint64_t savedAt = 0;
    // Read from a file older than FILE_VERSION: written again the next time a scene using it
    // is saved, even if nothing changed
    bool needsUpgrade = false;
    // What the asset's file load could not read (docs/data-safety.md): with dropped issues
    // outstanding, its saves are gated (Project::savePrefabToDisk)
    LoadReport loadReport;
    // SHA-1 of the file's bytes as last loaded or saved by this session
    std::string diskSha1;
    uint32_t nextInstanceId = 1;

    uint32_t getInstanceId(uint32_t sceneId, Entity entity) const;
    // A member's registry entity (NULL_ENTITY for an instance root: see getObjectRegistryEntity)
    Entity getRegistryEntity(uint32_t sceneId, Entity entity) const;
    // The registry entity of a root or a member
    Entity getObjectRegistryEntity(uint32_t sceneId, Entity entity) const;
    // The local entity of a registry entity in an instance, the root's included
    Entity getLocalEntity(uint32_t sceneId, uint32_t instanceId, Entity registryEntity) const;
    Entity getRootEntity(uint32_t sceneId, Entity entity) const;

    // Membership and instance access
    bool hasInstances(uint32_t sceneId) const;
    bool containsEntity(uint32_t sceneId, Entity entity) const;
    Instance* getInstance(uint32_t sceneId, Entity entity);
    const Instance* getInstance(uint32_t sceneId, Entity entity) const;
    Instance* getInstanceById(uint32_t sceneId, uint32_t instanceId);
    // The instance whose root is `root` (roots only: one comparison per instance)
    Instance* getInstanceByRoot(uint32_t sceneId, Entity root);
    const Instance* getInstanceById(uint32_t sceneId, uint32_t instanceId) const;

    // Removed GameObjects: whether the registry entity is left out of the instance, being
    // removed itself or below a removed one. One hash lookup per registry ancestor.
    bool isRegistryEntityAbsent(const Instance& instance, Entity registryEntity) const;
    // The registry Transform parent of a registry entity, NULL_ENTITY for a top-level one
    Entity getRegistryParent(Entity registryEntity) const;
    // Whether a registry entity is the root of a prefab instance nested in this prefab
    bool isNestedRoot(Entity registryEntity) const;
    // Unity's Prefab Variant: the prefab's root GameObject is an instance of another prefab
    // (its base), named by the root's PrefabComponent (PREFABS.md 19). A variant is the
    // nested model with the root as the nested root: what it overrides of its base is
    // nestedInstances[root], the GameObjects it adds are the root's registry children.
    bool isVariant() const;
    // The base's PrefabComponent on the root, nullptr for an original prefab
    const PrefabComponent* getBaseComponent() const;
    // A registry entity rooting an instance of another prefab: a nested root, or the root of
    // a variant (its base). Everything keyed by a nested root works on both.
    bool isInstanceRoot(Entity registryEntity) const;

    // ---- Stripped objects and added GameObjects of nested instances (PREFABS.md 17) ----

    // A new id of this prefab (a GameObject or a stripped object): above every id it had
    Entity allocateId();
    // The stripped id for a way down into the nested instances, made when missing
    Entity findStripped(const std::vector<Entity>& source) const;
    Entity addStripped(const std::vector<Entity>& source);
    // The registry children of a nested root: the tops of the GameObjects this prefab adds
    // to that nested instance, in registry order
    std::vector<Entity> getAddedObjectTops(Entity nestedRoot) const;
    // Moves a loaded file's asset data in (registry, root, nested data, stripped objects,
    // added GameObjects), keeping the instances and the id counter's high-water mark
    void takeAssetData(Prefab&& loaded);

};

} // namespace jasperengine::editor
