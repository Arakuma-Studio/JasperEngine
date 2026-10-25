// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AtmospherePresetCmd.h"

#include "Project.h"
#include "command/CommandGroup.h"
#include "command/CommandNames.h"
#include "command/type/PropertyCmd.h"
#include "render/AtmospherePresets.h"

#include <memory>

using namespace jasperengine;

namespace {

    // a PropertyCmd setting `name` to the value `preset` holds, when it differs from `live`
    std::unique_ptr<editor::Command> propertyEdit(editor::Project* project, uint32_t sceneId, Entity entity, editor::ComponentType component,
                                                  const std::string& name, const editor::PropertyData& live, const editor::PropertyData& preset){
        switch (live.type){
            case editor::PropertyType::Bool:
                if (*static_cast<bool*>(live.ref) == *static_cast<bool*>(preset.ref)) return nullptr;
                return std::make_unique<editor::PropertyCmd<bool>>(project, sceneId, entity, component, name, *static_cast<bool*>(preset.ref));
            case editor::PropertyType::Float:
                if (*static_cast<float*>(live.ref) == *static_cast<float*>(preset.ref)) return nullptr;
                return std::make_unique<editor::PropertyCmd<float>>(project, sceneId, entity, component, name, *static_cast<float*>(preset.ref));
            case editor::PropertyType::Vector3:
                if (*static_cast<Vector3*>(live.ref) == *static_cast<Vector3*>(preset.ref)) return nullptr;
                return std::make_unique<editor::PropertyCmd<Vector3>>(project, sceneId, entity, component, name, *static_cast<Vector3*>(preset.ref));
            case editor::PropertyType::Enum:
                if (*static_cast<int*>(live.ref) == *static_cast<int*>(preset.ref)) return nullptr;
                return std::make_unique<editor::PropertyCmd<int>>(project, sceneId, entity, component, name, *static_cast<int*>(preset.ref));
            default:
                // presets set no other kind of field (textures stay)
                return nullptr;
        }
    }

    // the component of `entity` as the preset makes it, false for an unknown preset
    template<typename T>
    bool presetCopy(Scene* scene, Entity entity, const std::string& presetId, T& copy);

    template<>
    bool presetCopy<FogComponent>(Scene* scene, Entity entity, const std::string& presetId, FogComponent& copy){
        copy = scene->getComponent<FogComponent>(entity);
        return AtmospherePresets::applyFogPreset(presetId, copy);
    }

    template<>
    bool presetCopy<VolumetricCloudsComponent>(Scene* scene, Entity entity, const std::string& presetId, VolumetricCloudsComponent& copy){
        copy = scene->getComponent<VolumetricCloudsComponent>(entity);
        return AtmospherePresets::applyCloudsPreset(presetId, copy);
    }

    template<typename T>
    bool appendEdits(editor::Project* project, uint32_t sceneId, Scene* scene, Entity entity, editor::ComponentType component,
                     const std::string& presetId, std::vector<std::unique_ptr<editor::Command>>& commands, std::string& error){
        if (!scene->findComponent<T>(entity)){
            error = "GameObject " + scene->getEntityName(entity) + " has no " + editor::Catalog::getComponentDisplayName(component) + " component";
            return false;
        }
        T copy;
        if (!presetCopy<T>(scene, entity, presetId, copy)){
            error = "Unknown " + editor::Catalog::getComponentDisplayName(component) + " preset '" + presetId + "'";
            return false;
        }
        const std::map<std::string, editor::PropertyData> live = editor::Catalog::findEntityProperties(scene, entity, component);
        const std::map<std::string, editor::PropertyData> preset = editor::Catalog::getProperties(component, &copy);
        for (const auto& [name, property] : live){
            auto presetProperty = preset.find(name);
            if (presetProperty == preset.end() || !property.ref || !presetProperty->second.ref || property.type != presetProperty->second.type)
                continue;
            if (std::unique_ptr<editor::Command> edit = propertyEdit(project, sceneId, entity, component, name, property, presetProperty->second))
                commands.push_back(std::move(edit));
        }
        return true;
    }

}

editor::Command* editor::AtmospherePresetCmd::build(Project* project, uint32_t sceneId, const std::vector<Entity>& entities, ComponentType component,
                                                    const std::string& presetId, std::string& error){
    error.clear();
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    if (!sceneProject || !sceneProject->scene){
        error = "Scene not found";
        return nullptr;
    }
    if (component != ComponentType::FogComponent && component != ComponentType::VolumetricCloudsComponent){
        error = "Atmosphere presets apply to Fog and Volumetric Clouds";
        return nullptr;
    }
    Scene* scene = sceneProject->scene;
    std::vector<std::unique_ptr<Command>> commands;
    for (Entity entity : entities){
        const bool ok = component == ComponentType::FogComponent
            ? appendEdits<FogComponent>(project, sceneId, scene, entity, component, presetId, commands, error)
            : appendEdits<VolumetricCloudsComponent>(project, sceneId, scene, entity, component, presetId, commands, error);
        if (!ok)
            return nullptr;
    }
    if (commands.empty())
        return nullptr;
    const std::string label = CommandNames::verb("Apply Atmosphere Preset", CommandNames::entities(project, sceneId, entities));
    return new CommandGroup(std::move(commands), label, false);
}
