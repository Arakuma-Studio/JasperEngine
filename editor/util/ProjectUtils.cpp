// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ProjectUtils.h"

#include "Out.h"
#include "Catalog.h"
#include "Stream.h"
#include "shader/ShaderVariantCompiler.h"
#include "util/Util.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <unordered_set>

#include "component/ActionComponent.h"
#include "component/AnimationComponent.h"
#include "component/BoneComponent.h"
#include "component/ButtonComponent.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "component/MeshComponent.h"
#include "component/SkyboxComponent.h"
#include "component/FogComponent.h"
#include "component/LightComponent.h"
#include "component/CameraComponent.h"
#include "component/SpriteComponent.h"
#include "component/PointsComponent.h"
#include "component/LineRendererComponent.h"
#include "component/TerrainComponent.h"
#include "component/AudioSourceComponent.h"
#include "component/LayoutGroupComponent.h"
#include "component/UIComponent.h"
#include "component/TextComponent.h"
#include "component/InputFieldComponent.h"
#include "component/ImageComponent.h"
#include "component/PanelComponent.h"
#include "component/ScrollbarComponent.h"
#include "component/ProgressbarComponent.h"
#include "component/PolygonComponent.h"
#include "component/MeshPolygonComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "subsystem/UISystem.h"
#include "subsystem/MeshSystem.h"
#include "component/RigidbodyComponent.h"
#include "component/Joint2DComponent.h"
#include "component/Joint3DComponent.h"

#include "component/TilemapComponent.h"
#include "component/InstancedMeshComponent.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PropertyCmd.h"

#include "EntityHandle.h"
#include "GameObject.h"
#include "Mesh.h"
#include "Primitive.h"
#include "Model.h"
#include "Points.h"
#include "Sprite.h"
#include "Light.h"
#include "Camera.h"

#include "resources/sky/Daylight_Box_Back_png.h"
#include "resources/sky/Daylight_Box_Bottom_png.h"
#include "resources/sky/Daylight_Box_Front_png.h"
#include "resources/sky/Daylight_Box_Left_png.h"
#include "resources/sky/Daylight_Box_Right_png.h"
#include "resources/sky/Daylight_Box_Top_png.h"

using namespace jasperengine;

static void parseLuaPropertiesTable(lua_State* L, ScriptEntry& entry);
static bool parseLuaPropertiesTableProtected(lua_State* L, ScriptEntry& entry, std::string* error);

namespace {

bool splitTrailingDuplicateNumber(const std::string& name, std::string& baseName, size_t& nextNumber){
    size_t end = name.find_last_not_of(' ');
    if (end == std::string::npos || !std::isdigit(static_cast<unsigned char>(name[end])))
        return false;

    size_t start = end;
    while (start > 0 && std::isdigit(static_cast<unsigned char>(name[start - 1]))) {
        start--;
    }

    if (start == 0 || name[start - 1] != ' ' || start - 1 == 0)
        return false;

    size_t value = 0;
    constexpr size_t maxValue = std::numeric_limits<size_t>::max();
    for (size_t i = start; i <= end; i++) {
        size_t digit = static_cast<size_t>(name[i] - '0');
        if (value > (maxValue - digit) / 10)
            return false;

        value = value * 10 + digit;
    }

    if (value == maxValue)
        return false;

    baseName = name.substr(0, start - 1);
    nextNumber = std::max<size_t>(2, value + 1);
    return true;
}

}

void editor::ProjectUtils::setDefaultSkyTexture(Texture& outTexture) {
    TextureData skyBack;
    TextureData skyBottom;
    TextureData skyFront;
    TextureData skyLeft;
    TextureData skyRight;
    TextureData skyTop;

    skyBack.loadTextureFromMemory(Daylight_Box_Back_png, Daylight_Box_Back_png_len);
    skyBottom.loadTextureFromMemory(Daylight_Box_Bottom_png, Daylight_Box_Bottom_png_len);
    skyFront.loadTextureFromMemory(Daylight_Box_Front_png, Daylight_Box_Front_png_len);
    skyLeft.loadTextureFromMemory(Daylight_Box_Left_png, Daylight_Box_Left_png_len);
    skyRight.loadTextureFromMemory(Daylight_Box_Right_png, Daylight_Box_Right_png_len);
    skyTop.loadTextureFromMemory(Daylight_Box_Top_png, Daylight_Box_Top_png_len);

    outTexture.setId("editor:resources:default_sky");
    outTexture.setCubeDatas("editor:resources:default_sky", skyFront, skyBack, skyLeft, skyRight, skyTop, skyBottom);
}

namespace {

std::string shaderTypeFileName(ShaderType shaderType) {
    switch (shaderType) {
        case ShaderType::MESH:   return "mesh";
        case ShaderType::DEPTH:  return "depth";
        case ShaderType::UI:     return "ui";
        case ShaderType::POINTS: return "points";
        case ShaderType::LINES:  return "lines";
        case ShaderType::SKYBOX: return "sky";
        case ShaderType::POSTPROCESS: return "postprocess";
        default:                 return "";
    }
}

std::string sanitizeShaderName(ShaderType shaderType, const std::string& desiredName) {
    std::string name;
    for (char c : desiredName) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
            name += c;
    }
    std::string typeFile = shaderTypeFileName(shaderType);
    if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_'))
        name = typeFile + name;
    return name.empty() ? "shader" : name;
}

// Every .glsl the entry points reach through the engine library, keyed by its
// include key ("includes/pbr.glsl"). Sorted, so the dialog preview is stable.
std::map<std::string, std::string> collectBuiltInShaderIncludes(const std::string& vertSource,
                                                                const std::string& fragSource) {
    std::map<std::string, std::string> collected;
    std::unordered_set<std::string> visited;

    std::function<void(const std::string&)> visitSource = [&](const std::string& source) {
        for (const std::string& key : ShaderVariantCompiler::getIncludeKeys(source)) {
            if (!visited.insert(key).second)
                continue;

            auto sourceIt = ShaderVariantCompiler::getLibrarySources().find(key);
            if (sourceIt == ShaderVariantCompiler::getLibrarySources().end())
                continue;

            if (std::filesystem::path(key).extension() == ".glsl")
                collected[key] = sourceIt->second;
            visitSource(sourceIt->second);
        }
    };

    visitSource(vertSource);
    visitSource(fragSource);
    return collected;
}

// Removes the directories a fork created. remove() only succeeds on an empty one,
// so anything the user put there survives.
void pruneForkDirectories(const std::filesystem::path& root) {
    if (root.empty())
        return;

    std::error_code ec;
    std::vector<std::filesystem::path> dirs;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec))
            dirs.push_back(it->path());
    }
    dirs.push_back(root);

    // The iterator visits parents first, so deleting in reverse empties the leaves first.
    for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) {
        std::filesystem::remove(*it, ec);
    }
}

} // namespace

std::filesystem::path editor::ProjectUtils::defaultShaderForkDir() {
    return "shaders";
}

std::string editor::ProjectUtils::makeUniqueShaderName(const std::filesystem::path& absDir,
                                                       ShaderType shaderType,
                                                       const std::string& desiredName) {
    // Both layouts are checked, so toggling "fork includes" cannot land on a name
    // that is free in one and taken in the other.
    std::string baseName = sanitizeShaderName(shaderType, desiredName);
    std::string name = baseName;
    int suffix = 2;
    while (std::filesystem::exists(absDir / (name + ".vert")) ||
           std::filesystem::exists(absDir / (name + ".frag")) ||
           std::filesystem::exists(absDir / name)) {
        name = baseName + std::to_string(suffix++);
    }
    return name;
}

editor::ProjectUtils::ShaderForkPlan editor::ProjectUtils::prepareShaderFork(
        Project* project, ShaderType shaderType, const std::filesystem::path& targetDirRel,
        const std::string& baseName, bool forkIncludes) {
    ShaderForkPlan plan;
    if (!project) {
        plan.error = "Project is unavailable.";
        return plan;
    }

    std::string typeFile = shaderTypeFileName(shaderType);
    if (typeFile.empty()) {
        plan.error = "This shader type cannot be forked.";
        return plan;
    }

    if (baseName.empty() || std::filesystem::path(baseName).filename() != baseName) {
        plan.error = "Enter a shader name without a path or extension.";
        return plan;
    }
    for (char c : baseName) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) {
            plan.error = "Shader names may contain only letters, numbers, and underscores.";
            return plan;
        }
    }

    std::filesystem::path normalizedDir = targetDirRel.empty() ? std::filesystem::path(".")
                                                               : targetDirRel.lexically_normal();
    if (!Util::isSafeRelativePath(normalizedDir)) {
        plan.error = "The shader directory must be inside the project.";
        return plan;
    }

    std::filesystem::path projectRoot = project->getProjectPath();

    // fullscreen passes share one vertex entry point; the fork still gets its own copy
    std::string vertFile = (shaderType == ShaderType::POSTPROCESS) ? "fullscreen" : typeFile;
    auto vertIt = ShaderVariantCompiler::getLibrarySources().find(vertFile + ".vert");
    auto fragIt = ShaderVariantCompiler::getLibrarySources().find(typeFile + ".frag");
    if (vertIt == ShaderVariantCompiler::getLibrarySources().end() || fragIt == ShaderVariantCompiler::getLibrarySources().end()) {
        plan.error = "The built-in shader source is unavailable.";
        return plan;
    }

    // Forked includes override the engine library for every entry point beside them,
    // so a fork carrying them gets a directory to itself.
    std::filesystem::path forkDirRel = forkIncludes ? normalizedDir / baseName : normalizedDir;
    std::filesystem::path forkDir = projectRoot / forkDirRel;

    plan.base = (forkDirRel / baseName).lexically_normal().generic_string();
    plan.vertPath = forkDir / (baseName + ".vert");
    plan.fragPath = forkDir / (baseName + ".frag");
    plan.vertContent = vertIt->second;
    plan.fragContent = fragIt->second;
    if (forkIncludes)
        plan.forkDirPath = forkDir;

    // With includes the whole directory must be free, so a fork never merges into
    // someone else's folder.
    std::vector<std::filesystem::path> collisions = {plan.vertPath, plan.fragPath};
    if (forkIncludes)
        collisions = {plan.forkDirPath};

    for (const std::filesystem::path& collision : collisions) {
        std::error_code ec;
        if (std::filesystem::exists(collision, ec) && !ec) {
            plan.error = "A fork target already exists: " +
                         std::filesystem::relative(collision, projectRoot, ec).generic_string();
            return plan;
        }
    }

    if (forkIncludes) {
        // The fork directory mirrors the shaderlib root, so each key keeps its shape:
        // "includes/pbr.glsl" -> <fork>/includes/pbr.glsl.
        for (const auto& [key, content] : collectBuiltInShaderIncludes(plan.vertContent, plan.fragContent)) {
            if (Util::isSafeRelativePath(key))
                plan.includeFiles.push_back({forkDir / key, content});
        }
    }

    plan.valid = true;
    return plan;
}

