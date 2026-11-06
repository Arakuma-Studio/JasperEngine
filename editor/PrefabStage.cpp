// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Unity's Prefab Mode: prefab stages. A stage is a SceneProject of its own (outside
// Project::scenes, reached through getScene) holding the prefab asset's GameObjects with
// their file ids as entity ids, so it is edited with the Hierarchy, the Inspector, the
// Scene view and the commands like any scene, and saving it writes the asset. See
// editor/project/PREFABS.md, section 16.

#include "PrefabStage.h"
#include "component/PrefabComponent.h"

#include "Project.h"
#include "EditorHost.h"
#include "StreamPreserved.h"
#include "Out.h"
#include "Stream.h"
#include "reflect/Reflect.h"
#include "Factory.h"
#include "command/CommandHandle.h"
#include "render/SceneRender.h"
#include "util/ProjectUtils.h"
#include "util/Util.h"
#include "util/YamlNumbers.h"
#include "subsystem/RenderSystem.h"

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <unordered_set>

using namespace jasperengine;

namespace {

    using Clock = std::chrono::steady_clock;

    // Auto Save writes this long after the last change (Unity saves after each change; a
    // drag or a typed value is one change)
    constexpr auto AUTO_SAVE_DELAY = std::chrono::milliseconds(350);
    // How often a stage looks for changes its asset got from outside it
    constexpr auto OUTSIDE_CHANGE_INTERVAL = std::chrono::milliseconds(1000);

    // The keys a scene keeps for an instance and a prefab file does not (PREFABS.md
    // section 8): the stage's nested instances are written as the asset's nested data
    void stripSceneOnlyKeys(YAML::Node node, bool inBlock) {
        if (node.IsMap()) {
            if (inBlock) {
                node.remove("version");
                node.remove("memberIds");
            }
            for (auto it = node.begin(); it != node.end(); ++it) {
                const std::string key = it->first.as<std::string>();
                stripSceneOnlyKeys(it->second, inBlock || key == "prefabInstance");
            }
        } else if (node.IsSequence()) {
            for (YAML::Node child : node) {
                stripSceneOnlyKeys(child, inBlock);
            }
        }
    }

    void markNestedRootsCurrent(YAML::Node node) {
        if (!node.IsMap()) {
            return;
        }
        const YAML::Node readNode = node;
        const YAML::Node components = readNode["components"];
        if (components && components.IsMap() && components["prefab"] && !readNode["prefabInstance"]) {
            YAML::Node block(YAML::NodeType::Map);
            block["version"] = editor::Prefab::FILE_VERSION;
            node["prefabInstance"] = block;
        }
        if (readNode["children"] && readNode["children"].IsSequence()) {
            for (YAML::Node child : node["children"]) {
                markNestedRootsCurrent(child);
            }
        }
    }

    bool hasAnyComponent(editor::Project* project, EntityRegistry* registry, const std::vector<Entity>& entities,
            const std::initializer_list<editor::ComponentType>& types) {
        (void)project;
        for (Entity entity : entities) {
            if (!registry->isEntityCreated(entity)) {
                continue;
            }
            for (editor::ComponentType type : editor::Catalog::findComponents(registry, entity)) {
                if (std::find(types.begin(), types.end(), type) != types.end()) {
                    return true;
                }
            }
        }
        return false;
    }

    // Position and rotation of a transform as one matrix (scale left out: the view only
    // needs to be moved)
    Matrix4 placement(const Vector3& position, const Quaternion& rotation) {
        return Matrix4::translateMatrix(position) * rotation.getRotationMatrix();
    }

}

// ---- PrefabContextOverlay ----

editor::PrefabContextOverlay::PrefabContextOverlay() {
    scene = new Scene(EntityPool::System);
    camera = new Camera(scene);
    camera->setType(CameraType::CAMERA_UI);
    camera->setDepthTest(false);
    veil = new Polygon(scene);
    // Unity's Gray context: the scene behind the stage, dimmed and washed out
    veil->setColor(Vector4(0.22f, 0.22f, 0.24f, 0.62f));
    scene->setCamera(camera);
    resize(1, 1);
}

editor::PrefabContextOverlay::~PrefabContextOverlay() {
    delete veil;
    delete camera;
    delete scene;
}

void editor::PrefabContextOverlay::resize(int newWidth, int newHeight) {
    newWidth = std::max(newWidth, 1);
    newHeight = std::max(newHeight, 1);
    if (newWidth == width && newHeight == height) {
        return;
    }
    width = newWidth;
    height = newHeight;
    veil->clearVertices();
    veil->addVertex(0, 0);
    veil->addVertex(0, (float)height);
    veil->addVertex((float)width, 0);
    veil->addVertex((float)width, (float)height);
    veil->setSize(static_cast<unsigned int>(width), static_cast<unsigned int>(height));
    scene->getSystem<RenderSystem>()->updateCameraSizeForCanvas(camera->getEntity(), (float)width, (float)height);
}

// ---- Lookup ----

editor::SceneProject* editor::Project::findPrefabStageScene(uint32_t sceneId) const {
    if (sceneId == NULL_PROJECT_SCENE) {
        return nullptr;
    }
    for (const auto& stage : prefabStages) {
        if (stage->sceneProject.id == sceneId) {
            return const_cast<SceneProject*>(&stage->sceneProject);
        }
    }
    return nullptr;
}

std::vector<editor::SceneProject*> editor::Project::getScenesAndPrefabStages() {
    std::vector<SceneProject*> result;
    result.reserve(scenes.size() + prefabStages.size());
    for (SceneProject& sceneProject : scenes) {
        result.push_back(&sceneProject);
    }
    for (const auto& stage : prefabStages) {
        result.push_back(&stage->sceneProject);
    }
    return result;
}

void editor::Project::compactAllComponents() {
    for (SceneProject* sceneProject : getScenesAndPrefabStages()) {
        if (sceneProject->scene) {
            sceneProject->scene->compactComponents();
        }
    }
    for (auto& [path, prefab] : prefabAssets) {
        if (prefab.registry) {
            prefab.registry->compactComponents();
        }
    }
}

bool editor::Project::isPrefabStage(uint32_t sceneId) const {
    return findPrefabStageScene(sceneId) != nullptr;
}

