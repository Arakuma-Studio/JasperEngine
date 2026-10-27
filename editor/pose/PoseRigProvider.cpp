// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PoseRigProvider.h"

#include "Project.h"
#include "Scene.h"
#include "component/ModelComponent.h"
#include "object/Model.h"
#include "pose/HumanoidMapIO.h"
#include "pose/ModelRig.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"

using namespace jasperengine;

namespace fs = std::filesystem;

namespace {

    struct Stamp {
        int64_t time = 0;
        uint64_t size = 0;
        bool exists = false;
        bool operator==(const Stamp& o) const { return time == o.time && size == o.size && exists == o.exists; }
    };

    Stamp stampOf(const fs::path& path){
        Stamp stamp;
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) return stamp;
        const auto time = fs::last_write_time(path, ec);
        if (!ec) stamp.time = static_cast<int64_t>(time.time_since_epoch().count());
        const auto size = fs::file_size(path, ec);
        if (!ec) stamp.size = static_cast<uint64_t>(size);
        stamp.exists = true;
        return stamp;
    }

    struct CacheEntry {
        Stamp model, sidecar;
        std::shared_ptr<const editor::PoseRigInfo> info;
    };

    std::map<std::string, CacheEntry>& cache(){
        static std::map<std::string, CacheEntry> entries;
        return entries;
    }

    fs::path sidecarOf(const fs::path& model){
        return editor::FileUtils::pathFromUtf8(pose::humanoidSidecarPath(editor::FileUtils::pathToUtf8(model)));
    }

    // The sidecar's overrides (a missing sidecar is all auto); false + error when it is unreadable
    bool readOverrides(editor::Project* project, const fs::path& sidecar, pose::HumanoidOverrides& overrides,
                       std::vector<std::string>& notes, std::string& error){
        std::error_code ec;
        if (!fs::exists(sidecar, ec)) return true;
        std::string text;
        std::vector<std::string> warnings;
        const std::string rel = editor::ProjectChecks::projectRelative(project->getProjectPath(), sidecar);
        if (!editor::ProjectChecks::readFile(sidecar, text) || !pose::parseHumanoidOverrides(text, overrides, warnings, &error)){
            error = rel + ": " + (error.empty() ? std::string("cannot be read") : error);
            return false;
        }
        for (const std::string& warning : warnings) notes.push_back(rel + ": " + warning);
        return true;
    }

    // Skeleton, humanoid map, skin and rig of a loaded model entity
    void buildRig(editor::Project* project, Scene* scene, Entity model, const fs::path& modelFile, editor::PoseRigInfo& info){
        info.model = editor::ProjectChecks::projectRelative(project->getProjectPath(), modelFile);
        pose::HumanoidOverrides overrides;
        if (!readOverrides(project, sidecarOf(modelFile), overrides, info.notes, info.error)){
            return;
        }
        pose::Skeleton skeleton;
        if (!pose::buildSkeleton(scene, model, skeleton, info.boneEntities, &info.bridges) || skeleton.bones.empty()){
            info.error = info.model + " has no skeleton (bones): pose animations need a skinned humanoid model";
            return;
        }
        for (const pose::SkeletonBone& bone : skeleton.bones) info.boneNames.push_back(bone.name);
        std::string error;
        const bool valid = pose::resolveHumanoid(skeleton, overrides, info.humanoid, &error, &info.notes);
        for (int b = 0; b < pose::HUMAN_BONE_COUNT; b++){
            const pose::HumanBone bone = pose::boneAt(b);
            if (pose::humanBoneInfo(bone).required && info.humanoid.bones[b].empty()){
                info.missing.push_back(pose::humanBoneName(bone));
            }
        }
        if (!valid){
            info.error = info.model + ": not a usable humanoid: " + error;
            return;
        }
        auto rig = std::make_shared<pose::PoseRig>();
        rig->setSkin(pose::buildSkin(scene, model, skeleton, info.boneEntities));
        if (!rig->build(skeleton, info.humanoid, &error)){
            info.error = info.model + ": " + error;
            return;
        }
        info.rig = rig;
    }

}

std::shared_ptr<const editor::PoseRigInfo> editor::PoseRigProvider::forModelFile(Project* project, const fs::path& model){
    const fs::path file = model.lexically_normal();
    const std::string key = file.generic_string();
    const Stamp modelStamp = stampOf(file);
    const Stamp sidecarStamp = stampOf(sidecarOf(file));
    auto it = cache().find(key);
    if (it != cache().end() && it->second.model == modelStamp && it->second.sidecar == sidecarStamp){
        return it->second.info;
    }

    auto info = std::make_shared<PoseRigInfo>();
    if (!modelStamp.exists){
        info->model = editor::ProjectChecks::projectRelative(project->getProjectPath(), file);
        info->error = info->model + " does not exist";
        return info;
    }
    // a private scene the Engine never sees, as the model previews use (ModelRender)
    Scene* scene = new Scene(EntityPool::System);
    Model* loaded = new Model(scene);
    if (!loaded->loadModel(file.string())){
        info->model = editor::ProjectChecks::projectRelative(project->getProjectPath(), file);
        info->error = info->model + " could not be loaded";
    }else{
        buildRig(project, scene, loaded->getEntity(), file, *info);
    }
    // the bone entities die with the private scene
    info->boneEntities.clear();
    info->bridges.clear();
    delete loaded;
    delete scene;

    cache()[key] = {modelStamp, sidecarStamp, info};
    return info;
}

std::shared_ptr<const editor::PoseRigInfo> editor::PoseRigProvider::forSceneModel(Project* project, Scene* scene, Entity model){
    auto info = std::make_shared<PoseRigInfo>();
    ModelComponent* component = scene ? scene->findComponent<ModelComponent>(model) : nullptr;
    if (!component){
        info->error = "not a model GameObject (no Model component)";
        return info;
    }
    if (component->filename.empty()){
        info->error = "the model GameObject has no model file";
        return info;
    }
    const fs::path file = project->resolveAssetPath(FileUtils::pathFromUtf8(component->filename.path)).lexically_normal();
    buildRig(project, scene, model, file, *info);
    return info;
}

void editor::PoseRigProvider::clear(){
    cache().clear();
}