bool editor::ProjectUtils::writeShaderFork(const ShaderForkPlan& plan) {
    if (!plan.valid)
        return false;

    // Never overwrite: undoing a fork that clobbered a file would destroy its contents.
    std::vector<std::filesystem::path> written;
    auto writeFile = [&](const std::filesystem::path& path, const std::string& content) -> bool {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec)
            return false;
        if (std::filesystem::exists(path, ec) || ec)
            return false;

        std::ofstream f(path, std::ios::trunc);
        if (!f.is_open())
            return false;
        written.push_back(path);
        f << content;
        return static_cast<bool>(f);
    };

    bool success = writeFile(plan.vertPath, plan.vertContent) &&
                   writeFile(plan.fragPath, plan.fragContent);
    for (const auto& include : plan.includeFiles) {
        if (!success)
            break;
        success = writeFile(include.path, include.content);
    }

    if (!success) {
        std::error_code ec;
        for (const auto& path : written)
            std::filesystem::remove(path, ec);
        pruneForkDirectories(plan.forkDirPath);
        Out::error("Could not write forked shader files");
    }
    return success;
}

void editor::ProjectUtils::removeShaderFork(const ShaderForkPlan& plan) {
    std::error_code ec;
    std::filesystem::remove(plan.vertPath, ec);
    std::filesystem::remove(plan.fragPath, ec);
    for (const auto& include : plan.includeFiles)
        std::filesystem::remove(include.path, ec);
    pruneForkDirectories(plan.forkDirPath);
}

void editor::ProjectUtils::collectModelEntities(Scene* scene, const ModelComponent& model, std::vector<Entity>& out){
    if (!model.nodesIdMapping.empty()) {
        for (const auto& node : model.nodesIdMapping){
            out.push_back(node.second);
        }
    } else {
        for (const auto& bone : model.bonesIdMapping){
            out.push_back(bone.second);
        }
        for (const auto& node : model.meshNodesMapping){
            out.push_back(node.second);
        }
    }
    for (const auto& anim : model.animations){
        out.push_back(anim);
        AnimationComponent* animComp = scene->findComponent<AnimationComponent>(anim);
        if (animComp){
            for (const auto& frame : animComp->actions){
                if (frame.action != NULL_ENTITY){
                    out.push_back(frame.action);
                }
            }
        }
    }
}

bool editor::ProjectUtils::hasModelMeshChildrenWithoutRootGeometry(EntityRegistry* registry, Entity entity){
    if (!registry)
        return false;

    MeshComponent* rootMesh = registry->findComponent<MeshComponent>(entity);
    ModelComponent* model = registry->findComponent<ModelComponent>(entity);
    if (!rootMesh || !model || rootMesh->numSubmeshes != 0 || model->meshNodesMapping.empty())
        return false;

    for (const auto& node : model->meshNodesMapping){
        MeshComponent* childMesh = registry->findComponent<MeshComponent>(node.second);
        if (childMesh && childMesh->numSubmeshes > 0)
            return true;
    }

    return false;
}

Entity editor::ProjectUtils::getParentModel(Scene* scene, Entity entity) {
    Transform* transform = scene->findComponent<Transform>(entity);

    while (transform && transform->parent != NULL_ENTITY) {
        if (scene->findComponent<ModelComponent>(transform->parent)) {
            return transform->parent;
        }
        transform = scene->findComponent<Transform>(transform->parent);
    }

    return NULL_ENTITY;
}

Entity editor::ProjectUtils::getModelBranchOwner(Scene* scene, Entity entity) {
    Entity owner = scene->findComponent<ModelComponent>(entity) ? NULL_ENTITY : getParentModel(scene, entity);
    if (owner == NULL_ENTITY) {
        return NULL_ENTITY;
    }

    const ModelComponent& model = scene->getComponent<ModelComponent>(owner);
    for (const auto& node : model.meshNodesMapping) {
        if (node.second == entity) {
            return owner;
        }
    }

    // Bones and transform-only nodes are the model's own structure, not something to organize
    if (isModelNode(model, entity)) {
        return NULL_ENTITY;
    }

    // A local group answers to the model only while it holds imported parts
    for (const auto& node : model.meshNodesMapping) {
        if (scene->isParentOf(entity, node.second)) {
            return owner;
        }
    }

    return NULL_ENTITY;
}

bool editor::ProjectUtils::isModelNode(const ModelComponent& model, Entity entity) {
    if (entity == NULL_ENTITY) {
        return false;
    }
    if (entity == model.skeleton) {
        return true;
    }
    for (const auto* mapping : {&model.nodesIdMapping, &model.meshNodesMapping, &model.bonesIdMapping}) {
        for (const auto& node : *mapping) {
            if (node.second == entity) {
                return true;
            }
        }
    }
    return false;
}

bool editor::ProjectUtils::canEditModelBranch(Scene* scene, Entity entity, std::string* reason) {
    if (reason) reason->clear();

    Entity owner = getModelBranchOwner(scene, entity);
    if (owner == NULL_ENTITY) {
        Entity model = getParentModel(scene, entity);
        if (reason && model != NULL_ENTITY && isModelNode(scene->getComponent<ModelComponent>(model), entity)) {
            *reason = scene->findComponent<BoneComponent>(entity) ? "Joints cannot be reparented" : "Model nodes cannot be reparented";
        }
        return false;
    }

    const ModelComponent& model = scene->getComponent<ModelComponent>(owner);
    std::shared_ptr<MeshSystem> meshSys = scene->getSystem<MeshSystem>();
    return meshSys->canEditModelHierarchy(model, reason) && meshSys->canEditModelPart(model, entity, reason);
}

bool editor::ProjectUtils::hasCustomMeshParenting(Scene* scene, Entity model) {
    return scene->getSystem<MeshSystem>()->hasCustomMeshParenting(model, scene->getComponent<ModelComponent>(model));
}

Entity editor::ProjectUtils::getLockedEntityParent(Scene* scene, Entity entity){
    if (entity == NULL_ENTITY)
        return NULL_ENTITY;

    Signature signature = scene->getSignature(entity);

    // Foliage instances are resolved, not authored: they belong to their terrain the same way
    // model nodes belong to their model.
    Entity foliageOwner = scene->getSystem<MeshSystem>()->getFoliageOwner(entity);
    if (foliageOwner != NULL_ENTITY) {
        return foliageOwner;
    }

    auto models = scene->getComponentArray<ModelComponent>();
    for (size_t i = 0; i < models->size(); ++i) {
        Entity modelEntity = models->getEntity(i);
        ModelComponent& model = models->getComponentFromIndex(i);

        if (model.skeleton == entity) {
            return modelEntity;
        }

        for (const auto& anim : model.animations){
            if (anim == entity) {
                return modelEntity;
            }

            AnimationComponent* animComp = scene->findComponent<AnimationComponent>(anim);
            if (animComp){
                for (const auto& frame : animComp->actions){
                    if (frame.action == entity){
                        return anim;
                    }
                }
            }
        }

        for (const auto& bone : model.bonesIdMapping) {
            if (bone.second == entity) {
                Transform* transform = scene->findComponent<Transform>(entity);
                if (transform && transform->parent != NULL_ENTITY) {
                    return transform->parent;
                }
                return modelEntity;
            }
        }

        for (const auto& node : model.meshNodesMapping) {
            if (node.second == entity) {
                Transform* transform = scene->findComponent<Transform>(entity);
                if (transform && transform->parent != NULL_ENTITY) {
                    return transform->parent;
                }
                return modelEntity;
            }
        }
        for (const auto& node : model.nodesIdMapping) {
            if (node.second == entity) {
                Transform* transform = scene->findComponent<Transform>(entity);
                if (transform && transform->parent != NULL_ENTITY) {
                    return transform->parent;
                }
                return modelEntity;
            }
        }
    }

    if (!signature.test(scene->getComponentId<Transform>())){
        return NULL_ENTITY;
    }

    Transform& transform = scene->getComponent<Transform>(entity);
    if (transform.parent == NULL_ENTITY){
        return NULL_ENTITY;
    }

    Entity parent = transform.parent;
    Signature parentSignature = scene->getSignature(parent);

    if (parentSignature.test(scene->getComponentId<ButtonComponent>())){
        ButtonComponent& button = scene->getComponent<ButtonComponent>(parent);
        if (button.label == entity){
            return parent;
        }
    }

    if (parentSignature.test(scene->getComponentId<ScrollbarComponent>())){
        ScrollbarComponent& scrollbar = scene->getComponent<ScrollbarComponent>(parent);
        if (scrollbar.bar == entity){
            return parent;
        }
    }

    if (parentSignature.test(scene->getComponentId<ProgressbarComponent>())){
        ProgressbarComponent& progressbar = scene->getComponent<ProgressbarComponent>(parent);
        if (progressbar.fill == entity){
            return parent;
        }
    }

    if (parentSignature.test(scene->getComponentId<InputFieldComponent>())){
        InputFieldComponent& inputfield = scene->getComponent<InputFieldComponent>(parent);
        if (inputfield.text == entity || inputfield.selection == entity || inputfield.cursor == entity){
            return parent;
        }
    }

    auto panels = scene->getComponentArray<PanelComponent>();
    for (size_t i = 0; i < panels->size(); ++i) {
        Entity panelEntity = panels->getEntity(i);
        PanelComponent& panel = panels->getComponentFromIndex(i);
        if (panel.headerimage == entity || panel.headercontainer == entity || panel.headertext == entity) {
            return panelEntity;
        }
    }

    return NULL_ENTITY;
}