bool editor::Project::isEditingPrefabAsset(const SceneProject* sceneProject) const {
    return sceneProject && isPrefabStage(sceneProject->id);
}

editor::PrefabStage* editor::Project::getPrefabStage(uint32_t sceneId) {
    for (const auto& stage : prefabStages) {
        if (stage->sceneProject.id == sceneId) {
            return stage.get();
        }
    }
    return nullptr;
}

const editor::PrefabStage* editor::Project::getPrefabStage(uint32_t sceneId) const {
    for (const auto& stage : prefabStages) {
        if (stage->sceneProject.id == sceneId) {
            return stage.get();
        }
    }
    return nullptr;
}

editor::PrefabStage* editor::Project::getActivePrefabStage() {
    return prefabStages.empty() ? nullptr : prefabStages.back().get();
}

size_t editor::Project::getPrefabStageDepth(uint32_t sceneId) const {
    for (size_t i = 0; i < prefabStages.size(); i++) {
        if (prefabStages[i]->sceneProject.id == sceneId) {
            return i + 1;
        }
    }
    return 0;
}

bool editor::Project::hasUnsavedPrefabStage(uint32_t sceneId) const {
    const PrefabStage* stage = getPrefabStage(sceneId);
    return stage && (stage->sceneProject.isModified || hasUnsavedPrefabs(sceneId));
}

bool editor::Project::hasUnsavedPrefabStages() const {
    for (const auto& stage : prefabStages) {
        if (hasUnsavedPrefabStage(stage->sceneProject.id)) {
            return true;
        }
    }
    return false;
}

uint32_t editor::Project::getSceneBehindPrefabStage(uint32_t sceneId) const {
    const PrefabStage* stage = getPrefabStage(sceneId);
    if (!stage) {
        return sceneId;
    }
    for (const SceneProject& sceneProject : scenes) {
        if (sceneProject.id == stage->returnSceneId) {
            return stage->returnSceneId;
        }
    }
    for (const SceneProject& sceneProject : scenes) {
        if (sceneProject.opened) {
            return sceneProject.id;
        }
    }
    return scenes.empty() ? NULL_PROJECT_SCENE : scenes.front().id;
}

bool editor::Project::isPrefabStageRoot(uint32_t sceneId, Entity entity) const {
    const PrefabStage* stage = getPrefabStage(sceneId);
    return stage && entity != NULL_ENTITY && stage->root == entity;
}

bool editor::Project::canInstantiatePrefabIn(uint32_t sceneId, const std::filesystem::path& prefabPath) {
    const size_t depth = getPrefabStageDepth(sceneId);
    if (depth == 0 || prefabPath.empty()) {
        return true;
    }
    // A prefab whose instance goes in this stage holds it from then on, and every prefab
    // below it in the stack holds this one: none of them may be in the new prefab
    std::vector<std::string> forbidden;
    for (size_t i = 0; i < depth; i++) {
        forbidden.push_back(prefabStages[i]->assetPath.lexically_normal().generic_string());
    }
    std::unordered_set<std::string> visited;
    std::function<bool(const std::filesystem::path&)> holdsForbidden = [&](const std::filesystem::path& path) -> bool {
        const std::string key = path.lexically_normal().generic_string();
        if (std::find(forbidden.begin(), forbidden.end(), key) != forbidden.end()) {
            return true;
        }
        if (!visited.insert(key).second) {
            return false;
        }
        Prefab* prefab = loadPrefabAsset(path);
        if (!prefab || !prefab->registry) {
            return false;
        }
        auto components = prefab->registry->getComponentArray<PrefabComponent>();
        std::vector<std::filesystem::path> nested;
        for (size_t i = 0; i < components->size(); i++) {
            nested.push_back(components->getComponentFromIndex(i).path);
        }
        for (const std::filesystem::path& nestedPath : nested) {
            if (!nestedPath.empty() && holdsForbidden(nestedPath)) {
                return true;
            }
        }
        return false;
    };
    return !holdsForbidden(normalizeToProjectRelative(prefabPath));
}

// ---- Open ----

bool editor::Project::openPrefabStage(const std::filesystem::path& path, uint32_t contextSceneId, Entity contextRoot, bool inContext) {
    std::filesystem::path relative = path.is_absolute() ? normalizeToProjectRelative(path) : path.lexically_normal();
    relative = getPrefabAssetPath(relative);
    if (relative.empty() || !Util::isPrefabFile(relative.string())) {
        Out::error("Not a prefab: %s", path.string().c_str());
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(getProjectPath() / relative, ec)) {
        editor::getEditorHost().registerAlert("Open Prefab", "The prefab \"" + relative.generic_string() + "\" does not exist.");
        return false;
    }
    // S29 R19: a prefab in conflict opens once it is resolved
    if (documentOpenGuard) {
        std::string why;
        if (!documentOpenGuard(getProjectPath() / relative, why)) {
            editor::getEditorHost().registerAlert("Open Prefab", why);
            return false;
        }
    }

    // From an instance inside a stage the new stage goes on top of that one; from anywhere
    // else it replaces the whole stack
    size_t keep = 0;
    if (contextSceneId != NULL_PROJECT_SCENE) {
        keep = getPrefabStageDepth(contextSceneId);
    }
    if (!inContext) {
        contextRoot = NULL_ENTITY;
    }
    if (contextRoot == NULL_ENTITY) {
        contextSceneId = keep > 0 ? contextSceneId : NULL_PROJECT_SCENE;
    }

    // Already the shown stage, opened the same way: nothing to do but show it
    if (prefabStages.size() == keep + 1) {
        PrefabStage* top = prefabStages.back().get();
        if (top->assetPath.lexically_normal() == relative && top->contextSceneId == (contextRoot != NULL_ENTITY ? contextSceneId : NULL_PROJECT_SCENE)
                && top->contextRoot == contextRoot) {
            setSelectedSceneId(top->sceneProject.id);
            return true;
        }
    }

    closePrefabStages(keep, [this, relative, contextSceneId, contextRoot, inContext](bool closed) {
        if (closed) {
            pushPrefabStage(relative, contextSceneId, contextRoot, inContext);
        }
    });
    return true;
}

