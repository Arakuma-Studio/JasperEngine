// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/CommandNames.h"
#include "command/PlayChangeReview.h"
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
        // ui-multitouch: on-screen controls (the stick in Joystick Pack's three modes)
        ONSCREEN_STICK_FIXED,
        ONSCREEN_STICK_FLOATING,
        ONSCREEN_STICK_DYNAMIC,
        ONSCREEN_BUTTON,
        TOUCH_AREA,
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
        // a version 2 Particle System on its own GameObject (Unity's: the default quad renderer)
        PARTICLE_SYSTEM,
        FORCE_FIELD,
        // a Particle System in UI units under a UI element (unscaled time, local space, the soft
        // image): its particles drawn in canvas order, clipped by panels
        UI_PARTICLE_SYSTEM,
        POINTS,
        LINE_RENDERER,
        TRAIL_RENDERER,
        MESH_POLYGON,
        TERRAIN,
        REFLECTION_PROBE,
        UI_DOCUMENT,
        WATER,
        FLUID_SIMULATION,
        // a Video Player into its render texture (video|<entity>), and one on the main
        // camera's far plane (a looping background)
        VIDEO_PLAYER,
        VIDEO_BACKGROUND,
        // a unit lattice (Transform + LatticeComponent); the menus create one through
        // LatticeEditorOps::createLattice, which fits and binds it around the selection
        LATTICE,
        LOCAL_VOLUMETRIC_FOG,
        VOLUMETRIC_CLOUDS,
        // GameObject › Spline (S18 R23): a spline configured by its preset (SplinePresets).
        // SPLINE_DRAW is an empty spline the Draw Spline tool starts on.
        SPLINE_EMPTY,
        SPLINE_DRAW,
        SPLINE_ROAD,
        SPLINE_PATH,
        SPLINE_WALL,
        SPLINE_FENCE,
        SPLINE_RIVER,
        SPLINE_LAKE,
        SPLINE_CIRCLE,
        SPLINE_SQUARE,
        SPLINE_EDGE_2D,
        SPLINE_FILL_2D,
        SPLINE_CIRCLE_2D,
        // not a GameObject: the Hierarchy's "3D Object > Ragdoll..." opens the wizard on the selection
        RAGDOLL_WIZARD,
        // a full-stretch RectTransform the Safe Area component drives (GameObject > UI > Safe Area)
        SAFE_AREA,
        // the legacy action timeline's sound (S40 R17, R18): an action with an AudioSource that plays
        // a clip or a Sound Event, and a Mixer Snapshot action
        AUDIO_ACTION,
        SOUND_EVENT_ACTION,
        MIXER_SNAPSHOT_ACTION,
        // GameObject > UI > Title Art and 2D Object > Title Art (S81 R6): an Image or a Sprite with a
        // Title Art Renderer (setTitleArt: its asset, at the bake's native size)
        TITLE_ART_UI,
        TITLE_ART_SPRITE
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
        // TITLE_ART_*: the .titleart it shows (project-relative), and the step's name when set
        std::string titleArtPath;
        std::string description;

        // Component type -> property name -> property setter function
        std::unordered_map<ComponentType, std::unordered_map<std::string, std::function<void(Entity)>>> propertySetters;
        uint64_t updateFlags;

    public:
        CreateEntityCmd(Project* project, uint32_t sceneId, std::string entityName, bool addToPrefab = false);
        CreateEntityCmd(Project* project, uint32_t sceneId, std::string entityName, EntityCreationType type, bool addToPrefab = false);
        CreateEntityCmd(Project* project, uint32_t sceneId, std::string entityName, EntityCreationType type, Entity parent, bool addToPrefab = false);

        bool execute() override;
        void undo() override;

        // "Create Cube": the name it got (or the name set with setDescription)
        std::string describe() const override { return description.empty() ? "Create " + entityName : description; }
        const char* typeName() const override { return "CreateEntityCmd"; }
        // The GameObject as it is now, pasted back with its id
        void collectKeepCommands(KeepContext& context) const override { context.addCreated(entity); }
        size_t memoryBytes() const override {
            return sizeof(*this) + entityName.size() + childEntities.capacity() * sizeof(ChildEntityData);
        }
        uint32_t getSceneId() const { return sceneId; }
        Entity getParent() const { return parent; }


        Entity getEntity();

        // Skips the selection change, scene focus and log line, for brush-rate creation
        void setQuiet(bool quiet);
        // TITLE_ART_*: the title it shows; the step is "Create Title Art (<stem>)" (S81 R6)
        void setTitleArt(const std::string& projectRelativePath);
        void setDescription(const std::string& text) { description = text; }

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