bool editor::ProjectUtils::isEntityLocked(Scene* scene, Entity entity){
    return getLockedEntityParent(scene, entity) != NULL_ENTITY;
}

Entity editor::ProjectUtils::getEffectiveParent(Scene* scene, Entity entity) {
    Entity virtualParent = getVirtualParent(scene, entity);
    if (virtualParent != NULL_ENTITY) {
        return virtualParent;
    }

    Transform* transform = scene->findComponent<Transform>(entity);
    if (transform) {
        return transform->parent;
    }

    return getLockedEntityParent(scene, entity);
}

bool editor::ProjectUtils::canMoveLockedEntityOrder(Scene* scene, Entity source, Entity target, InsertionType type, std::string* reason) {
    auto reject = [reason](const char* message) {
        if (reason) *reason = message;
        return false;
    };

    if (reason) reason->clear();

    Entity parent = (type == InsertionType::INTO) ? target : getEffectiveParent(scene, target);

    Entity owner = getModelBranchOwner(scene, source);
    if (owner != NULL_ENTITY) {
        // The nearest model at or above the destination, so parts cannot escape through a
        // local group nor land inside a nested model
        Entity destination = scene->findComponent<ModelComponent>(parent) ? parent : getParentModel(scene, parent);
        if (destination != owner) {
            return reject("Imported parts must stay inside their own model");
        }
        if (parent != getEffectiveParent(scene, source) && !canEditModelBranch(scene, source, reason)) {
            // A locked part can still go back under the node the file gave it
            const ModelComponent& model = scene->getComponent<ModelComponent>(owner);
            std::shared_ptr<MeshSystem> meshSys = scene->getSystem<MeshSystem>();
            if (!meshSys->canEditModelHierarchy(model)) {
                return false;
            }
            std::map<int, Entity> defaults = meshSys->getModelNodeDefaultParents(owner, model);
            for (const auto& node : model.meshNodesMapping) {
                auto defaultParent = defaults.find(node.first);
                if (node.second == source && defaultParent != defaults.end() && defaultParent->second == parent) {
                    if (reason) reason->clear();
                    return true;
                }
            }
            return false;
        }

        return true;
    }

    if (!isEntityLocked(scene, source)) {
        return true;
    }

    if (type != InsertionType::INTO && getEffectiveParent(scene, source) == getEffectiveParent(scene, target)) {
        return true;
    }

    return reject("Generated entities can only be reordered within the same parent");
}

std::string editor::ProjectUtils::makeUniqueEntityName(const std::string& baseName, const std::unordered_set<std::string>& existingNames) {
    if (baseName.empty() || existingNames.find(baseName) == existingNames.end()) {
        return baseName;
    }

    std::string cleanBase = baseName;
    size_t nameCount = 2;
    splitTrailingDuplicateNumber(baseName, cleanBase, nameCount);

    std::string uniqueName = cleanBase + " " + std::to_string(nameCount++);
    while (existingNames.find(uniqueName) != existingNames.end()) {
        uniqueName = cleanBase + " " + std::to_string(nameCount++);
    }

    return uniqueName;
}

editor::UniqueEntityNames::UniqueEntityNames(std::unordered_set<std::string> existingNames)
    : existingNames(std::move(existingNames)) {
}

std::string editor::UniqueEntityNames::make(const std::string& baseName) {
    // What makeUniqueEntityName gives: the name itself when free, else the first free
    // "<base> <n>" from the number after its own
    if (baseName.empty() || existingNames.find(baseName) == existingNames.end()) {
        existingNames.insert(baseName);
        return baseName;
    }

    std::string cleanBase = baseName;
    size_t nameCount = 2;
    splitTrailingDuplicateNumber(baseName, cleanBase, nameCount);

    // Every number of a known taken run is skipped at once
    size_t number = nameCount;
    auto run = takenRuns.find(cleanBase);
    const bool extendsRun = run != takenRuns.end() && run->second.first <= number && number <= run->second.second + 1;
    if (extendsRun) {
        number = run->second.second + 1;
    }

    std::string uniqueName = cleanBase + " " + std::to_string(number);
    while (existingNames.find(uniqueName) != existingNames.end()) {
        uniqueName = cleanBase + " " + std::to_string(++number);
    }
    existingNames.insert(uniqueName);

    if (extendsRun) {
        run->second.second = number;
    } else {
        takenRuns[cleanBase] = {nameCount, number};
    }
    return uniqueName;
}

std::string editor::ProjectUtils::makeUniqueEntityName(Scene* scene, const std::vector<Entity>& entities, const std::string& baseName, const std::unordered_set<Entity>& ignoredEntities) {
    if (!scene || baseName.empty()) {
        return baseName;
    }

    std::unordered_set<std::string> existingNames;
    for (Entity sceneEntity : entities) {
        if (ignoredEntities.find(sceneEntity) != ignoredEntities.end()) {
            continue;
        }

        existingNames.insert(scene->getEntityName(sceneEntity));
    }

    return makeUniqueEntityName(baseName, existingNames);
}

size_t editor::ProjectUtils::getTransformIndex(EntityRegistry* registry, Entity entity){
    Signature signature = registry->getSignature(entity);
    if (signature.test(registry->getComponentId<Transform>())) {
        Transform& transform = registry->getComponent<Transform>(entity);
        auto transforms = registry->getComponentArray<Transform>();
        return transforms->getIndex(entity);
    }

    return 0;
}

void editor::ProjectUtils::sortEntitiesByTransformOrder(EntityRegistry* registry, std::vector<Entity>& entities) {
    // Key: Transform index, 0 without a Transform. Stable, so equal keys (the flat
    // GameObjects) keep the order they are listed in.
    auto transforms = registry->getComponentArray<Transform>();
    const ComponentId transformId = registry->getComponentId<Transform>();
    std::vector<std::pair<size_t, Entity>> keyed;
    keyed.reserve(entities.size());
    bool sorted = true;
    for (Entity entity : entities) {
        size_t key = 0;
        if (registry->isEntityCreated(entity) && registry->getSignature(entity).test(transformId)) {
            key = transforms->getIndex(entity);
        }
        if (!keyed.empty() && key < keyed.back().first) {
            sorted = false;
        }
        keyed.emplace_back(key, entity);
    }
    if (sorted) {
        return;
    }
    std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    for (size_t i = 0; i < keyed.size(); i++) {
        entities[i] = keyed[i].second;
    }
}

bool editor::ProjectUtils::moveEntityOrderByTarget(EntityRegistry* registry, std::vector<Entity>& entities, Entity source, Entity target, InsertionType type, Entity& oldParent, size_t& oldIndex, bool& hasTransform) {
    Transform* transformSource = registry->findComponent<Transform>(source);
    Transform* transformTarget = registry->findComponent<Transform>(target);

    if (transformSource && transformTarget){
        hasTransform = true;

        if (registry->isParentOf(source, target)){
            Out::error("Cannot move an entity to its own child");
            return false;
        }

        auto transforms = registry->getComponentArray<Transform>();

        size_t sourceTransformIndex = transforms->getIndex(source);
        size_t targetTransformIndex = transforms->getIndex(target);

        Entity newParent = NULL_ENTITY;
        if (type == InsertionType::INTO){
            newParent = target;
        }else{
            newParent = transformTarget->parent;
        }

        oldParent = transformSource->parent;
        oldIndex = sourceTransformIndex;

        if (type == InsertionType::AFTER || type == InsertionType::INTO){
            targetTransformIndex++;
        }

        moveEntityOrderByTransform(registry, entities, source, newParent, targetTransformIndex);

    }else{

        hasTransform = false;

        auto itSource = std::find(entities.begin(), entities.end(), source);
        auto itTarget = std::find(entities.begin(), entities.end(), target);

        if (itSource == entities.end() || itTarget == entities.end()) {
            Out::error("Source or Target entity not found in entities vector");
            return false;
        }

        oldIndex = std::distance(entities.begin(), itSource);

        Entity tempSource = *itSource;
        size_t sourceIndex = std::distance(entities.begin(), itSource);
        size_t targetIndex = std::distance(entities.begin(), itTarget);

        entities.erase(itSource);

        if (type == InsertionType::BEFORE) {
            if (sourceIndex < targetIndex) targetIndex--;
            entities.insert(entities.begin() + targetIndex, tempSource);
        } else if (type == InsertionType::AFTER) {
            if (sourceIndex < targetIndex) targetIndex--;
            entities.insert(entities.begin() + targetIndex + 1, tempSource);
        } else if (type == InsertionType::INTO) {
            // "IN" is ambiguous without hierarchy, treat as AFTER
            if (sourceIndex < targetIndex) targetIndex--;
            entities.insert(entities.begin() + targetIndex + 1, tempSource);
        }

    }

    return true;
}

void editor::ProjectUtils::moveEntityOrderByIndex(EntityRegistry* registry, std::vector<Entity>& entities, Entity source, Entity parent, size_t index, bool hasTransform, bool sortEntities){
    if (hasTransform){

        auto transforms = registry->getComponentArray<Transform>();
        size_t sourceTransformIndex = transforms->getIndex(source);

        size_t sizeOfSourceBranch = registry->findBranchLastIndex(source) - sourceTransformIndex + 1;
        if (sourceTransformIndex < index){
            index += sizeOfSourceBranch;
        }

        moveEntityOrderByTransform(registry, entities, source, parent, index, true, sortEntities);

    }else{

        auto itSource = std::find(entities.begin(), entities.end(), source);
        if (itSource == entities.end()) {
            Out::error("Source entity not found in entities vector for undo");
            return;
        }

        Entity tempSource = *itSource;
        entities.erase(itSource);

        // Clamp index for safety
        if (index > entities.size()) {
            entities.push_back(tempSource);
        } else {
            entities.insert(entities.begin() + index, tempSource);
        }

    }
}