bool editor::Project::pushPrefabStage(const std::filesystem::path& named, uint32_t contextSceneId, Entity contextRoot, bool inContext) {
    // the asset, at its path, whichever path of its file was named
    const std::filesystem::path relative = getPrefabAssetPath(named);
    Prefab* prefab = loadPrefabAsset(relative);
    if (!prefab || !prefab->registry || prefab->root == NULL_ENTITY) {
        editor::getEditorHost().registerAlert("Open Prefab", "The prefab \"" + relative.generic_string() + "\" could not be read.");
        return false;
    }

    SceneProject* context = nullptr;
    if (inContext && contextRoot != NULL_ENTITY) {
        context = getScene(contextSceneId);
        if (context && (!context->scene || !context->scene->isEntityCreated(contextRoot))) {
            context = nullptr;
        }
    }

    auto stagePtr = std::make_unique<PrefabStage>();
    PrefabStage& stage = *stagePtr;
    stage.assetPath = relative;
    stage.guid = getPrefabGuid(relative);
    stage.returnSceneId = prefabStages.empty() ? getSceneBehindPrefabStage(selectedScene) : prefabStages.front()->returnSceneId;
    if (context) {
        stage.contextSceneId = context->id;
        stage.contextRoot = contextRoot;
    }

    // The environment the prefab is edited in: the context's kind of scene, else the kind
    // of the scenes using it, else what its components are made for (Unity gives a UI
    // prefab a canvas and anything else a 3D environment)
    SceneType type = SceneType::SCENE_3D;
    bool typed = false;
    if (context) {
        type = context->sceneType;
        typed = true;
    }
    for (auto it = prefab->instances.begin(); !typed && it != prefab->instances.end(); ++it) {
        const SceneProject* user = it->second.empty() ? nullptr : getScene(it->first);
        if (user && user->scene) {
            type = user->sceneType;
            typed = true;
        }
    }
    if (!typed) {
        // (a variant's GameObjects are mostly its bases': the whole chain says)
        std::vector<Prefab*> chain;
        for (const std::filesystem::path& level : getPrefabVariantChain(relative)) {
            if (Prefab* levelPrefab = getPrefab(level); levelPrefab && levelPrefab->registry) {
                chain.push_back(levelPrefab);
            }
        }
        if (chain.empty()) {
            chain.push_back(prefab);
        }
        bool ui = false;
        bool twoD = false;
        for (Prefab* levelPrefab : chain) {
            EntityRegistry* registry = levelPrefab->registry.get();
            ui = ui || hasAnyComponent(this, registry, {levelPrefab->root}, {ComponentType::RectTransformComponent, ComponentType::UIComponent});
            twoD = twoD || hasAnyComponent(this, registry, levelPrefab->registryEntities, {ComponentType::SpriteComponent, ComponentType::TilemapComponent,
                ComponentType::Rigidbody2DComponent, ComponentType::Joint2DComponent, ComponentType::CharacterController2DComponent, ComponentType::Light2DComponent,
                ComponentType::ShadowCaster2DComponent, ComponentType::SpriteAnimationComponent});
        }
        if (ui) {
            type = SceneType::SCENE_UI;
        } else if (twoD) {
            type = SceneType::SCENE_2D;
        }
        prefab = loadPrefabAsset(relative);
        if (!prefab) {
            return false;
        }
    }

    SceneProject& sceneProject = stage.sceneProject;
    sceneProject.id = allocateSceneId();
    if (sceneProject.id == NULL_PROJECT_SCENE) {
        return false;
    }
    sceneProject.name = relative.stem().string();
    sceneProject.scene = new Scene();
    sceneProject.sceneType = type;
    sceneProject.sceneRender = createSceneRender(type, sceneProject.scene);
    sceneProject.defaultCamera = createDefaultCamera(type, sceneProject.scene);
    sceneProject.isVisible = true;
    sceneProject.opened = true;

    // Environment: system entities, never listed, picked or saved
    Scene* scene = sceneProject.scene;
    if (type == SceneType::SCENE_3D) {
        scene->setLightState(LightState::ON);
        scene->setGlobalIllumination(0.2);
        scene->setBackgroundColor(Vector4(0.25, 0.45, 0.65, 1.0));

        Entity sun = scene->createSystemEntity();
        scene->setEntityName(sun, "Prefab Mode Light");
        scene->addComponent<Transform>(sun, {});
        scene->addComponent<LightComponent>(sun, {});
        Transform& sunTransform = scene->getComponent<Transform>(sun);
        sunTransform.position = Vector3(0.0f, 10.0f, 0.0f);
        LightComponent& light = scene->getComponent<LightComponent>(sun);
        light.type = LightType::DIRECTIONAL;
        light.direction = Vector3(-0.2f, -0.5f, 0.3f);
        light.intensity = 4.0f;
        light.shadows = true;
        light.range = 100;
        Catalog::updateEntity(scene, sun, Catalog::getComponentStructuralUpdateFlags(ComponentType::LightComponent));
        stage.environment.push_back(sun);

        // In context the scene behind is the background
        if (!context) {
            Entity sky = scene->createSystemEntity();
            scene->setEntityName(sky, "Prefab Mode Skybox");
            scene->addComponent<SkyboxComponent>(sky, {});
            PropertyData texture = Catalog::findProperty(scene, sky, ComponentType::SkyboxComponent, "texture");
            if (texture.ref && texture.type == PropertyType::Texture) {
                ProjectUtils::setDefaultSkyTexture(*static_cast<Texture*>(texture.ref));
                Catalog::updateEntity(scene, sky, texture.updateFlags);
            }
            stage.environment.push_back(sky);
        }
    }

    prefabStages.push_back(std::move(stagePtr));
    if (!loadPrefabStageContent(stage)) {
        editor::getEditorHost().registerAlert("Open Prefab", "The prefab \"" + relative.generic_string() + "\" could not be opened.");
        popPrefabStage();
        return false;
    }

    // In context the stage's view starts where the context's was (Unity keeps the camera)
    if (context && context->sceneRender && type == SceneType::SCENE_3D && sceneProject.sceneRender) {
        Scene* contextScene = context->scene;
        Transform* instanceTransform = contextScene->findComponent<Transform>(contextRoot);
        Transform* rootTransform = scene->findComponent<Transform>(stage.root);
        Camera* fromCamera = context->sceneRender->getCamera();
        Camera* toCamera = sceneProject.sceneRender->getCamera();
        if (instanceTransform && rootTransform && fromCamera && toCamera) {
            const Matrix4 toStage = placement(rootTransform->position, rootTransform->rotation)
                * placement(instanceTransform->worldPosition, instanceTransform->worldRotation).inverse();
            CameraComponent& from = contextScene->getComponent<CameraComponent>(fromCamera->getEntity());
            Transform& fromTransform = contextScene->getComponent<Transform>(fromCamera->getEntity());
            CameraComponent& to = scene->getComponent<CameraComponent>(toCamera->getEntity());
            Transform& toTransform = scene->getComponent<Transform>(toCamera->getEntity());
            const Vector3 position = toStage * fromTransform.position;
            toTransform.position = position;
            to.target = toStage * from.target;
            to.up = (toStage * (fromTransform.position + from.up)) - position;
            to.needUpdate = true;
            toTransform.needUpdate = true;
            stage.frameOnOpen = false;
        }
    }

    setSelectedSceneId(sceneProject.id);
    replaceSelectedEntities(sceneProject.id, {stage.root});
    setSelectedSceneForProperties(sceneProject.id);
    editor::getEditorHost().resetLastActivatedScene();

    Out::info("Prefab Mode: %s%s", relative.generic_string().c_str(),
        context ? " (in context)" : "");
    return true;
}

