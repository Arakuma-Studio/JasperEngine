// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PoseRigProvider.h"

#include "Out.h"
#include "Project.h"
#include "Scene.h"
#include "io/AtomicFile.h"
#include "component/ModelComponent.h"
#include "object/Model.h"
#include "pose/HumanoidMapIO.h"
#include "pose/ModelRig.h"
#include "util/FbxImporter.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"
#include "util/ProjectFileWrites.h"

#include <cstdint>
#include <cstring>
#include <string_view>

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

    // The skeleton of a glTF (.gltf, or a .glb's JSON chunk) read from its JSON alone: the joints of
    // every skin, each under its nearest joint ancestor, named as written. All the auto-detect reads
    // (names and parents), without loading the model or its buffers, so a model arriving before its
    // .bin is read the same. False when the file is no glTF the reader understands.
    bool gltfJsonSkeleton(const fs::path& model, pose::Skeleton& out){
        out.bones.clear();
        std::string bytes;
        if (!editor::ProjectChecks::readFile(model, bytes)) return false;
        std::string_view json = bytes;
        if (bytes.size() >= 20 && bytes.compare(0, 4, "glTF") == 0){
            uint32_t length = 0;
            std::memcpy(&length, bytes.data() + 12, sizeof(length));
            if (bytes.compare(16, 4, "JSON") != 0 || length > bytes.size() - 20) return false;
            json = std::string_view(bytes.data() + 20, length);
        }
        const nlohmann::json root = nlohmann::json::parse(json.begin(), json.end(), nullptr, false);
        if (root.is_discarded() || !root.is_object()) return false;
        const auto nodes = root.find("nodes");
        const auto skins = root.find("skins");
        if (nodes == root.end() || !nodes->is_array() || skins == root.end() || !skins->is_array()) return true;
        const int count = (int)nodes->size();
        std::vector<int> parent((size_t)count, -1);
        for (int n = 0; n < count; n++){
            const auto children = (*nodes)[(size_t)n].find("children");
            if (children == (*nodes)[(size_t)n].end() || !children->is_array()) continue;
            for (const nlohmann::json& child : *children){
                if (child.is_number_integer() && child.get<int>() >= 0 && child.get<int>() < count && child.get<int>() != n) parent[(size_t)child.get<int>()] = n;
            }
        }
        std::vector<int> boneOf((size_t)count, -1);
        for (const nlohmann::json& skin : *skins){
            const auto joints = skin.is_object() ? skin.find("joints") : skin.end();
            if (!skin.is_object() || joints == skin.end() || !joints->is_array()) continue;
            for (const nlohmann::json& joint : *joints){
                if (joint.is_number_integer() && joint.get<int>() >= 0 && joint.get<int>() < count) boneOf[(size_t)joint.get<int>()] = 0;
            }
        }
        for (int n = 0; n < count; n++){
            if (boneOf[(size_t)n] < 0) continue;
            boneOf[(size_t)n] = (int)out.bones.size();
            pose::SkeletonBone bone;
            const auto name = (*nodes)[(size_t)n].find("name");
            if (name != (*nodes)[(size_t)n].end() && name->is_string()) bone.name = name->get<std::string>();
            out.bones.push_back(bone);
        }
        for (int n = 0; n < count; n++){
            if (boneOf[(size_t)n] < 0) continue;
            // a cycle in the node graph ends the walk (at most `count` steps)
            int up = parent[(size_t)n];
            for (int steps = 0; up >= 0 && boneOf[(size_t)up] < 0 && steps < count; steps++) up = parent[(size_t)up];
            if (up >= 0 && up != n && boneOf[(size_t)up] >= 0) out.bones[(size_t)boneOf[(size_t)n]].parent = boneOf[(size_t)up];
        }
        return true;
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
            info.sidecarError = info.error;
            return;
        }
        info.overrides = overrides;
        pose::Skeleton& skeleton = info.skeleton;
        if (!pose::buildSkeleton(scene, model, skeleton, info.boneEntities, &info.bridges) || skeleton.bones.empty()){
            info.error = info.model + " has no skeleton (bones): pose animations need a skinned humanoid model";
            return;
        }
        for (const pose::SkeletonBone& bone : skeleton.bones) info.boneNames.push_back(bone.name);
        std::string error;
        const bool valid = pose::resolveHumanoid(skeleton, overrides, info.humanoid, &error, &info.notes);
        pose::noteSpineProxies(scene, model, info.humanoid, info.notes);
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
        // the Avatar's fingers, eyes and jaw (anim-retarget 6.3; a bad override is said, the body
        // map still works: the runtime's fallback, pose::resolveHumanoid) and its twist
        // corrections (6.2: they move the neutral pose)
        pose::resolveExtraBonesOrAuto(skeleton, info.humanoid, overrides.extraBones, info.extra, &info.extraError, &info.notes);
        auto rig = std::make_shared<pose::PoseRig>();
        rig->setSkin(pose::buildSkin(scene, model, skeleton, info.boneEntities));
        const std::array<float, pose::HUMAN_BONE_COUNT> twist = overrides.twistDegrees();
        if (!rig->build(skeleton, info.humanoid, &error, &twist)){
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

std::string editor::PoseRigProvider::unreadableSidecarRefusal(const std::string& sidecarError){
    return sidecarError + " (fix or delete the file first: rewriting it would lose its Avatar)";
}

bool editor::PoseRigProvider::setAnimationType(Project* project, const fs::path& model, pose::AnimationType type, std::string& error){
    const std::shared_ptr<const PoseRigInfo> info = forModelFile(project, model);
    if (!info->sidecarError.empty()){
        error = unreadableSidecarRefusal(info->sidecarError);
        return false;
    }
    if (info->overrides.isHumanoid() == (type == pose::AnimationType::Humanoid)) return true;
    pose::HumanoidOverrides changed = info->overrides;
    if (type == pose::AnimationType::Humanoid) changed.animationType = type;
    else changed.animationType.reset();
    return ProjectFileWrites::writeUndoable(project, {{sidecarOf(model.lexically_normal()), pose::writeHumanoidOverrides(changed)}}, error);
}

bool editor::PoseRigProvider::adoptNameRules(Project* project, const fs::path& model){
    const fs::path file = model.lexically_normal();
    // the rig of the model file, or of an FBX's converted .glb, against the current naming rules
    auto adopt = [](Project* project, const fs::path& file, bool fbx) -> bool {
        pose::Skeleton skeleton;
        if (!gltfJsonSkeleton(fbx ? project->getImportedModelPath(file) : file, skeleton) || skeleton.bones.empty()) return false;
        const fs::path sidecar = sidecarOf(file);
        pose::HumanoidOverrides overrides;
        std::vector<std::string> notes;
        std::string error;
        if (!readOverrides(project, sidecar, overrides, notes, error)){
            Out::warning("%s", unreadableSidecarRefusal(error).c_str());
            return false;
        }
        const int rules = overrides.detectNameRules();
        if (rules >= pose::HUMANOID_NAME_RULES) return false;
        // the rules agree on this rig: nothing to record, its sidecar stays as it is (or absent)
        if (pose::autoDetectHumanoid(skeleton, nullptr, rules).bones == pose::autoDetectHumanoid(skeleton).bones) return false;
        overrides.nameRules = pose::HUMANOID_NAME_RULES;
        const io::WriteResult written = io::writeFileAtomic(sidecar, pose::writeHumanoidOverrides(overrides));
        if (!written.ok()){
            Out::warning("%s: %s", ProjectChecks::projectRelative(project->getProjectPath(), sidecar).c_str(), io::describeWriteError(written).c_str());
            return false;
        }
        return true;
    };
    const bool fbx = FbxImporter::isFbxFile(file);
    if (fbx && !project->importFbxModel(file)) return false;
    // the main thread only queues the conversion (S98 REQ-32): the rig is read once it landed, without
    // asking for the import again (a request queues even a current FBX)
    if (fbx && project->isModelImportPending(file)) {
        project->whenModelImported(file, [project, file, adopt]() { adopt(project, file, true); });
        return false;
    }
    return adopt(project, file, fbx);
}