void editor::ProjectUtils::moveEntityOrderByTransform(EntityRegistry* registry, std::vector<Entity>& entities, Entity source, Entity parent, size_t transformIndex, bool enableMove, bool sortEntities){
    registry->addEntityChild(parent, source, true);

    if (enableMove){
        registry->moveChildToIndex(source, transformIndex);
    }

    if (sortEntities){
        ProjectUtils::sortEntitiesByTransformOrder(registry, entities);
    }
}

void editor::ProjectUtils::addEntityComponent(EntityRegistry* registry, Entity entity, ComponentType componentType, std::vector<Entity>& entities, YAML::Node componentNode){
    switch (componentType) {
        case ComponentType::Transform:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<Transform>(entity, {});
            }else{
                registry->addComponent<Transform>(entity, Stream::decodeTransform(componentNode));
            }
            ProjectUtils::sortEntitiesByTransformOrder(registry, entities);
            break;
        case ComponentType::MeshComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<MeshComponent>(entity, {});
            }else{
                registry->addComponent<MeshComponent>(entity, Stream::decodeMeshComponent(componentNode));
            }
            break;
        case ComponentType::UIComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<UIComponent>(entity, {});
            }else{
                registry->addComponent<UIComponent>(entity, Stream::decodeUIComponent(componentNode));
            }
            break;
        case ComponentType::RectTransformComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<RectTransformComponent>(entity, {});
            }else{
                registry->addComponent<RectTransformComponent>(entity, Stream::decodeRectTransformComponent(componentNode));
            }
            break;
        case ComponentType::ActionComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<ActionComponent>(entity, {});
            }else{
                registry->addComponent<ActionComponent>(entity, Stream::decodeActionComponent(componentNode));
            }
            break;
        case ComponentType::AlphaTweenComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<AlphaTweenComponent>(entity, {});
            }else{
                registry->addComponent<AlphaTweenComponent>(entity, Stream::decodeAlphaTweenComponent(componentNode));
            }
            break;
        case ComponentType::AnimationComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<AnimationComponent>(entity, {});
            }else{
                registry->addComponent<AnimationComponent>(entity, Stream::decodeAnimationComponent(componentNode));
            }
            break;
        case ComponentType::AudioSourceComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<AudioSourceComponent>(entity, {});
            }else{
                registry->addComponent<AudioSourceComponent>(entity, Stream::decodeAudioSourceComponent(componentNode));
            }
            break;
        case ComponentType::Rigidbody2DComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<Rigidbody2DComponent>(entity, {});
            }else{
                registry->addComponent<Rigidbody2DComponent>(entity, Stream::decodeRigidbody2DComponent(componentNode));
            }
            break;
        case ComponentType::RigidbodyComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<RigidbodyComponent>(entity, {});
            }else{
                Transform* bodyTransform = registry->findComponent<Transform>(entity);
                const Vector3 bodyScale = bodyTransform ? bodyTransform->scale : Vector3::UNIT_SCALE;
                registry->addComponent<RigidbodyComponent>(entity, Stream::decodeRigidbodyComponent(componentNode, nullptr, bodyScale));
            }
            break;
        case ComponentType::BoneComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<BoneComponent>(entity, {});
            }else{
                registry->addComponent<BoneComponent>(entity, Stream::decodeBoneComponent(componentNode));
            }
            break;
        case ComponentType::ButtonComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<ButtonComponent>(entity, {});
            }else{
                registry->addComponent<ButtonComponent>(entity, Stream::decodeButtonComponent(componentNode));
            }
            break;
        case ComponentType::PrefabComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<PrefabComponent>(entity, {});
            }else{
                registry->addComponent<PrefabComponent>(entity, Stream::decodePrefabComponent(componentNode));
            }
            break;
        case ComponentType::CameraComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<CameraComponent>(entity, {});
            }else{
                registry->addComponent<CameraComponent>(entity, Stream::decodeCameraComponent(componentNode));
            }
            break;
        case ComponentType::ColorTweenComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<ColorTweenComponent>(entity, {});
            }else{
                registry->addComponent<ColorTweenComponent>(entity, Stream::decodeColorTweenComponent(componentNode));
            }
            break;
        case ComponentType::FogComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<FogComponent>(entity, {});
            }else{
                registry->addComponent<FogComponent>(entity, Stream::decodeFogComponent(componentNode));
            }
            break;
        case ComponentType::MirrorComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<MirrorComponent>(entity, {});
            }else{
                registry->addComponent<MirrorComponent>(entity, Stream::decodeMirrorComponent(componentNode));
            }
            break;
        case ComponentType::ReflectionProbeComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<ReflectionProbeComponent>(entity, {});
            }else{
                registry->addComponent<ReflectionProbeComponent>(entity, Stream::decodeReflectionProbeComponent(componentNode));
            }
            break;
        case ComponentType::Light2DComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<Light2DComponent>(entity, {});
            }else{
                registry->addComponent<Light2DComponent>(entity, Stream::decodeLight2DComponent(componentNode));
            }
            break;
        case ComponentType::ShadowCaster2DComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<ShadowCaster2DComponent>(entity, {});
            }else{
                registry->addComponent<ShadowCaster2DComponent>(entity, Stream::decodeShadowCaster2DComponent(componentNode));
            }
            break;
        case ComponentType::ImageComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<ImageComponent>(entity, {});
            }else{
                registry->addComponent<ImageComponent>(entity, Stream::decodeImageComponent(componentNode));
            }
            break;
        case ComponentType::InstancedMeshComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<InstancedMeshComponent>(entity, {});
            }else{
                registry->addComponent<InstancedMeshComponent>(entity, Stream::decodeInstancedMeshComponent(componentNode));
            }
            break;
        case ComponentType::Joint2DComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<Joint2DComponent>(entity, {});
            }else{
                registry->addComponent<Joint2DComponent>(entity, Stream::decodeJoint2DComponent(componentNode));
            }
            break;
        case ComponentType::Joint3DComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<Joint3DComponent>(entity, {});
            }else{
                registry->addComponent<Joint3DComponent>(entity, Stream::decodeJoint3DComponent(componentNode));
            }
            break;
        case ComponentType::KeyframeTracksComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<KeyframeTracksComponent>(entity, {});
            }else{
                registry->addComponent<KeyframeTracksComponent>(entity, Stream::decodeKeyframeTracksComponent(componentNode));
            }
            break;
        case ComponentType::LightComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<LightComponent>(entity, {});
            }else{
                registry->addComponent<LightComponent>(entity, Stream::decodeLightComponent(componentNode));
            }
            break;
        case ComponentType::LineRendererComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<LineRendererComponent>(entity, {});
            }else{
                registry->addComponent<LineRendererComponent>(entity, Stream::decodeLineRendererComponent(componentNode));
            }
            break;
        case ComponentType::MeshPolygonComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<MeshPolygonComponent>(entity, {});
            }else{
                registry->addComponent<MeshPolygonComponent>(entity, Stream::decodeMeshPolygonComponent(componentNode));
            }
            break;
        case ComponentType::ModelComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<ModelComponent>(entity, {});
            }else{
                registry->addComponent<ModelComponent>(entity, Stream::decodeModelComponent(componentNode));
            }
            break;
        case ComponentType::MorphTracksComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<MorphTracksComponent>(entity, {});
            }else{
                registry->addComponent<MorphTracksComponent>(entity, Stream::decodeMorphTracksComponent(componentNode));
            }
            break;
        case ComponentType::PanelComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<PanelComponent>(entity, {});
            }else{
                registry->addComponent<PanelComponent>(entity, Stream::decodePanelComponent(componentNode));
            }
            break;
        case ComponentType::ParticleSystemComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<ParticleSystemComponent>(entity, {});
            }else{
                registry->addComponent<ParticleSystemComponent>(entity, Stream::decodeParticleSystemComponent(componentNode));
            }
            break;
        case ComponentType::PointsComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<PointsComponent>(entity, {});
            }else{
                registry->addComponent<PointsComponent>(entity, Stream::decodePointsComponent(componentNode));
            }
            break;
        case ComponentType::PolygonComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<PolygonComponent>(entity, {});
            }else{
                registry->addComponent<PolygonComponent>(entity, Stream::decodePolygonComponent(componentNode));
            }
            break;
        case ComponentType::PositionTweenComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<PositionTweenComponent>(entity, {});
            }else{
                registry->addComponent<PositionTweenComponent>(entity, Stream::decodePositionTweenComponent(componentNode));
            }
            break;
        case ComponentType::ProgressbarComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<ProgressbarComponent>(entity, {});
            }else{
                registry->addComponent<ProgressbarComponent>(entity, Stream::decodeProgressbarComponent(componentNode));
            }
            break;
        case ComponentType::RotateTracksComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<RotateTracksComponent>(entity, {});
            }else{
                registry->addComponent<RotateTracksComponent>(entity, Stream::decodeRotateTracksComponent(componentNode));
            }
            break;
        case ComponentType::RotationTweenComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<RotationTweenComponent>(entity, {});
            }else{
                registry->addComponent<RotationTweenComponent>(entity, Stream::decodeRotationTweenComponent(componentNode));
            }
            break;
        case ComponentType::ScaleTweenComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<ScaleTweenComponent>(entity, {});
            }else{
                registry->addComponent<ScaleTweenComponent>(entity, Stream::decodeScaleTweenComponent(componentNode));
            }
            break;
        case ComponentType::ScaleTracksComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<ScaleTracksComponent>(entity, {});
            }else{
                registry->addComponent<ScaleTracksComponent>(entity, Stream::decodeScaleTracksComponent(componentNode));
            }
            break;
        case ComponentType::ScriptComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<ScriptComponent>(entity, {});
            }else{
                registry->addComponent<ScriptComponent>(entity, Stream::decodeScriptComponent(componentNode));
            }
            break;
        case ComponentType::ScrollbarComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<ScrollbarComponent>(entity, {});
            }else{
                registry->addComponent<ScrollbarComponent>(entity, Stream::decodeScrollbarComponent(componentNode));
            }
            break;
        case ComponentType::SkyboxComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<SkyboxComponent>(entity, {});
            }else{
                registry->addComponent<SkyboxComponent>(entity, Stream::decodeSkyboxComponent(componentNode));
            }
            break;
        case ComponentType::SpriteAnimationComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<SpriteAnimationComponent>(entity, {});
            }else{
                registry->addComponent<SpriteAnimationComponent>(entity, Stream::decodeSpriteAnimationComponent(componentNode));
            }
            break;
        case ComponentType::SpriteComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<SpriteComponent>(entity, {});
            }else{
                registry->addComponent<SpriteComponent>(entity, Stream::decodeSpriteComponent(componentNode));
            }
            break;
        case ComponentType::TerrainComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<TerrainComponent>(entity, {});
            }else{
                registry->addComponent<TerrainComponent>(entity, Stream::decodeTerrainComponent(componentNode));
            }
            break;
        case ComponentType::TextComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<TextComponent>(entity, {});
            }else{
                registry->addComponent<TextComponent>(entity, Stream::decodeTextComponent(componentNode));
            }
            break;
        case ComponentType::InputFieldComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<InputFieldComponent>(entity, {});
            }else{
                registry->addComponent<InputFieldComponent>(entity, Stream::decodeInputFieldComponent(componentNode));
            }
            break;
        case ComponentType::TilemapComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<TilemapComponent>(entity, {});
            }else{
                registry->addComponent<TilemapComponent>(entity, Stream::decodeTilemapComponent(componentNode));
            }
            break;
        case ComponentType::TweenComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<TweenComponent>(entity, {});
            }else{
                registry->addComponent<TweenComponent>(entity, Stream::decodeTweenComponent(componentNode));
            }
            break;
        case ComponentType::TranslateTracksComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<TranslateTracksComponent>(entity, {});
            }else{
                registry->addComponent<TranslateTracksComponent>(entity, Stream::decodeTranslateTracksComponent(componentNode));
            }
            break;
        case ComponentType::LayoutGroupComponent:
            if (!componentNode.IsDefined() || componentNode.IsNull()){
                registry->addComponent<LayoutGroupComponent>(entity, {});
            }else{
                registry->addComponent<LayoutGroupComponent>(entity, Stream::decodeLayoutGroupComponent(componentNode));
            }
            break;
        default:
            break;
    }

    Catalog::updateEntity(registry, entity, Catalog::getComponentStructuralUpdateFlags(componentType));
}