bool editor::Project::loadPrefabStageContent(PrefabStage& stage) {
    Prefab* prefab = loadPrefabAsset(stage.assetPath);
    if (!prefab || !prefab->registry || prefab->root == NULL_ENTITY) {
        return false;
    }
    // A nested root read from a file older than version 3 holds the old wrapper's values:
    // what they override of the nested prefab becomes data first (as an import does)
    for (Entity entity : std::vector<Entity>(prefab->registryEntities)) {
        if (prefab->isNestedRoot(entity)) {
            nestedAssetLayerFor(*prefab, stage.assetPath, entity);
        }
    }

    const YAML::Node assetNode = encodePrefabNode(stage.assetPath);
    if (!assetNode || !assetNode["members"]) {
        return false;
    }
    stage.guid = assetNode["guid"] ? assetNode["guid"].as<std::string>() : getPrefabGuid(stage.assetPath);
    const Entity nextEntity = assetNode["nextEntity"] ? YamlNumbers::as<Entity>(assetNode["nextEntity"]) : prefab->getNextEntity();

    SceneProject& sceneProject = stage.sceneProject;
    YAML::Node sceneNode;
    sceneNode["entities"] = YAML::Clone(assetNode["members"]);
    // The GameObjects the asset adds to its nested instances are those instances' added
    // GameObjects in the stage (PREFABS.md 17)
    prefabNodeToStage(*prefab, sceneNode["entities"]);
    // A nested root the asset overrides nothing of has no block in the file: in a scene a
    // root without one is from before the block (legacy), so it gets an empty one
    for (YAML::Node member : sceneNode["entities"]) {
        markNestedRootsCurrent(member);
    }
    try {
        // The members are the asset's GameObjects with their file ids; the members of the
        // instances nested in it take ids above every id the asset ever handed out
        Stream::decodeSceneProjectEntities(this, &sceneProject, sceneNode, nextEntity);
    } catch (const std::exception& e) {
        Out::error("Failed to open prefab %s: %s", stage.assetPath.string().c_str(), e.what());
        return false;
    }

    // References into the nested instances (stripped objects) to their GameObjects here
    resolveStageStrippedReferences(&sceneProject, *prefab);

    stage.root = prefab->root;
    if (!sceneProject.scene->isEntityCreated(stage.root)) {
        Out::error("Prefab %s: its root GameObject could not be read", stage.assetPath.string().c_str());
        return false;
    }

    sceneProject.isModified = false;
    sceneProject.needUpdateRender = true;
    sceneProject.structureVersion++;
    stage.changePending = false;
    stage.outsideChangeWarned = false;
    stage.syncedAsset = YAML::Dump(encodePrefabNode(stage.assetPath));
    stage.lastSyncCheck = Clock::now();
    return true;
}

void editor::Project::destroyPrefabStageContent(PrefabStage& stage) {
    SceneProject& sceneProject = stage.sceneProject;
    // The nested instances stop being tracked; the prefab assets stay loaded (one may have
    // changes in memory that a reload must see)
    for (auto& [path, prefab] : prefabAssets) {
        prefab.instances.erase(sceneProject.id);
    }
    if (sceneProject.scene) {
        for (Entity entity : std::vector<Entity>(sceneProject.entities)) {
            if (sceneProject.scene->isEntityCreated(entity)) {
                sceneProject.scene->destroyEntity(entity);
            }
        }
    }
    sceneProject.entities.clear();
    sceneProject.selectedEntities.clear();
    sceneProject.missingPrefabs.clear();
    sceneProject.mainCamera = NULL_ENTITY;
    sceneProject.structureVersion++;
    sceneProject.needUpdateRender = true;
}

// ---- Save ----

void editor::Project::fixPrefabStageRoots(PrefabStage& stage) {
    SceneProject& sceneProject = stage.sceneProject;
    Scene* scene = sceneProject.scene;
    if (!scene) {
        return;
    }
    const bool rootListed = scene->isEntityCreated(stage.root)
        && std::find(sceneProject.entities.begin(), sceneProject.entities.end(), stage.root) != sceneProject.entities.end();

    std::vector<Entity> topLevel;
    for (Entity entity : sceneProject.entities) {
        Transform* transform = scene->findComponent<Transform>(entity);
        if (transform && transform->parent == NULL_ENTITY && entity != stage.root) {
            topLevel.push_back(entity);
        }
    }
    if (!rootListed) {
        // the root went (an undo of what made it): a single top-level GameObject takes over
        if (topLevel.size() == 1) {
            stage.root = topLevel.front();
            topLevel.clear();
        } else {
            return;
        }
    }
    if (topLevel.empty()) {
        return;
    }
    // A prefab has one root (Unity puts what goes to the top of Prefab Mode below it)
    for (Entity entity : topLevel) {
        scene->addEntityChild(stage.root, entity, true);
    }
    ProjectUtils::sortEntitiesByTransformOrder(scene, sceneProject.entities);
    sceneProject.isModified = true;
    sceneProject.needUpdateRender = true;
    sceneProject.structureVersion++;
    notePrefabStageChange(sceneProject.id);
}

