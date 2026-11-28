// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LinkFogProfileCmd.h"

#include "Catalog.h"
#include "FogProfileAssets.h"
#include "Project.h"
#include "Scene.h"
#include "asset/AssetTypes.h"
#include "command/CommandNames.h"
#include "command/type/FogProfileAssetCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/FogComponent.h"
#include "component/FogZoneComponent.h"
#include "util/AssetDatabase.h"

#include <memory>

using namespace jasperengine;
using namespace jasperengine::editor;

// file-local, in a namespace of its own: a unity build merges the editor's sources
namespace jasperengine::editor::linkfogprofile {
namespace {

    bool sameRef(const AssetRef& a, const AssetRef& b) {
        return a.path == b.path && a.guid == b.guid;
    }

    const AssetRef* profileRefOf(Scene* scene, Entity entity, ComponentType component) {
        if (component == ComponentType::FogComponent) {
            const FogComponent* fog = scene->findComponent<FogComponent>(entity);
            return fog ? &fog->profile : nullptr;
        }
        if (!scene->isComponentRegistered<FogZoneComponent>()) return nullptr;
        const FogZoneComponent* zone = scene->findComponent<FogZoneComponent>(entity);
        return zone ? &zone->profile : nullptr;
    }

    // the scene, the component kind and every GameObject having it, or false with the error
    Scene* checkTargets(Project* project, uint32_t sceneId, const std::vector<Entity>& entities, ComponentType component, std::string& error) {
        error.clear();
        SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
        if (!sceneProject || !sceneProject->scene) {
            error = "Scene not found";
            return nullptr;
        }
        if (component != ComponentType::FogComponent && component != ComponentType::FogZoneComponent) {
            error = "Fog Profiles link to a Fog or a Fog Zone";
            return nullptr;
        }
        if (entities.empty()) {
            error = "No GameObject given";
            return nullptr;
        }
        for (Entity entity : entities) {
            if (!profileRefOf(sceneProject->scene, entity, component)) {
                error = "GameObject " + sceneProject->scene->getEntityName(entity) + " has no " + Catalog::getComponentDisplayName(component) +
                        " component";
                return nullptr;
            }
        }
        return sceneProject->scene;
    }

    // the reference and the Type switch on every GameObject, after `prefix` (a file it creates)
    LinkFogProfileCmd* buildLink(Project* project, uint32_t sceneId, Scene* scene, const std::vector<Entity>& entities, ComponentType component,
                                 std::unique_ptr<Command> prefix, const AssetRef& ref, const FogProfileData& data, std::string& error) {
        const std::string name = data.name.empty() ? CommandNames::file(ref.path) : data.name;
        // the GameObjects in brackets: "Link Fog Profile Night Fog" would read as one name
        auto command = std::make_unique<LinkFogProfileCmd>("Link Fog Profile " + name + " (" + CommandNames::entities(project, sceneId, entities) + ")");
        if (prefix) command->addCommand(std::move(prefix));
        const bool height = LinkFogProfileCmd::setsHeightFields(data);
        bool changes = false;
        for (Entity entity : entities) {
            const AssetRef* current = profileRefOf(scene, entity, component);
            if (!sameRef(*current, ref)) {
                command->addPropertyCmd<AssetRef>(project, sceneId, entity, component, "profile", ref);
                changes = true;
            }
            // as the presets did: a profile of height fields makes the Fog a Height fog
            if (component == ComponentType::FogComponent && height && scene->getComponent<FogComponent>(entity).type != FogType::HEIGHT) {
                command->addPropertyCmd<int>(project, sceneId, entity, component, "type", static_cast<int>(FogType::HEIGHT));
                changes = true;
            }
        }
        if (!changes) {
            error.clear();
            return nullptr;
        }
        return command.release();
    }

}
}

editor::LinkFogProfileCmd::LinkFogProfileCmd(std::string label) : label(std::move(label)) {
    // one step of its own: a later edit never folds into it
    setNoMerge();
}

bool editor::LinkFogProfileCmd::isHeightField(FogField field) {
    return field != FogField::Color && field != FogField::Density && field != FogField::LinearStart && field != FogField::LinearEnd;
}

bool editor::LinkFogProfileCmd::setsHeightFields(const FogProfileData& profile) {
    for (size_t i = 0; i < FOG_FIELD_COUNT; i++) {
        if (profile.set.test(i) && isHeightField(static_cast<FogField>(i))) return true;
    }
    return false;
}

FogComponent editor::LinkFogProfileCmd::resolvedLook(const FogComponent& fog, const FogProfileData* profile) {
    FogComponent look = fog;
    if (profile) profile->applyTo(look);
    return look;
}

