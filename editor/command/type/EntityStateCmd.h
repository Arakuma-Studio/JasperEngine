// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/CommandNames.h"
#include "command/EntityEdit.h"
#include "Project.h"
#include "command/PrefabEditUndo.h"
#include <cstdint>
#include <map>
#include <string>

namespace jasperengine::editor{

    // GameObject state edited in the Inspector header: active, tag or layer
    enum class EntityStateField{
        Active,
        Tag,
        Layer
    };

    struct EntityStateValue{
        bool active = true;
        std::string tag;
        int layer = 0;
    };

    // Sets one GameObject field on one or more entities as a single undo step
    class EntityStateCmd: public Command, public EntityEdit{

    private:
        Project* project;
        uint32_t sceneId;
        EntityStateField field;
        EntityStateValue newValue;
        std::map<Entity, EntityStateValue> oldValues;
        bool wasModified = false;
        bool differs = false;   // the last execute changed a GameObject
        // Unity: a prefab instance's GameObject state edited is an override of it
        PrefabEditUndo prefabUndo;

        static EntityStateValue read(Scene* scene, Entity entity){
            EntityStateValue value;
            value.active = scene->isEntityActive(entity);
            value.tag = scene->getEntityTag(entity);
            value.layer = scene->getEntityLayer(entity);
            return value;
        }

        void write(Scene* scene, Entity entity, const EntityStateValue& value) const{
            switch (field){
                case EntityStateField::Active: scene->setEntityActive(entity, value.active); break;
                case EntityStateField::Tag: scene->setEntityTag(entity, value.tag); break;
                case EntityStateField::Layer: scene->setEntityLayer(entity, value.layer); break;
            }
        }

        uint8_t stateBit() const{
            switch (field){
                case EntityStateField::Active: return PrefabObjectOverrides::StateActive;
                case EntityStateField::Tag: return PrefabObjectOverrides::StateTag;
                case EntityStateField::Layer: return PrefabObjectOverrides::StateLayer;
            }
            return 0;
        }

    public:
        EntityStateCmd(Project* project, uint32_t sceneId, const std::vector<Entity>& entities, EntityStateField field, const EntityStateValue& value){
            this->project = project;
            this->sceneId = sceneId;
            this->field = field;
            this->newValue = value;
            for (Entity entity : entities){
                oldValues[entity] = EntityStateValue();
            }
        }

        static EntityStateCmd* active(Project* project, uint32_t sceneId, const std::vector<Entity>& entities, bool active){
            EntityStateValue value;
            value.active = active;
            return new EntityStateCmd(project, sceneId, entities, EntityStateField::Active, value);
        }

        static EntityStateCmd* tag(Project* project, uint32_t sceneId, const std::vector<Entity>& entities, const std::string& tag){
            EntityStateValue value;
            value.tag = tag;
            return new EntityStateCmd(project, sceneId, entities, EntityStateField::Tag, value);
        }

        static EntityStateCmd* layer(Project* project, uint32_t sceneId, const std::vector<Entity>& entities, int layer){
            EntityStateValue value;
            value.layer = layer;
            return new EntityStateCmd(project, sceneId, entities, EntityStateField::Layer, value);
        }

        bool execute() override{
            SceneProject* sceneProject = project->getScene(sceneId);
            if (!sceneProject){
                return false;
            }

            wasModified = sceneProject->isModified;

            bool changed = false;
            differs = false;
            for (auto& [entity, oldValue] : oldValues){
                // a Play-created entity is gone after Stop
                if (!sceneProject->scene->isEntityCreated(entity)){
                    continue;
                }
                oldValue = read(sceneProject->scene, entity);
                prefabUndo.capture(project, sceneId, entity);
                write(sceneProject->scene, entity, newValue);
                const EntityStateValue now = read(sceneProject->scene, entity);
                const bool entityDiffers = (field == EntityStateField::Active && now.active != oldValue.active) ||
                    (field == EntityStateField::Tag && now.tag != oldValue.tag) ||
                    (field == EntityStateField::Layer && now.layer != oldValue.layer);
                if (entityDiffers){
                    project->recordPrefabStateEdit(sceneId, entity, stateBit());
                    differs = true;
                }
                changed = true;
            }

            if (differs){
                sceneProject->isModified = true;
            }

            return changed;
        }

        void undo() override{
            SceneProject* sceneProject = project->getScene(sceneId);
            if (!sceneProject){
                return;
            }

            for (auto& [entity, oldValue] : oldValues){
                if (sceneProject->scene->isEntityCreated(entity)){
                    write(sceneProject->scene, entity, oldValue);
                }
            }
            prefabUndo.restore(project, sceneId);

            sceneProject->isModified = wasModified;
        }

        // the Hierarchy greys out inactive GameObjects
        bool affectsStructure() const override{
            return field == EntityStateField::Active;
        }

        // "Set Active (Player)", "Set Tag (3 Objects)", "Set Layer (Sun)"
        std::string describe() const override{
            return "Set " + editedLabel() + " (" + CommandNames::entities(project, sceneId, editedEntities()) + ")";
        }
        const char* typeName() const override{ return "EntityStateCmd"; }
        bool isNoOp() const override{ return !differs; }

        Project* editedProject() const override{ return project; }
        uint32_t editedSceneId() const override{ return sceneId; }
        std::vector<Entity> editedEntities() const override{
            std::vector<Entity> entities;
            entities.reserve(oldValues.size());
            for (auto const& [entity, value] : oldValues){
                entities.push_back(entity);
            }
            return entities;
        }
        std::string editedLabel() const override{
            switch (field){
                case EntityStateField::Active: return "Active";
                case EntityStateField::Tag: return "Tag";
                case EntityStateField::Layer: return "Layer";
            }
            return "State";
        }

        EntityStateField getField() const{ return field; }
    };

}