YAML::Node editor::Project::encodePrefabStage(PrefabStage& stage) {
    // the prefab file Prefab Mode saves: without the values loading computes (S29 R20)
    reflect::DocumentEncodeScope document;
    SceneProject& sceneProject = stage.sceneProject;
    Scene* scene = sceneProject.scene;
    fixPrefabStageRoots(stage);
    if (!scene || !scene->isEntityCreated(stage.root)) {
        return YAML::Node();
    }

    std::vector<Entity> topLevel = getTopLevelEntities(scene, sceneProject.entities);
    auto rootIt = std::find(topLevel.begin(), topLevel.end(), stage.root);
    if (rootIt == topLevel.end()) {
        return YAML::Node();
    }
    topLevel.erase(rootIt);
    topLevel.insert(topLevel.begin(), stage.root);

    YAML::Node membersNode(YAML::NodeType::Sequence);
    {
        // Every GameObject of the stage in the asset, or no save (savePrefabStage)
        Stream::SavedEntitiesCheck check(&sceneProject);
        for (Entity entity : topLevel) {
            membersNode.push_back(Stream::encodeEntity(entity, scene, this, &sceneProject));
        }
        const std::vector<Entity> left = check.leftOut(this);
        if (!left.empty()) {
            throw Stream::IncompleteSave(check.describe(left, "the stage") + " (an editor fault: please report it); nothing was written");
        }
    }
    stripSceneOnlyKeys(membersNode, false);

    const Prefab* prefab = getPrefab(stage.assetPath);
    Entity nextEntity = scene->getLastEntity() + 1;
    if (prefab) {
        nextEntity = std::max(nextEntity, prefab->getNextEntity());
    }
    std::string guid = getPrefabGuid(stage.assetPath);
    if (guid.empty()) {
        guid = stage.guid;
    }

    YAML::Node node;
    node["type"] = "Prefab";
    node["version"] = Prefab::FILE_VERSION;
    node["guid"] = guid;
    node["nextEntity"] = nextEntity;
    node["root"] = stage.root;
    node["members"] = membersNode;
    // the asset's header keys this editor does not know go with it (docs/data-safety.md)
    if (prefab && prefab->registry) {
        StreamSave::applyRootKeys(prefab->registry.get(), NULL_ENTITY, node);
    }
    return node;
}

YAML::Node editor::Project::encodeDocument(uint32_t sceneId) {
    // a document whose encode would leave a GameObject out has no encoding (the save says why)
    try {
        if (PrefabStage* stage = getPrefabStage(sceneId)) return encodePrefabStage(*stage);
    } catch (const std::exception&) {
        return YAML::Node(YAML::NodeType::Undefined);
    }
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return YAML::Node(YAML::NodeType::Undefined);
    YAML::Node root;
    try {
        root = Stream::encodeSceneProject(this, sceneProject);
    } catch (const std::exception&) {
        return YAML::Node(YAML::NodeType::Undefined);
    }
    StreamMigrations::stampCurrentVersion(DocumentKind::Scene, root);
    // the identity keys a save puts in (guid, formerGuids, labels)
    if (!sceneProject->filepath.empty()) prepareDocumentIdentity(root, sceneProject->filepath);
    return root;
}

bool editor::Project::isPrefabStageDegraded(uint32_t sceneId) const {
    const PrefabStage* stage = getPrefabStage(sceneId);
    if (!stage) {
        return false;
    }
    auto it = prefabAssets.find(stage->assetPath);
    return it != prefabAssets.end() && it->second.loadReport.dropped() > 0;
}

bool editor::Project::savePrefabStage(uint32_t sceneId, DegradedSavePolicy policy) {
    PrefabStage* stage = getPrefabStage(sceneId);
    if (!stage) {
        return false;
    }
    // previews (a physics gesture's poses) settle or go back before the asset is written
    editor::getEditorHost().stopTransientPreviews();
    SceneProject& sceneProject = stage->sceneProject;
    const std::filesystem::path path = stage->assetPath;

    // the asset did not load completely: the host decides (Refuse when headless)
    if (policy == DegradedSavePolicy::Refuse && isPrefabStageDegraded(sceneId)) {
        DegradedSaveRequest request;
        request.documentName = path.filename().string();
        request.path = path;
        for (const LoadIssue& issue : prefabAssets.at(path).loadReport.issues) {
            if (issue.fate == LoadIssueFate::Dropped) {
                request.dropped.push_back(issue);
            }
        }
        request.saveAnyway = [this, sceneId]() {
            savePrefabStage(sceneId, DegradedSavePolicy::SaveAnyway);
        };
        policy = editor::getEditorHost().confirmDegradedSave(request);
        if (policy == DegradedSavePolicy::Refuse) {
            Out::warning("Prefab %s not saved: it did not load completely and saving it would remove %zu value(s) "
                         "this editor could not read.", path.generic_string().c_str(), request.dropped.size());
            return false;
        }
    }
    stage->changePending = false;

    YAML::Node node;
    try {
        node = encodePrefabStage(*stage);
    } catch (const std::exception& e) {
        Out::error("Prefab %s not saved: %s", path.generic_string().c_str(), e.what());
        return false;
    }
    if (!node || !node.IsMap()) {
        Out::error("Prefab %s not saved: a prefab needs its root GameObject", path.generic_string().c_str());
        return false;
    }
    stage->changePending = false; // fixPrefabStageRoots counts as part of this save

    Prefab* prefab = loadPrefabAsset(path);
    if (!prefab) {
        Out::error("Prefab %s not saved: the asset is not available", path.generic_string().c_str());
        return false;
    }

    // The registry becomes the stage's GameObjects with their ids (their file ids) and the
    // nested instances' data; the instances follow below
    const std::map<Entity, std::vector<Entity>> previousStripped = prefab->stripped;
    prefab->loadRegistry(node, path.stem().string());
    resolveRegistryPrefabRefs(prefab->registry.get());
    // The GameObjects added to the nested instances are the asset's; references stay inside
    // the asset, into its nested instances as stripped objects (PREFABS.md 17)
    stageToPrefabAsset(&sceneProject, *prefab, previousStripped);
    for (auto& [instanceSceneId, instances] : prefab->instances) {
        for (Prefab::Instance& instance : instances) {
            instance.rootRegistryEntity = prefab->root;
        }
    }
    prefab->needsUpgrade = false;
    prefab->isModified = true;
    savePrefabToDisk(path, policy);
    if (prefab->isModified) {
        return false; // could not be written (logged)
    }

    // Prefabs nested in the stage that its edits changed (edits shared with them)
    std::vector<std::filesystem::path> nestedChanged;
    for (const auto& [nestedPath, nested] : prefabAssets) {
        if (nested.isModified && nested.hasInstances(sceneProject.id)) {
            nestedChanged.push_back(nestedPath);
        }
    }
    for (const std::filesystem::path& nestedPath : nestedChanged) {
        savePrefabToDisk(nestedPath);
        refreshPrefabInstances(nestedPath, false);
    }

    // Every loaded instance (scenes and other stages) follows; scenes not loaded read the
    // file when they load. A playing scene is put back by Stop from its snapshot: it
    // follows once stopped.
    refreshPrefabInstances(path, false);
    if (isAnyScenePlaying()) {
        prefabsSavedWhilePlaying.insert(path);
        prefabsSavedWhilePlaying.insert(nestedChanged.begin(), nestedChanged.end());
    }

    markSceneSaved(&sceneProject);
    stage->outsideChangeWarned = false;
    if (Prefab* saved = getPrefab(path)) {
        (void)saved;
        stage->syncedAsset = YAML::Dump(encodePrefabNode(path));
    }
    stage->lastSyncCheck = Clock::now();

    // Generated sources: the scenes using it (and the prefab sources) are written again
    const std::string guid = getPrefabGuid(path);
    bool invalidated = false;
    for (const SceneProject& other : scenes) {
        const bool uses = std::any_of(other.prefabs.begin(), other.prefabs.end(), [&](const PrefabSceneInfo& info) {
            return (!guid.empty() && info.guid == guid) || info.prefabPath.lexically_normal() == path.lexically_normal();
        });
        if (uses) {
            generator.clearSceneSource(other.id, other.name, getProjectInternalPath());
            invalidated = true;
        }
    }
    std::error_code ec;
    if (std::filesystem::exists(getProjectPath() / "CMakeLists.txt", ec)) {
        generator.writePrefabSources(prefabAssets, NULL_PROJECT_SCENE, getProjectPath(), getProjectInternalPath(),
            [this](const std::filesystem::path& prefabPath) { return getPrefabGuid(prefabPath); }, bakePrefabsForExport());
    }
    if (invalidated || isStandalonePrefab(path)) {
        generatedSourcesDirty.store(true);
    }

    Out::info("Prefab saved: \"%s\"", path.generic_string().c_str());
    return true;
}