std::unique_ptr<editor::Command> editor::fogFieldPropertyCmd(Project* project, uint32_t sceneId, Entity entity, FogField field, const FogComponent& values) {
    const char* name = FogFields::name(field);
    const ComponentType component = ComponentType::FogComponent;
    switch (FogFields::info(field).kind) {
        case FogFieldKind::Float:
        case FogFieldKind::Extinction: {
            float value = 0.0f;
            FogFields::read(values, field, &value);
            return std::make_unique<PropertyCmd<float>>(project, sceneId, entity, component, name, value);
        }
        case FogFieldKind::Color:
        case FogFieldKind::Vector3: {
            float value[3] = {0.0f, 0.0f, 0.0f};
            FogFields::read(values, field, value);
            return std::make_unique<PropertyCmd<Vector3>>(project, sceneId, entity, component, name, Vector3(value[0], value[1], value[2]));
        }
        case FogFieldKind::Bool: {
            float value = 0.0f;
            FogFields::read(values, field, &value);
            return std::make_unique<PropertyCmd<bool>>(project, sceneId, entity, component, name, value != 0.0f);
        }
        case FogFieldKind::Enum: {
            float value = 0.0f;
            FogFields::read(values, field, &value);
            return std::make_unique<PropertyCmd<int>>(project, sceneId, entity, component, name, static_cast<int>(value));
        }
    }
    return nullptr;
}

editor::LinkFogProfileCmd* editor::LinkFogProfileCmd::link(Project* project, uint32_t sceneId, const std::vector<Entity>& entities, ComponentType component,
                                                           const std::string& profile, std::string& error) {
    Scene* scene = linkfogprofile::checkTargets(project, sceneId, entities, component, error);
    if (!scene) return nullptr;
    const fogprofiles::ResolvedFogProfile resolved = fogprofiles::resolveProfileName(*project, profile);
    if (!resolved.ok) {
        error = resolved.error;
        return nullptr;
    }
    std::unique_ptr<Command> copy;
    AssetRef ref;
    if (resolved.needsCopy) {
        // a built-in's first use: its project copy, made in this step (R4)
        const Guid guid = Guid::parse(AssetDatabase::generateGuid());
        copy = std::make_unique<FogProfileAssetCmd>(project, resolved.assetPath, std::string(), resolved.copyText, guid);
        ref = AssetRef(resolved.assetPath, guid);
    } else {
        ref = project->makeAssetRef(resolved.assetPath, AssetPathBase::Assets);
        if (ref.path.empty()) ref = AssetRef(resolved.assetPath);
    }
    return linkfogprofile::buildLink(project, sceneId, scene, entities, component, std::move(copy), ref, resolved.data, error);
}

editor::LinkFogProfileCmd* editor::LinkFogProfileCmd::linkNew(Project* project, uint32_t sceneId, const std::vector<Entity>& entities, ComponentType component,
                                                              const std::string& assetPath, const FogProfileData& data, std::string& error) {
    Scene* scene = linkfogprofile::checkTargets(project, sceneId, entities, component, error);
    if (!scene) return nullptr;
    if (!FogProfileIO::isProfilePath(assetPath)) {
        error = "'" + assetPath + "' is not a .fogprofile path";
        return nullptr;
    }
    std::unique_ptr<FogProfileAssetCmd> create(FogProfileAssetCmd::create(project, assetPath, data));
    const AssetRef ref(create->getPath(), create->getGuid());
    return linkfogprofile::buildLink(project, sceneId, scene, entities, component, std::move(create), ref, data, error);
}

editor::LinkFogProfileCmd* editor::LinkFogProfileCmd::unlink(Project* project, uint32_t sceneId, const std::vector<Entity>& entities, ComponentType component,
                                                             std::string& error) {
    Scene* scene = linkfogprofile::checkTargets(project, sceneId, entities, component, error);
    if (!scene) return nullptr;
    auto command = std::make_unique<LinkFogProfileCmd>(CommandNames::verb("Unlink Fog Profile", CommandNames::entities(project, sceneId, entities)));
    for (Entity entity : entities) {
        const AssetRef* ref = linkfogprofile::profileRefOf(scene, entity, component);
        if (!ref->isSet()) continue;
        if (component == ComponentType::FogComponent) {
            // the look stays: what the profile set becomes this Fog's own (R23)
            const FogComponent& fog = scene->getComponent<FogComponent>(entity);
            if (std::shared_ptr<const FogProfileData> profile = FogProfileLibrary::getRef(*ref)) {
                const FogComponent look = resolvedLook(fog, profile.get());
                for (size_t i = 0; i < FOG_FIELD_COUNT; i++) {
                    const FogField field = static_cast<FogField>(i);
                    if (profile->isSet(field) && !FogFields::equal(fog, look, field))
                        command->addCommand(fogFieldPropertyCmd(project, sceneId, entity, field, look));
                }
            }
        }
        command->addPropertyCmd<AssetRef>(project, sceneId, entity, component, "profile", AssetRef());
    }
    if (command->empty()) {
        error.clear();
        return nullptr;
    }
    return command.release();
}
