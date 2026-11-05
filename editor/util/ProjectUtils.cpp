// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ProjectUtils.h"
#include "component/AlphaTweenComponent.h"
#include "component/ColorTweenComponent.h"
#include "component/KeyframeTracksComponent.h"
#include "component/LatticeComponent.h"
#include "component/LatticeTracksComponent.h"
#include "component/Light2DComponent.h"
#include "component/MirrorComponent.h"
#include "component/MorphTracksComponent.h"
#include "component/PositionTweenComponent.h"
#include "component/RagdollComponent.h"
#include "component/ReflectionProbeComponent.h"
#include "component/RotateTracksComponent.h"
#include "component/RotationTweenComponent.h"
#include "component/ScaleTracksComponent.h"
#include "component/ScaleTweenComponent.h"
#include "component/SpriteAnimationComponent.h"
#include "component/TranslateTracksComponent.h"
#include "component/TweenComponent.h"
#include "component/ScriptComponent.h"
#include "component/TrailRendererComponent.h"
#include "component/OnScreenControlComponents.h"
#include "reflect/Reflect.h"
#include "io/AtomicFile.h"

#include "Out.h"
#include "util/FileUtils.h"
#include "stb_image_write.h"
#include "Catalog.h"
#include "Stream.h"
#include "shader/ShaderVariantCompiler.h"
#include "util/Util.h"
#include "texture/TextureData.h"
#include "util/ProjectChecks.h"
#include "script/LuaBinding.h"
#include "System.h"
#include "pool/TextureDataPool.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "component/ActionComponent.h"
#include "component/AnimationComponent.h"
#include "component/BoneComponent.h"
#include "component/CapsuleShadowCasterComponent.h"
#include "component/ButtonComponent.h"
#include "component/UIDocumentComponent.h"
#include "component/FluidSimulationComponent.h"
#include "component/VideoPlayerComponent.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "component/MeshComponent.h"
#include "component/SkyboxComponent.h"
#include "component/FogComponent.h"
#include "component/LocalVolumetricFogComponent.h"
#include "component/ForceFieldComponent.h"
#include "component/VolumetricCloudsComponent.h"
#include "component/LightComponent.h"
#include "component/CameraComponent.h"
#include "component/SpriteComponent.h"
#include "component/PointsComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/LineRendererComponent.h"
#include "component/TerrainComponent.h"
#include "component/AudioSourceComponent.h"
#include "audio/AudioMixerDocument.h"
#include "component/LayoutGroupComponent.h"
#include "component/CanvasGroupComponent.h"
#include "component/UIComponent.h"
#include "component/TextComponent.h"
#include "component/InputFieldComponent.h"
#include "stb_image_write.h"
#include "component/ImageComponent.h"
#include "component/PanelComponent.h"
#include "component/ScrollbarComponent.h"
#include "component/ProgressbarComponent.h"
#include "component/PolygonComponent.h"
#include "component/MeshPolygonComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "subsystem/SplineSystem.h"
#include "subsystem/UISystem.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/ParticleSubsystem.h"
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

// An RGBA square written to a project file through the atomic writer (docs/data-safety.md)
static bool writePngFile(const fs::path& file, int size, const std::vector<unsigned char>& pixels) {
    std::string png;
    auto append = [](void* context, void* data, int count) {
        static_cast<std::string*>(context)->append(static_cast<const char*>(data), static_cast<size_t>(count));
    };
    if (!stbi_write_png_to_func(append, &png, size, size, 4, pixels.data(), size * 4)) return false;
    return editor::FileUtils::writeFile(file, png) != editor::FileUtils::WriteResult::Failed;
}

