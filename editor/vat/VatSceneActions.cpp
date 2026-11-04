// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vat/VatSceneActions.h"

#include "Project.h"
#include "Stream.h"
#include "command/CommandHandle.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/ModelLoadCmd.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"
#include "util/ProjectUtils.h"
#include "util/Util.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;
namespace jv = jasperengine::vat;

namespace{

    bool fail(std::string* error, const std::string& message){
        if (error) *error = message;
        return false;
    }

}

bool editor::resolveVatObjectSource(Project* project, const std::string& path, VatObjectSource& out, std::string* error){
    if (!project || project->getProjectPath().empty()) return fail(error, "no project is open");
    if (!Util::isVertexAnimationFile(path)){
        return fail(error, "'" + fs::path(path).filename().string() + "' is not a Vertex Animation (.vat)");
    }
    const fs::path given = FileUtils::pathFromUtf8(path);
    fs::path absolute = given.is_absolute() ? given : project->resolveAssetPath(given);
    std::error_code ec;
    if (!given.is_absolute() && !fs::is_regular_file(absolute, ec) && fs::is_regular_file(project->getProjectPath() / given, ec)){
        absolute = project->getProjectPath() / given;     // a project-relative form
    }
    absolute = absolute.lexically_normal();
    if (!project->isInsideAssetsPath(absolute)){
        return fail(error, "'" + path + "' is outside the assets folder: a Vertex Animator plays a .vat inside it");
    }
    std::string text;
    if (!fs::is_regular_file(absolute, ec) || !ProjectChecks::readFile(absolute, text)){
        return fail(error, "cannot read '" + path + "'");
    }
    jv::VatParseResult parsed = jv::parseVat(text);
    if (!parsed.ok()){
        return fail(error, fs::path(path).filename().string() + ": " + parsed.error);
    }
    out.data = std::move(*parsed.data);
    out.vat = project->normalizeToAssetsRelative(absolute).generic_string();
    out.mesh = (fs::path(out.vat).parent_path() / out.data.mesh).lexically_normal().generic_string();
    const fs::path meshFile = absolute.parent_path() / FileUtils::pathFromUtf8(out.data.mesh);
    if (!fs::is_regular_file(meshFile, ec)){
        return fail(error, "the generated mesh " + out.data.mesh + " of " + absolute.filename().string() +
                           " is missing: rebake (Inspector > Rebake)");
    }
    out.guid = project->getAssetDatabase().getGuid(absolute);
    if (out.guid.empty()) out.guid = out.data.guid;
    std::string stem = absolute.filename().string();
    stem = stem.substr(0, stem.size() - 4);     // drop ".vat"
    out.stem = stem.empty() ? std::string("Vertex Animation") : stem;
    return true;
}

// ---- CreateVatObjectCmd ----

editor::CreateVatObjectCmd::CreateVatObjectCmd(Project* project, uint32_t sceneId, Spec spec)
    : project(project), sceneId(sceneId), spec(std::move(spec)){
    SceneProject* sceneProject = project->getScene(sceneId);
    wasModified = sceneProject && sceneProject->isModified;
}

editor::CreateVatObjectCmd::~CreateVatObjectCmd() = default;

bool editor::CreateVatObjectCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return false;
    if (!createEntity){
        createEntity.reset(new CreateEntityCmd(project, sceneId, spec.name, EntityCreationType::MODEL, spec.parent));
        createEntity->addProperty<Vector3>(ComponentType::Transform, "position", spec.position);
        createEntity->addProperty<Quaternion>(ComponentType::Transform, "rotation", spec.rotation);
        createEntity->addProperty<Vector3>(ComponentType::Transform, "scale", spec.scale);
    }
    if (!createEntity->execute()){
        return false;
    }
    entity = createEntity->getEntity();
    if (!modelLoad){
        modelLoad.reset(new ModelLoadCmd(project, sceneId, entity, spec.mesh));
    }
    if (!modelLoad->execute()){
        createEntity->undo();
        return false;
    }
    Scene* scene = sceneProject->scene;
    if (spec.instanced){
        InstancedMeshComponent instanced;
        instanced.instances = spec.instances;
        instanced.maxInstances = std::max<unsigned int>(instanced.maxInstances, (unsigned int)spec.instances.size());
        ProjectUtils::addEntityComponent(scene, entity, ComponentType::InstancedMeshComponent, sceneProject->entities,
                                         Stream::encodeInstancedMeshComponent(instanced));
    }
    ProjectUtils::addEntityComponent(scene, entity, ComponentType::VertexAnimatorComponent, sceneProject->entities,
                                     Stream::encodeVertexAnimatorComponent(spec.animator));
    sceneProject->isModified = true;
    return true;
}

void editor::CreateVatObjectCmd::undo(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return;
    // the components go with the GameObject; the model's nodes with the model load's undo
    if (modelLoad) modelLoad->undo();
    if (createEntity) createEntity->undo();
    sceneProject->isModified = wasModified;
}

// ---- drag-create ----

Entity editor::createVatObject(Project* project, uint32_t sceneId, const std::string& vatPath, const Vector3& position,
                               Entity parent, std::string* error){
    VatObjectSource source;
    if (!resolveVatObjectSource(project, vatPath, source, error)){
        return NULL_ENTITY;
    }
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene){
        fail(error, "no such scene");
        return NULL_ENTITY;
    }
    if (sceneProject->sceneType != SceneType::SCENE_3D){
        fail(error, "a Vertex Animation plays in a 3D scene");
        return NULL_ENTITY;
    }
    CreateVatObjectCmd::Spec spec;
    spec.name = source.stem;
    spec.parent = parent;
    spec.position = position;
    spec.mesh = source.mesh;
    spec.animator.animation = source.vat;
    spec.animator.guid = source.guid;
    auto* command = new CreateVatObjectCmd(project, sceneId, spec);
    command->setNoMerge();
    if (!CommandHandle::get(sceneId)->addCommand(command)){
        fail(error, "the GameObject could not be created (the mesh " + source.mesh + " did not load)");
        return NULL_ENTITY;
    }
    return command->getEntity();
}
