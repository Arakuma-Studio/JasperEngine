// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "InspectorAnimLayers.h"
#include "animation/AnimationClipIO.h"

#include "Project.h"
#include "Scene.h"
#include "animation/AnimationPlayerRuntime.h"
#include "animation/ClipBinding.h"
#include "animation/ModelSubClips.h"
#include "command/Command.h"
#include "command/type/ClipSceneActions.h"
#include "animation/AnimatorAssetDocument.h"
#include "animation/AnimatorAssetIO.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/AnimatorComponent.h"
#include "component/Transform.h"
#include "pose/HumanoidMap.h"
#include "pose/HumanoidMapIO.h"
#include "pose/ModelRig.h"
#include "registry/EntityRegistry.h"
#include "util/Util.h"

#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    // AvatarBodyPart's order
    const char* const partLabels[] = {"Root", "Body", "Head", "Left Arm", "Right Arm", "Left Hand", "Right Hand", "Left Leg", "Right Leg",
                                      "Left Fingers", "Right Fingers"};

    bool fail(std::string& error, const std::string& message){
        error = message;
        return false;
    }

    // a project-relative path of a new .mask / .controller to write (ClipSceneActions' check of
    // clips): inside the project and its assets folder, with the extension; `path` normalized
    bool checkNewAssetPath(Project* project, std::string& path, bool (*isKind)(const std::string&), const char* kind, std::string& error){
        if (path.empty()) return fail(error, std::string("no ") + kind + " path given");
        const std::string given = path;
        fs::path absolute = fs::u8path(path);
        absolute = (absolute.is_absolute() ? absolute : project->getProjectPath() / absolute).lexically_normal();
        const fs::path relative = project->normalizeToProjectRelative(absolute);
        const std::string text = relative.generic_string();
        if (relative.empty() || relative.is_absolute() || text.compare(0, 2, "..") == 0) return fail(error, "'" + given + "' is outside the project");
        if (!isKind(text)) return fail(error, "'" + given + "' is not a " + kind + " path");
        if (!project->isInsideAssetsPath(absolute)){
            return fail(error, "'" + given + "' is outside the assets folder (" +
                        project->normalizeToProjectRelative(project->getAssetsPath()).generic_string() + "): the runtime can't load it");
        }
        std::error_code ec;
        if (fs::exists(absolute, ec)) return fail(error, "'" + given + "' exists: choose a new file");
        path = text;
        return true;
    }

    std::string readText(const fs::path& path){
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    // the stored (asset-relative) form of a project-relative path
    std::string storedForm(Project* project, const std::string& projectRelative){
        return project->normalizeToAssetsRelative(project->getProjectPath() / fs::u8path(projectRelative)).generic_string();
    }

    // the name of a path's last segment (its "#k" dropped)
    std::string leafName(const std::string& path){
        std::vector<anim::PathSegment> segments;
        if (anim::parsePath(path, segments) && !segments.empty()) return segments.back().name;
        const size_t slash = path.rfind('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    // the first model a prefab file names ("filename: models/x.glb" of a Model component)
    std::string prefabModel(const fs::path& prefab){
        YAML::Node root;
        try{
            root = YAML::LoadFile(prefab.string());
        }catch (...){
            return "";
        }
        std::string found;
        std::function<void(const YAML::Node&)> walk = [&](const YAML::Node& node){
            if (!found.empty()) return;
            if (node.IsMap()){
                for (auto it = node.begin(); it != node.end() && found.empty(); ++it){
                    if (it->first.IsScalar() && it->first.Scalar() == "filename" && it->second.IsScalar() && anim::isModelFilePath(it->second.Scalar())){
                        found = it->second.Scalar();
                        return;
                    }
                    walk(it->second);
                }
            }else if (node.IsSequence()){
                for (const YAML::Node& item : node) walk(item);
            }
        };
        walk(root);
        return found;
    }

}

// ---- figure ----

const std::vector<animlayersui::FigureShape>& animlayersui::figureShapes(){
    // a front view (the character's right on the viewer's left, as Unity draws it)
    // (the hand is the bone, the fingers what is below it)
    static const std::vector<FigureShape> shapes = {
        {AvatarBodyPart::Root, 0.36f, 0.90f, 0.64f, 0.98f, true},
        {AvatarBodyPart::LeftLeg, 0.52f, 0.52f, 0.64f, 0.90f, false},
        {AvatarBodyPart::RightLeg, 0.36f, 0.52f, 0.48f, 0.90f, false},
        {AvatarBodyPart::Body, 0.36f, 0.20f, 0.64f, 0.54f, false},
        {AvatarBodyPart::Head, 0.42f, 0.03f, 0.58f, 0.18f, false},
        {AvatarBodyPart::LeftArm, 0.66f, 0.21f, 0.76f, 0.47f, false},
        {AvatarBodyPart::RightArm, 0.24f, 0.21f, 0.34f, 0.47f, false},
        {AvatarBodyPart::LeftHand, 0.66f, 0.48f, 0.77f, 0.54f, false},
        {AvatarBodyPart::RightHand, 0.23f, 0.48f, 0.34f, 0.54f, false},
        {AvatarBodyPart::LeftFingers, 0.66f, 0.55f, 0.78f, 0.63f, false},
        {AvatarBodyPart::RightFingers, 0.22f, 0.55f, 0.34f, 0.63f, false},
    };
    return shapes;
}

int animlayersui::figurePartAt(float x, float y){
    int found = -1;
    for (const FigureShape& shape : figureShapes()){
        if (shape.disc){
            const float cx = (shape.x0 + shape.x1) * 0.5f, cy = (shape.y0 + shape.y1) * 0.5f;
            const float rx = (shape.x1 - shape.x0) * 0.5f, ry = (shape.y1 - shape.y0) * 0.5f;
            const float dx = (x - cx) / rx, dy = (y - cy) / ry;
            if (dx * dx + dy * dy <= 1.0f) found = (int)shape.part;
        }else if (x >= shape.x0 && x <= shape.x1 && y >= shape.y0 && y <= shape.y1){
            found = (int)shape.part;
        }
    }
    return found;
}

void animlayersui::figurePartCenter(AvatarBodyPart part, float& x, float& y){
    x = y = -1.0f;
    for (const FigureShape& shape : figureShapes()){
        if (shape.part == part){
            x = (shape.x0 + shape.x1) * 0.5f;
            y = (shape.y0 + shape.y1) * 0.5f;
            return;
        }
    }
}

const char* animlayersui::partLabel(AvatarBodyPart part){
    const size_t index = (size_t)part;
    return index < sizeof(partLabels) / sizeof(partLabels[0]) ? partLabels[index] : "";
}

anim::AvatarMaskDef animlayersui::togglePart(const anim::AvatarMaskDef& data, AvatarBodyPart part){
    anim::AvatarMaskDef out = data;
    out.bodyParts[(int)part] = data.partWeight(part) >= 1.0f ? 0.0f : 1.0f;
    return out;
}

anim::AvatarMaskDef animlayersui::setTransform(const anim::AvatarMaskDef& data, const std::string& path, bool on, bool subtree,
                                               const std::vector<std::string>& allPaths){
    anim::AvatarMaskDef out = data;
    std::vector<std::string> targets = {path};
    if (subtree){
        const std::string prefix = path.empty() ? std::string() : path + "/";
        for (const std::string& other : allPaths){
            if (other != path && (path.empty() || other.compare(0, prefix.size(), prefix) == 0)) targets.push_back(other);
        }
    }
    for (const std::string& target : targets){
        const int index = out.findPath(target);
        if (on){
            if (index < 0) out.paths.push_back({target, 1.0f, true});
            else if (out.paths[(size_t)index].weight <= 0.0f) out.paths[(size_t)index].weight = 1.0f;
        }else if (index >= 0){
            out.paths.erase(out.paths.begin() + index);
        }
    }
    anim::normalizeMask(out, nullptr);
    return out;
}

// ---- builds ----

bool animlayersui::maskFromSelection(EntityRegistry* registry, Entity root, const std::vector<Entity>& selection, anim::AvatarMaskDef& out,
                                     std::string* error){
    out = anim::AvatarMaskDef();
    out.defaultWeight = 0.0f;
    for (Entity entity : selection){
        std::string path;
        if (!anim::relativePath(registry, root, entity, path)) continue;
        if (out.findPath(path) < 0) out.paths.push_back({path, 1.0f, true});
    }
    if (out.paths.empty()){
        if (error) *error = "Select the GameObjects the layer animates first (the player's GameObject or GameObjects under it).";
        return false;
    }
    anim::normalizeMask(out, nullptr);
    return true;
}

anim::ControllerDef animlayersui::controllerFromClips(const AnimationPlayerComponent& player){
    anim::ControllerDef data;
    // each state takes its entry's Retarget, Mirror and Foot IK (appendPlayerStates)
    appendPlayerStates(data, player);
    anim::normalizeController(data, nullptr);
    return data;
}

std::string animlayersui::freeAssetPath(Project* project, const std::string& directory, const std::string& stem, const std::string& extension){
    const fs::path dir = fs::u8path(directory);
    std::error_code ec;
    for (int k = 1; k < 10000; k++){
        const std::string name = stem + (k == 1 ? std::string() : " " + std::to_string(k)) + extension;
        const fs::path relative = dir / fs::u8path(name);
        if (!fs::exists(project->getProjectPath() / relative, ec)) return relative.generic_string();
    }
    return (dir / fs::u8path(stem + extension)).generic_string();
}

bool animlayersui::createMaskFromSelection(Project* project, uint32_t sceneId, Entity entity, int layer, const std::string& maskPathIn,
                                           const std::vector<Entity>& selection, std::string& error){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return fail(error, "no scene");
    Scene* scene = sceneProject->scene;
    const AnimationPlayerComponent* player = scene->findComponent<AnimationPlayerComponent>(entity);
    if (!player) return fail(error, "the GameObject has no Animation Player");
    std::vector<AnimationPlayerLayer> layers = anim::playerLayers(*player);
    if (layer < 0 || (size_t)layer >= layers.size()) return fail(error, "no layer " + std::to_string(layer));
    std::string maskPath = maskPathIn;
    if (!checkNewAssetPath(project, maskPath, Util::isAvatarMaskFile, ".mask", error)) return false;
    anim::AvatarMaskDef data;
    std::string why;
    if (!maskFromSelection(scene, entity, selection, data, &why)) return fail(error, why);
    data.guid = anim::generateClipGuid();
    const std::string stored = storedForm(project, maskPath);

    std::vector<Command*> commands;
    if (player->layers.empty()){
        layers[(size_t)layer].mask = stored;
        layers[(size_t)layer].maskGuid = data.guid;
        commands.push_back(new PropertyCmd<std::vector<AnimationPlayerLayer>>(project, sceneId, entity, ComponentType::AnimationPlayerComponent,
                                                                              "layers", layers));
    }else{
        const std::string prefix = "layers[" + std::to_string(layer) + "]";
        commands.push_back(new PropertyCmd<std::string>(project, sceneId, entity, ComponentType::AnimationPlayerComponent, prefix + ".mask", stored));
        commands.push_back(new PropertyCmd<std::string>(project, sceneId, entity, ComponentType::AnimationPlayerComponent, prefix + ".maskGuid", data.guid));
    }
    ClipActionResult result;
    return ClipSceneActions::runStep(project, sceneId, {{maskPath, anim::writeMask(data)}}, std::move(commands), NULL_ENTITY, result, error);
}

bool animlayersui::createControllerFromClips(Project* project, uint32_t sceneId, Entity entity, const std::string& controllerPath, std::string& error){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return fail(error, "no scene");
    const AnimationPlayerComponent* player = sceneProject->scene->findComponent<AnimationPlayerComponent>(entity);
    if (!player) return fail(error, "the GameObject has no Animation Player");
    anim::ControllerDef data = controllerFromClips(*player);
    data.guid = anim::generateClipGuid();
    return createControllerFile(project, sceneId, entity, controllerPath, data, error);
}

bool animlayersui::createControllerFile(Project* project, uint32_t sceneId, Entity entity, const std::string& controllerPathIn,
                                        const anim::ControllerDef& input, std::string& error){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || !sceneProject->scene->findComponent<AnimationPlayerComponent>(entity)){
        return fail(error, "the GameObject has no Animation Player");
    }
    Scene* scene = sceneProject->scene;
    std::string controllerPath = controllerPathIn;
    if (!checkNewAssetPath(project, controllerPath, Util::isAnimatorControllerFile, ".controller", error)) return false;
    anim::ControllerDef data = input;
    if (data.guid.empty()) data.guid = anim::generateClipGuid();
    const AnimationPlayerComponent& player = scene->getComponent<AnimationPlayerComponent>(entity);
    const AnimatorComponent* existing = scene->findComponent<AnimatorComponent>(entity);
    std::vector<Command*> commands;
    if (!existing) commands.push_back(new AddComponentCmd(project, sceneId, entity, ComponentType::AnimatorComponent));
    AssetRef ref(storedForm(project, controllerPath));
    ref.guid = Guid::parse(data.guid);
    commands.push_back(new PropertyCmd<AssetRef>(project, sceneId, entity, ComponentType::AnimatorComponent, "controller", ref));
    // the Animator plays as the player did (its checkbox); the player hands over to it
    commands.push_back(new PropertyCmd<bool>(project, sceneId, entity, ComponentType::AnimatorComponent, "enabled", player.enabled));
    if (player.enabled) commands.push_back(new PropertyCmd<bool>(project, sceneId, entity, ComponentType::AnimationPlayerComponent, "enabled", false));
    ClipActionResult result;
    return ClipSceneActions::runStep(project, sceneId, {{controllerPath, anim::writeController(data)}}, std::move(commands), NULL_ENTITY, result, error);
}

// ---- coverage ----

animlayersui::MaskCoverage animlayersui::maskCoverage(Scene* scene, Entity root, const anim::AvatarMaskDef& mask, const std::vector<Entity>& targets){
    MaskCoverage coverage;
    anim::HumanoidSlots slots;
    if (mask.usesHumanoid()){
        slots = anim::resolveHumanoidSlots(scene, root);
        if (!slots.valid){
            coverage.humanoidWithoutRig = true;
            coverage.rigError = slots.error;
        }
    }
    const std::vector<Entity> entries = anim::bindMaskPaths(mask, scene, root);
    std::set<Entity> seen;
    for (Entity target : targets){
        if (target == NULL_ENTITY || !seen.insert(target).second) continue;
        coverage.total++;
        if (anim::maskWeightOf(mask, scene, root, target, entries, slots.valid ? &slots : nullptr).weight > 0.0f) coverage.covered++;
    }
    return coverage;
}

// ---- preview hierarchy ----

animlayersui::PreviewHierarchy::PreviewHierarchy() = default;
animlayersui::PreviewHierarchy::~PreviewHierarchy() = default;
animlayersui::PreviewHierarchy::PreviewHierarchy(PreviewHierarchy&&) noexcept = default;
animlayersui::PreviewHierarchy& animlayersui::PreviewHierarchy::operator=(PreviewHierarchy&&) noexcept = default;

bool animlayersui::previewHierarchyFromModel(Project* project, const std::string& model, PreviewHierarchy& out){
    out = PreviewHierarchy();
    std::string modelPath = model;
    if (Util::isPrefabFile(model)){
        modelPath = prefabModel(project->resolveAssetPath(model));
        if (modelPath.empty()){
            out.error = "the prefab has no model";
            return false;
        }
    }
    std::vector<std::string> paths;
    std::string error;
    if (!anim::listModelFileNodes(modelPath, paths, &error)){
        out.error = error.empty() ? "cannot read " + modelPath : error;
        return false;
    }
    out.registry.reset(new EntityRegistry());
    EntityRegistry* registry = out.registry.get();
    out.root = registry->createEntity();
    registry->setEntityName(out.root, fs::u8path(modelPath).stem().string());
    registry->addComponent<Transform>(out.root, {});
    // parents before children: sorted by depth, then the file's order
    std::vector<size_t> order(paths.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    auto depthOf = [&](size_t i){ return (int)std::count(paths[i].begin(), paths[i].end(), '/'); };
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b){ return depthOf(a) < depthOf(b); });
    std::map<std::string, Entity> byPath;
    std::vector<Entity> nodeEntities(paths.size(), NULL_ENTITY);
    for (size_t i : order){
        const std::string& path = paths[i];
        const size_t slash = path.rfind('/');
        const Entity parent = slash == std::string::npos ? out.root : (byPath.count(path.substr(0, slash)) ? byPath[path.substr(0, slash)] : out.root);
        Entity entity = registry->createEntity();
        registry->setEntityName(entity, leafName(path));
        registry->addComponent<Transform>(entity, {});
        registry->addEntityChild(parent, entity, false);
        byPath[path] = entity;
        nodeEntities[i] = entity;
    }
    // depth-first order (what the tree draws), with resolvable paths
    for (const std::pair<std::string, Entity>& entry : anim::descendantPaths(registry, out.root)){
        if (entry.first.empty()) continue;
        out.paths.push_back(entry.first);
        out.entities.push_back(entry.second);
        out.depths.push_back((int)std::count(entry.first.begin(), entry.first.end(), '/'));
    }
    // the humanoid map from the node names and the model's .humanoid sidecar
    pose::Skeleton skeleton;
    std::map<Entity, int> boneOf;
    for (size_t i = 0; i < paths.size(); i++) boneOf[nodeEntities[i]] = (int)i;
    for (size_t i = 0; i < paths.size(); i++){
        pose::SkeletonBone bone;
        bone.name = leafName(paths[i]);
        const Transform* transform = registry->findComponent<Transform>(nodeEntities[i]);
        const auto parent = transform ? boneOf.find(transform->parent) : boneOf.end();
        bone.parent = parent == boneOf.end() ? -1 : parent->second;
        skeleton.bones.push_back(bone);
    }
    pose::HumanoidOverrides overrides;
    const fs::path sidecar = project->resolveAssetPath(pose::humanoidSidecarPath(modelPath));
    std::error_code ec;
    if (fs::is_regular_file(sidecar, ec)){
        std::string sidecarError;
        std::vector<std::string> sidecarWarnings;
        pose::parseHumanoidOverrides(readText(sidecar), overrides, sidecarWarnings, &sidecarError);
    }
    pose::HumanoidMap map;
    std::string rigError;
    if (pose::resolveHumanoid(skeleton, overrides, map, &rigError, nullptr)){
        out.humanoid = anim::humanoidSlotsByName(registry, out.root, map.bones);
    }else{
        out.humanoid = anim::HumanoidSlots();
        out.humanoid.error = rigError;
    }
    return true;
}