Entity editor::ProjectUtils::getVirtualParent(Scene* scene, Entity entity) {
    Signature signature = scene->getSignature(entity);
    if (!signature.test(scene->getComponentId<ActionComponent>())) return NULL_ENTITY;
    if (signature.test(scene->getComponentId<Transform>())) return NULL_ENTITY;

    ActionComponent& action = scene->getComponent<ActionComponent>(entity);
    if (action.target != NULL_ENTITY && action.target != entity) {
        return action.target;
    }

    return NULL_ENTITY;
}

std::vector<Entity> editor::ProjectUtils::getVirtualChildren(Scene* scene, const std::vector<Entity>& parentEntities, const std::unordered_set<Entity>& excludedEntities) {
    std::vector<Entity> result;
    auto actionArr = scene->getComponentArray<ActionComponent>();
    if (!actionArr) {
        return result;
    }

    // Expand parent entities to include all Transform descendants
    std::vector<Entity> allEntities = parentEntities;
    auto transforms = scene->getComponentArray<Transform>();
    if (transforms) {
        for (const Entity& parent : parentEntities) {
            if (!scene->findComponent<Transform>(parent)) {
                continue;
            }
            size_t parentIdx = transforms->getIndex(parent);
            size_t branchEnd = scene->findBranchLastIndex(parent);
            for (size_t i = parentIdx + 1; i <= branchEnd; ++i) {
                Entity descendant = transforms->getEntity(i);
                if (std::find(allEntities.begin(), allEntities.end(), descendant) == allEntities.end()) {
                    allEntities.push_back(descendant);
                }
            }
        }
    }

    bool foundNew;
    do {
        foundNew = false;
        for (size_t i = 0; i < actionArr->size(); ++i) {
            Entity entity = actionArr->getEntity(i);
            if (excludedEntities.count(entity)) {
                continue;
            }
            Entity vParent = getVirtualParent(scene, entity);
            if (vParent != NULL_ENTITY
                && std::find(allEntities.begin(), allEntities.end(), vParent) != allEntities.end()
                && std::find(allEntities.begin(), allEntities.end(), entity) == allEntities.end()) {
                allEntities.push_back(entity);
                result.push_back(entity);
                foundNew = true;
            }
        }
    } while (foundNew);

    return result;
}

std::vector<Entity> editor::ProjectUtils::getOwnedCascadeEntities(EntityRegistry* registry,
        Entity owner, ComponentType componentType) {
    std::vector<Entity> result;
    auto append = [&](Entity entity) {
        if (entity != NULL_ENTITY && entity != owner &&
            std::find(result.begin(), result.end(), entity) == result.end()) {
            result.push_back(entity);
        }
    };

    if (componentType == ComponentType::ActionComponent) {
        const ActionComponent* action = registry->findComponent<ActionComponent>(owner);
        if (action && action->ownedTarget) {
            append(action->target);
        }
    } else if (componentType == ComponentType::AnimationComponent) {
        const AnimationComponent* animation = registry->findComponent<AnimationComponent>(owner);
        if (animation && animation->ownedActions) {
            for (const ActionFrame& frame : animation->actions) {
                append(frame.action);
            }
        }
    }

    return result;
}

