// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Data a scene or prefab file holds that this editor cannot read, kept verbatim and written
// back on save (docs/data-safety.md, docs/specs/S03-data-safety.md R26-R27):
//   (a) an unknown key under `components`: the whole component node;
//   (b) an unknown top-level key of a known component;
//   (c) an unknown key at an entity node's root or a document's root;
//   (d) a known key whose value does not decode (an unknown enum name, text where a number
//       goes): the field takes its default and the authored value is written back while the
//       live value still encodes to what it encoded to right after the load;
//   (e) a component whose decoder fails as a whole: kept as in (a) and not instantiated,
//       except a Transform, which gets a default value (so the hierarchy stays) that the
//       authored node stands in for in the same way as (d).
// The side table (PreservedStore) belongs to the registry the data was decoded into, so it
// follows duplicate, clipboard, undo snapshots and prefab apply (they all encode and decode
// entities). Entries go away with their entity or component, and (d)/(e) ones retire once the
// live value is written.

#include "Catalog.h"
#include "LoadReport.h"
#include "StreamSchema.h"

#include "registry/EntityRegistry.h"
#include "yaml-cpp/yaml.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace jasperengine::editor {

    struct PreservedField {
        YAML::Node authored;
        // (d): a known key standing in for the live value; false for an unknown key (b)
        bool substitution = false;
        // YAML::Dump of the live value right after the load ("" when the encoder omits it)
        std::string liveAtLoad;
    };

    struct PreservedComponent {
        YAML::Node authored;
        // (e) Transform: the live default is there, the authored node stands in for it
        bool shadow = false;
        std::string liveAtLoad;
        // a stand-in goes with the live component
        ComponentId componentId = 0;
    };

    struct PreservedFieldGroup {
        // a component name, or "#scene" for the scene settings block
        std::string component;
        // the component's id in the registry: its removal retires the group
        bool hasComponentId = false;
        ComponentId componentId = 0;
        std::vector<std::pair<std::string, PreservedField>> fields;
    };

    struct PreservedEntity {
        std::vector<std::pair<std::string, YAML::Node>> rootKeys;
        std::vector<std::pair<std::string, PreservedComponent>> components;
        std::vector<PreservedFieldGroup> fieldGroups;

        bool empty() const;
        PreservedFieldGroup* group(const std::string& component);
        const PreservedFieldGroup* group(const std::string& component) const;
    };

    // The scene settings block's pseudo component name in the store and the load report
    constexpr const char* kSceneSettingsBlock = "#scene";

    // The preserved data of one registry, by entity (NULL_ENTITY: the document itself)
    class PreservedStore final : public EntityRegistryEditorData {
    public:
        void entityDestroyed(Entity entity) override;
        void componentRemoved(Entity entity, ComponentId componentId) override;
        void registryCleared() override;

        PreservedEntity* find(Entity entity);
        const PreservedEntity* find(Entity entity) const;
        PreservedEntity& entry(Entity entity);
        void reset(Entity entity);
        // The component's fields and whole-component entry (a decode of it starts afresh)
        void resetComponent(Entity entity, const std::string& component);
        // Drops an entity's entry once nothing is left in it
        void prune(Entity entity);

        // True while the store holds what an issue is about: (entity, component, "") a whole
        // component, (entity, component, field) a field, (entity, "", key) a root key
        bool holds(Entity entity, const std::string& component, const std::string& field) const;
        // Unknown and stand-in components, fields and root keys, over every entity
        size_t count() const;
        bool empty() const { return entities.empty(); }
        // The names of the components an entity holds only as preserved data (a)/(e), in order
        std::vector<std::string> preservedComponentNames(Entity entity) const;
        // Unknown components (a) and components kept whole (e), over every entity: a build has none of them
        size_t preservedComponentCount() const;
        // The authored node of one (a null node when there is none)
        YAML::Node preservedComponent(Entity entity, const std::string& component) const;
        // Removes an unknown component (Inspector Remove Component): the next save leaves it out
        bool removePreservedComponent(Entity entity, const std::string& component);
        // Puts one back at its place in the authored order (undo of the removal)
        void restorePreservedComponent(Entity entity, const std::string& component, const YAML::Node& authored, size_t index);
        // Where a preserved component sits in the entity's authored order
        size_t preservedComponentIndex(Entity entity, const std::string& component) const;

    private:
        std::unordered_map<Entity, PreservedEntity> entities;
    };

    // The store of a registry, nullptr when nothing was ever preserved in it
    PreservedStore* findPreservedStore(const EntityRegistry* registry);
    // The store of a registry, made when missing
    PreservedStore& preservedStore(EntityRegistry* registry);

    // ---- decoding (Stream.cpp calls these) ----
    namespace StreamLoad {

        // What the decoders of the component or block being decoded record into
        struct Frame;

        // While one entity node decodes (decodeEntity): its registry and entity
        class EntityScope {
        public:
            EntityScope(EntityRegistry* registry, Entity entity);
            ~EntityScope();
            EntityScope(const EntityScope&) = delete;
            EntityScope& operator=(const EntityScope&) = delete;
        private:
            EntityRegistry* previousRegistry;
            Entity previousEntity;
        };

        // A document block decoded under the rules of a component's top level: unknown enum
        // values of its keys stand in (d) and unknown keys are preserved (c). finish() takes
        // the live encoding of the block right after the decode. Without a registry nothing is
        // preserved (project.yaml and .material files keep theirs with a file overlay, R28):
        // unknown enum values are then reported as dropped.
        class BlockScope {
        public:
            BlockScope(EntityRegistry* registry, const std::string& block, const YAML::Node& node, RootKind kind);
            ~BlockScope();
            void finish(const YAML::Node& live);
            BlockScope(const BlockScope&) = delete;
            BlockScope& operator=(const BlockScope&) = delete;
        private:
            Frame* frame;
            Frame* previous;
            bool finished = false;
        };

        // One block of decodeComponents: decode(node) for compNode[name]; a failure is
        // narrowed to the keys that do not decode (d), or the component is kept whole (e).
        // Then the unknown keys of the component are preserved (b).
        void decodeComponent(ComponentType type, Entity entity, EntityRegistry* registry, const YAML::Node& compNode,
                const std::function<void(const YAML::Node&)>& decode);
        // After the known components: the keys of compNode that name no component (a)
        void preserveUnknownComponents(Entity entity, EntityRegistry* registry, const YAML::Node& compNode);
        // The unknown keys of an entity node or document root (c)
        void preserveRootKeys(EntityRegistry* registry, Entity entity, const YAML::Node& node, RootKind kind);
        // Reported, never preserved: the root keys of a document kept by a file overlay
        // (project.yaml, .material), as Preserved issues
        void reportRootKeys(const YAML::Node& node, RootKind kind);
        // A value a lenient decoder replaced by its default ("expected": "3 numbers"): kept as a
        // stand-in at a component's top level, reported dropped below it
        void invalidValue(const YAML::Node& value, const std::string& expected);
        // A value a reflection codec could not read as its field's type, null included (the decoders
        // before it threw there, S03 then kept the authored text): kept as a stand-in at a
        // component's top level, reported dropped below it ("invalid value (reason)")
        void unreadableValue(const YAML::Node& value, const std::string& reason);
        // A value read, but resolved another way than it says (an Info issue: nothing is lost)
        void fallbackValue(const YAML::Node& value, const std::string& message);
        // The keys of a nested structure inside a component: an unknown one is reported dropped
        void checkNestedKeys(const YAML::Node& node, NestedKind kind);
        // An entity node of a type this editor does not decode (dropped)
        void unknownEntityType(const YAML::Node& entityNode, EntityRegistry* registry, Entity parent);
        // An unknown component name in a prefab instance's override record (dropped)
        void unknownOverrideComponent(const YAML::Node& node, const std::string& name);

        // The path of `target` inside `root` ("submeshes[0].material"), empty when not found
        std::string locate(const YAML::Node& root, const YAML::Node& target);
        // "Player/Arm": the entity's name below its Transform ancestors' names
        std::string entityPath(const EntityRegistry* registry, Entity entity);
    }

    // ---- encoding ----
    namespace StreamSave {
        // The preserved data of an entity's components into an encoded `components` map:
        // stand-ins (d)/(e) while their live value is unchanged (retired otherwise), unknown
        // fields (b), and, when wholeEntity (every component was encoded), unknown components (a)
        void applyToComponents(const EntityRegistry* registry, Entity entity, YAML::Node& compNode, bool wholeEntity);
        // The preserved root keys of an entity node (or of the document, NULL_ENTITY) after
        // the known keys
        void applyRootKeys(const EntityRegistry* registry, Entity entity, YAML::Node& node);
        // A document block's stand-ins and unknown keys (the scene settings)
        void applyBlock(const EntityRegistry* registry, const std::string& block, YAML::Node& node);
    }

}