// ---- Close ----

void editor::Project::releasePrefabStageContext(PrefabStage& stage) {
    SceneProject* context = stage.contextSceneId != NULL_PROJECT_SCENE ? getScene(stage.contextSceneId) : nullptr;
    if (context && context != &stage.sceneProject && context->scene) {
        Scene* scene = context->scene;
        if (stage.contextHidden && scene->isEntityCreated(stage.contextRoot)) {
            scene->setEntityActive(stage.contextRoot, stage.contextWasActive);
        }
        if (stage.contextCamera != NULL_ENTITY && scene->isEntityCreated(stage.contextCamera)) {
            if (scene->getCamera() == stage.contextCamera && context->sceneRender && context->sceneRender->getCamera()) {
                scene->setCamera(context->sceneRender->getCamera()->getEntity());
            }
            scene->destroyEntity(stage.contextCamera);
        }
        scene->getSystem<RenderSystem>()->setDrawEditorOverlays(true);
        context->needUpdateRender = true;
    }
    stage.contextHidden = false;
    stage.contextCamera = NULL_ENTITY;
    if (stage.sceneProject.sceneRender) {
        stage.sceneProject.sceneRender->setUnderlay(nullptr, NULL_ENTITY, nullptr);
    }
    stage.overlay.reset();
}

void editor::Project::popPrefabStage() {
    if (prefabStages.empty()) {
        return;
    }
    std::unique_ptr<PrefabStage> stage = std::move(prefabStages.back());
    prefabStages.pop_back();
    SceneProject& sceneProject = stage->sceneProject;
    const uint32_t sceneId = sceneProject.id;

    releasePrefabStageContext(*stage);
    editor::getEditorHost().flushSceneMaterialWrites(sceneId);
    unlinkAllMaterialFiles(sceneId);
    destroyPrefabStageContent(*stage);
    deleteSceneProject(&sceneProject);
    CommandHandle::remove(sceneId);
    editor::getEditorHost().clearSceneWindowState(sceneId);

    // Prefabs only the stage used are dropped (not one with changes in memory, nor the
    // asset of another stage)
    for (auto it = prefabAssets.begin(); it != prefabAssets.end();) {
        bool usedByStage = false;
        for (const auto& other : prefabStages) {
            if (other->assetPath == it->first) {
                usedByStage = true;
            }
        }
        if (it->second.instances.empty() && !isStandalonePrefab(it->first) && !it->second.isModified && !usedByStage) {
            it = prefabAssets.erase(it);
        } else {
            ++it;
        }
    }

    // Back to the stage below, or to the scene Prefab Mode was entered from
    uint32_t next = !prefabStages.empty() ? prefabStages.back()->sceneProject.id : stage->returnSceneId;
    const SceneProject* nextScene = getScene(next);
    if (!nextScene || (!nextScene->opened && !isPrefabStage(next))) {
        next = NULL_PROJECT_SCENE;
        for (const SceneProject& other : scenes) {
            if (other.opened) {
                next = other.id;
                break;
            }
        }
    }
    if (selectedScene == sceneId || isPrefabStage(next) || !getScene(selectedScene)) {
        // setSelectedSceneId() early-outs on an unchanged id
        selectedScene = NULL_PROJECT_SCENE;
        setSelectedSceneId(next);
    }
    if (selectedSceneForProperties == sceneId) {
        selectedSceneForProperties = selectedScene;
    }
    if (SceneProject* shown = getScene(next)) {
        shown->needUpdateRender = true;
    }
    editor::getEditorHost().resetLastActivatedScene();
}