YAML::Node editor::ProjectUtils::removeEntityComponent(EntityRegistry* registry, Entity entity, ComponentType componentType, std::vector<Entity>& entities, bool encodeComponent){
    YAML::Node oldComponent;

    // Editor commands own the undo policy for owned targets/actions. Suppress those two
    // callbacks here so component removal never destroys entities outside its recovery data.
    switch (componentType) {
        case ComponentType::Transform:
            if (encodeComponent){
                oldComponent = Stream::encodeTransform(registry->getComponent<Transform>(entity));
            }
            registry->removeComponent<Transform>(entity);
            ProjectUtils::sortEntitiesByTransformOrder(registry, entities);
            break;
        case ComponentType::MeshComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeMeshComponent(registry->getComponent<MeshComponent>(entity));
            }
            registry->removeComponent<MeshComponent>(entity);
            break;
        case ComponentType::UIComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeUIComponent(registry->getComponent<UIComponent>(entity));
            }
            registry->removeComponent<UIComponent>(entity);
            break;
        case ComponentType::RectTransformComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeRectTransformComponent(registry->getComponent<RectTransformComponent>(entity));
            }
            registry->removeComponent<RectTransformComponent>(entity);
            break;
        case ComponentType::ActionComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeActionComponent(registry->getComponent<ActionComponent>(entity));
            }
            registry->getComponent<ActionComponent>(entity).ownedTarget = false;
            registry->removeComponent<ActionComponent>(entity);
            break;
        case ComponentType::AlphaTweenComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeAlphaTweenComponent(registry->getComponent<AlphaTweenComponent>(entity));
            }
            registry->removeComponent<AlphaTweenComponent>(entity);
            break;
        case ComponentType::AnimationComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeAnimationComponent(registry->getComponent<AnimationComponent>(entity));
            }
            registry->getComponent<AnimationComponent>(entity).ownedActions = false;
            registry->removeComponent<AnimationComponent>(entity);
            break;
        case ComponentType::AudioSourceComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeAudioSourceComponent(registry->getComponent<AudioSourceComponent>(entity));
            }
            registry->removeComponent<AudioSourceComponent>(entity);
            break;
        case ComponentType::Rigidbody2DComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeRigidbody2DComponent(registry->getComponent<Rigidbody2DComponent>(entity));
            }
            registry->removeComponent<Rigidbody2DComponent>(entity);
            break;
        case ComponentType::RigidbodyComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeRigidbodyComponent(registry->getComponent<RigidbodyComponent>(entity));
            }
            registry->removeComponent<RigidbodyComponent>(entity);
            break;
        case ComponentType::BoneComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeBoneComponent(registry->getComponent<BoneComponent>(entity));
            }
            registry->removeComponent<BoneComponent>(entity);
            break;
        case ComponentType::ButtonComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeButtonComponent(registry->getComponent<ButtonComponent>(entity));
            }
            registry->removeComponent<ButtonComponent>(entity);
            break;
        case ComponentType::PrefabComponent:
            if (encodeComponent){
                oldComponent = Stream::encodePrefabComponent(registry->getComponent<PrefabComponent>(entity));
            }
            registry->removeComponent<PrefabComponent>(entity);
            break;
        case ComponentType::CameraComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeCameraComponent(registry->getComponent<CameraComponent>(entity));
            }
            registry->removeComponent<CameraComponent>(entity);
            break;
        case ComponentType::ColorTweenComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeColorTweenComponent(registry->getComponent<ColorTweenComponent>(entity));
            }
            registry->removeComponent<ColorTweenComponent>(entity);
            break;
        case ComponentType::FogComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeFogComponent(registry->getComponent<FogComponent>(entity));
            }
            registry->removeComponent<FogComponent>(entity);
            break;
        case ComponentType::MirrorComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeMirrorComponent(registry->getComponent<MirrorComponent>(entity));
            }
            registry->removeComponent<MirrorComponent>(entity);
            break;
        case ComponentType::ReflectionProbeComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeReflectionProbeComponent(registry->getComponent<ReflectionProbeComponent>(entity));
            }
            registry->removeComponent<ReflectionProbeComponent>(entity);
            break;
        case ComponentType::Light2DComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeLight2DComponent(registry->getComponent<Light2DComponent>(entity));
            }
            registry->removeComponent<Light2DComponent>(entity);
            break;
        case ComponentType::ShadowCaster2DComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeShadowCaster2DComponent(registry->getComponent<ShadowCaster2DComponent>(entity));
            }
            registry->removeComponent<ShadowCaster2DComponent>(entity);
            break;
        case ComponentType::ImageComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeImageComponent(registry->getComponent<ImageComponent>(entity));
            }
            registry->removeComponent<ImageComponent>(entity);
            break;
        case ComponentType::InstancedMeshComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeInstancedMeshComponent(registry->getComponent<InstancedMeshComponent>(entity));
            }
            registry->removeComponent<InstancedMeshComponent>(entity);
            break;
        case ComponentType::Joint2DComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeJoint2DComponent(registry->getComponent<Joint2DComponent>(entity));
            }
            registry->removeComponent<Joint2DComponent>(entity);
            break;
        case ComponentType::Joint3DComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeJoint3DComponent(registry->getComponent<Joint3DComponent>(entity));
            }
            registry->removeComponent<Joint3DComponent>(entity);
            break;
        case ComponentType::KeyframeTracksComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeKeyframeTracksComponent(registry->getComponent<KeyframeTracksComponent>(entity));
            }
            registry->removeComponent<KeyframeTracksComponent>(entity);
            break;
        case ComponentType::LightComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeLightComponent(registry->getComponent<LightComponent>(entity));
            }
            registry->removeComponent<LightComponent>(entity);
            break;
        case ComponentType::LineRendererComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeLineRendererComponent(registry->getComponent<LineRendererComponent>(entity));
            }
            registry->removeComponent<LineRendererComponent>(entity);
            break;
        case ComponentType::MeshPolygonComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeMeshPolygonComponent(registry->getComponent<MeshPolygonComponent>(entity));
            }
            registry->removeComponent<MeshPolygonComponent>(entity);
            break;
        case ComponentType::ModelComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeModelComponent(registry->getComponent<ModelComponent>(entity));
            }
            registry->removeComponent<ModelComponent>(entity);
            break;
        case ComponentType::MorphTracksComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeMorphTracksComponent(registry->getComponent<MorphTracksComponent>(entity));
            }
            registry->removeComponent<MorphTracksComponent>(entity);
            break;
        case ComponentType::PanelComponent:
            if (encodeComponent){
                oldComponent = Stream::encodePanelComponent(registry->getComponent<PanelComponent>(entity));
            }
            registry->removeComponent<PanelComponent>(entity);
            break;
        case ComponentType::ParticleSystemComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeParticleSystemComponent(registry->getComponent<ParticleSystemComponent>(entity));
            }
            registry->removeComponent<ParticleSystemComponent>(entity);
            break;
        case ComponentType::PointsComponent:
            if (encodeComponent){
                oldComponent = Stream::encodePointsComponent(registry->getComponent<PointsComponent>(entity));
            }
            registry->removeComponent<PointsComponent>(entity);
            break;
        case ComponentType::PolygonComponent:
            if (encodeComponent){
                oldComponent = Stream::encodePolygonComponent(registry->getComponent<PolygonComponent>(entity));
            }
            registry->removeComponent<PolygonComponent>(entity);
            break;
        case ComponentType::PositionTweenComponent:
            if (encodeComponent){
                oldComponent = Stream::encodePositionTweenComponent(registry->getComponent<PositionTweenComponent>(entity));
            }
            registry->removeComponent<PositionTweenComponent>(entity);
            break;
        case ComponentType::ProgressbarComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeProgressbarComponent(registry->getComponent<ProgressbarComponent>(entity));
            }
            registry->removeComponent<ProgressbarComponent>(entity);
            break;
        case ComponentType::RotateTracksComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeRotateTracksComponent(registry->getComponent<RotateTracksComponent>(entity));
            }
            registry->removeComponent<RotateTracksComponent>(entity);
            break;
        case ComponentType::RotationTweenComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeRotationTweenComponent(registry->getComponent<RotationTweenComponent>(entity));
            }
            registry->removeComponent<RotationTweenComponent>(entity);
            break;
        case ComponentType::ScaleTweenComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeScaleTweenComponent(registry->getComponent<ScaleTweenComponent>(entity));
            }
            registry->removeComponent<ScaleTweenComponent>(entity);
            break;
        case ComponentType::ScaleTracksComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeScaleTracksComponent(registry->getComponent<ScaleTracksComponent>(entity));
            }
            registry->removeComponent<ScaleTracksComponent>(entity);
            break;
        case ComponentType::ScriptComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeScriptComponent(registry->getComponent<ScriptComponent>(entity));
            }
            registry->removeComponent<ScriptComponent>(entity);
            break;
        case ComponentType::ScrollbarComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeScrollbarComponent(registry->getComponent<ScrollbarComponent>(entity));
            }
            registry->removeComponent<ScrollbarComponent>(entity);
            break;
        case ComponentType::SkyboxComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeSkyboxComponent(registry->getComponent<SkyboxComponent>(entity));
            }
            registry->removeComponent<SkyboxComponent>(entity);
            break;
        case ComponentType::SpriteAnimationComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeSpriteAnimationComponent(registry->getComponent<SpriteAnimationComponent>(entity));
            }
            registry->removeComponent<SpriteAnimationComponent>(entity);
            break;
        case ComponentType::SpriteComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeSpriteComponent(registry->getComponent<SpriteComponent>(entity));
            }
            registry->removeComponent<SpriteComponent>(entity);
            break;
        case ComponentType::TerrainComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeTerrainComponent(registry->getComponent<TerrainComponent>(entity));
            }
            registry->removeComponent<TerrainComponent>(entity);
            break;
        case ComponentType::TextComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeTextComponent(registry->getComponent<TextComponent>(entity));
            }
            registry->removeComponent<TextComponent>(entity);
            break;
        case ComponentType::InputFieldComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeInputFieldComponent(registry->getComponent<InputFieldComponent>(entity));
            }
            registry->removeComponent<InputFieldComponent>(entity);
            break;
        case ComponentType::TilemapComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeTilemapComponent(registry->getComponent<TilemapComponent>(entity));
            }
            registry->removeComponent<TilemapComponent>(entity);
            break;
        case ComponentType::TweenComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeTweenComponent(registry->getComponent<TweenComponent>(entity));
            }
            registry->removeComponent<TweenComponent>(entity);
            break;
        case ComponentType::TranslateTracksComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeTranslateTracksComponent(registry->getComponent<TranslateTracksComponent>(entity));
            }
            registry->removeComponent<TranslateTracksComponent>(entity);
            break;
        case ComponentType::LayoutGroupComponent:
            if (encodeComponent){
                oldComponent = Stream::encodeLayoutGroupComponent(registry->getComponent<LayoutGroupComponent>(entity));
            }
            registry->removeComponent<LayoutGroupComponent>(entity);
            break;
        default:
            break;
    }

    Catalog::updateEntity(registry, entity, Catalog::getComponentStructuralUpdateFlags(componentType));

    return oldComponent;
}

void editor::ProjectUtils::reconcileTrackedEntities(EntityRegistry* registry, std::vector<Entity>& entities, Project* project, uint32_t sceneId){
    bool clearSelection = false;
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;

    auto firstRemoved = std::remove_if(entities.begin(), entities.end(), [&](Entity entity) {
        if (registry->isEntityCreated(entity)) {
            return false;
        }
        if (project && project->isSelectedEntity(sceneId, entity)) {
            clearSelection = true;
        }
        if (sceneProject && sceneProject->mainCamera == entity) {
            sceneProject->mainCamera = NULL_ENTITY;
        }
        return true;
    });
    entities.erase(firstRemoved, entities.end());

    if (clearSelection) {
        project->clearSelectedEntities(sceneId);
    }
}

ScriptPropertyValue editor::ProjectUtils::luaValueToScriptPropertyValue(lua_State* L, int idx, ScriptPropertyType type) {
    // The vector cases push onto the stack, moving a relative index off the table
    idx = lua_absindex(L, idx);

    switch (type) {
    case ScriptPropertyType::Bool:
        return ScriptPropertyValue(lua_toboolean(L, idx) != 0);

    case ScriptPropertyType::Int:
        return ScriptPropertyValue((int)luaL_optinteger(L, idx, 0));

    case ScriptPropertyType::Float:
        return ScriptPropertyValue((float)luaL_optnumber(L, idx, 0.0));

    case ScriptPropertyType::String:
        if (lua_isstring(L, idx))
            return ScriptPropertyValue(std::string(lua_tostring(L, idx)));
        return ScriptPropertyValue(std::string());

    case ScriptPropertyType::Vector2: {
        Vector2 v(0, 0);
        if (lua_istable(L, idx)) {
            lua_rawgeti(L, idx, 1);
            lua_rawgeti(L, idx, 2);
            v.x = (float)luaL_optnumber(L, -2, 0.0);
            v.y = (float)luaL_optnumber(L, -1, 0.0);
            lua_pop(L, 2);
        }
        return ScriptPropertyValue(v);
    }

    case ScriptPropertyType::Vector3:
    case ScriptPropertyType::Color3: {
        Vector3 v(0, 0, 0);
        if (lua_istable(L, idx)) {
            lua_rawgeti(L, idx, 1);
            lua_rawgeti(L, idx, 2);
            lua_rawgeti(L, idx, 3);
            v.x = (float)luaL_optnumber(L, -3, 0.0);
            v.y = (float)luaL_optnumber(L, -2, 0.0);
            v.z = (float)luaL_optnumber(L, -1, 0.0);
            lua_pop(L, 3);
        }
        return ScriptPropertyValue(v);
    }

    case ScriptPropertyType::Vector4:
    case ScriptPropertyType::Color4: {
        Vector4 v(0, 0, 0, 1);
        if (lua_istable(L, idx)) {
            lua_rawgeti(L, idx, 1);
            lua_rawgeti(L, idx, 2);
            lua_rawgeti(L, idx, 3);
            lua_rawgeti(L, idx, 4);
            v.x = (float)luaL_optnumber(L, -4, 0.0);
            v.y = (float)luaL_optnumber(L, -3, 0.0);
            v.z = (float)luaL_optnumber(L, -2, 0.0);
            v.w = (float)luaL_optnumber(L, -1, 1.0);
            lua_pop(L, 4);
        }
        return ScriptPropertyValue(v);
    }

    case ScriptPropertyType::EntityReference: {
        // For now, leave as null entity. The editor will fill it later.
        return ScriptPropertyValue(EntityReference{NULL_ENTITY, 0});
    }

    case ScriptPropertyType::Prefab: {
        // None: a prefab is dragged onto the property in the Inspector
        return ScriptPropertyValue(PrefabAsset());
    }
    }

    return ScriptPropertyValue{};
}