void editor::ProjectUtils::setDefaultSkyTexture(Texture& outTexture) {
    static const char* const id = "editor:resources:default_sky";
    // Decoded once per process, and the pixels stay reachable from here: the texture pool keeps
    // copies of the faces that do not own them (a face copy carries its owner flag, so an owning
    // one would be freed by every temporary), and an entry the pool drops (clearUnusedPools on a
    // project close) must not take the only pointer to 6 MB with it, as it did before (a
    // LeakSanitizer report for every editor test process that decoded a scene with the default sky)
    static const std::array<TextureData, 6>* const faces = []() {
        auto* decoded = new std::array<TextureData, 6>();
        // the order setCubeDatas takes: front, back, left, right, top, bottom
        decoded->at(0).loadTextureFromMemory(Daylight_Box_Front_png, Daylight_Box_Front_png_len);
        decoded->at(1).loadTextureFromMemory(Daylight_Box_Back_png, Daylight_Box_Back_png_len);
        decoded->at(2).loadTextureFromMemory(Daylight_Box_Left_png, Daylight_Box_Left_png_len);
        decoded->at(3).loadTextureFromMemory(Daylight_Box_Right_png, Daylight_Box_Right_png_len);
        decoded->at(4).loadTextureFromMemory(Daylight_Box_Top_png, Daylight_Box_Top_png_len);
        decoded->at(5).loadTextureFromMemory(Daylight_Box_Bottom_png, Daylight_Box_Bottom_png_len);
        return decoded;
    }();

    outTexture.setId(id);
    outTexture.setCubeDatas(id, faces->at(0), faces->at(1), faces->at(2), faces->at(3), faces->at(4), faces->at(5));
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
        editor::FileUtils::removePath(*it, ec);   // empty folders only (in a write scope: beneath its root)
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
        if (std::filesystem::exists(std::filesystem::symlink_status(path, ec)))
            return false;
        // in a write scope (fork_shader): beneath its root, no link on the way followed
        if (const FileUtils::ScopedOps* ops = FileUtils::scopedOps()) {
            const int r = ops->write(path, content, ec);
            if (r >= 0) {
                if (r == 1) written.push_back(path);
                return r == 1;
            }
        }
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec)
            return false;
        if (std::filesystem::exists(path, ec) || ec)
            return false;

        if (!io::writeFileAtomic(path, content).ok())
            return false;
        written.push_back(path);
        return true;
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
            FileUtils::removePath(path, ec);
        pruneForkDirectories(plan.forkDirPath);
        Out::error("Could not write forked shader files");
    }
    return success;
}