void editor::Project::closePrefabStages(size_t depth, std::function<void(bool)> done, int choice) {
    if (prefabStages.size() <= depth) {
        if (done) done(true);
        return;
    }
    PrefabStage& top = *prefabStages.back();
    const uint32_t sceneId = top.sceneProject.id;
    auto next = [this, depth, done, choice]() {
        closePrefabStages(depth, done, choice);
    };
    auto popIfTop = [this, sceneId]() {
        if (!prefabStages.empty() && prefabStages.back()->sceneProject.id == sceneId) {
            popPrefabStage();
        }
    };
    if (!hasUnsavedPrefabStage(sceneId)) {
        popIfTop();
        next();
        return;
    }

    auto save = [this, sceneId, popIfTop, next, done]() {
        if (isPrefabStage(sceneId) && !savePrefabStage(sceneId)) {
            if (done) done(false);
            return;
        }
        popIfTop();
        next();
    };
    auto discard = [popIfTop, next]() {
        popIfTop();
        next();
    };
    auto cancel = [done]() {
        if (done) done(false);
    };
    // With Auto Save on a change not written yet (its delay running) is just saved: Unity
    // asks only when Auto Save is off
    if (choice == 0 && prefabAutoSave) {
        choice = 1;
    }
    switch (choice) {
        case 1: save(); return;
        case 2: discard(); return;
        case 3: cancel(); return;
        default: break;
    }
    editor::getEditorHost().registerSaveDiscardAlert("Prefab Has Been Modified",
        "Do you want to save the changes you made in Prefab Mode to \"" + top.assetPath.generic_string()
        + "\"? Your changes will be lost if you don't save them.",
        save, discard, cancel);
}

// ---- Per frame ----

void editor::Project::notePrefabStageChange(uint32_t sceneId) {
    if (PrefabStage* stage = getPrefabStage(sceneId)) {
        stage->changePending = true;
        stage->autoSaveFailed = false;
        stage->lastChange = Clock::now();
        // An undo back past a save still differs from the asset written then (commands put
        // back the modified flag they found)
        stage->sceneProject.isModified = true;
    }
}

void editor::Project::setPrefabContextVisibility(PrefabContextVisibility visibility) {
    if (prefabContextVisibility == visibility) {
        return;
    }
    prefabContextVisibility = visibility;
    if (PrefabStage* stage = getActivePrefabStage()) {
        stage->sceneProject.needUpdateRender = true;
    }
    editor::getEditorHost().resetLastActivatedScene();
}

void editor::Project::updatePrefabStages(bool userEditing) {
    // Prefabs saved in Prefab Mode during Play: the scenes Stop restored follow them now
    if (!prefabsSavedWhilePlaying.empty() && !isAnyScenePlaying()) {
        const std::set<std::filesystem::path> saved = std::move(prefabsSavedWhilePlaying);
        prefabsSavedWhilePlaying.clear();
        for (const std::filesystem::path& path : saved) {
            refreshPrefabInstances(path, false);
        }
    }
    if (prefabStages.empty()) {
        return;
    }
    const auto now = Clock::now();
    size_t lostFrom = prefabStages.size();

    for (size_t i = 0; i < prefabStages.size(); i++) {
        PrefabStage& stage = *prefabStages[i];
        SceneProject& sceneProject = stage.sceneProject;
        const uint32_t sceneId = sceneProject.id;

        // A context closed (its scene, or the stage it was) leaves the stage in isolation
        if (stage.inContext()) {
            SceneProject* context = getScene(stage.contextSceneId);
            if (!context || !context->scene || context == &sceneProject) {
                stage.contextHidden = false;
                stage.contextCamera = NULL_ENTITY;
                if (sceneProject.sceneRender) {
                    sceneProject.sceneRender->setUnderlay(nullptr, NULL_ENTITY, nullptr);
                }
                stage.overlay.reset();
                stage.contextSceneId = NULL_PROJECT_SCENE;
                stage.contextRoot = NULL_ENTITY;
                sceneProject.needUpdateRender = true;
                editor::getEditorHost().resetLastActivatedScene();
            }
        }

        if (!userEditing) {
            fixPrefabStageRoots(stage);
        }

        // Auto Save, a moment after the last change once the edit is over
        const bool unsaved = hasUnsavedPrefabStage(sceneId);
        if (unsaved && !stage.changePending && !stage.autoSaveFailed) {
            // a change made without a command
            stage.changePending = true;
            stage.lastChange = now;
        }
        // never for an asset that did not load completely (the stage bar says Auto Save is paused)
        if (prefabAutoSave && unsaved && stage.changePending && !userEditing && now - stage.lastChange >= AUTO_SAVE_DELAY &&
                !isPrefabStageDegraded(sceneId)) {
            // a save that fails (no root) is tried again after the next change only
            stage.autoSaveFailed = !savePrefabStage(sceneId);
        }

        // The asset changed from outside the stage (an Apply from a scene, the file edited):
        // an unmodified stage reloads, a modified one keeps its version (written on save)
        if (!userEditing && now - stage.lastSyncCheck >= OUTSIDE_CHANGE_INTERVAL) {
            stage.lastSyncCheck = now;
            // Renamed or moved: the stage follows its asset's GUID; deleted: it closes
            const std::filesystem::path current = resolvePrefabPath(stage.guid, stage.assetPath);
            if (current.empty()) {
                lostFrom = std::min(lostFrom, i);
                continue;
            }
            if (current.lexically_normal() != stage.assetPath.lexically_normal()) {
                Out::info("Prefab Mode: \"%s\" is now \"%s\"", stage.assetPath.generic_string().c_str(), current.generic_string().c_str());
                stage.assetPath = current;
                sceneProject.name = current.stem().string();
                sceneProject.structureVersion++;
            }
            if (getPrefab(stage.assetPath)) {
                const std::string current = YAML::Dump(encodePrefabNode(stage.assetPath));
                if (current != stage.syncedAsset) {
                    if (!hasUnsavedPrefabStage(sceneId)) {
                        const std::vector<Entity> selection = sceneProject.selectedEntities;
                        destroyPrefabStageContent(stage);
                        if (loadPrefabStageContent(stage)) {
                            std::vector<Entity> kept;
                            for (Entity entity : selection) {
                                if (sceneProject.scene->isEntityCreated(entity)) {
                                    kept.push_back(entity);
                                }
                            }
                            SelectionState keptSelection;
                            keptSelection.entities = kept;
                            setSelection(sceneId, keptSelection, SelectionCause::Forced);
                            CommandHandle::get(sceneId)->clear();
                            Out::info("Prefab \"%s\" changed outside Prefab Mode: reloaded", stage.assetPath.generic_string().c_str());
                        } else {
                            Out::error("Prefab \"%s\" changed outside Prefab Mode and could not be read again", stage.assetPath.generic_string().c_str());
                        }
                    } else if (!stage.outsideChangeWarned) {
                        stage.outsideChangeWarned = true;
                        Out::warning("Prefab \"%s\" changed outside Prefab Mode: saving the stage writes its own version over that",
                            stage.assetPath.generic_string().c_str());
                    }
                }
            }
        }
    }

    // An asset deleted under its stage (and the stages above it, which it held) ends Prefab
    // Mode there, as Unity does; nothing can be saved into it any more
    if (lostFrom < prefabStages.size()) {
        Out::warning("Prefab \"%s\" was deleted: its Prefab Mode closed", prefabStages[lostFrom]->assetPath.generic_string().c_str());
        closePrefabStages(lostFrom, nullptr, 2);
    }
}