bool animlayersui::previewHierarchyFromEntity(Scene* scene, Entity root, PreviewHierarchy& out){
    out = PreviewHierarchy();
    if (!scene || root == NULL_ENTITY || !scene->isEntityCreated(root)){
        out.error = "no GameObject";
        return false;
    }
    // a copy of the names and hierarchy (the scene may change while the Inspector shows it)
    out.registry.reset(new EntityRegistry());
    EntityRegistry* registry = out.registry.get();
    out.root = registry->createEntity();
    registry->setEntityName(out.root, scene->getEntityName(root));
    registry->addComponent<Transform>(out.root, {});
    std::map<Entity, Entity> copyOf = {{root, out.root}};
    std::map<Entity, Entity> sourceOf;
    for (const std::pair<std::string, Entity>& entry : anim::descendantPaths(scene, root)){
        if (entry.first.empty()) continue;
        const Transform* transform = scene->findComponent<Transform>(entry.second);
        const auto parent = transform ? copyOf.find(transform->parent) : copyOf.end();
        Entity entity = registry->createEntity();
        registry->setEntityName(entity, scene->getEntityName(entry.second));
        registry->addComponent<Transform>(entity, {});
        registry->addEntityChild(parent == copyOf.end() ? out.root : parent->second, entity, false);
        copyOf[entry.second] = entity;
        sourceOf[entity] = entry.second;
        out.paths.push_back(entry.first);
        out.entities.push_back(entity);
        out.depths.push_back((int)std::count(entry.first.begin(), entry.first.end(), '/'));
    }
    const anim::HumanoidSlots slots = anim::resolveHumanoidSlots(scene, root);
    out.humanoid = anim::HumanoidSlots();
    out.humanoid.valid = slots.valid;
    out.humanoid.error = slots.error;
    for (int slot = 0; slot < pose::HUMAN_BONE_COUNT; slot++){
        const auto copy = copyOf.find(slots.entities[(size_t)slot]);
        if (copy != copyOf.end()) out.humanoid.entities[(size_t)slot] = copy->second;
    }
    return true;
}