void editor::ProjectUtils::removeShaderFork(const ShaderForkPlan& plan) {
    std::error_code ec;
    // (in a write scope: beneath its root, the entries themselves)
    FileUtils::removePath(plan.vertPath, ec);
    FileUtils::removePath(plan.fragPath, ec);
    for (const auto& include : plan.includeFiles)
        FileUtils::removePath(include.path, ec);
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
    // so are a spline's generated chunks and pieces: clicking the road selects the Road
    if (std::shared_ptr<SplineSystem> splines = scene->findSystem<SplineSystem>()) {
        const Entity splineOwner = splines->getGeneratedOwner(entity);
        if (splineOwner != NULL_ENTITY) {
            return splineOwner;
        }
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

    bool checkPanels = false;
    const Entity uiOwner = lockedByParentUI(scene, entity, signature, checkPanels);
    if (uiOwner != NULL_ENTITY || !checkPanels){
        return uiOwner;
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

Entity editor::ProjectUtils::lockedByParentUI(Scene* scene, Entity entity, Signature signature, bool& checkPanels){
    checkPanels = false;
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

    checkPanels = true;
    return NULL_ENTITY;
}

editor::LockedParentIndex::LockedParentIndex(Scene* scene): scene(scene){
    // in the order getLockedEntityParent looks: the first model (and in it the first kind of
    // reference) that names an entity answers for it
    auto models = scene->getComponentArray<ModelComponent>();
    for (size_t i = 0; i < models->size(); ++i) {
        const Entity modelEntity = models->getEntity(i);
        const ModelComponent& model = models->getComponentFromIndex(i);
        auto add = [this](Entity entity, Entity owner, bool byTransformParent){
            modelReferences.emplace(entity, ModelReference{owner, byTransformParent});
        };
        add(model.skeleton, modelEntity, false);
        for (Entity anim : model.animations){
            add(anim, modelEntity, false);
            if (const AnimationComponent* animComp = scene->findComponent<AnimationComponent>(anim)){
                for (const auto& frame : animComp->actions){
                    add(frame.action, anim, false);
                }
            }
        }
        for (const auto& bone : model.bonesIdMapping) add(bone.second, modelEntity, true);
        for (const auto& node : model.meshNodesMapping) add(node.second, modelEntity, true);
        for (const auto& node : model.nodesIdMapping) add(node.second, modelEntity, true);
    }
    auto panels = scene->getComponentArray<PanelComponent>();
    for (size_t i = 0; i < panels->size(); ++i) {
        const Entity panelEntity = panels->getEntity(i);
        const PanelComponent& panel = panels->getComponentFromIndex(i);
        for (Entity part : {panel.headerimage, panel.headercontainer, panel.headertext}) {
            panelParts.emplace(part, panelEntity);
        }
    }
}

Entity editor::LockedParentIndex::get(Entity entity) const{
    if (entity == NULL_ENTITY)
        return NULL_ENTITY;

    Entity foliageOwner = scene->getSystem<MeshSystem>()->getFoliageOwner(entity);
    if (foliageOwner != NULL_ENTITY) {
        return foliageOwner;
    }

    auto reference = modelReferences.find(entity);
    if (reference != modelReferences.end()) {
        if (reference->second.byTransformParent) {
            Transform* transform = scene->findComponent<Transform>(entity);
            if (transform && transform->parent != NULL_ENTITY) {
                return transform->parent;
            }
        }
        return reference->second.owner;
    }

    bool checkPanels = false;
    const Entity uiOwner = ProjectUtils::lockedByParentUI(scene, entity, scene->getSignature(entity), checkPanels);
    if (uiOwner != NULL_ENTITY || !checkPanels){
        return uiOwner;
    }
    auto part = panelParts.find(entity);
    return part != panelParts.end() ? part->second : NULL_ENTITY;
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
        auto transforms = registry->getComponentArray<Transform>();
        return transforms->getIndex(entity);
    }

    return 0;
}

bool editor::ProjectUtils::keepInParentBranch(EntityRegistry* registry, Entity entity) {
    const ComponentId transformId = registry->getComponentId<Transform>();
    const Transform* transform = registry->findComponent<Transform>(entity);
    if (!transform) {
        return false;
    }
    // read before the index queries, which settle the array (and can move its components)
    const Entity parent = transform->parent;
    if (parent == NULL_ENTITY || !registry->isEntityCreated(parent) || !registry->getSignature(parent).test(transformId)) {
        return false;
    }
    auto transforms = registry->getComponentArray<Transform>();
    const size_t index = transforms->getIndex(entity);
    if (index > transforms->getIndex(parent)) {
        // The array before it is in order, so it is in its parent's branch when the GameObject
        // just before it is the parent or below it (a walk up that GameObject's parents, not
        // along the branch: a decode places each of its children so)
        Entity before = transforms->getEntity(index - 1);
        for (size_t depth = 0; before != NULL_ENTITY && depth <= transforms->size(); depth++) {
            if (before == parent) {
                return false;
            }
            const Transform* beforeTransform = registry->findComponent<Transform>(before);
            before = beforeTransform ? beforeTransform->parent : NULL_ENTITY;
        }
    }

    // the whole branch moves, and the arrays kept in Transform order follow it
    registry->moveChildToIndex(entity, registry->findBranchLastIndex(parent) + 1);
    return true;
}

Entity editor::ProjectUtils::findSplitBranch(EntityRegistry* registry) {
    const ComponentId transformId = registry->getComponentId<Transform>();
    auto transforms = registry->getComponentArray<Transform>();
    // The GameObject just read and its parents, outermost first: in order, the next one's parent
    // is among them, or it has none
    std::vector<Entity> path;
    const size_t count = transforms->size();
    for (size_t i = 0; i < count; i++) {
        const Entity parent = transforms->getComponentFromIndex(i).parent;
        if (parent == NULL_ENTITY || !registry->isEntityCreated(parent) || !registry->getSignature(parent).test(transformId)) {
            path.clear();
        } else {
            while (!path.empty() && path.back() != parent) {
                path.pop_back();
            }
            if (path.empty()) {
                return parent;
            }
        }
        path.push_back(transforms->getEntity(i));
    }
    return NULL_ENTITY;
}

bool editor::ProjectUtils::isBranchPlace(EntityRegistry* registry, Entity entity) {
    const ComponentId transformId = registry->getComponentId<Transform>();
    auto parentOf = [&](Entity of) {
        const Transform* transform = registry->findComponent<Transform>(of);
        return transform ? transform->parent : NULL_ENTITY;
    };
    const Entity parent = parentOf(entity);
    auto transforms = registry->getComponentArray<Transform>();
    const size_t first = transforms->getIndex(entity);
    const size_t last = registry->findBranchLastIndex(entity);
    // in its parent's branch: the GameObject before it is the parent or below it
    if (parent != NULL_ENTITY && registry->isEntityCreated(parent) && registry->getSignature(parent).test(transformId)) {
        bool inBranch = false;
        Entity before = first > 0 ? transforms->getEntity(first - 1) : NULL_ENTITY;
        for (size_t depth = 0; before != NULL_ENTITY && depth <= transforms->size(); depth++) {
            if (before == parent) {
                inBranch = true;
                break;
            }
            before = parentOf(before);
        }
        if (!inBranch) {
            return false;
        }
    }
    // and not between a GameObject and its children: the next one's parent is none, the parent
    // or one of the parent's parents
    if (last + 1 < transforms->size()) {
        const Entity nextParent = transforms->getComponentFromIndex(last + 1).parent;
        Entity up = parent;
        for (size_t depth = 0; nextParent != NULL_ENTITY && depth <= transforms->size(); depth++) {
            if (up == nextParent) {
                return true;
            }
            if (up == NULL_ENTITY) {
                return false;
            }
            up = parentOf(up);
        }
    }
    return true;
}

void editor::ProjectUtils::sortEntitiesByTransformOrder(EntityRegistry* registry, std::vector<Entity>& entities, Entity moved) {
    // The GameObjects with a Transform go in Transform order. One without (a Skybox) has no
    // place there: it stays in its gap among the top-level GameObjects, after the branch of
    // the top-level one it followed (the front when none), which is where the scene and
    // prefab files write it. They keep their order among themselves (the Hierarchy lists
    // them in it, and the first Skybox is the sky). A GameObject just moved is not one they
    // follow: it is listed where it was, and they stay with the others.
    auto transforms = registry->getComponentArray<Transform>();
    const ComponentId transformId = registry->getComponentId<Transform>();
    std::vector<std::pair<size_t, Entity>> keyed;  // (Transform index, GameObject), listed order
    keyed.reserve(entities.size());
    std::vector<std::pair<size_t, Entity>> loose;  // (how many with a Transform before it, GameObject)
    bool sorted = true;
    for (Entity entity : entities) {
        if (!registry->isEntityCreated(entity) || !registry->getSignature(entity).test(transformId)) {
            loose.emplace_back(keyed.size(), entity);
            continue;
        }
        const size_t key = transforms->getIndex(entity);
        if (!keyed.empty() && key < keyed.back().first) {
            sorted = false;
        }
        keyed.emplace_back(key, entity);
    }
    if (sorted) {
        return;
    }

    // Top level as the files write it: no listed parent
    const std::unordered_set<Entity> listed(entities.begin(), entities.end());
    std::unordered_map<Entity, bool> topLevelOf;
    topLevelOf.reserve(keyed.size());
    for (const auto& item : keyed) {
        topLevelOf[item.second] = listed.count(registry->getComponent<Transform>(item.second).parent) == 0;
    }
    std::vector<Entity> anchors;  // per loose GameObject: the top-level one listed before it
    anchors.reserve(loose.size());
    Entity top = NULL_ENTITY;
    size_t scanned = 0;
    for (const auto& item : loose) {
        for (; scanned < item.first; scanned++) {
            if (keyed[scanned].second != moved && topLevelOf[keyed[scanned].second]) {
                top = keyed[scanned].second;
            }
        }
        anchors.push_back(top);
    }

    std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });

    // Where each top-level GameObject's branch ends in Transform order
    std::unordered_map<Entity, size_t> branchEnd;
    top = NULL_ENTITY;
    for (size_t i = 0; i < keyed.size(); i++) {
        if (topLevelOf[keyed[i].second]) {
            if (top != NULL_ENTITY) {
                branchEnd[top] = i;
            }
            top = keyed[i].second;
        }
    }
    if (top != NULL_ENTITY) {
        branchEnd[top] = keyed.size();
    }

    std::vector<Entity> result;
    result.reserve(entities.size());
    size_t placed = 0;
    for (size_t i = 0; i < loose.size(); i++) {
        // never ahead of one that was listed before it
        if (anchors[i] != NULL_ENTITY) {
            const size_t at = branchEnd[anchors[i]];
            for (; placed < at; placed++) {
                result.push_back(keyed[placed].second);
            }
        }
        result.push_back(loose[i].second);
    }
    for (; placed < keyed.size(); placed++) {
        result.push_back(keyed[placed].second);
    }
    entities = std::move(result);
}

