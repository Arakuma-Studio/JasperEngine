// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PoseBaker.h"

#include "PoseAnimationDocument.h"
#include "PoseRigProvider.h"

#include "Out.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/EntityNameCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "component/ActionComponent.h"
#include "component/AnimationComponent.h"
#include "component/KeyframeTracksComponent.h"
#include "component/ModelComponent.h"
#include "component/RotateTracksComponent.h"
#include "component/TranslateTracksComponent.h"

#include "pose/PoseInspectorHooks.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>

using namespace jasperengine;

namespace {

    // One track to write: a bone's rotations, or the hips' positions
    struct TrackPlan {
        Entity bone = NULL_ENTITY;
        bool translation = false;
        std::string name;
        std::vector<Quaternion> rotations;
        std::vector<Vector3> positions;
    };

    template<typename T>
    void addValues(editor::MultiPropertyCmd& multi, editor::Project* project, uint32_t sceneId, Entity track,
                   editor::ComponentType type, const std::vector<T>& values){
        multi.addPropertyCmd<std::vector<T>>(project, sceneId, track, type, "values", values);
        multi.addPropertyCmd<std::vector<T>>(project, sceneId, track, type, "inTangents", std::vector<T>());
        multi.addPropertyCmd<std::vector<T>>(project, sceneId, track, type, "outTangents", std::vector<T>());
    }

}