void editor::ProjectUtils::loadLuaScriptProperties(ScriptEntry& entry, const std::string& luaPath) {
    lua_State* L = LuaBinding::getLuaState();
    if (!L) return;

    if (luaL_dofile(L, luaPath.c_str()) != LUA_OK) {
        Out::error("Failed to load Lua script \"%s\": %s", luaPath.c_str(), lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }

    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }

    std::string error;
    if (!parseLuaPropertiesTableProtected(L, entry, &error)) {
        Out::error("Failed to read the properties of Lua script \"%s\": %s", luaPath.c_str(), error.c_str());
    }
}

void editor::ProjectUtils::loadLuaScriptPropertiesFromString(ScriptEntry& entry, const std::string& scriptContent, const std::string& chunkName) {
    lua_State* L = LuaBinding::getLuaState();
    if (!L) return;

    if (luaL_dostring(L, scriptContent.c_str()) != LUA_OK) {
        // Silently ignore parse errors for in-memory content (code may be mid-edit)
        lua_pop(L, 1);
        return;
    }

    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }

    // a property that can't be read is ignored like a parse error (the code may be mid-edit)
    parseLuaPropertiesTableProtected(L, entry, nullptr);
}

editor::ProjectUtils::LuaPropertiesResult editor::ProjectUtils::runLuaScriptProperties(lua_State* L, ScriptEntry& entry, const std::string& code, const std::string& chunkName) {
    LuaPropertiesResult result;
    if (!L) return result;

    const int top = lua_gettop(L);
    const std::string chunk = "@" + chunkName;
    if (luaL_loadbuffer(L, code.data(), code.size(), chunk.c_str()) != LUA_OK) {
        result.status = LuaPropertiesResult::LoadError;
        result.message = LuaBinding::getLuaStackErrorString(L, -1);
        lua_settop(L, top);
        return result;
    }
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        result.status = LuaPropertiesResult::RunError;
        result.message = LuaBinding::getLuaStackErrorString(L, -1);
        lua_settop(L, top);
        return result;
    }
    if (lua_istable(L, -1) && !parseLuaPropertiesTableProtected(L, entry, &result.message)) {
        result.status = LuaPropertiesResult::PropertyError;
    }
    lua_settop(L, top);
    return result;
}

namespace {
    int parseLuaPropertiesThunk(lua_State* L) {
        ScriptEntry* entry = static_cast<ScriptEntry*>(lua_touserdata(L, 1));
        lua_settop(L, 2);   // the script table on top, as parseLuaPropertiesTable wants it
        parseLuaPropertiesTable(L, *entry);
        return 0;
    }
}

// parseLuaPropertiesTable in a protected call: a property default luaL_opt* can't convert
// raises a Lua error, which unprotected would abort the editor. Pops the script table at the
// top of the stack, like parseLuaPropertiesTable. entry is left unchanged on failure.
static bool parseLuaPropertiesTableProtected(lua_State* L, ScriptEntry& entry, std::string* error) {
    const std::vector<ScriptProperty> previous = entry.properties;
    lua_pushcfunction(L, parseLuaPropertiesThunk);
    lua_pushlightuserdata(L, &entry);
    lua_pushvalue(L, -3);
    const bool ok = lua_pcall(L, 2, 0, 0) == LUA_OK;
    if (!ok) {
        if (error) *error = LuaBinding::getLuaStackErrorString(L, -1);
        lua_pop(L, 1);
        entry.properties = previous;
    }
    lua_pop(L, 1);  // the script table
    return ok;
}

static void parseLuaPropertiesTable(lua_State* L, ScriptEntry& entry) {

    lua_getfield(L, -1, "properties");  // Stack: script_table, properties_table

    if (lua_istable(L, -1)) {
        entry.properties.clear();

        lua_pushnil(L);  // Stack: script_table, properties_table, nil
        while (lua_next(L, -2) != 0) {  // Stack: script_table, properties_table, key, property_table
            if (lua_istable(L, -1)) {
                ScriptProperty prop;

                lua_getfield(L, -1, "name");
                if (lua_isstring(L, -1)) prop.name = lua_tostring(L, -1);
                lua_pop(L, 1);

                lua_getfield(L, -1, "displayName");
                prop.displayName = lua_isstring(L, -1) ? lua_tostring(L, -1) : prop.name;
                lua_pop(L, 1);

                lua_getfield(L, -1, "type");
                if (lua_isstring(L, -1)) {
                    std::string typeStr = lua_tostring(L, -1);
                    std::string typeLower = typeStr;
                    std::transform(typeLower.begin(), typeLower.end(), typeLower.begin(), 
                                [](unsigned char c){ return (char)std::tolower(c); });

                    if (typeLower == "bool" || typeLower == "boolean"){
                        prop.type = ScriptPropertyType::Bool;
                    }else if (typeLower == "int"  || typeLower == "integer"){
                        prop.type = ScriptPropertyType::Int;
                    }else if (typeLower == "float" || typeLower == "number"){
                        prop.type = ScriptPropertyType::Float;
                    }else if (typeLower == "string"){
                        prop.type = ScriptPropertyType::String;
                    }else if (typeLower == "vector2" || typeLower == "vec2"){
                        prop.type = ScriptPropertyType::Vector2;
                    }else if (typeLower == "vector3" || typeLower == "vec3"){
                        prop.type = ScriptPropertyType::Vector3;
                    }else if (typeLower == "vector4" || typeLower == "vec4"){
                        prop.type = ScriptPropertyType::Vector4;
                    }else if (typeLower == "color3"){
                        prop.type = ScriptPropertyType::Color3;
                    }else if (typeLower == "color4"){
                        prop.type = ScriptPropertyType::Color4;
                    }else if (typeLower == "prefab" || typeLower == "prefabasset"){
                        prop.type = ScriptPropertyType::Prefab;
                    }else if (typeLower == "entity" || typeLower == "entitypointer" || typeLower == "pointer"){
                        prop.type = ScriptPropertyType::EntityReference;
                    }else{
                        prop.type = ScriptPropertyType::EntityReference;
                        prop.ptrTypeName = typeStr;
                    }
                } else {
                    prop.type = ScriptPropertyType::Float;
                }
                lua_pop(L, 1);

                lua_getfield(L, -1, "default");
                if (!lua_isnoneornil(L, -1) && (prop.type == ScriptPropertyType::Int || prop.type == ScriptPropertyType::Float)) {
                    const bool isInt = prop.type == ScriptPropertyType::Int;
                    int convertible = 0;
                    if (isInt) lua_tointegerx(L, -1, &convertible);
                    else lua_tonumberx(L, -1, &convertible);
                    if (!convertible) {
                        const char* got = lua_isnumber(L, -1) ? lua_tostring(L, -1) : luaL_typename(L, -1);
                        luaL_error(L, "property '%s' (%s): default must be %s, got %s", prop.name.c_str(),
                                   isInt ? "int" : "float", isInt ? "an integer" : "a number", got);
                    }
                }
                prop.defaultValue = editor::ProjectUtils::luaValueToScriptPropertyValue(L, -1, prop.type);
                prop.value = prop.defaultValue;
                lua_pop(L, 1);

                entry.properties.push_back(std::move(prop));
            }
            lua_pop(L, 1);  // pop value, keep key
        }
    }

    lua_pop(L, 2);  // pop properties_table and script_table
}

void editor::ProjectUtils::collectEntities(const YAML::Node& entityNode, std::vector<Entity>& allEntities) {
    if (!entityNode || !entityNode.IsMap())
        return;

    if (entityNode["members"] && entityNode["members"].IsSequence()) {
        for (const auto& member : entityNode["members"]) {
            collectEntities(member, allEntities);
        }
        return;
    }

    if (entityNode["entity"]) {
        allEntities.push_back(entityNode["entity"].as<Entity>());
    }

    // Recursively process children
    if (entityNode["children"] && entityNode["children"].IsSequence()) {
        for (const auto& child : entityNode["children"]) {
            collectEntities(child, allEntities);
        }
    }
}

editor::Command* editor::ProjectUtils::buildDuplicateTileCmd(Project* project, uint32_t sceneId, Entity entity, unsigned int tileIndex) {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) {
        return nullptr;
    }

    TilemapComponent* tilemap = sceneProject->scene->findComponent<TilemapComponent>(entity);
    if (!tilemap || tileIndex >= tilemap->numTiles) {
        return nullptr;
    }

    unsigned int newSlot = tilemap->numTiles;
    if (newSlot >= tilemap->tiles.size()) {
        return nullptr;
    }

    ComponentType cpType = ComponentType::TilemapComponent;
    MultiPropertyCmd* multiCmd = new MultiPropertyCmd();

    const TileData& src = tilemap->tiles[tileIndex];
    std::string dstPrefix = "tiles[" + std::to_string(newSlot) + "]";

    // Copy tile data with a small position offset
    multiCmd->addPropertyCmd<std::string>(project, sceneId, entity, cpType, dstPrefix + ".name", src.name);
    multiCmd->addPropertyCmd<int>(project, sceneId, entity, cpType, dstPrefix + ".rectId", src.rectId);
    multiCmd->addPropertyCmd<Vector2>(project, sceneId, entity, cpType, dstPrefix + ".position", src.position + Vector2(10.0f, 10.0f));
    multiCmd->addPropertyCmd<float>(project, sceneId, entity, cpType, dstPrefix + ".width", src.width);
    multiCmd->addPropertyCmd<float>(project, sceneId, entity, cpType, dstPrefix + ".height", src.height);

    multiCmd->addPropertyCmd<unsigned int>(project, sceneId, entity, cpType, "numTiles", (unsigned int)(newSlot + 1));

    multiCmd->setNoMerge();
    return multiCmd;
}