void editor::ProjectUtils::keepUntransformedPlaces(EntityRegistry* registry, const std::vector<Entity>& before, std::vector<Entity>& entities) {
    const ComponentId transformId = registry->getComponentId<Transform>();
    auto untransformed = [&](Entity entity) {
        return !registry->isEntityCreated(entity) || !registry->getSignature(entity).test(transformId);
    };
    const std::unordered_set<Entity> listed(entities.begin(), entities.end());
    std::unordered_set<Entity> loose;
    for (Entity entity : before) {
        if (listed.count(entity) && untransformed(entity)) {
            loose.insert(entity);
        }
    }
    if (loose.empty()) {
        return;
    }
    std::vector<Entity> result;
    result.reserve(entities.size());
    for (Entity entity : entities) {
        if (!loose.count(entity)) {
            result.push_back(entity);
        }
    }
    // In the old order, so each one's predecessor is already back (a run of them keeps its order)
    Entity previous = NULL_ENTITY;
    for (Entity entity : before) {
        if (!listed.count(entity)) {
            continue;
        }
        if (loose.count(entity)) {
            auto at = result.begin();
            if (previous != NULL_ENTITY) {
                at = std::find(result.begin(), result.end(), previous);
                at = at == result.end() ? result.end() : at + 1;
            }
            result.insert(at, entity);
        }
        previous = entity;
    }
    entities = std::move(result);
}