bool editor::PoseBaker::bake(Project* project, uint32_t sceneId, Entity model, PoseAnimationDocument& document,
                             const std::string& requestedName, PoseBakeResult& result, std::string& error){
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    if (!sceneProject || !sceneProject->scene){
        error = "the scene is not open";
        return false;
    }
    if (sceneProject->playState != ScenePlayState::STOPPED){
        error = "the scene is playing: stop play mode before baking";
        return false;
    }
    Scene* scene = sceneProject->scene;
    if (!scene->findComponent<ModelComponent>(model)){
        error = "\"" + scene->getEntityName(model) + "\" is not a model GameObject";
        return false;
    }
    const std::shared_ptr<const PoseRigInfo> info = PoseRigProvider::forSceneModel(project, scene, model);
    if (!info->rig){
        error = info->error;
        return false;
    }
    const std::shared_ptr<const PoseRealisation> realised = document.realise(info->rig);
    if (!realised || document.data().poses.empty() || realised->frames.size() < 2){
        error = "the animation has no poses to bake";
        return false;
    }
    const pose::PoseRig& rig = *info->rig;
    const pose::PoseAnimationData& data = document.data();
    const std::vector<pose::HumanPose>& frames = realised->frames;
    const size_t count = frames.size() - 1;
    const float length = static_cast<float>(realised->timeline.length);

    std::vector<float> times;
    for (size_t f = 0; f < frames.size(); f++){
        times.push_back(f == count ? length : static_cast<float>(static_cast<double>(f) * realised->timeline.length / static_cast<double>(count)));
    }
    std::vector<EaseType> easings;
    if (data.settings.stepRate > 0.0f){
        easings.assign(count, EaseType::STEP);
    }

    std::vector<TrackPlan> plans;
    for (int b = 0; b < pose::HUMAN_BONE_COUNT; b++){
        const pose::HumanBone slot = pose::boneAt(b);
        const int bone = rig.boneIndex(slot);
        if (bone < 0 || static_cast<size_t>(bone) >= info->boneEntities.size() || info->boneEntities[bone] == NULL_ENTITY){
            continue;
        }
        TrackPlan plan;
        plan.bone = info->boneEntities[bone];
        plan.name = scene->getEntityName(plan.bone) + " rotation";
        TrackPlan hips;
        hips.bone = plan.bone;
        hips.translation = true;
        hips.name = scene->getEntityName(plan.bone) + " position";
        // the pose is in skeleton-local terms; the tracks write the bone's Transform, so the
        // non-bone ancestors ModelRig folded into the skeleton come off again
        const bool bridged = static_cast<size_t>(bone) < info->bridges.size() && !(info->bridges[bone] == Matrix4());
        const Matrix4 unbridge = bridged ? info->bridges[bone].inverse() : Matrix4();
        const pose::SkeletonBone& rest = rig.skeleton().bones[bone];
        for (const pose::HumanPose& frame : frames){
            const Vector3 position = slot == pose::HumanBone::Hips ? frame.hipsPosition : rest.position;
            if (bridged){
                const Matrix4 local = unbridge * Matrix4::translateMatrix(position) * frame.rotations[b].getRotationMatrix() *
                                      Matrix4::scaleMatrix(rest.scale);
                Vector3 p, s;
                Quaternion r;
                local.decompose(p, s, r);
                plan.rotations.push_back(r);
                hips.positions.push_back(p);
            }else{
                plan.rotations.push_back(frame.rotations[b]);
                hips.positions.push_back(position);
            }
        }
        plans.push_back(plan);
        if (slot == pose::HumanBone::Hips){
            plans.push_back(hips);
        }
    }

    std::string name = requestedName;
    if (name.empty()){
        // the file stem ("assets/anim/walk.poseanim" -> "walk"), UTF-8 as the path is
        const std::string& path = document.path();
        const size_t slash = path.find_last_of('/');
        name = slash == std::string::npos ? path : path.substr(slash + 1);
        const size_t dot = name.find_last_of('.');
        if (dot != std::string::npos && dot > 0) name = name.substr(0, dot);
    }

    // The Animation of that name among THIS model's (names only need to be unique per model for
    // Model:playAnimation): the exact name first, then the scene-unique form the editor may have
    // given it ("walk 2")
    ModelComponent& modelComponent = scene->getComponent<ModelComponent>(model);
    Entity animation = NULL_ENTITY;
    auto isAnimation = [&](Entity entity){
        return entity != NULL_ENTITY && scene->isEntityCreated(entity) && scene->findComponent<AnimationComponent>(entity);
    };
    for (Entity entity : modelComponent.animations){
        if (isAnimation(entity) && scene->getEntityName(entity) == name){
            animation = entity;
            break;
        }
    }
    for (size_t i = 0; animation == NULL_ENTITY && i < modelComponent.animations.size(); i++){
        const Entity entity = modelComponent.animations[i];
        if (!isAnimation(entity)) continue;
        const std::string other = scene->getEntityName(entity);
        if (other.size() > name.size() + 1 && other.compare(0, name.size() + 1, name + " ") == 0 &&
            other.find_first_not_of("0123456789", name.size() + 1) == std::string::npos){
            animation = entity;
        }
    }

    CommandHistory* history = CommandHandle::get(sceneId);
    const size_t start = history->mark();
    auto add = [&](Command* command){
        command->setNoMerge();
        return history->addCommand(command);
    };
    auto rollback = [&](const std::string& message){
        // undo() is false when the history did not move (a command refused): stop there
        while (history->position() > start && history->undo()) {}
        history->dropRedo();   // the rolled-back commands are not a redo step
        error = message;
        return false;
    };

    result = PoseBakeResult();
    if (animation == NULL_ENTITY){
        auto* create = new CreateEntityCmd(project, sceneId, name, EntityCreationType::ANIMATION, model);
        create->setQuiet(true);
        if (!add(create)){
            return rollback("could not create the Animation GameObject");
        }
        animation = create->getEntity();
        result.created = true;
    }
    // the exact name (CreateEntityCmd makes names unique in the whole scene: another model's
    // Animation may already be called so)
    if (scene->getEntityName(animation) != name && !add(new EntityNameCmd(project, sceneId, animation, name))){
        return rollback("could not name the Animation \"" + name + "\"");
    }

    // the tracks it has, by target and channel
    const std::vector<ActionFrame> oldActions = scene->getComponent<AnimationComponent>(animation).actions;
    std::map<std::pair<Entity, bool>, Entity> existing;
    for (const ActionFrame& frame : oldActions){
        if (frame.action == NULL_ENTITY || !scene->isEntityCreated(frame.action) || !scene->findComponent<KeyframeTracksComponent>(frame.action)) continue;
        const ActionComponent* action = scene->findComponent<ActionComponent>(frame.action);
        if (!action) continue;
        if (scene->findComponent<RotateTracksComponent>(frame.action)) existing[{action->target, false}] = frame.action;
        else if (scene->findComponent<TranslateTracksComponent>(frame.action)) existing[{action->target, true}] = frame.action;
    }

    auto* edits = new MultiPropertyCmd();
    std::unique_ptr<MultiPropertyCmd> editsGuard(edits);
    std::vector<ActionFrame> actions;
    std::set<Entity> used;
    uint32_t lane = 0;
    for (const TrackPlan& plan : plans){
        Entity track = NULL_ENTITY;
        auto it = existing.find({plan.bone, plan.translation});
        if (it != existing.end()){
            track = it->second;
            edits->addPropertyCmd<std::vector<float>>(project, sceneId, track, ComponentType::KeyframeTracksComponent, "times", times);
            edits->addPropertyCmd<std::vector<EaseType>>(project, sceneId, track, ComponentType::KeyframeTracksComponent, "easings", easings);
            if (plan.translation) addValues(*edits, project, sceneId, track, ComponentType::TranslateTracksComponent, plan.positions);
            else addValues(*edits, project, sceneId, track, ComponentType::RotateTracksComponent, plan.rotations);
        }else{
            auto* create = new CreateEntityCmd(project, sceneId, plan.name,
                                               plan.translation ? EntityCreationType::TRANSLATE_TRACKS : EntityCreationType::ROTATE_TRACKS, animation);
            create->setQuiet(true);
            create->addProperty<std::vector<float>>(ComponentType::KeyframeTracksComponent, "times", times);
            create->addProperty<std::vector<EaseType>>(ComponentType::KeyframeTracksComponent, "easings", easings);
            if (plan.translation) create->addProperty<std::vector<Vector3>>(ComponentType::TranslateTracksComponent, "values", plan.positions);
            else create->addProperty<std::vector<Quaternion>>(ComponentType::RotateTracksComponent, "values", plan.rotations);
            create->addProperty<Entity>(ComponentType::ActionComponent, "target", plan.bone);
            if (!add(create)){
                return rollback("could not create the track of " + plan.name);
            }
            track = create->getEntity();
        }
        used.insert(track);
        ActionFrame frame;
        frame.startTime = 0.0f;
        frame.duration = 0.0f;
        frame.action = track;
        frame.track = lane++;
        actions.push_back(frame);
    }

    // tracks of bones the rig no longer maps go; other actions stay
    std::vector<Entity> toDelete;
    for (const ActionFrame& frame : oldActions){
        if (used.count(frame.action)) continue;
        bool ours = false;
        for (const auto& entry : existing) ours |= entry.second == frame.action;
        if (ours){
            if (std::find(toDelete.begin(), toDelete.end(), frame.action) == toDelete.end()) toDelete.push_back(frame.action);
        }else{
            ActionFrame kept = frame;
            kept.track = lane++;
            actions.push_back(kept);
        }
    }

    edits->addPropertyCmd<std::vector<ActionFrame>>(project, sceneId, animation, ComponentType::AnimationComponent, "actions", actions);
    edits->addPropertyCmd<bool>(project, sceneId, animation, ComponentType::AnimationComponent, "loop", data.settings.loop);
    edits->addPropertyCmd<float>(project, sceneId, animation, ComponentType::AnimationComponent, "duration", length);
    std::vector<Entity> modelAnimations = modelComponent.animations;
    if (std::find(modelAnimations.begin(), modelAnimations.end(), animation) == modelAnimations.end()){
        modelAnimations.push_back(animation);
        edits->addPropertyCmd<std::vector<Entity>>(project, sceneId, model, ComponentType::ModelComponent, "animations", modelAnimations);
    }
    if (!add(editsGuard.release())){
        return rollback("the editor rejected the animation edit");
    }
    if (!toDelete.empty() && !add(new DeleteEntityCmd(project, sceneId, toDelete))){
        return rollback("could not delete the tracks of bones no longer mapped");
    }
    history->groupSince(start);

    result.animation = animation;
    result.name = scene->getEntityName(animation);
    result.frames = static_cast<int>(frames.size());
    result.tracks = static_cast<int>(plans.size());
    result.length = length;
    return true;
}

