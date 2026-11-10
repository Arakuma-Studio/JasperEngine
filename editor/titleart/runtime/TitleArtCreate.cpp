// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtCreate.h"

#include "Project.h"
#include "command/CommandHandle.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/RectTransformComponent.h"
#include "component/TitleArtRendererComponent.h"
#include "component/Transform.h"
#include "subsystem/TitleArtSystem.h"

#include <filesystem>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    // the project-relative path of a dropped file (as component asset paths are written)
    std::string projectRelative(Project* project, const std::string& path){
        const fs::path p(path);
        if (!p.is_absolute()) return p.generic_string();
        std::error_code ec;
        const fs::path relative = fs::relative(p, project->getProjectPath(), ec);
        return ec ? p.generic_string() : relative.generic_string();
    }

}

bool editor::titleArtDropIsUI(Project* project, uint32_t sceneId, Entity parent){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return false;
    if (sceneProject->sceneType == SceneType::SCENE_UI) return true;
    Scene* scene = sceneProject->scene;
    for (Entity e = parent; e != NULL_ENTITY;){
        if (scene->findComponent<RectTransformComponent>(e)) return true;
        const Transform* transform = scene->findComponent<Transform>(e);
        e = transform ? transform->parent : NULL_ENTITY;
    }
    return false;
}

Entity editor::createTitleArtObject(Project* project, uint32_t sceneId, const std::string& titlePath, Entity parent, std::string* error){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene){
        if (error) *error = "no scene to create the Title Art in";
        return NULL_ENTITY;
    }
    const std::string path = projectRelative(project, titlePath);
    std::string name = fs::path(path).filename().string();
    if (name.size() > 9 && name.compare(name.size() - 9, 9, ".titleart") == 0) name = name.substr(0, name.size() - 9);
    if (name.empty()) name = "Title Art";
    const EntityCreationType type = titleArtDropIsUI(project, sceneId, parent) ? EntityCreationType::TITLE_ART_UI : EntityCreationType::TITLE_ART_SPRITE;
    CreateEntityCmd* cmd = new CreateEntityCmd(project, sceneId, name, type, parent);
    cmd->setTitleArt(path);
    CommandHandle::get(sceneId)->addCommandNoMerge(cmd);
    return cmd->getEntity();
}

bool editor::addTitleArtToCompanion(Project* project, uint32_t sceneId, Entity entity, const std::string& titlePath, std::string* error){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || !sceneProject->scene->isEntityCreated(entity)){
        if (error) *error = "the GameObject is gone";
        return false;
    }
    Scene* scene = sceneProject->scene;
    if (TitleArtSystem::spaceOf(scene, entity) == titleart::TitleArtSpace::None){
        if (error) *error = "a Title Art goes on an Image or a Sprite";
        return false;
    }
    const std::string path = projectRelative(project, titlePath);
    auto multi = std::make_unique<MultiPropertyCmd>();
    if (!scene->findComponent<TitleArtRendererComponent>(entity))
        multi->addCommand(std::make_unique<AddComponentCmd>(project, sceneId, entity, ComponentType::TitleArtRendererComponent));
    multi->addPropertyCmd<AssetRef>(project, sceneId, entity, ComponentType::TitleArtRendererComponent, "title", AssetRef(path));
    CommandHandle::get(sceneId)->addCommandNoMerge(multi.release());
    return true;
}