editor::Command* editor::ProjectUtils::buildDeleteTileCmd(Project* project, uint32_t sceneId, Entity entity, unsigned int tileIndex) {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) {
        return nullptr;
    }

    TilemapComponent* tilemap = sceneProject->scene->findComponent<TilemapComponent>(entity);
    if (!tilemap || tileIndex >= tilemap->numTiles) {
        return nullptr;
    }

    ComponentType cpType = ComponentType::TilemapComponent;
    MultiPropertyCmd* multiCmd = new MultiPropertyCmd();

    // Shift tiles down
    for (unsigned int j = tileIndex; j < tilemap->numTiles - 1; j++) {
        std::string dstPrefix = "tiles[" + std::to_string(j) + "]";
        multiCmd->addPropertyCmd<std::string>(project, sceneId, entity, cpType, dstPrefix + ".name", tilemap->tiles[j + 1].name);
        multiCmd->addPropertyCmd<int>(project, sceneId, entity, cpType, dstPrefix + ".rectId", tilemap->tiles[j + 1].rectId);
        multiCmd->addPropertyCmd<Vector2>(project, sceneId, entity, cpType, dstPrefix + ".position", tilemap->tiles[j + 1].position);
        multiCmd->addPropertyCmd<float>(project, sceneId, entity, cpType, dstPrefix + ".width", tilemap->tiles[j + 1].width);
        multiCmd->addPropertyCmd<float>(project, sceneId, entity, cpType, dstPrefix + ".height", tilemap->tiles[j + 1].height);
    }

    // Clear last slot
    unsigned int lastIdx = tilemap->numTiles - 1;
    std::string lastPrefix = "tiles[" + std::to_string(lastIdx) + "]";
    multiCmd->addPropertyCmd<std::string>(project, sceneId, entity, cpType, lastPrefix + ".name", std::string(""));
    multiCmd->addPropertyCmd<int>(project, sceneId, entity, cpType, lastPrefix + ".rectId", 0);
    multiCmd->addPropertyCmd<Vector2>(project, sceneId, entity, cpType, lastPrefix + ".position", Vector2(0, 0));
    multiCmd->addPropertyCmd<float>(project, sceneId, entity, cpType, lastPrefix + ".width", 0.0f);
    multiCmd->addPropertyCmd<float>(project, sceneId, entity, cpType, lastPrefix + ".height", 0.0f);

    // Decrement count
    multiCmd->addPropertyCmd<unsigned int>(project, sceneId, entity, cpType, "numTiles", (unsigned int)(tilemap->numTiles - 1));

    multiCmd->setNoMerge();
    return multiCmd;
}

editor::Command* editor::ProjectUtils::buildDeleteInstanceCmd(Project* project, uint32_t sceneId, Entity entity, unsigned int instanceIndex) {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) {
        return nullptr;
    }

    InstancedMeshComponent* instmesh = sceneProject->scene->findComponent<InstancedMeshComponent>(entity);
    if (!instmesh || instanceIndex >= instmesh->instances.size()) {
        return nullptr;
    }

    std::vector<InstanceData> newInstances = instmesh->instances;
    newInstances.erase(newInstances.begin() + instanceIndex);

    auto* cmd = new PropertyCmd<std::vector<InstanceData>>(project, sceneId, entity, ComponentType::InstancedMeshComponent, "instances", newInstances);
    cmd->setNoMerge();
    return cmd;
}

editor::Command* editor::ProjectUtils::buildDuplicateInstanceCmd(Project* project, uint32_t sceneId, Entity entity, unsigned int instanceIndex) {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) {
        return nullptr;
    }

    InstancedMeshComponent* instmesh = sceneProject->scene->findComponent<InstancedMeshComponent>(entity);
    if (!instmesh || instanceIndex >= instmesh->instances.size()) {
        return nullptr;
    }

    std::vector<InstanceData> newInstances = instmesh->instances;
    newInstances.push_back(instmesh->instances[instanceIndex]);

    auto* cmd = new PropertyCmd<std::vector<InstanceData>>(project, sceneId, entity, ComponentType::InstancedMeshComponent, "instances", newInstances);
    cmd->setNoMerge();
    return cmd;
}

void editor::ProjectUtils::removeDynamicInstmesh(Entity entity, const YAML::Node& savedComponents, EntityRegistry* registry) {
    if (!savedComponents || savedComponents.IsNull()) return;
    if (savedComponents[Catalog::getComponentName(ComponentType::InstancedMeshComponent, true)]) return;
    if (entity == NULL_ENTITY || !registry->isEntityCreated(entity)) return;
    if (registry->getSignature(entity).test(registry->getComponentId<InstancedMeshComponent>())) {
        registry->removeComponent<InstancedMeshComponent>(entity);
    }
}

namespace {

using EntityClassInfo = editor::ProjectUtils::EntityClassInfo;

// Base each class inherits, stated next to the component that selects it
EntityClassInfo meshClass(const char* name)   { return {name, true,  true }; }
EntityClassInfo objectClass(const char* name) { return {name, true,  false}; }
EntityClassInfo handleClass(const char* name) { return {name, false, false}; }

} // namespace

editor::ProjectUtils::EntityClassInfo editor::ProjectUtils::getEntityClassInfo(Scene* scene, Entity entity) {
    if (!scene || entity == NULL_ENTITY || !scene->isEntityCreated(entity)) return handleClass("EntityHandle");

    Signature signature = scene->getSignature(entity);

    if (signature.test(scene->getComponentId<ModelComponent>()))       return meshClass("Model");
    if (signature.test(scene->getComponentId<BoneComponent>()))        return objectClass("Bone");
    if (signature.test(scene->getComponentId<TilemapComponent>()))     return meshClass("Tilemap");
    if (signature.test(scene->getComponentId<TerrainComponent>()))     return meshClass("Terrain");
    if (signature.test(scene->getComponentId<SpriteComponent>()))      return meshClass("Sprite");
    if (signature.test(scene->getComponentId<PointsComponent>()))      return objectClass("Points");
    if (signature.test(scene->getComponentId<LineRendererComponent>()))       return objectClass("LineRenderer");
    if (signature.test(scene->getComponentId<PolygonComponent>()))     return objectClass("Polygon");
    if (signature.test(scene->getComponentId<MeshPolygonComponent>())) return meshClass("MeshPolygon");
    if (signature.test(scene->getComponentId<MirrorComponent>()))      return meshClass("Mirror");
    if (signature.test(scene->getComponentId<MeshComponent>()))        return meshClass("Mesh");
    if (signature.test(scene->getComponentId<SkyboxComponent>()))         return handleClass("Skybox");
    if (signature.test(scene->getComponentId<FogComponent>()))         return handleClass("Fog");
    if (signature.test(scene->getComponentId<AudioSourceComponent>()))       return handleClass("AudioSource");
    if (signature.test(scene->getComponentId<ButtonComponent>()))      return objectClass("Button");
    if (signature.test(scene->getComponentId<ScrollbarComponent>()))   return objectClass("Scrollbar");
    if (signature.test(scene->getComponentId<ProgressbarComponent>())) return objectClass("Progressbar");
    if (signature.test(scene->getComponentId<InputFieldComponent>()))    return objectClass("InputField");
    if (signature.test(scene->getComponentId<TextComponent>()))        return objectClass("Text");
    if (signature.test(scene->getComponentId<PanelComponent>()))       return objectClass("Panel");
    if (signature.test(scene->getComponentId<ImageComponent>()))       return objectClass("Image");
    if (signature.test(scene->getComponentId<LayoutGroupComponent>())) return objectClass("LayoutGroup");
    if (signature.test(scene->getComponentId<UIComponent>()))          return objectClass("RectTransform");
    if (signature.test(scene->getComponentId<Light2DComponent>()))     return objectClass("Light2D");
    if (signature.test(scene->getComponentId<ShadowCaster2DComponent>()))  return objectClass("ShadowCaster2D");
    if (signature.test(scene->getComponentId<LightComponent>()))       return objectClass("Light");
    if (signature.test(scene->getComponentId<CameraComponent>()))      return objectClass("Camera");
    if (signature.test(scene->getComponentId<ReflectionProbeComponent>())) return objectClass("ReflectionProbe");
    if (signature.test(scene->getComponentId<Rigidbody2DComponent>()))      return handleClass("Rigidbody2D");
    if (signature.test(scene->getComponentId<RigidbodyComponent>()))      return handleClass("Rigidbody");
    if (signature.test(scene->getComponentId<Joint2DComponent>()))     return handleClass("Joint2D");
    if (signature.test(scene->getComponentId<Joint3DComponent>()))     return handleClass("Joint3D");
    if (signature.test(scene->getComponentId<AlphaTweenComponent>())) return handleClass("AlphaTween");
    if (signature.test(scene->getComponentId<ColorTweenComponent>())) return handleClass("ColorTween");
    if (signature.test(scene->getComponentId<PositionTweenComponent>())) return handleClass("PositionTween");
    if (signature.test(scene->getComponentId<RotationTweenComponent>())) return handleClass("RotationTween");
    if (signature.test(scene->getComponentId<ScaleTweenComponent>())) return handleClass("ScaleTween");
    if (signature.test(scene->getComponentId<TweenComponent>())) return handleClass("Tween");
    if (signature.test(scene->getComponentId<AnimationComponent>()))   return handleClass("Animation");
    if (signature.test(scene->getComponentId<SpriteAnimationComponent>())) return handleClass("SpriteAnimation");
    if (signature.test(scene->getComponentId<MorphTracksComponent>())) return handleClass("MorphTracks");
    if (signature.test(scene->getComponentId<RotateTracksComponent>())) return handleClass("RotateTracks");
    if (signature.test(scene->getComponentId<ScaleTracksComponent>())) return handleClass("ScaleTracks");
    if (signature.test(scene->getComponentId<TranslateTracksComponent>())) return handleClass("TranslateTracks");
    if (signature.test(scene->getComponentId<ParticleSystemComponent>()))   return handleClass("ParticleSystem");
    if (signature.test(scene->getComponentId<ActionComponent>()))      return handleClass("Action");
    if (signature.test(scene->getComponentId<Transform>()))            return objectClass("GameObject");

    // generic engine type for entities with no recognized component: valid as a
    // C++ pointer type (jasperengine::EntityHandle*) and known to the Lua dispatcher
    return handleClass("EntityHandle");
}

std::string editor::ProjectUtils::getEntityTypeName(Scene* scene, Entity entity) {
    return getEntityClassInfo(scene, entity).name;
}