bool editor::PoseBaker::bakeAnimatorAsset(Project* project, uint32_t sceneId, Entity model, const std::string& animation,
                                          PoseBakeResult& result, std::string& error){
    if (animation.empty()){
        error = "the Pose Animator has no animation";
        return false;
    }
    // the component stores the path as the runtime loads it: relative to the assets folder
    const std::filesystem::path file = project->resolveAssetPath(std::filesystem::u8path(animation)).lexically_normal();
#if defined(__cpp_lib_char8_t)
    const std::u8string utf8 = file.u8string();
    const std::string path(utf8.begin(), utf8.end());
#else
    const std::string path = file.u8string();
#endif
    std::shared_ptr<PoseAnimationDocument> document = PoseAnimationDocument::open(project, path, &error);
    if (!document){
        return false;
    }
    return bake(project, sceneId, model, *document, "", result, error);
}

std::string editor::PoseBaker::bakeStatusLine(const std::string& animation, const PoseBakeResult& result){
    return "Baked '" + animation + "' to the Animation '" + result.name + "' (" + std::to_string(result.tracks) + " tracks, " +
           std::to_string(result.frames) + " keys each): Model:playAnimation(\"" + result.name + "\")";
}

namespace {

    // The Pose Animator Inspector section's Bake button
    struct InstallBakeHook {
        InstallBakeHook(){
            editor::poseInspectorHooks().bake = [](editor::Project* project, uint32_t sceneId, Entity model, const std::string& animation){
                std::string error;
                editor::PoseBakeResult result;
                if (!editor::PoseBaker::bakeAnimatorAsset(project, sceneId, model, animation, result, error)){
                    editor::Out::error("Bake of '%s' failed: %s", animation.c_str(), error.c_str());
                    return;
                }
                editor::Out::info("%s", editor::PoseBaker::bakeStatusLine(animation, result).c_str());
            };
        }
    } installBakeHook;

}
