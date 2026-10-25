// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Catalog.h"
#include "ecs/Entity.h"
#include "math/Quaternion.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

namespace jasperengine::editor{

    class Command;
    class Project;
    struct SceneProject;

    // One edit made while playing that Stop can keep (Play Mode Changes, S24 R26-R30): the
    // value it left, and a factory for a fresh command that sets it on the restored scene
    struct KeepRow{
        enum class Kind{
            Property,       // one property (property = its path)
            Transform,      // position, rotation and scale together
            Component,      // a whole component's values (property = the differing keys)
            AddComponent,
            RemoveComponent,
            Name,
            State,          // active, tag or layer (property names which)
            Created,        // a GameObject (with its children) made while playing
            Deleted,
            Unsupported     // a step that can't be kept
        };

        uint64_t id = 0;
        Kind kind = Kind::Property;
        Entity entity = NULL_ENTITY;
        Entity parent = NULL_ENTITY;        // Created: where it goes back
        bool hasComponent = false;
        ComponentType component = ComponentType::Transform;
        std::string property;
        std::string gameObject;             // its name at Stop
        std::string playValue;              // display text
        std::string editValue;              // display text, read from the restored scene
        std::string source;                 // the play-time step's name
        bool keepable = true;
        std::string reason;                 // why not
        // The GameObject was made by a command while playing: it exists again only when its
        // Created row is kept
        bool madeInPlay = false;

        std::function<std::unique_ptr<Command>()> factory;
        std::function<std::string()> readEditValue;

        bool sameTarget(const KeepRow& other) const{
            return kind == other.kind && entity == other.entity && hasComponent == other.hasComponent &&
                (!hasComponent || component == other.component) && property == other.property;
        }
    };

    // What a command's collectKeepCommands writes to, during Stop with the play-time scene live
    struct KeepContext{
        Project* project = nullptr;
        uint32_t sceneId = 0;
        SceneProject* sceneProject = nullptr;
        std::string source;                 // the step being collected
        std::vector<KeepRow> rows;
        uint64_t nextId = 1;
        bool handled = false;

        // Not authored and not made by a command: what a script spawned
        bool runtimeOnly(Entity entity) const;
        bool madeInPlay(Entity entity) const;
        std::string name(Entity entity) const;

        // Adds a row (the last write to a target wins). A row on a GameObject a script made is
        // disabled; a GameObject made and deleted again while playing leaves nothing.
        void add(KeepRow row);
        // The command's rows; a command that adds none gets one disabled "can't be kept" row
        void collect(const Command& cmd, const std::string& stepName);
        // A command that leaves nothing to keep on purpose (a selection change) says so
        void nothingToKeep() { handled = true; }

        // Rows the commands share
        // A whole component's values now (kept with a ComponentValuesCmd); `added`: the
        // component was added while playing, so the row adds it too
        void addComponentValues(Entity entity, ComponentType type, bool added = false);
        // A GameObject made while playing, with its children, as it is now: kept by pasting it
        // back under its parent with the same ids
        void addCreated(Entity root);
        void addDeleted(Entity entity, const std::string& gameObject);
        void addRemovedComponent(Entity entity, ComponentType type);

        // A value as the review shows it
        template<typename T>
        static std::string text(const T& value){
            if constexpr (std::is_same_v<T, bool>){
                return value ? "true" : "false";
            }else if constexpr (std::is_enum_v<T>){
                return std::to_string(static_cast<long long>(value));
            }else if constexpr (std::is_integral_v<T>){
                return std::to_string(value);
            }else if constexpr (std::is_floating_point_v<T>){
                return number(value);
            }else if constexpr (std::is_same_v<T, std::string>){
                return "\"" + value + "\"";
            }else if constexpr (std::is_same_v<T, Vector2>){
                return "(" + number(value.x) + ", " + number(value.y) + ")";
            }else if constexpr (std::is_same_v<T, Vector3>){
                return "(" + number(value.x) + ", " + number(value.y) + ", " + number(value.z) + ")";
            }else if constexpr (std::is_same_v<T, Vector4>){
                return "(" + number(value.x) + ", " + number(value.y) + ", " + number(value.z) + ", " + number(value.w) + ")";
            }else if constexpr (std::is_same_v<T, Quaternion>){
                return "(" + number(value.w) + ", " + number(value.x) + ", " + number(value.y) + ", " + number(value.z) + ")";
            }else{
                return "(edited)";
            }
        }
        static std::string number(double value);
    };

    // The play-time edits Stop found, per scene, until they are kept or discarded
    class PlayChangeReview{
    public:
        struct Pending{
            uint32_t sceneId = 0;
            std::string sceneName;
            std::vector<KeepRow> rows;
        };

        struct ApplyResult{
            bool applied = false;
            uint64_t entryId = 0;           // the "Apply Play Mode Changes (N)" step
            std::string entryName;
            std::vector<uint64_t> kept;
            std::vector<std::pair<uint64_t, std::string>> skipped;  // row id, why
        };

        static PlayChangeReview& get();

        // Editor Settings > Undo > Review Play Mode Changes (off: Stop drops play edits as before)
        static bool enabled;
        // Rows were found at a Stop: the editor opens the Play Mode Changes window (unless
        // the Stop came from an agent, then it only logs)
        std::function<void(uint32_t sceneId, size_t rows)> onPending;

        // finalizeStop, before the snapshot restore: walks the play session's steps (and the
        // Keep Values After Play marks) while the play-time state is live
        void capture(Project* project, SceneProject* sceneProject);
        // After the restore: which rows can be kept, and their edit-time values
        void afterRestore(Project* project, SceneProject* sceneProject);

        // Keep Values After Play (during Play): the component's current values, or every
        // component of the GameObject, become rows at Stop
        void markComponent(uint32_t sceneId, Entity entity, ComponentType type);
        void markGameObject(uint32_t sceneId, Entity entity);
        bool isMarked(uint32_t sceneId, Entity entity, ComponentType type) const;

        // The next Stop of an agent: log instead of opening the window
        void setQuietStop(bool quiet) { quietStop = quiet; }
        bool isQuietStop() const { return quietStop; }

        const Pending* pending(uint32_t sceneId) const;
        std::vector<uint32_t> pendingScenes() const;
        size_t pendingCount(uint32_t sceneId) const;

        // Keeps the chosen rows (all when ids is null) as one undo step of the scene, origin
        // PlayReview, in capture order; the rest is discarded. A row whose command fails is
        // skipped and reported.
        ApplyResult apply(Project* project, uint32_t sceneId, const std::vector<uint64_t>* ids);
        void discard(uint32_t sceneId);
        void discardAll();

    private:
        std::map<uint32_t, Pending> reviews;
        struct Mark{
            Entity entity = NULL_ENTITY;
            std::optional<ComponentType> component;   // none: every component
        };
        std::map<uint32_t, std::vector<Mark>> marks;
        bool quietStop = false;
    };

}