// ---- In Context drawing ----

void editor::Project::preparePrefabStageView(uint32_t sceneId, int width, int height) {
    PrefabStage* stage = getPrefabStage(sceneId);
    if (!stage || !stage->sceneProject.sceneRender) {
        return;
    }
    SceneProject& sceneProject = stage->sceneProject;
    SceneRender* render = sceneProject.sceneRender;
    SceneProject* context = stage->inContext() ? getScene(stage->contextSceneId) : nullptr;
    const bool show = context && context != &sceneProject && context->scene && context->sceneRender
        && context->playState == ScenePlayState::STOPPED && prefabContextVisibility != PrefabContextVisibility::Hidden;
    if (!show) {
        if (render->setUnderlay(nullptr, NULL_ENTITY, nullptr)) {
            editor::getEditorHost().resetLastActivatedScene();
        }
        return;
    }

    Scene* contextScene = context->scene;
    if (stage->contextCamera == NULL_ENTITY || !contextScene->isEntityCreated(stage->contextCamera)) {
        stage->contextCamera = contextScene->createSystemEntity();
        contextScene->setEntityName(stage->contextCamera, "Prefab Mode Context Camera");
        contextScene->addComponent<Transform>(stage->contextCamera, {});
        contextScene->addComponent<CameraComponent>(stage->contextCamera, {});
    }

    Scene* veil = nullptr;
    if (prefabContextVisibility == PrefabContextVisibility::Gray) {
        if (!stage->overlay) {
            stage->overlay = std::make_shared<PrefabContextOverlay>();
        }
        stage->overlay->resize(width, height);
        veil = stage->overlay->getScene();
    } else {
        stage->overlay.reset();
    }
    if (render->setUnderlay(contextScene, stage->contextCamera, veil)) {
        editor::getEditorHost().resetLastActivatedScene();
    }

    // The context camera sees the context as the stage's camera sees the stage, the
    // prefab's root standing where the instance is (Unity shows the prefab in its place)
    Scene* scene = sceneProject.scene;
    const Entity viewCamera = scene->getCamera();
    CameraComponent* from = scene->findComponent<CameraComponent>(viewCamera);
    Transform* fromTransform = scene->findComponent<Transform>(viewCamera);
    if (!from || !fromTransform) {
        return;
    }
    Matrix4 toContext;  // identity
    Transform* instanceTransform = contextScene->isEntityCreated(stage->contextRoot) ? contextScene->findComponent<Transform>(stage->contextRoot) : nullptr;
    Transform* rootTransform = scene->findComponent<Transform>(stage->root);
    if (instanceTransform && rootTransform) {
        toContext = placement(instanceTransform->worldPosition, instanceTransform->worldRotation)
            * placement(rootTransform->position, rootTransform->rotation).inverse();
    }
    CameraComponent& to = contextScene->getComponent<CameraComponent>(stage->contextCamera);
    Transform& toTransform = contextScene->getComponent<Transform>(stage->contextCamera);
    to.type = from->type;
    to.leftClip = from->leftClip;
    to.rightClip = from->rightClip;
    to.bottomClip = from->bottomClip;
    to.topClip = from->topClip;
    to.yfov = from->yfov;
    to.aspect = from->aspect;
    to.nearClip = from->nearClip;
    to.farClip = from->farClip;
    to.useTarget = from->useTarget;
    to.autoResize = false;
    to.transparentSort = from->transparentSort;
    to.depthTest = from->depthTest;
    to.hasCustomViewMatrix = false;
    const Vector3 position = toContext * fromTransform->worldPosition;
    toTransform.position = position;
    Vector3 dummyPosition;
    Vector3 dummyScale;
    Quaternion contextRotation;
    toContext.decompose(dummyPosition, dummyScale, contextRotation);
    toTransform.rotation = contextRotation * fromTransform->worldRotation;
    to.target = toContext * from->worldTarget;
    to.up = (toContext * (fromTransform->worldPosition + from->worldUp)) - position;
    to.needUpdate = true;
    toTransform.needUpdate = true;
}

void editor::Project::beginPrefabStageDraw(uint32_t sceneId) {
    PrefabStage* stage = getPrefabStage(sceneId);
    if (!stage || !stage->sceneProject.sceneRender || !stage->sceneProject.sceneRender->hasUnderlay()) {
        return;
    }
    SceneProject* context = getScene(stage->contextSceneId);
    if (!context || !context->scene || !context->scene->isEntityCreated(stage->contextRoot)) {
        return;
    }
    // The instance edited is the stage: hidden in the context while it draws
    stage->contextWasActive = context->scene->isEntityActive(stage->contextRoot);
    context->scene->setEntityActive(stage->contextRoot, false);
    stage->contextHidden = true;
}

void editor::Project::endPrefabStageDraw(uint32_t sceneId) {
    PrefabStage* stage = getPrefabStage(sceneId);
    if (!stage || !stage->contextHidden) {
        return;
    }
    stage->contextHidden = false;
    SceneProject* context = getScene(stage->contextSceneId);
    if (context && context->scene && context->scene->isEntityCreated(stage->contextRoot)) {
        context->scene->setEntityActive(stage->contextRoot, stage->contextWasActive);
    }
}