bool editor::ProjectUtils::moveEntityOrderByTarget(EntityRegistry* registry, std::vector<Entity>& entities, Entity source, Entity target, InsertionType type, Entity& oldParent, size_t& oldIndex, bool& hasTransform) {
    Transform* transformSource = registry->findComponent<Transform>(source);
    Transform* transformTarget = registry->findComponent<Transform>(target);

    if (transformSource && transformTarget){
        hasTransform = true;
        // the parents read by entity first, before the hierarchy and index queries
        const Entity newParent = (type == InsertionType::INTO) ? target : transformTarget->parent;
        const Entity sourceParent = transformSource->parent;

        if (registry->isParentOf(source, target)){
            Out::error("Cannot move an entity to its own child");
            return false;
        }
        oldParent = sourceParent;

        auto transforms = registry->getComponentArray<Transform>();

        oldIndex = transforms->getIndex(source);

        // Below its new parent first (that can move its branch, and so the target's place), then
        // the place read from the target as it is now: after it means after its whole branch, as
        // a sibling of it. Right after the target itself went between a GameObject with children
        // and them, which split its branch and lost them from the saved file
        registry->addEntityChild(newParent, source, true);
        size_t targetTransformIndex;
        if (type == InsertionType::AFTER){
            targetTransformIndex = registry->findBranchLastIndex(target) + 1;
        }else if (type == InsertionType::INTO){
            targetTransformIndex = transforms->getIndex(target) + 1;
        }else{
            targetTransformIndex = transforms->getIndex(target);
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

    // the scene's sky is the first skybox in this order, live as after a reload
    registry->sortComponentsByEntityOrder<SkyboxComponent>(entities);

    return true;
}

void editor::ProjectUtils::moveEntityOrderByIndex(EntityRegistry* registry, std::vector<Entity>& entities, Entity source, Entity parent, size_t index, bool hasTransform, bool sortEntities){
    if (hasTransform){

        // Below its parent first, then its place read where that left it: the reparent moves the
        // branch to the end of the parent's, so a place read before it (where the move had put it)
        // was one branch length off when the move had taken it to an earlier index, out of a
        // parent it was not the last child of. It came back past its next sibling, or between
        // that sibling and its children, which split the sibling's branch
        registry->addEntityChild(parent, source, true);

        auto transforms = registry->getComponentArray<Transform>();
        size_t sourceTransformIndex = transforms->getIndex(source);

        size_t sizeOfSourceBranch = registry->findBranchLastIndex(source) - sourceTransformIndex + 1;
        if (sourceTransformIndex < index){
            index += sizeOfSourceBranch;
        }
        index = std::min(index, transforms->size());

        moveEntityOrderByTransform(registry, entities, source, parent, index, true, false);
        // An index the array no longer fits (something outside the history changed it since)
        // never splits a branch: the GameObject goes to the end of its parent's instead
        if (!isBranchPlace(registry, source)){
            registry->moveChildToIndex(source, parent != NULL_ENTITY && registry->findComponent<Transform>(parent) ?
                registry->findBranchLastIndex(parent) + 1 : transforms->size());
        }
        if (sortEntities){
            ProjectUtils::sortEntitiesByTransformOrder(registry, entities, source);
        }

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

    registry->sortComponentsByEntityOrder<SkyboxComponent>(entities);
}

void editor::ProjectUtils::moveEntityOrderByTransform(EntityRegistry* registry, std::vector<Entity>& entities, Entity source, Entity parent, size_t transformIndex, bool enableMove, bool sortEntities){
    registry->addEntityChild(parent, source, true);

    if (enableMove){
        registry->moveChildToIndex(source, transformIndex);
    }

    if (sortEntities){
        ProjectUtils::sortEntitiesByTransformOrder(registry, entities, source);
    }
}

void editor::ProjectUtils::addEntityComponent(EntityRegistry* registry, Entity entity, ComponentType componentType, std::vector<Entity>& entities, YAML::Node componentNode){
    const reflect::ComponentDesc* desc = reflect::find(componentType);
    if (!desc) {
        return;
    }
    // a default component, or the one the node describes (decoded as a scene loads it: a body's
    // colliders by its GameObject's scale)
    reflect::Value value(desc->body.ops);
    if (componentNode.IsDefined() && !componentNode.IsNull()) {
        reflect::DecodeContext ctx;
        ctx.registry = registry;
        ctx.entity = entity;
        ctx.component = desc->key;
        reflect::DecodeReport report;
        reflect::decode(*desc, componentNode, value.get(), ctx, report);
    }
    desc->add(registry, entity, value.get());
    if (componentType == ComponentType::Transform) {
        // last in Transform order now, listed where it was
        ProjectUtils::sortEntitiesByTransformOrder(registry, entities, entity);
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
    const reflect::ComponentDesc* desc = reflect::find(componentType);
    void* comp = desc ? desc->find(registry, entity) : nullptr;
    if (!comp) {
        return oldComponent;
    }
    if (encodeComponent){
        oldComponent = reflect::encode(*desc, comp, reflect::EncodeContext{});
    }

    // Editor commands own the undo policy for owned targets/actions. Suppress those two
    // callbacks here so component removal never destroys entities outside its recovery data.
    if (componentType == ComponentType::ActionComponent) {
        static_cast<ActionComponent*>(comp)->ownedTarget = false;
    } else if (componentType == ComponentType::AnimationComponent) {
        static_cast<AnimationComponent*>(comp)->ownedActions = false;
    }
    desc->remove(registry, entity);
    if (componentType == ComponentType::Transform) {
        ProjectUtils::sortEntitiesByTransformOrder(registry, entities);
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

    case ScriptPropertyType::Asset: {
        // a default path ("textures/hero.png"), else None
        return ScriptPropertyValue(AssetRef(lua_isstring(L, idx) ? std::string(lua_tostring(L, idx)) : std::string()));
    }
    }

    return ScriptPropertyValue{};
}

void editor::ProjectUtils::loadLuaScriptProperties(ScriptEntry& entry, const std::string& luaPath) {
    std::string code;
    if (!ProjectChecks::readFile(luaPath, code)) {
        Out::error("Failed to load Lua script \"%s\": cannot open %s", luaPath.c_str(), luaPath.c_str());
        return;
    }

    const LuaPropertiesResult result = readLuaScriptProperties(entry, code, luaPath);
    if (result.status == LuaPropertiesResult::LoadError || result.status == LuaPropertiesResult::RunError) {
        Out::error("Failed to load Lua script \"%s\": %s", luaPath.c_str(), result.message.c_str());
    } else if (result.status == LuaPropertiesResult::PropertyError) {
        Out::error("Failed to read the properties of Lua script \"%s\": %s", luaPath.c_str(), result.message.c_str());
    }
}

void editor::ProjectUtils::loadLuaScriptPropertiesFromString(ScriptEntry& entry, const std::string& scriptContent, const std::string& chunkName) {
    // errors are ignored: the code may be mid-edit. A table without `properties` keeps the
    // fields until the file is saved: this refresh has no undo, and the table may be mid-cut
    const std::vector<ScriptProperty> previous = entry.properties;
    const LuaPropertiesResult result = readLuaScriptProperties(entry, scriptContent, chunkName);
    if (result.noPropertiesField) {
        entry.properties = previous;
    }
}

// Never the engine's state (LuaBinding::getLuaState): Lua states are not thread-safe, and Play
// reads the properties on its startup thread while the main thread runs Lua; nor should a
// script's top level run in the game's state each time the editor looks at it.
editor::ProjectUtils::LuaPropertiesResult editor::ProjectUtils::readLuaScriptProperties(ScriptEntry& entry, const std::string& code, const std::string& chunkName) {
    lua_State* L = ProjectChecks::newIsolatedLuaState(std::filesystem::path(System::instance().getLuaPath()), LuaBinding::getEngineGlobalNames());
    if (!L) {
        LuaPropertiesResult result;
        result.status = LuaPropertiesResult::LoadError;
        result.message = "no Lua state to read the properties in (out of memory?)";
        return result;
    }
    const LuaPropertiesResult result = runLuaScriptProperties(L, entry, code, chunkName);
    ProjectChecks::closeIsolatedLuaState(L);
    return result;
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
    if (lua_istable(L, -1)) {
        result.returnedTable = true;
        lua_getfield(L, -1, "properties");
        result.noPropertiesField = lua_isnil(L, -1);
        lua_pop(L, 1);
        if (result.noPropertiesField) {
            // the script declares no properties (any it had are gone)
            entry.properties.clear();
        } else if (!parseLuaPropertiesTableProtected(L, entry, &result.message)) {
            result.status = LuaPropertiesResult::PropertyError;
        }
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
                    }else if (typeLower == "asset" || typeLower == "assetref"){
                        prop.type = ScriptPropertyType::Asset;
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

                // Asset properties: the asset type they take (assetType = "texture", ...)
                lua_getfield(L, -1, "assetType");
                if (prop.type == ScriptPropertyType::Asset && lua_isstring(L, -1)) {
                    prop.assetType = lua_tostring(L, -1);
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

namespace {

using EntityClassInfo = editor::ProjectUtils::EntityClassInfo;

// Base each class inherits, stated next to the component that selects it
EntityClassInfo meshClass(const char* name)   { return {name, true,  true }; }
EntityClassInfo objectClass(const char* name) { return {name, true,  false}; }
EntityClassInfo handleClass(const char* name) { return {name, false, false}; }

} // namespace

std::string editor::ProjectUtils::ensureOnScreenControlTexture(const std::filesystem::path& assetsPath, bool knob) {
    const std::string stored = knob ? "ui/onscreen/stick_knob.png" : "ui/onscreen/stick_base.png";
    const std::filesystem::path file = assetsPath / "ui" / "onscreen" / (knob ? "stick_knob.png" : "stick_base.png");
    std::error_code ec;
    if (std::filesystem::exists(file, ec))
        return stored;
    std::filesystem::create_directories(file.parent_path(), ec);

    // white, so the Image color tints it: a translucent disc with a brighter rim (the base) or
    // a nearly opaque disc (the knob), antialiased over one pixel
    const int size = knob ? 128 : 256;
    std::vector<unsigned char> pixels(static_cast<size_t>(size) * size * 4, 0);
    const float radius = size / 2.0f - 1.0f;
    const float rim = knob ? 0.0f : size * 0.03f;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            const float dx = x + 0.5f - size / 2.0f;
            const float dy = y + 0.5f - size / 2.0f;
            const float d = std::sqrt(dx * dx + dy * dy);
            const float coverage = std::clamp(radius - d + 0.5f, 0.0f, 1.0f);
            float alpha = knob ? 0.85f : 0.35f;
            if (!knob && d > radius - rim)
                alpha = 0.7f;
            unsigned char* p = &pixels[(static_cast<size_t>(y) * size + x) * 4];
            p[0] = p[1] = p[2] = 255;
            p[3] = static_cast<unsigned char>(std::lround(255.0f * alpha * coverage));
        }
    }
    if (!writePngFile(file, size, pixels)) {
        Out::error("Could not write %s", file.string().c_str());
        return "";
    }
    return stored;
}

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
    if (signature.test(scene->getComponentId<VolumetricCloudsComponent>())) return handleClass("VolumetricClouds");
    if (signature.test(scene->getComponentId<LocalVolumetricFogComponent>())) return objectClass("LocalVolumetricFog");
    if (signature.test(scene->getComponentId<AudioSourceComponent>()))       return handleClass("AudioSource");
    if (signature.test(scene->getComponentId<OnScreenStickComponent>()))  return objectClass("OnScreenStick");
    if (signature.test(scene->getComponentId<OnScreenButtonComponent>())) return objectClass("OnScreenButton");
    if (signature.test(scene->getComponentId<TouchAreaComponent>()))      return objectClass("TouchArea");
    if (signature.test(scene->getComponentId<ButtonComponent>()))      return objectClass("Button");
    if (signature.test(scene->getComponentId<ScrollbarComponent>()))   return objectClass("Scrollbar");
    if (signature.test(scene->getComponentId<ProgressbarComponent>())) return objectClass("Progressbar");
    if (signature.test(scene->getComponentId<InputFieldComponent>()))    return objectClass("InputField");
    if (signature.test(scene->getComponentId<TextComponent>()))        return objectClass("Text");
    if (signature.test(scene->getComponentId<PanelComponent>()))       return objectClass("Panel");
    if (signature.test(scene->getComponentId<ImageComponent>()))       return objectClass("Image");
    if (signature.test(scene->getComponentId<LayoutGroupComponent>())) return objectClass("LayoutGroup");
    if (signature.test(scene->getComponentId<UIDocumentComponent>()))  return objectClass("UIDocument");
    if (signature.test(scene->getComponentId<UIComponent>()))          return objectClass("RectTransform");
    // a container whose own component is its Canvas Group (CanvasGroup(scene, entity) there)
    if (signature.test(scene->getComponentId<CanvasGroupComponent>())) return handleClass("CanvasGroup");
    // after the renderers: a fluid shown on a Sprite or a Mesh is that object first (its
    // FluidSimulation is FluidSimulation(scene, entity) there)
    if (signature.test(scene->getComponentId<FluidSimulationComponent>())) return objectClass("FluidSimulation");
    // after the renderers too: a trail behind a Sprite or a Mesh is that object first
    if (signature.test(scene->getComponentId<TrailRendererComponent>())) return objectClass("TrailRenderer");
    if (signature.test(scene->getComponentId<Light2DComponent>()))     return objectClass("Light2D");
    if (signature.test(scene->getComponentId<ShadowCaster2DComponent>()))  return objectClass("ShadowCaster2D");
    if (signature.test(scene->getComponentId<CapsuleShadowCasterComponent>())) return objectClass("CapsuleShadowCaster");
    if (signature.test(scene->getComponentId<LightComponent>()))       return objectClass("Light");
    if (signature.test(scene->getComponentId<CameraComponent>()))      return objectClass("Camera");
    if (signature.test(scene->getComponentId<ReflectionProbeComponent>())) return objectClass("ReflectionProbe");
    if (signature.test(scene->getComponentId<LatticeComponent>()))     return objectClass("Lattice");
    if (signature.test(scene->getComponentId<Rigidbody2DComponent>()))      return handleClass("Rigidbody2D");
    if (signature.test(scene->getComponentId<RigidbodyComponent>()))      return handleClass("Rigidbody");
    if (signature.test(scene->getComponentId<Joint2DComponent>()))     return handleClass("Joint2D");
    if (signature.test(scene->getComponentId<Joint3DComponent>()))     return handleClass("Joint3D");
    if (signature.test(scene->getComponentId<RagdollComponent>()))     return handleClass("Ragdoll");
    if (signature.test(scene->getComponentId<AlphaTweenComponent>())) return handleClass("AlphaTween");
    if (signature.test(scene->getComponentId<ColorTweenComponent>())) return handleClass("ColorTween");
    if (signature.test(scene->getComponentId<PositionTweenComponent>())) return handleClass("PositionTween");
    if (signature.test(scene->getComponentId<RotationTweenComponent>())) return handleClass("RotationTween");
    if (signature.test(scene->getComponentId<ScaleTweenComponent>())) return handleClass("ScaleTween");
    if (signature.test(scene->getComponentId<TweenComponent>())) return handleClass("Tween");
    if (signature.test(scene->getComponentId<AnimationComponent>()))   return handleClass("Animation");
    if (signature.test(scene->getComponentId<SpriteAnimationComponent>())) return handleClass("SpriteAnimation");
    if (signature.test(scene->getComponentId<MorphTracksComponent>())) return handleClass("MorphTracks");
    if (signature.test(scene->getComponentId<LatticeTracksComponent>())) return handleClass("LatticeTracks");
    if (signature.test(scene->getComponentId<RotateTracksComponent>())) return handleClass("RotateTracks");
    if (signature.test(scene->getComponentId<ScaleTracksComponent>())) return handleClass("ScaleTracks");
    if (signature.test(scene->getComponentId<TranslateTracksComponent>())) return handleClass("TranslateTracks");
    if (signature.test(scene->getComponentId<KeyframeTracksComponent>())) return handleClass("KeyframeTracks");
    if (signature.test(scene->getComponentId<ParticleSystemComponent>()))   return objectClass("ParticleSystem");
    if (signature.test(scene->getComponentId<ActionComponent>()))      return handleClass("Action");
    if (signature.test(scene->getComponentId<Transform>()))            return objectClass("GameObject");

    // generic engine type for entities with no recognized component: valid as a
    // C++ pointer type (jasperengine::EntityHandle*) and known to the Lua dispatcher
    return handleClass("EntityHandle");
}

void editor::ProjectUtils::clearPreviewParticles(Scene* scene, Entity entity) {
    if (scene->findComponent<ParticleSystemComponent>(entity)) {
        scene->getSystem<ParticleSubsystem>()->stop(entity, true);
    }
}

std::string editor::ProjectUtils::getEntityTypeName(Scene* scene, Entity entity) {
    return getEntityClassInfo(scene, entity).name;
}

std::string editor::ProjectUtils::ensureSoftParticleTexture(Project* project) {
    if (!project) return "";
    // project-relative, as texture paths are ("assets/ui/particle_soft.png")
    const std::string relative = (project->getAssetsDir() / "ui" / "particle_soft.png").generic_string();
    const fs::path file = project->getAssetsPath() / "ui" / "particle_soft.png";
    std::error_code ec;
    if (fs::exists(file, ec)) return relative;

    // a 64 x 64 white disc whose alpha falls off smoothly to the edge
    constexpr int size = 64;
    std::vector<unsigned char> pixels((size_t)size * size * 4, 255);
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            const float dx = ((float)x + 0.5f) / (float)size * 2.0f - 1.0f;
            const float dy = ((float)y + 0.5f) / (float)size * 2.0f - 1.0f;
            const float d = std::sqrt(dx * dx + dy * dy);
            const float t = std::clamp(1.0f - d, 0.0f, 1.0f);
            const float alpha = t * t * (3.0f - 2.0f * t);
            pixels[((size_t)y * size + x) * 4 + 3] = (unsigned char)std::lround(alpha * 255.0f);
        }
    }
    fs::create_directories(file.parent_path(), ec);
    if (!writePngFile(file, size, pixels)) {
        Out::warning("Could not write %s", FileUtils::pathToUtf8(file).c_str());
        return "";
    }
    return relative;
}

void editor::ProjectUtils::setupNewLineRenderer(LineRendererComponent& lines, bool scene2D){
    lines.mode = LineRendererMode::Polyline;
    lines.useWorldSpace = true;
    if (scene2D){
        lines.positions = {Vector3(0.0f, 0.0f, 0.0f), Vector3(100.0f, 0.0f, 0.0f)};
        lines.widthMultiplier = 4.0f;
    }else{
        lines.positions = {Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, 1.0f)};
        lines.widthMultiplier = 0.1f;
    }
    lines.needUpdateRibbon = true;
}

void editor::ProjectUtils::setupNewTrailRenderer(TrailRendererComponent& trail, bool scene2D){
    trail.widthMultiplier = scene2D ? 8.0f : 0.2f;
    if (scene2D){
        trail.minVertexDistance = 2.0f;
    }
    trail.widthCurve = Curve::linear(0.0f, 1.0f, 1.0f, 0.0f);
    trail.colorGradient = Gradient::twoColors(Vector3(1.0f, 1.0f, 1.0f), Vector3(1.0f, 1.0f, 1.0f), 1.0f, 0.0f);
    trail.needUpdateRibbon = true;
}

