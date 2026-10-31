// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <any>
#include <functional>

namespace jasperengine::editor{

    enum class EntityCreationType{
        EMPTY,
        GAMEOBJECT,
        BOX,
        PLANE,
        WALL,
        MIRROR,
        SPHERE,
        CYLINDER,
        CAPSULE,
        TORUS,
        IMAGE,
        SPRITE,
        TILEMAP,
        TEXT,
        BUTTON,
        SCROLLBAR,
        PROGRESSBAR,
        INPUT_FIELD,
        PANEL,
        POLYGON,
        LAYOUT_GROUP,
        CANVAS_GROUP,
        POINT_LIGHT,
        DIRECTIONAL_LIGHT,
        SPOT_LIGHT,
        POINT_LIGHT_2D,
        SHADOW_CASTER_2D,
        JOINT2D,
        JOINT3D,
        RIGIDBODY2D,
        RIGIDBODY,
        SKYBOX,
        FOG,
        CAMERA,
        AUDIO_SOURCE,
        AUDIO_SOURCE_3D,
        ANIMATION,
        SPRITE_ANIMATION,
        POSITION_TWEEN,
        ROTATION_TWEEN,
        SCALE_TWEEN,
        COLOR_TWEEN,
        ALPHA_TWEEN,
        TRANSLATE_TRACKS,
        ROTATE_TRACKS,
        SCALE_TRACKS,
        MORPH_TRACKS,
        LATTICE_TRACKS,
        MODEL,
        PARTICLE_SYSTEM,
        POINTS,
        LINE_RENDERER,
        TRAIL_RENDERER,
        MESH_POLYGON,
        TERRAIN,
        REFLECTION_PROBE,
        UI_DOCUMENT,
        WATER,
        FLUID_SIMULATION,
        // a unit lattice (Transform + LatticeComponent); the menus create one through
        // LatticeEditorOps::createLattice, which fits and binds it around the selection
        LATTICE
    };

    class CreateEntityCmd: public Command{

    private:
        struct ChildEntityData{
            Entity entity = NULL_ENTITY;
            Entity parent = NULL_ENTITY;
        };

        Project* project;
        uint32_t sceneId;
        std::string entityName;

        Entity entity;
        std::vector<ChildEntityData> childEntities;
        Entity parent;
        EntityCreationType type;
        bool addToPrefab;
        bool quiet = false;
        bool wasModified;
        bool wasMainCamera;
        // WATER: the library:underwater pass it added to the scene's post-processing (-1: none)
        int addedUnderwaterPass = -1;

        // Component type -> property name -> property setter function
        std::unordered_map<ComponentType, std::unordered_map<std::string, std::function<void(Entity)>>> propertySetters;
        uint64_t updateFlags;

    public:
        CreateEntityCmd(Project* project, uint32_t sceneId, std::string entityName, bool addToPrefab = false);
        CreateEntityCmd(Project* project, uint32_t sceneId, std::string entityName, EntityCreationType type, bool addToPrefab = false);
        CreateEntityCmd(Project* project, uint32_t sceneId, std::string entityName, EntityCreationType type, Entity parent, bool addToPrefab = false);

        bool execute() override;
        void undo() override;


        Entity getEntity();

        // Skips the selection change, scene focus and log line, for brush-rate creation
        void setQuiet(bool quiet);

        template<typename T>
        void addProperty(ComponentType componentType, const std::string& propertyName, T value) {
            Scene* scene = project->getScene(sceneId)->scene;

            auto properties = Catalog::getProperties(componentType, nullptr);
            auto it = properties.find(propertyName);
            if (it != properties.end()) {
                this->updateFlags |= it->second.updateFlags;
            }

            propertySetters[componentType][propertyName] = [value, scene, propertyName, componentType](Entity entity) {
                T* valueRef = Catalog::getPropertyRef<T>(scene, entity, componentType, propertyName);
                if (valueRef != nullptr) {
                    *valueRef = value;
                }
            };
        }
    };

}
