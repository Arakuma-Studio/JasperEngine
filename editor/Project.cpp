// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Project.h"
#include "render/LayerViewCamera.h"
#include "component/MirrorComponent.h"
#include "script/LuaBinding.h"
#include "subsystem/ActionSystem.h"
#include "subsystem/AudioSystem.h"
#include "subsystem/RenderSystem.h"
#include "component/ButtonComponent.h"
#include "component/MeshPolygonComponent.h"
#include "io/watch/LinkedFolders.h"
#include "diag/HangWatchdog.h"
#include "util/EditorDiagnostics.h"
#include "util/ScriptReloadService.h"
#include "util/ScriptStatus.h"
#include "util/EditorScripts.h"
#include "script/ScriptBase.h"
#include "reload/HotReloadService.h"
#include "asset/AssetRefSchema.h"
#include "asset/AssetFileWrite.h"
#include "navigation/NavigationEditor.h"
#include "util/CrashHook.h"
#include "util/Scheduler.h"
#include "io/AtomicFile.h"
#include "subsystem/PhysicsSystem.h"
#include "animation/AnimationClipDocument.h"
#include "animation/CompiledController.h"
#include "component/AnimatorComponent.h"
#include "pool/AnimatorAssetPool.h"
#include "vat/VatDocument.h"
#include "animation/ModelSubClips.h"
#include "pose/PoseAnimationDocument.h"
#include "pose/PoseRigProvider.h"
#include "pose/PoseSheetRenderer.h"
#include "Factory.h"
#include "picking/ScenePicker.h"
#include "sceneview/SceneViewVisibility.h"
#include "picking/SceneBvh.h"
#include "Workspace.h"
#include "registry/PhysicsLayers.h"

#include "EditorHost.h"
#include "Platform.h"
#include "Screen.h"
#include "util/FileUtils.h"
#include "vcs/VcsService.h"
#include "vcs/SceneDiffTracker.h"
#include <cstdlib>
#include "util/SafeWrite.h"
#include "util/FbxImporter.h"
#include "window/CodeEditor.h"
#include "window/ImageViewerWindow.h"
#include "window/TerrainEditWindow.h"

#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>
#include <chrono>
#include <unordered_set>
#include <algorithm>
#include <unordered_map>
#include <limits>
#include <random>
#include <thread>
#include <mutex>
#include <set>

#include "render/SceneRender2D.h"
#include "render/SceneRender3D.h"

#include "lua.hpp"
#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "AppSettings.h"
#include "Out.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/ParticleSubsystem.h"
#include "subsystem/LatticeSystem.h"
#include "subsystem/SplineSystem.h"
#include "subsystem/UISystem.h"
#include "subsystem/UIDocumentSystem.h"
#include "subsystem/FluidSystem.h"
#include "fluid/FluidSimulator.h"
#include "subsystem/PoseAnimatorSystem.h"
#include "subsystem/VertexAnimatorSystem.h"
#include "pool/SoundPool.h"
#include "audio/AudioMixer.h"
#include "audio/AudioMixerDocument.h"
#include "subsystem/VideoPlayerSystem.h"
#include "subsystem/TrailSystem.h"
#include "subsystem/CameraControlSystem.h"
#include "pose/ModelRig.h"
#include "pose/PoseAnimatorRuntime.h"
#include "animation/ModelSubClips.h"
#include "htmlui/anim/AnimEngine.h"
#include "native/NativeBridge.h"
#include "script/ScriptScheduler.h"
#include "net/Http.h"
#include "media/MediaCache.h"
#include "util/NativePlugins.h"
#include "thread/MainThreadQueue.h"
#include "command/CommandHandle.h"
#include "command/PlayChangeReview.h"
#include "command/UndoTimeline.h"
#include "command/type/SelectionCmd.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/MoveEntityOrderCmd.h"
#include "command/type/ModelLoadCmd.h"
#include "Stream.h"
#include "StreamPreserved.h"
#include "DocumentFile.h"
#include "StreamSchema.h"
#include "util/FileDialogs.h"
#include "util/SHA1.h"
#include "util/GraphicUtils.h"
#include "util/CameraTextureLink.h"
#include "util/ProjectUtils.h"
#include "util/Util.h"
#include "json.hpp"

#include "texture/Texture.h"
#include "texture/TextureCookQueue.h"
#include "component/UIDocumentComponent.h"
#include "component/VideoPlayerComponent.h"
#include "Engine.h"
#include "shader/ShaderWarmup.h"
#include "Input.h"
#include "input/InputActions.h"
#include "io/UserSettings.h"
#include "io/UserSettingsStore.h"
#include "pool/ShaderPool.h"
#include "shader/ShaderBuilder.h"
#include "shader/ShaderVariantCollector.h"
#include "shader/ShaderManifest.h"
#include "shader/ShaderVariantCompiler.h"
#include "SceneManager.h"
#include "PrefabManager.h"
#include "ObjectManager.h"
#include "script/ScriptRuntime.h"

using namespace jasperengine;

std::vector<Entity> editor::Project::getTopLevelEntities(const EntityRegistry* registry, const std::vector<Entity>& orderedEntities) {
    std::unordered_set<Entity> entitySet(orderedEntities.begin(), orderedEntities.end());
    std::vector<Entity> topLevelEntities;
    topLevelEntities.reserve(orderedEntities.size());

    for (Entity entity : orderedEntities) {
        if (registry->getSignature(entity).test(registry->getComponentId<Transform>())) {
            const Transform& transform = registry->getComponent<Transform>(entity);
            if (entitySet.find(transform.parent) != entitySet.end()) {
                continue;
            }
        }
        topLevelEntities.push_back(entity);
    }

    return topLevelEntities;
}

static bool scriptPropertyFloatEqual(float lhs, float rhs, bool useTolerance) {
    if (lhs == rhs) return true;
    if (std::isnan(lhs) || std::isnan(rhs)) return std::isnan(lhs) && std::isnan(rhs);
    return useTolerance && std::fabs(lhs - rhs) <= 1e-4f;
}

// A matching index only means both sides hold the same alternative, not that it is the
// one type asks for, so a leftover pair compares unequal instead of throwing
static bool scriptPropertyValuesEqual(ScriptPropertyType type, const ScriptPropertyValue& lhs, const ScriptPropertyValue& rhs, bool useTolerance = false) {
    if (lhs.index() != rhs.index()) {
        return false;
    }

    switch (type) {
        case ScriptPropertyType::Bool: {
            const bool* a = std::get_if<bool>(&lhs);
            const bool* b = std::get_if<bool>(&rhs);
            return a && b && *a == *b;
        }
        case ScriptPropertyType::Int: {
            const int* a = std::get_if<int>(&lhs);
            const int* b = std::get_if<int>(&rhs);
            return a && b && *a == *b;
        }
        case ScriptPropertyType::Float: {
            const float* a = std::get_if<float>(&lhs);
            const float* b = std::get_if<float>(&rhs);
            return a && b && scriptPropertyFloatEqual(*a, *b, false);
        }
        case ScriptPropertyType::String: {
            const std::string* a = std::get_if<std::string>(&lhs);
            const std::string* b = std::get_if<std::string>(&rhs);
            return a && b && *a == *b;
        }
        case ScriptPropertyType::Vector2: {
            const Vector2* a = std::get_if<Vector2>(&lhs);
            const Vector2* b = std::get_if<Vector2>(&rhs);
            return a && b &&
                scriptPropertyFloatEqual(a->x, b->x, useTolerance) &&
                scriptPropertyFloatEqual(a->y, b->y, useTolerance);
        }
        case ScriptPropertyType::Vector3:
        case ScriptPropertyType::Color3: {
            const Vector3* a = std::get_if<Vector3>(&lhs);
            const Vector3* b = std::get_if<Vector3>(&rhs);
            return a && b &&
                scriptPropertyFloatEqual(a->x, b->x, useTolerance) &&
                scriptPropertyFloatEqual(a->y, b->y, useTolerance) &&
                scriptPropertyFloatEqual(a->z, b->z, useTolerance);
        }
        case ScriptPropertyType::Vector4:
        case ScriptPropertyType::Color4: {
            const Vector4* a = std::get_if<Vector4>(&lhs);
            const Vector4* b = std::get_if<Vector4>(&rhs);
            return a && b &&
                scriptPropertyFloatEqual(a->x, b->x, useTolerance) &&
                scriptPropertyFloatEqual(a->y, b->y, useTolerance) &&
                scriptPropertyFloatEqual(a->z, b->z, useTolerance) &&
                scriptPropertyFloatEqual(a->w, b->w, useTolerance);
        }
        case ScriptPropertyType::EntityReference: {
            const EntityReference* a = std::get_if<EntityReference>(&lhs);
            const EntityReference* b = std::get_if<EntityReference>(&rhs);
            return a && b && a->entity == b->entity;
        }
        case ScriptPropertyType::Prefab: {
            const PrefabAsset* a = std::get_if<PrefabAsset>(&lhs);
            const PrefabAsset* b = std::get_if<PrefabAsset>(&rhs);
            return a && b && a->getGuid() == b->getGuid() && a->getPath() == b->getPath();
        }
        case ScriptPropertyType::Asset: {
            const AssetRef* a = std::get_if<AssetRef>(&lhs);
            const AssetRef* b = std::get_if<AssetRef>(&rhs);
            return a && b && *a == *b;
        }
    }

    return false;
}

static bool mergeScriptProperties(std::vector<ScriptProperty>& properties, const std::vector<ScriptProperty>& parsedProperties) {
    bool hasChanges = properties.size() != parsedProperties.size();
    std::vector<ScriptProperty> mergedProperties;
    mergedProperties.reserve(parsedProperties.size());

    for (size_t parsedIndex = 0; parsedIndex < parsedProperties.size(); ++parsedIndex) {
        const ScriptProperty& parsedProp = parsedProperties[parsedIndex];
        auto currentIt = std::find_if(properties.begin(), properties.end(),
            [&](const ScriptProperty& current) { return current.name == parsedProp.name; });

        if (currentIt == properties.end()) {
            hasChanges = true;
            mergedProperties.push_back(parsedProp);
            continue;
        }

        const size_t currentIndex = static_cast<size_t>(currentIt - properties.begin());
        const bool sameType = currentIt->type == parsedProp.type;
        const bool defaultChanged = sameType &&
            !scriptPropertyValuesEqual(currentIt->type, currentIt->defaultValue, parsedProp.defaultValue);
        const bool updateValue = !sameType || (defaultChanged &&
            scriptPropertyValuesEqual(currentIt->type, currentIt->value, currentIt->defaultValue, true));

        if (currentIndex != parsedIndex ||
            currentIt->displayName != parsedProp.displayName ||
            !sameType ||
            currentIt->ptrTypeName != parsedProp.ptrTypeName ||
            defaultChanged) {
            hasChanges = true;
        }

        ScriptProperty merged = *currentIt;
        merged.displayName = parsedProp.displayName;
        merged.type = parsedProp.type;
        merged.ptrTypeName = parsedProp.ptrTypeName;
        merged.defaultValue = parsedProp.defaultValue;

        if (updateValue) {
            merged.value = parsedProp.defaultValue;
        }

        mergedProperties.push_back(std::move(merged));
    }

    properties = std::move(mergedProperties);
    return hasChanges;
}

static std::vector<editor::ScriptPropertyInfo> toScriptPropertyInfos(const std::vector<ScriptProperty>& properties) {
    std::vector<editor::ScriptPropertyInfo> infos;
    infos.reserve(properties.size());

    for (const auto& prop : properties) {
        editor::ScriptPropertyInfo info;
        info.name = prop.name;
        info.isPtr = (prop.type == ScriptPropertyType::EntityReference) || !prop.ptrTypeName.empty();
        info.ptrTypeName = prop.ptrTypeName;
        infos.push_back(std::move(info));
    }

    return infos;
}

bool editor::Project::recordSelectionInUndo = true;

editor::Project::Project(): vcsService(std::make_unique<vcs::VcsService>()), sceneDiffs(std::make_unique<vcs::SceneDiffTracker>()){
    resetConfigs();
    installUndoHooks();
}

editor::Project::~Project(){
    // the poller and the worker read nothing of the project, but its backups go with it
    vcsService->close();
    removeVcsBackups();
    // the jobs capture the project (the asset walk its database)
    fbxImportScan.wait();
    waitForAssetScan();
    playStartupTask.wait();
    playStopTask.wait();
    sceneSaveTask.wait();
    // the hooks point here: an editor or tool with a later Project installs its own
    if (UndoTimeline::getProjectHistory() == &projectHistory){
        UndoTimeline::setProjectHistory(nullptr);
        UndoTimeline::selectedScene = nullptr;
        UndoTimeline::historyName = nullptr;
        CommandHistory::selectionHooks = CommandHistory::SelectionHooks();
        CommandHistory::beforeMutation = nullptr;
    }
}

void editor::Project::installUndoHooks(){
    UndoTimeline::setProjectHistory(&projectHistory);
    UndoTimeline::selectedScene = [this]() { return getSelectedSceneId(); };
    UndoTimeline::historyName = [this](uint32_t sceneId) -> std::string {
        if (sceneId == NULL_PROJECT_SCENE){
            return "Project";
        }
        const SceneProject* sceneProject = getScene(sceneId);
        return "Scene '" + (sceneProject ? sceneProject->name : std::to_string(sceneId)) + "'";
    };
    CommandHistory::selectionHooks.read = [this](size_t sceneId, SelectionState& state) {
        const uint32_t id = (uint32_t)sceneId;
        if (id == NULL_PROJECT_SCENE || !getScene(id) || isPlayRuntimeScene(id)){
            return false;
        }
        state = getSelection(id);
        return true;
    };
    CommandHistory::selectionHooks.restore = [this](size_t sceneId, const SelectionState& state) {
        SceneProject* sceneProject = getScene((uint32_t)sceneId);
        if (!sceneProject || !sceneProject->scene){
            return;
        }
        // the GameObjects that exist again
        SelectionState existing;
        for (Entity entity : state.entities){
            if (sceneProject->scene->isEntityCreated(entity)){
                existing.entities.push_back(entity);
            }
        }
        existing.active = existing.entities.empty() ? NULL_ENTITY : existing.entities.back();
        setSelection((uint32_t)sceneId, existing, SelectionCause::Restore);
    };
    // Play start/stop swaps the scene for its snapshot: no edit may land meanwhile
    CommandHistory::beforeMutation = [this](std::string& whyNot) {
        for (const SceneProject& sceneProject : scenes){
            switch (sceneProject.playState){
                case ScenePlayState::SAVING: whyNot = "saving before Play"; return false;
                case ScenePlayState::LOADING: whyNot = "Play mode is starting"; return false;
                case ScenePlayState::CANCELLING: whyNot = "Play mode is stopping"; return false;
                default: break;
            }
        }
        return true;
    };
}

fs::path editor::Project::normalizeToProjectRelative(const fs::path& path) const {
    if (path.empty()) {
        return {};
    }

    fs::path normalizedPath = path.lexically_normal();
    std::error_code ec;

    if (normalizedPath.is_absolute()) {
        fs::path relativePath = fs::relative(normalizedPath, projectPath, ec);
        if (!ec) {
            relativePath = relativePath.lexically_normal();
            // a link out of the project is a folder of the project: its files keep the path
            // they are reached by, not "../.." (S68 R7)
            if (!relativePath.empty() && *relativePath.begin() == "..") {
                const fs::path known = assetDatabase.toRelative(normalizedPath);
                if (!known.empty() && !known.is_absolute() && *known.begin() != "..") return known;
            }
            return relativePath;
        }
    }

    return normalizedPath;
}

bool editor::Project::matchesRelativePath(const fs::path& relativeBase, const fs::path& currentPath) {
    if (relativeBase.empty() || currentPath.empty()) {
        return false;
    }

    const std::string relativeBaseStr = relativeBase.lexically_normal().generic_string();
    const std::string currentPathStr = currentPath.lexically_normal().generic_string();
    const std::string relativePrefix = relativeBaseStr + "/";

    return currentPathStr == relativeBaseStr ||
           (!relativeBaseStr.empty() && currentPathStr.rfind(relativePrefix, 0) == 0);
}

bool editor::Project::matchesRelativeString(const fs::path& relativeBase, const std::string& currentPath) {
    if (relativeBase.empty() || currentPath.empty()) {
        return false;
    }

    return matchesRelativePath(relativeBase, fs::path(currentPath));
}

std::vector<editor::ChildSceneRef>::iterator editor::Project::findChildScene(std::vector<ChildSceneRef>& childScenes, uint32_t childSceneId) {
    return std::find_if(childScenes.begin(), childScenes.end(),
        [childSceneId](const ChildSceneRef& childScene) {
            return childScene.id == childSceneId;
        });
}

std::vector<editor::ChildSceneRef>::const_iterator editor::Project::findChildScene(const std::vector<ChildSceneRef>& childScenes, uint32_t childSceneId) {
    return std::find_if(childScenes.begin(), childScenes.end(),
        [childSceneId](const ChildSceneRef& childScene) {
            return childScene.id == childSceneId;
        });
}

bool editor::Project::eraseChildSceneReference(std::vector<ChildSceneRef>& childScenes, uint32_t childSceneId) {
    auto it = std::remove_if(childScenes.begin(), childScenes.end(),
        [childSceneId](const ChildSceneRef& childScene) {
            return childScene.id == childSceneId;
        });
    if (it == childScenes.end()) {
        return false;
    }

    childScenes.erase(it, childScenes.end());
    return true;
}

bool editor::Project::remapRelativePath(const fs::path& oldRelative, const fs::path& newRelative,
                                        const fs::path& currentPath, fs::path& updatedPath) {
    if (oldRelative.empty() || newRelative.empty() || currentPath.empty()) {
        return false;
    }

    const std::string oldRelativeStr = oldRelative.lexically_normal().generic_string();
    const std::string currentPathStr = currentPath.lexically_normal().generic_string();
    const bool isExactMatch = (currentPathStr == oldRelativeStr);
    const bool isChildMatch = matchesRelativePath(oldRelative, currentPath) && !isExactMatch;

    if (!isExactMatch && !isChildMatch) {
        return false;
    }

    std::string updated = newRelative.generic_string();
    if (isChildMatch) {
        updated += currentPathStr.substr(oldRelativeStr.size());
    }

    updatedPath = fs::path(updated).lexically_normal();
    return true;
}

bool editor::Project::remapRelativeString(const fs::path& oldRelative, const fs::path& newRelative,
                                          const std::string& currentPath, std::string& updatedPath) {
    fs::path updated;
    if (!remapRelativePath(oldRelative, newRelative, fs::path(currentPath), updated)) {
        return false;
    }

    updatedPath = updated.generic_string();
    return true;
}

bool editor::Project::visitAssetPathsInRegistry(EntityRegistry* registry, const std::function<bool(std::string&)>& transform) {
    if (!registry) {
        return false;
    }
    // the fields relative to the assets directory (textures, models, audio, fonts, documents)
    const bool refs = AssetRefSchema::forEachAssetRef(*registry, [&](AssetRef& ref, const AssetRefSite& site) {
        if (site.base != AssetPathBase::Assets || ref.path.empty()) {
            return;
        }
        std::string path = ref.path;
        if (transform(path)) {
            ref.path = path;
        }
    });
    const bool clips = visitClipPathsInRegistry(registry, transform);
    return refs || clips;
}

// The Animation Player clips are paths relative to the assets directory too: the .anim path
// (its GUID stays), or the model part of "<model>#<clip>" (R26). A reference the transform
// empties (its file deleted) is kept: the Inspector shows it missing, and undoing the delete
// relinks it (R8). (The Vertex Animators' .vat is a GuidSibling row of AssetRefSchema.)
bool editor::Project::visitClipPathsInRegistry(EntityRegistry* registry, const std::function<bool(std::string&)>& transform) {
    bool changed = false;
    auto players = registry->getComponentArray<AnimationPlayerComponent>();
    for (size_t i = 0; i < players->size(); i++) {
        AnimationPlayerComponent& player = players->getComponentFromIndex(i);
        bool playerChanged = false;
        for (AnimationPlayerClip& entry : player.clips) {
            if (entry.clip.empty()) {
                continue;
            }
            anim::SubClipReference reference;
            if (anim::parseSubClipReference(entry.clip, reference)) {
                std::string model = reference.model;
                if (transform(model) && !model.empty() && model != reference.model) {
                    entry.clip = anim::formatSubClipReference(model, reference.clip);
                    playerChanged = true;
                }
                continue;
            }
            std::string path = entry.clip;
            if (transform(path) && !path.empty() && path != entry.clip) {
                entry.clip = path;
                playerChanged = true;
            }
        }
        // layer masks: the path hint (the GUID stays); a deleted file keeps it
        auto follow = [&](std::string& reference) {
            std::string path = reference;
            if (!reference.empty() && transform(path) && !path.empty() && path != reference) {
                reference = path;
                playerChanged = true;
            }
        };
        for (AnimationPlayerLayer& layer : player.layers) {
            follow(layer.mask);
        }
        if (playerChanged) {
            player.needsLoad = true;
            changed = true;
        }
    }

    return changed;
}

SceneResources editor::Project::collectResourcesInRegistry(EntityRegistry* registry) {
    SceneResources resources;
    if (!registry) {
        return resources;
    }

    // alphaBorderAuto is part of the pool id, so it is set like the system drawing it does
    auto addTexture = [&resources](const Texture& texture, bool alphaBorderAuto) {
        if (texture.getPath(0).empty() || texture.isFramebuffer()) {
            return;
        }

        Texture file(texture);
        if (alphaBorderAuto) {
            file.setAlphaBorderAuto(true);
        }

        auto sameRender = [&file](const Texture& other) { return other.getRenderId() == file.getRenderId(); };
        if (std::none_of(resources.textures.begin(), resources.textures.end(), sameRender)) {
            resources.textures.push_back(file);
        }
    };

    auto addFile = [](std::vector<std::string>& files, const std::string& file) {
        if (!file.empty() && std::find(files.begin(), files.end(), file) == files.end()) {
            files.push_back(file);
        }
    };

    auto visitComponents = [&](auto* componentArray, auto&& visitComponent) {
        for (size_t i = 0; i < componentArray->size(); i++) {
            visitComponent(componentArray->getComponentFromIndex(i));
        }
    };

    auto meshes = registry->getComponentArray<MeshComponent>();
    for (size_t i = 0; i < meshes->size(); i++) {
        MeshComponent& mesh = meshes->getComponentFromIndex(i);
        Signature signature = registry->getSignature(meshes->getEntity(i));
        // see RenderSystem::loadMesh
        bool bordered = signature.test(registry->getComponentId<SpriteComponent>()) ||
                        signature.test(registry->getComponentId<TilemapComponent>()) ||
                        signature.test(registry->getComponentId<MeshPolygonComponent>());

        for (unsigned int s = 0; s < mesh.numSubmeshes; s++) {
            Material& material = mesh.submeshes[s].material;
            addTexture(material.baseColorTexture, bordered);
            addTexture(material.emissiveTexture, false);
            addTexture(material.metallicRoughnessTexture, false);
            addTexture(material.occlusionTexture, false);
            addTexture(material.normalTexture, false);
            for (const auto& shaderTexture : material.shaderTextures) {
                addTexture(shaderTexture.second, false);
            }
            for (const auto& shaderTexture : material.extraPass->shaderTextures) {
                addTexture(shaderTexture.second, false);
            }
        }
    }

    // shader library textures of the components without a material
    visitComponents(registry->getComponentArray<UIComponent>(), [&](UIComponent& ui) {
        for (const auto& shaderTexture : ui.shaderTextures) addTexture(shaderTexture.second, false);
    });
    visitComponents(registry->getComponentArray<PointsComponent>(), [&](PointsComponent& points) {
        for (const auto& shaderTexture : points.shaderTextures) addTexture(shaderTexture.second, false);
    });
    visitComponents(registry->getComponentArray<LineRendererComponent>(), [&](LineRendererComponent& lines) {
        for (const auto& shaderTexture : lines.shaderTextures) addTexture(shaderTexture.second, false);
    });
    visitComponents(registry->getComponentArray<TrailRendererComponent>(), [&](TrailRendererComponent& trail) {
        for (const auto& shaderTexture : trail.shaderTextures) addTexture(shaderTexture.second, false);
    });
    visitComponents(registry->getComponentArray<SkyboxComponent>(), [&](SkyboxComponent& sky) {
        for (const auto& shaderTexture : sky.shaderTextures) addTexture(shaderTexture.second, false);
    });

    visitComponents(registry->getComponentArray<UIComponent>(), [&](UIComponent& ui) {
        addTexture(ui.texture, true);
    });

    visitComponents(registry->getComponentArray<ButtonComponent>(), [&](ButtonComponent& button) {
        addTexture(button.textureNormal, true);
        addTexture(button.textureHovered, true);
        addTexture(button.texturePressed, true);
        addTexture(button.textureDisabled, true);
    });

    visitComponents(registry->getComponentArray<SkyboxComponent>(), [&](SkyboxComponent& sky) {
        addTexture(sky.texture, false);
    });

    visitComponents(registry->getComponentArray<TerrainComponent>(), [&](TerrainComponent& terrain) {
        addTexture(terrain.heightMap, false);
        for (Texture& blendMap : terrain.blendMaps) {
            addTexture(blendMap, false);
        }
        for (TerrainSurfaceLayer& layer : terrain.surfaceLayers) {
            forEachTerrainLayerTexture(layer, [&](Texture& texture) {
                addTexture(texture, false);
            });
        }
        for (TerrainFoliageLayer& layer : terrain.foliageLayers) {
            addTexture(layer.densityMap, false);
            addFile(resources.models, layer.meshPath);
        }
    });

    visitComponents(registry->getComponentArray<LightComponent>(), [&](LightComponent& light) {
        addTexture(light.spotMask, false);
    });

    visitComponents(registry->getComponentArray<PointsComponent>(), [&](PointsComponent& points) {
        addTexture(points.texture, true);
    });

    visitComponents(registry->getComponentArray<ReflectionProbeComponent>(), [&](ReflectionProbeComponent& probe) {
        addTexture(probe.texture, false);
    });

    visitComponents(registry->getComponentArray<ModelComponent>(), [&](ModelComponent& model) {
        addFile(resources.models, model.filename);
    });

    visitComponents(registry->getComponentArray<AudioSourceComponent>(), [&](AudioSourceComponent& sound) {
        // the source's Load Type rides along (SoundPool::parseClipRef): Auto is the plain file
        if (sound.filename.empty() || sound.loadType == AudioLoadType::Auto) {
            addFile(resources.sounds, sound.filename);
        } else {
            addFile(resources.sounds, sound.filename + "#" + SoundPool::loadTypeName(sound.loadType));
        }
    });

    // Animation Player clips (AnimationClipPool); a model sub-clip is built once its model is
    // loaded, so the model is preloaded too
    visitComponents(registry->getComponentArray<AnimationPlayerComponent>(), [&](AnimationPlayerComponent& player) {
        auto addClip = [&](const std::string& clip) {
            anim::SubClipReference reference;
            if (anim::parseSubClipReference(clip, reference)) {
                addFile(resources.models, reference.model);
            }
            addFile(resources.clips, clip);
        };
        for (const AnimationPlayerClip& entry : player.clips) {
            addClip(entry.clip);
        }
        for (const AnimationPlayerLayer& layer : player.layers) {
            addFile(resources.masks, layer.mask);
        }
    });

    // Animator Controllers (AnimatorAssetPool): the controller, its clips and its layers' masks
    // (read through the engine's file API, as the build will: the pool caches it)
    visitComponents(registry->getComponentArray<AnimatorComponent>(), [&](AnimatorComponent& animator) {
        if (animator.controller.path.empty()) return;
        addFile(resources.controllers, animator.controller.path);
        std::shared_ptr<const anim::CompiledController> controller = AnimatorAssetPool::controller(animator.controller.path);
        if (!controller) return;
        for (const anim::CompiledLayer& layer : controller->layers) {
            addFile(resources.masks, layer.mask.path);
        }
        for (const anim::AnimatorAssetRef& clip : controller->clips) {
            anim::SubClipReference reference;
            if (anim::parseSubClipReference(clip.path, reference)) {
                addFile(resources.models, reference.model);
            }
            addFile(resources.clips, clip.path);
        }
    });

    // Vertex Animations (VertexAnimationPool); the .vat.glb is the Model's, in `models`
    visitComponents(registry->getComponentArray<VertexAnimatorComponent>(), [&](VertexAnimatorComponent& animator) {
        addFile(resources.vats, animator.animation);
    });

    // UIDocumentComponent: nothing to preload. RmlUi reads a document and its stylesheets when
    // it loads it, and its images through RmlRenderer::LoadTexture, not the texture pool

    return resources;
}

bool editor::Project::visitLuaPathsInRegistry(EntityRegistry* registry, const std::function<bool(std::string&)>& transform) {
    if (!registry) {
        return false;
    }
    return AssetRefSchema::forEachAssetRef(*registry, [&](AssetRef& ref, const AssetRefSite& site) {
        if (site.base != AssetPathBase::Lua || ref.path.empty()) {
            return;
        }
        std::string path = ref.path;
        if (transform(path)) {
            ref.path = path;
        }
    });
}

bool editor::Project::detachChildSceneFromParents(uint32_t childSceneId, const std::set<uint32_t>& skippedSceneIds) {
    bool changed = false;

    for (auto& sceneProject : scenes) {
        if (sceneProject.id == childSceneId || skippedSceneIds.find(sceneProject.id) != skippedSceneIds.end()) {
            continue;
        }

        if (!eraseChildSceneReference(sceneProject.childScenes, childSceneId)) {
            continue;
        }

        if (sceneProject.scene) {
            sceneProject.needUpdateRender = true;
            sceneProject.isModified = true;
        }

        changed = true;
    }

    if (changed) {
        // Dropped an inline child, so the Engine layers need rebuilding.
        editor::getEditorHost().resetLastActivatedScene();
    }

    return changed;
}

bool editor::Project::removeMissingChildSceneReferences(SceneProject& sceneProject) {
    bool changed = false;

    for (auto it = sceneProject.childScenes.begin(); it != sceneProject.childScenes.end();) {
        uint32_t childSceneId = it->id;
        if (getScene(childSceneId)) {
            ++it;
            continue;
        }

        Out::warning("Scene '%s' references missing child scene ID %u. Removing child scene reference.",
            sceneProject.name.c_str(), childSceneId);
        it = sceneProject.childScenes.erase(it);
        changed = true;
    }

    if (changed && sceneProject.scene) {
        sceneProject.needUpdateRender = true;
        sceneProject.isModified = true;
    }

    return changed;
}

void editor::Project::linkMaterialFile(uint32_t sceneId, Entity entity, unsigned int submeshIndex, const std::string& filePath) {
    MaterialLinkKey key{sceneId, entity, submeshIndex};
    MaterialLinkEntry entry;
    entry.filePath = fs::path(filePath).lexically_normal().generic_string();
    materialFileLinks[key] = entry;
}

bool editor::Project::isMaterialFileLinked(uint32_t sceneId, Entity entity, unsigned int submeshIndex) const {
    return materialFileLinks.find(MaterialLinkKey{sceneId, entity, submeshIndex}) != materialFileLinks.end();
}

std::string editor::Project::getMaterialFilePath(uint32_t sceneId, Entity entity, unsigned int submeshIndex) const {
    auto it = materialFileLinks.find(MaterialLinkKey{sceneId, entity, submeshIndex});
    if (it != materialFileLinks.end()) {
        return it->second.filePath;
    }
    return {};
}

void editor::Project::unlinkMaterialFile(uint32_t sceneId, Entity entity, unsigned int submeshIndex) {
    materialFileLinks.erase(MaterialLinkKey{sceneId, entity, submeshIndex});
}

void editor::Project::unlinkAllMaterialFiles(uint32_t sceneId, Entity entity) {
    for (auto it = materialFileLinks.begin(); it != materialFileLinks.end();) {
        if (std::get<0>(it->first) == sceneId && std::get<1>(it->first) == entity) {
            it = materialFileLinks.erase(it);
        } else {
            ++it;
        }
    }
}

void editor::Project::unlinkAllMaterialFiles(uint32_t sceneId) {
    for (auto it = materialFileLinks.begin(); it != materialFileLinks.end();) {
        if (std::get<0>(it->first) == sceneId) {
            it = materialFileLinks.erase(it);
        } else {
            ++it;
        }
    }
}

void editor::Project::markReferencedSceneIdsPersisted(EntityRegistry* registry) {
    if (!registry) {
        return;
    }

    auto scriptsArray = registry->getComponentArray<ScriptComponent>();
    for (size_t i = 0; i < scriptsArray->size(); i++) {
        ScriptComponent& scriptComponent = scriptsArray->getComponentFromIndex(i);
        for (const auto& scriptEntry : scriptComponent.scripts) {
            for (const auto& property : scriptEntry.properties) {
                const EntityReference* ref = std::get_if<EntityReference>(&property.value);
                if (!ref || ref->sceneId == NULL_PROJECT_SCENE) {
                    continue;
                }
                if (SceneProject* referenced = getScene(ref->sceneId)) {
                    referenced->transientId = false;
                }
            }
        }
    }
}

bool editor::Project::clearEntityReferencesInScene(SceneProject& sceneProject, uint32_t sceneId) {
    if (!sceneProject.scene) {
        return false;
    }

    bool changed = false;
    auto scriptsArray = sceneProject.scene->getComponentArray<ScriptComponent>();
    for (size_t i = 0; i < scriptsArray->size(); i++) {
        ScriptComponent& scriptComponent = scriptsArray->getComponentFromIndex(i);
        for (auto& scriptEntry : scriptComponent.scripts) {
            for (auto& property : scriptEntry.properties) {
                EntityReference* ref = std::get_if<EntityReference>(&property.value);
                if (!ref || ref->sceneId != sceneId) {
                    continue;
                }
                *ref = EntityReference{NULL_ENTITY, 0};
                changed = true;
            }
        }
    }

    return changed;
}

void editor::Project::purgeReferencesToScene(uint32_t sceneId) {
    bool detached = false;

    for (auto& sceneProject : scenes) {
        if (sceneProject.id == sceneId) {
            continue;
        }

        bool unlinkedChild = eraseChildSceneReference(sceneProject.childScenes, sceneId);
        bool changed = clearEntityReferencesInScene(sceneProject, sceneId) || unlinkedChild;
        detached = detached || unlinkedChild;

        if (!changed) {
            continue;
        }

        sceneProject.isModified = true;
        if (sceneProject.scene) {
            sceneProject.needUpdateRender = true;
        }

        // A redo would otherwise reconnect whatever scene inherits the id
        CommandHandle::remove(sceneProject.id);

        Out::warning("Scene '%s' referenced the discarded scene, references and history cleared", sceneProject.name.c_str());
    }

    if (detached) {
        // Dropped an inline child, so the Engine layers need rebuilding.
        editor::getEditorHost().resetLastActivatedScene();
    }
}

bool editor::Project::moveSceneFileHints(const std::filesystem::path& oldPath, const std::filesystem::path& newPath) {
    if (projectPath.empty()) {
        return false;
    }

    fs::path oldRelative = normalizeToProjectRelative(oldPath);
    fs::path newRelative = normalizeToProjectRelative(newPath);

    if (oldRelative.empty() || newRelative.empty()) {
        return false;
    }

    bool changed = false;

    for (auto& sceneProject : scenes) {
        if (sceneProject.filepath.empty()) {
            continue;
        }

        fs::path updatedPath;
        if (remapRelativePath(oldRelative, newRelative, sceneProject.filepath, updatedPath)) {
            std::string oldStr = sceneProject.filepath.string();
            sceneProject.filepath = updatedPath;
            // Update corresponding tab entry
            for (auto& tab : tabs) {
                if (tab.type == TabType::SCENE && tab.filepath == oldStr) {
                    tab.filepath = updatedPath.string();
                }
            }
            changed = true;
        }
    }

    if (changed) {
        saveProject();
    }
    return changed;
}

std::vector<editor::SceneProject*> editor::Project::getHtmlUIScenes() {
    std::vector<SceneProject*> result;
    for (SceneProject& sceneProject : scenes) {
        if (sceneProject.scene) {
            result.push_back(&sceneProject);
        }
    }
    for (const std::unique_ptr<PrefabStage>& stage : prefabStages) {
        if (stage && stage->sceneProject.scene) {
            result.push_back(&stage->sceneProject);
        }
    }

    // the played scene runs in place; its child scenes run as copies
    std::scoped_lock lock(playSessionMutex);
    if (activePlaySession && !activePlaySession->cancelled.load(std::memory_order_acquire) &&
        activePlaySession->startupSucceeded.load(std::memory_order_acquire)) {
        for (const PlayRuntimeScene& entry : activePlaySession->runtimeScenes) {
            if (entry.ownedRuntime && entry.copy && entry.copy->scene &&
                std::find(result.begin(), result.end(), entry.copy) == result.end()) {
                result.push_back(entry.copy);
            }
        }
    }
    return result;
}

std::vector<std::string> editor::Project::getLinkedMaterialFiles() const {
    std::set<std::string> files;
    for (const auto& [key, entry] : materialFileLinks) {
        files.insert(entry.filePath);
    }
    return std::vector<std::string>(files.begin(), files.end());
}

void editor::Project::applyLinkedMaterialFile(const std::string& relativePath, const Material& fileMaterial) {
    const std::string filePath = fs::path(relativePath).lexically_normal().generic_string();
    Material updatedMaterial = fileMaterial;
    updatedMaterial.name = filePath;

    for (auto it = materialFileLinks.begin(); it != materialFileLinks.end();) {
        const auto [keySceneId, keyEntity, keySubmeshIndex] = it->first;
        SceneProject* sceneProject = getScene(keySceneId);
        MeshComponent* mesh = sceneProject && sceneProject->scene ? sceneProject->scene->findComponent<MeshComponent>(keyEntity) : nullptr;
        if (!mesh || keySubmeshIndex >= mesh->numSubmeshes) {
            it = materialFileLinks.erase(it);
            continue;
        }
        // the material no longer names the linked file (an undo, say): the link is gone
        const std::string currentName = fs::path(mesh->submeshes[keySubmeshIndex].material.name).lexically_normal().generic_string();
        if (currentName != it->second.filePath) {
            it = materialFileLinks.erase(it);
            continue;
        }
        if (it->second.filePath == filePath && mesh->submeshes[keySubmeshIndex].material != updatedMaterial) {
            // a new shader, features or render state reload the mesh, parameters its uniforms
            const uint64_t updateFlags = Catalog::getMaterialUpdateFlags(mesh->submeshes[keySubmeshIndex].material, updatedMaterial);
            mesh->submeshes[keySubmeshIndex].material = updatedMaterial;
            mesh->submeshes[keySubmeshIndex].needUpdateTexture = true;
            Catalog::updateEntity(sceneProject->scene, keyEntity, updateFlags);
            sceneProject->needUpdateRender = true;
            sceneProject->isModified = true;
        }
        ++it;
    }
}

// Imports an FBX when its .glb is missing or stale, or always when forced; without canReplace only
// a missing one is created. False when there is no .glb to load.
static bool importFbx(const fs::path& fbxPath, const fs::path& glbPath, const fs::path& assetsRoot, bool force, bool canReplace, bool& written) {
    written = false;
    const std::string fbxName = editor::FileUtils::pathToUtf8(fbxPath.filename());

    const editor::FbxImporter::State state = editor::FbxImporter::getState(fbxPath, glbPath, assetsRoot);
    if (state == editor::FbxImporter::State::UpToDate && !force) {
        return true;
    }
    if (!canReplace && state != editor::FbxImporter::State::Missing) {
        if (force) {
            editor::Out::warning("Cannot reimport %s while a project export is running", fbxName.c_str());
        }
        return true;
    }

    editor::Out::info("Importing %s", fbxName.c_str());
    // the converted glTF goes to the editor's import cache (.jasperengine/imported), which an MCP
    // call's deny-by-default guard refuses: the editor names the one file it writes (util/SafeWrite.h)
    jasperengine::editor::safewrite::EditorInternal ownImport(glbPath);
    const editor::FbxImporter::Result result = editor::FbxImporter::import(fbxPath, glbPath, assetsRoot, force);
    for (const std::string& warning : result.warnings) {
        editor::Out::warning("%s: %s", fbxName.c_str(), warning.c_str());
    }
    if (!result.success) {
        editor::Out::error("Cannot import %s: %s", fbxName.c_str(), result.error.c_str());
        // A stale import still loads
        return state != editor::FbxImporter::State::Missing;
    }
    written = true;
    return true;
}

fs::path editor::Project::getImportedModelPath(const fs::path& fbxPath) const {
    return FileUtils::pathFromUtf8(MeshSystem::getImportedModelPath(FileUtils::pathToUtf8(fbxPath)));
}

bool editor::Project::importFbxModel(const fs::path& fbxPath, bool force) {
    // The import mirrors the assets directory
    if (!isInsideAssetsPath(fbxPath)) {
        Out::error("Cannot import %s: it is outside the project assets", FileUtils::pathToUtf8(fbxPath.filename()).c_str());
        return false;
    }
    std::lock_guard<std::mutex> callLock(fbxImportCallMutex);
    bool exported;
    {
        std::lock_guard<std::mutex> lock(fbxImportMutex);
        exported = exportedImports.count(fbxPath.lexically_normal()) > 0;
    }
    bool written = false;
    const bool loadable = importFbx(fbxPath, getImportedModelPath(fbxPath), getAssetsPath(), force, !exported, written);
    if (written) {
        queueModelReload(fbxPath);
    }
    return loadable;
}

bool editor::Project::prepareModelFile(const std::string& modelPath) {
    return !FbxImporter::isFbxFile(modelPath) || importFbxModel(resolveAssetPath(FileUtils::pathFromUtf8(modelPath)));
}

void editor::Project::queueModelReload(const fs::path& fbxPath) {
    std::lock_guard<std::mutex> lock(fbxImportMutex);
    modelReloads.insert(fbxPath.lexically_normal());
}

bool editor::Project::reloadModelFile(const fs::path& fbxPath, bool synchronous) {
    const std::string filename = FileUtils::pathToUtf8(fbxPath);
    const std::string key = MeshSystem::getModelFilenameKey(filename);
    const fs::path glbPath = getImportedModelPath(fbxPath);

    // A parsed model of another import reloads to keep its part arrangement, unloaded ones read the
    // new file anyway. A load in flight would put the old file back in the pool, so the reload waits.
    std::vector<std::pair<SceneProject*, std::vector<Entity>>> reloads;
    for (SceneProject& sceneProject : scenes) {
        if (!sceneProject.scene) {
            continue;
        }
        std::shared_ptr<MeshSystem> meshSystem = sceneProject.scene->getSystem<MeshSystem>();
        const std::unordered_set<Entity> sceneEntities(sceneProject.entities.begin(), sceneProject.entities.end());
        std::vector<Entity> users;
        auto models = sceneProject.scene->getComponentArray<ModelComponent>();
        for (size_t i = 0; i < models->size(); i++) {
            const ModelComponent& model = models->getComponentFromIndex(i);
            const Entity entity = models->getEntity(i);
            if (model.filename.empty() || MeshSystem::getModelFilenameKey(model.filename) != key) {
                continue;
            }
            if (meshSystem->isAsyncModelLoadPending(entity, model.filename)) {
                return false;
            }
            if (model.gltfModel && sceneEntities.count(entity) && !FbxImporter::isCurrentImport(*model.gltfModel, glbPath)) {
                users.push_back(entity);
            }
        }
        reloads.emplace_back(&sceneProject, users);
    }

    MeshSystem::invalidateModelFile(filename);
    // its sub-clips (AnimationClipPool "<model>#*"), their documents and the players using them
    AnimationClipDocument::filesChanged({fbxPath.lexically_normal()}, this);
    const bool wasAsync = Engine::isAsyncLoading();
    if (synchronous) {
        Engine::setAsyncLoading(false);
    }
    for (const auto& [sceneProject, users] : reloads) {
        std::shared_ptr<MeshSystem> meshSystem = sceneProject->scene->getSystem<MeshSystem>();
        meshSystem->reloadFoliageMesh(filename);
        meshSystem->retryFailedModelLoads(filename);
        sceneProject->needUpdateRender = true;

        for (Entity entity : users) {
            const ModelComponent& model = sceneProject->scene->getComponent<ModelComponent>(entity);
            // Not a user edit: the redo steps stay (undoing it puts the parts of the old import back)
            CommandHandle::get(sceneProject->id)->insertCommand(
                new ModelLoadCmd(this, sceneProject->id, entity, model.filename, model.mergeStaticMeshes));
        }
    }
    Engine::setAsyncLoading(wasAsync);
    return true;
}

fs::path editor::Project::applyModelReloads(bool synchronous) {
    std::set<fs::path> reloads;
    {
        std::lock_guard<std::mutex> lock(fbxImportMutex);
        reloads.swap(modelReloads);
    }
    fs::path waiting;
    for (const fs::path& fbxPath : reloads) {
        if (!reloadModelFile(fbxPath, synchronous)) {
            queueModelReload(fbxPath);
            waiting = fbxPath;
        }
    }
    return waiting;
}

void editor::Project::importFbxModels() {
    std::error_code ec;
    for (watch::LinkedDirectoryIterator it(getAssetsPath(), fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        // Hidden, build, Recordings and EditorSettings directories hold no assets (S27 R31)
        if (it->is_directory(ec) && isNonAssetDir(it->path())) {
            it.disable_recursion_pending();
            continue;
        }
        if (FbxImporter::isFbxFile(it->path()) && it->is_regular_file(ec)) {
            importFbxModel(it->path());
        }
    }
}

void editor::Project::setFbxImportsPaused(bool paused) {
    fbxImportsPaused = paused;
    if (!paused) {
        std::lock_guard<std::mutex> lock(fbxImportMutex);
        exportedImports.clear();
    }
}

bool editor::Project::flushFbxReloads(std::string& error) {
    if (fbxImportScan.isValid()) {
        for (const auto& [fbxPath, failure] : fbxImportScan.get()) {
            failedFbxImports[fbxPath] = failure;
        }
        fbxImportScan.reset();
    }
    // An import started before the pause queues its reload when it returns
    {
        std::lock_guard<std::mutex> callLock(fbxImportCallMutex);
    }

    const fs::path waiting = applyModelReloads(true);
    if (!waiting.empty()) {
        error = "Cannot export: " + FileUtils::pathToUtf8(waiting.filename()) + " is still loading. Export again once it finishes.";
        return false;
    }
    return true;
}

std::map<fs::path, std::vector<const editor::SceneProject*>> editor::Project::collectSceneModelFiles() const {
    auto fbxPathOf = [this](const std::string& filename) -> fs::path {
        return FbxImporter::isFbxFile(filename) ? resolveAssetPath(FileUtils::pathFromUtf8(filename)).lexically_normal() : fs::path();
    };

    std::map<fs::path, std::vector<const SceneProject*>> files;
    for (const SceneProject& sceneProject : scenes) {
        if (!sceneProject.scene) {
            continue;
        }
        const std::unordered_set<Entity> sceneEntities(sceneProject.entities.begin(), sceneProject.entities.end());
        auto models = sceneProject.scene->getComponentArray<ModelComponent>();
        for (size_t i = 0; i < models->size(); i++) {
            const ModelComponent& model = models->getComponentFromIndex(i);
            const fs::path fbxPath = fbxPathOf(model.filename);
            if (fbxPath.empty() || !sceneEntities.count(models->getEntity(i))) {
                continue;
            }
            std::vector<const SceneProject*>& unloadedScenes = files[fbxPath];
            if (!model.gltfModel && (unloadedScenes.empty() || unloadedScenes.back() != &sceneProject)) {
                unloadedScenes.push_back(&sceneProject);
            }
        }
        // Foliage saves no model parts, so it never needs a reload
        auto terrains = sceneProject.scene->getComponentArray<TerrainComponent>();
        for (size_t i = 0; i < terrains->size(); i++) {
            for (const TerrainFoliageLayer& layer : terrains->getComponentFromIndex(i).foliageLayers) {
                const fs::path fbxPath = fbxPathOf(layer.meshPath);
                if (!fbxPath.empty()) {
                    files[fbxPath];
                }
            }
        }
    }
    return files;
}

// Records which import the saved parts of each FBX model come from, so an export can check
// a scene it cannot reload against the current FBX
void editor::Project::updateSceneImports(SceneProject* sceneProject) {
    if (!sceneProject->scene) {
        return;
    }
    const std::unordered_set<Entity> sceneEntities(sceneProject->entities.begin(), sceneProject->entities.end());
    // FBX -> the import its models were loaded from, empty when one is not loaded or they differ
    std::map<std::string, std::string> loaded;
    auto models = sceneProject->scene->getComponentArray<ModelComponent>();
    for (size_t i = 0; i < models->size(); i++) {
        const ModelComponent& model = models->getComponentFromIndex(i);
        if (!FbxImporter::isFbxFile(model.filename) || !sceneEntities.count(models->getEntity(i))) {
            continue;
        }
        const std::string key = normalizeToAssetsRelative(resolveAssetPath(FileUtils::pathFromUtf8(model.filename.path))).generic_string();
        const std::string id = model.gltfModel ? FbxImporter::getImportId(*model.gltfModel) : std::string();
        auto it = loaded.emplace(key, id).first;
        if (it->second != id) {
            it->second.clear();
        }
    }

    std::map<std::string, std::string> imports;
    for (const auto& [key, id] : loaded) {
        auto saved = sceneProject->imports.find(key);
        if (!id.empty()) {
            imports[key] = id;
        } else if (saved != sceneProject->imports.end()) {
            imports[key] = saved->second;
        }
    }
    sceneProject->imports = std::move(imports);
}

bool editor::Project::prepareFbxModelsForExport(std::string& error) {
    const fs::path assetsRoot = getAssetsPath();
    const auto files = collectSceneModelFiles();
    {
        std::lock_guard<std::mutex> lock(fbxImportMutex);
        for (const auto& entry : files) {
            exportedImports.insert(entry.first);
        }
    }

    for (const auto& [fbxPath, unloadedScenes] : files) {
        const std::string key = normalizeToAssetsRelative(fbxPath).generic_string();
        const std::string fbxName = FileUtils::pathToUtf8(fbxPath.filename());
        std::error_code ec;
        if (!fs::is_regular_file(fbxPath, ec)) {
            error = "Cannot export: " + key + " is missing";
            return false;
        }

        // A scene that cannot reload keeps the model parts saved with it, which must come from this FBX
        const std::string sourceId = FbxImporter::getSourceId(fbxPath);
        std::string outdatedScenes;
        for (const SceneProject* sceneProject : unloadedScenes) {
            auto saved = sceneProject->imports.find(key);
            if (saved == sceneProject->imports.end() || saved->second != sourceId) {
                outdatedScenes += (outdatedScenes.empty() ? "" : ", ") + sceneProject->name;
            }
        }
        if (!outdatedScenes.empty()) {
            error = "Cannot export: " + fbxName + " changed after these scenes were saved: " + outdatedScenes +
                ". Open them in the editor so their models reload, save them, then export again.";
            return false;
        }

        const fs::path glbPath = getImportedModelPath(fbxPath);
        if (FbxImporter::getState(fbxPath, glbPath, assetsRoot) != FbxImporter::State::UpToDate) {
            bool written = false;
            importFbx(fbxPath, glbPath, assetsRoot, false, true, written);
            if (!written) {
                error = "Cannot export: " + fbxName + " could not be imported";
                return false;
            }
            if (!reloadModelFile(fbxPath, true)) {
                queueModelReload(fbxPath);
                error = "Cannot export: " + fbxName + " is still loading. Export again once it finishes.";
                return false;
            }
        }
        for (const fs::path& missing : FbxImporter::findMissingLinkedFiles(fbxPath, glbPath)) {
            Out::warning("%s uses %s, which is missing, so it is exported without that texture",
                fbxName.c_str(), FileUtils::pathToUtf8(missing.filename()).c_str());
        }
    }
    return true;
}

void editor::Project::refreshFbxImports() {
    if (fbxImportScan.isReady()) {
        for (const auto& [fbxPath, failure] : fbxImportScan.get()) {
            failedFbxImports[fbxPath] = failure;
        }
        fbxImportScan.reset();
    }

    // A playing scene would restore its snapshot over the reload on Stop
    if (fbxImportsPaused || isAnyScenePlaying()) {
        return;
    }

    applyModelReloads(false);

    const auto now = std::chrono::steady_clock::now();
    if (fbxImportScan.isValid() || projectPath.empty() ||
        std::chrono::duration<double>(now - lastFbxImportCheck).count() < fbxImportCheckIntervalSec) {
        return;
    }
    lastFbxImportCheck = now;

    // FBX -> its .glb
    std::map<fs::path, fs::path> files;
    for (const auto& entry : collectSceneModelFiles()) {
        files[entry.first] = getImportedModelPath(entry.first);
    }
    if (files.empty()) {
        return;
    }

    // Hashing and converting can take seconds, so the check runs off the main thread: a
    // Background job (S06), Heavy (a conversion parses the whole model)
    jobs::SubmitOptions options;
    options.lane = jobs::Lane::Background;
    options.gate = jobs::GateClass::Heavy;
    options.name = "fbx-import-scan";
    fbxImportScan = jobs::submit(
        [this, files, assetsRoot = getAssetsPath(), known = failedFbxImports, writeRoot = safewrite::Scope::currentRoots(), writeStrict = safewrite::Scope::currentStrict()]() {
            // the conversions write in the write scope of the thread that started the scan
            safewrite::Scope scope(writeRoot, writeStrict);
            std::map<fs::path, FbxImportFailure> failures;
            for (const auto& [fbxPath, glbPath] : files) {
                std::error_code ec;
                FbxImportFailure stamp;
                stamp.size = fs::file_size(fbxPath, ec);
                if (ec) {
                    continue;
                }
                stamp.time = fs::last_write_time(fbxPath, ec);
                // A failed file is retried once it changes again
                auto failed = known.find(fbxPath);
                if (failed != known.end() && failed->second.size == stamp.size && failed->second.time == stamp.time) {
                    continue;
                }
                // A missing .glb (a fresh clone, or a cleared cache) is built like a changed one
                const FbxImporter::State state = FbxImporter::getState(fbxPath, glbPath, assetsRoot);
                if (state == FbxImporter::State::UpToDate) {
                    continue;
                }
                bool written = false;
                {
                    // One import of a file at a time, with importFbxModel (Play startup, a drop...)
                    std::lock_guard<std::mutex> callLock(fbxImportCallMutex);
                    importFbx(fbxPath, glbPath, assetsRoot, false, true, written);
                }
                if (written) {
                    queueModelReload(fbxPath);
                } else if (FbxImporter::getState(fbxPath, glbPath, assetsRoot) != FbxImporter::State::UpToDate) {
                    failures[fbxPath] = stamp;
                }
            }
            return failures;
        }, options);
}

editor::SceneRender* editor::Project::createSceneRender(SceneType type, Scene* scene) const {
    if (!scene) {
        return nullptr;
    }

    pauseEngineScene(scene, true);

    SceneRender* sceneRender = nullptr;
    switch (type) {
        case SceneType::SCENE_2D:
            sceneRender = new SceneRender2D(scene, canvasWidth, canvasHeight, false);
            break;
        case SceneType::SCENE_UI:
            sceneRender = new SceneRender2D(scene, canvasWidth, canvasHeight, true);
            break;
        case SceneType::SCENE_3D:
        default:
            sceneRender = new SceneRender3D(scene);
            break;
    }

    // the UI size of the scene's Canvas Scaler for the Game view (the canvas when it follows it)
    applyEditCanvasScale(scene, type, sceneRender);

    return sceneRender;
}

Entity editor::Project::createDefaultCamera(SceneType type, Scene* scene) const {
    if (!scene) {
        return NULL_ENTITY;
    }
    if (type == SceneType::SCENE_3D){
        return NULL_ENTITY; // 3D scenes use Camera entity created in SceneRender3D
    }

    Entity defaultCamera = scene->createSystemEntity();
    scene->addComponent<CameraComponent>(defaultCamera, {});
    scene->addComponent<Transform>(defaultCamera, {});

    CameraComponent& camera = scene->getComponent<CameraComponent>(defaultCamera);
    camera.transparentSort = false;
    camera.depthTest = false;

    switch (type) {
        case SceneType::SCENE_UI:
            camera.type = CameraType::CAMERA_UI;
            break;
        case SceneType::SCENE_2D:
            camera.type = CameraType::CAMERA_ORTHO;
            break;
        default:
            break;
    }

    Transform& cameratransform = scene->getComponent<Transform>(defaultCamera);
    cameratransform.position = Vector3(0.0, 0.0, 1.0);

    return defaultCamera;
}

void editor::Project::checkUnsavedAndExecute(uint32_t sceneId, std::function<void()> action) {
    SceneProject* sceneProject = getScene(sceneId);

    if (sceneProject && hasSceneUnsavedChanges(sceneId)) {
        editor::getEditorHost().registerThreeButtonAlert(
            "Unsaved Changes",
            "There are unsaved changes. Do you want to save first?",
            [this, sceneId, action]() {
                saveScene(sceneId, [action](bool success) {
                    if (success && action) action();
                });
            },
            [action]() {
                // No callback - execute action without saving
                if (action) action();
            },
            nullptr // Cancel: do nothing, keep scene open
        );
    } else {
        // No unsaved changes, execute action directly
        if (action) action();
    }
}

std::string editor::Project::getName() const {
    return name; 
}

void editor::Project::setName(std::string name){
    this->name = name;
    this->libName = Factory::toIdentifier(name);
    editor::getEditorHost().updateWindowTitle(name);
}

void editor::Project::setCanvasSize(unsigned int width, unsigned int height){
    this->canvasWidth = width;
    this->canvasHeight = height;

    for (SceneProject* sceneProjectPtr : getScenesAndPrefabStages()) {
        SceneProject& sceneProject = *sceneProjectPtr;
        if (!sceneProject.scene) {
            continue;
        }

        if (sceneProject.playState == ScenePlayState::STOPPED) {
            applyEditCanvasScale(sceneProject.scene, sceneProject.sceneType, sceneProject.sceneRender);
        } else if (sceneProject.sceneRender && sceneProject.sceneType != SceneType::SCENE_3D) {
            static_cast<SceneRender2D*>(sceneProject.sceneRender)->setCanvasFrameSize(width, height);
        }

        sceneProject.needUpdateRender = true;
    }
}

unsigned int editor::Project::getCanvasWidth() const{
    return canvasWidth;
}

unsigned int editor::Project::getCanvasHeight() const{
    return canvasHeight;
}

void editor::Project::setScalingMode(Scaling scalingMode){
    this->scalingMode = scalingMode;
}

Scaling editor::Project::getScalingMode() const{
    return scalingMode;
}

void editor::Project::setUIScale(const CanvasScalerSettings& uiScale){
    this->uiScale = uiScale;
    // the Engine's default is what edit-mode scenes on the project default resolve to as well
    Engine::setDefaultCanvasScaler(uiScale);
    applyEditCanvasScaleToAll();
}

const CanvasScalerSettings& editor::Project::getUIScale() const{
    return uiScale;
}

void editor::Project::setEditViewSize(float width, float height, float dpi){
    if (width == editViewWidth && height == editViewHeight && dpi == editViewDpi)
        return;
    editViewWidth = width;
    editViewHeight = height;
    editViewDpi = dpi;
    applyEditCanvasScaleToAll();
}

void editor::Project::getEditViewSize(float& width, float& height, float& dpi) const{
    width = (editViewWidth > 0.0f) ? editViewWidth : (float)canvasWidth;
    height = (editViewHeight > 0.0f) ? editViewHeight : (float)canvasHeight;
    dpi = editViewDpi;
}

void editor::Project::applyEditCanvasScale(Scene* scene, SceneType type, SceneRender* sceneRender) const{
    if (!scene)
        return;

    // the Canvas Scaler as the Game view's game would compute it: its view and the game
    // canvas the scaling mode gives that view. Edit mode lays out the project canvas, as before,
    // unless the scaler changes the UI size.
    float viewWidth, viewHeight, dpi;
    getEditViewSize(viewWidth, viewHeight, dpi);
    int gameCanvasWidth = (int)canvasWidth, gameCanvasHeight = (int)canvasHeight;
    Rect viewRect;
    Engine::computeViewLayout((int)std::lround(viewWidth), (int)std::lround(viewHeight), (int)canvasWidth, (int)canvasHeight, scalingMode,
                              gameCanvasWidth, gameCanvasHeight, viewRect);
    scene->updateCanvasScale(viewRect.getWidth(), viewRect.getHeight(), (float)gameCanvasWidth, (float)gameCanvasHeight, dpi);

    const CanvasScalerResult& scale = scene->getCanvasScale();
    const bool uiScaled = scale.scaled && type == SceneType::SCENE_UI;
    const float frameWidth = uiScaled ? scale.uiWidth : (float)canvasWidth;
    const float frameHeight = uiScaled ? scale.uiHeight : (float)canvasHeight;

    scene->getSystem<UISystem>()->setAnchorReferenceSize(frameWidth, frameHeight);

    UIDocumentSystem* documents = scene->getSystem<UIDocumentSystem>().get();
    if (scale.scaled){
        documents->setReferenceSize(std::max(1, (int)std::lround(scale.uiWidth)), std::max(1, (int)std::lround(scale.uiHeight)));
        documents->setFrameSize(frameWidth, frameHeight);
    }else{
        documents->setReferenceSize((int)canvasWidth, (int)canvasHeight);
        documents->setFrameSize(0.0f, 0.0f);
    }

    if (sceneRender && type != SceneType::SCENE_3D){
        static_cast<SceneRender2D*>(sceneRender)->setCanvasFrameSize((unsigned int)std::lround(frameWidth), (unsigned int)std::lround(frameHeight));
    }
}

void editor::Project::applyEditCanvasScaleToAll(){
    for (SceneProject* sceneProjectPtr : getScenesAndPrefabStages()) {
        SceneProject& sceneProject = *sceneProjectPtr;
        // a playing scene lays out in the Engine's view
        if (!sceneProject.scene || sceneProject.playState != ScenePlayState::STOPPED) {
            continue;
        }
        applyEditCanvasScale(sceneProject.scene, sceneProject.sceneType, sceneProject.sceneRender);
        sceneProject.needUpdateRender = true;
    }
}

void editor::Project::setTextureStrategy(TextureStrategy textureStrategy){
    const bool changed = this->textureStrategy != textureStrategy;
    this->textureStrategy = textureStrategy;
    // FIT / RESIZE apply at cook time (S14 R31): a key input
    if (changed && TextureCookQueue::get().getProject() == this){
        TextureCookQueue::get().projectSettingsChanged();
    }
}

TextureStrategy editor::Project::getTextureStrategy() const{
    return textureStrategy;
}

void editor::Project::setVSyncEnabled(bool enabled){
    this->vsyncEnabled = enabled;
}

bool editor::Project::isVSyncEnabled() const{
    return vsyncEnabled;
}

void editor::Project::setAtmosphereQuality(const std::array<AtmosphereQuality, 3>& quality){
    atmosphereQuality = quality;
    Engine::setAtmosphereQualityDefaults(quality[0], quality[1], quality[2]);
}

const std::array<AtmosphereQuality, 3>& editor::Project::getAtmosphereQuality() const{
    return atmosphereQuality;
}

void editor::Project::setPlayerLogging(const PlayerLoggingSettings& settings){
    playerLogging = settings;
}

const editor::PlayerLoggingSettings& editor::Project::getPlayerLogging() const{
    return playerLogging;
}

void editor::Project::setInputActionsAsset(const std::filesystem::path& asset){
    inputActionsAsset = asset;
}

std::filesystem::path& editor::Project::getInputActionsAsset(){
    return inputActionsAsset;
}

const std::filesystem::path& editor::Project::getInputActionsAsset() const{
    return inputActionsAsset;
}

std::string editor::Project::getInputActionsEnginePath() const{
    if (inputActionsAsset.empty())
        return std::string();
    const std::filesystem::path absolute = (getProjectPath() / inputActionsAsset).lexically_normal();
    const std::filesystem::path relative = absolute.lexically_relative(getAssetsPath().lexically_normal());
    if (relative.empty() || *relative.begin() == "..")
        return std::string();
    return relative.generic_string();
}

void editor::Project::setInputDeadzone(float deadzone){
    inputDeadzone = std::clamp(deadzone, 0.0f, 0.95f);
    InputActions::setDefaultDeadzone(inputDeadzone);
}

float editor::Project::getInputDeadzone() const{
    return inputDeadzone;
}

void editor::Project::setPlayerCrash(const PlayerCrashSettings& settings){
    playerCrash = settings;
    playerCrash.hangSeconds = std::clamp(settings.hangSeconds, PlayerCrashSettings::minHangSeconds, PlayerCrashSettings::maxHangSeconds);
}

const editor::PlayerCrashSettings& editor::Project::getPlayerCrash() const{
    return playerCrash;
}

bool editor::Project::isValidHtmlUiMsaaSamples(int samples){
    return samples == 1 || samples == 2 || samples == 4 || samples == 8;
}

void editor::Project::setHtmlUiMsaaSamples(int samples){
    int valid = 1;
    while (valid * 2 <= samples && valid < 8){
        valid *= 2;
    }
    this->htmlUiMsaaSamples = valid;
    // documents already drawn pick it up when their target is created again
    UIDocumentSystem::setPreferredSampleCount(valid);
}

int editor::Project::getHtmlUiMsaaSamples() const{
    return htmlUiMsaaSamples;
}

bool editor::Project::isValidHtmlUiGlyphAtlasMiB(int mib){
    return mib == 0 || (mib >= minHtmlUiGlyphAtlasMiB && mib <= maxHtmlUiGlyphAtlasMiB);
}

void editor::Project::setHtmlUiRenderSettings(const HtmlUiRenderSettings& settings){
    htmlUiRender = settings;
    if (!isValidHtmlUiGlyphAtlasMiB(htmlUiRender.glyphAtlasMiB))
        htmlUiRender.glyphAtlasMiB = std::clamp(htmlUiRender.glyphAtlasMiB, minHtmlUiGlyphAtlasMiB, maxHtmlUiGlyphAtlasMiB);
    // the editor's documents follow at once
    UIDocumentSystem::setHtmlUiRenderOptions(htmlUiRender.glyphAtlasMiB, htmlUiRender.sdfPrimitives, htmlUiRender.transformAwareText);
}

const editor::HtmlUiRenderSettings& editor::Project::getHtmlUiRenderSettings() const{
    return htmlUiRender;
}

void editor::Project::setShadowTier(int platform, ShadowTier tier){
    if (platform < 0 || platform >= ShadowTiers::PLATFORM_COUNT)
        return;
    shadowTiers[platform] = tier;
    // the editor renders with the tiers at once (RenderSystem resolves them every update)
    ShadowTiers::setPlatformTier(platform, tier);
}

ShadowTier editor::Project::getShadowTier(int platform) const{
    if (platform < 0 || platform >= ShadowTiers::PLATFORM_COUNT)
        return ShadowTier::HIGH;
    return shadowTiers[platform];
}

const char* editor::Project::getShadowTierPlatformName(int platform){
    // Engine::Platform order
    static const char* const names[ShadowTiers::PLATFORM_COUNT] = {"macos", "ios", "web", "android", "linux", "windows"};
    return (platform >= 0 && platform < ShadowTiers::PLATFORM_COUNT) ? names[platform] : "";
}

int editor::Project::findShadowTierPlatform(const std::string& name){
    for (int platform = 0; platform < ShadowTiers::PLATFORM_COUNT; platform++){
        if (name == getShadowTierPlatformName(platform))
            return platform;
    }
    return -1;
}

void editor::Project::setWindowMode(WindowMode windowMode){
    this->windowMode = windowMode;
}

editor::WindowMode editor::Project::getWindowMode() const{
    return windowMode;
}

void editor::Project::setWindowSize(unsigned int width, unsigned int height){
    this->windowWidth = width;
    this->windowHeight = height;
}

unsigned int editor::Project::getWindowWidth() const{
    return windowWidth;
}

unsigned int editor::Project::getWindowHeight() const{
    return windowHeight;
}

void editor::Project::setWindowResizable(bool resizable){
    this->windowResizable = resizable;
}

bool editor::Project::isWindowResizable() const{
    return windowResizable;
}

void editor::Project::setWindowTitle(const std::string& title){
    this->windowTitle = title;
}

std::string editor::Project::getWindowTitle() const{
    return windowTitle;
}

void editor::Project::setWindowIcon(const std::filesystem::path& iconPath){
    this->windowIcon = iconPath;
}

std::filesystem::path editor::Project::getWindowIcon() const{
    return windowIcon;
}

editor::WindowSettings editor::Project::getWindowSettings() const{
    WindowSettings settings;
    settings.mode = windowMode;
    settings.width = windowWidth;
    settings.height = windowHeight;
    settings.resizable = windowResizable;
    if (!windowTitle.empty()) {
        settings.title = windowTitle;
    } else if (!name.empty()) {
        settings.title = name;
    }
    // The title is embedded in generated C++ and CMake string literals whose
    // escaping only covers quotes/backslashes; a control character (e.g. a
    // newline from a hand-edited project.yaml) would still break the literal.
    // Unsigned compare so UTF-8 continuation bytes are left untouched.
    for (char& c : settings.title) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7F) c = ' ';
    }
    return settings;
}

void editor::Project::setAssetsDir(const std::filesystem::path& assetsDir){
    this->assetsDir = assetsDir;
    refreshTrustedRoots();   // resolved now, when the setting is applied
}

std::vector<std::filesystem::path> editor::Project::getTrustedRoots() const{
    const std::shared_ptr<const std::vector<std::filesystem::path>> snapshot = std::atomic_load(&trustedRoots);
    return snapshot ? *snapshot : std::vector<std::filesystem::path>();
}

void editor::Project::refreshTrustedRoots(){
    auto resolvedRoots = std::make_shared<std::vector<std::filesystem::path>>();
    std::vector<AssetDatabase::PinnedRoot> pinned;
    if (!projectPath.empty()){
        for (const std::filesystem::path& root : {projectPath, getAssetsPath(), getLuaPath()}){
            const std::filesystem::path resolved = safewrite::resolvedPath(root);
            if (resolved.empty()) continue;
            pinned.push_back({root.lexically_normal(), resolved});
            if (std::find(resolvedRoots->begin(), resolvedRoots->end(), resolved) != resolvedRoots->end()) continue;
            resolvedRoots->push_back(resolved);
            safewrite::pinTrustedRoot(resolved);
        }
    }
    assetDatabase.setPinnedRoots(std::move(pinned), projectPath.empty() ? std::filesystem::path() : getLuaPath().lexically_normal());
    std::atomic_store(&trustedRoots, std::shared_ptr<const std::vector<std::filesystem::path>>(std::move(resolvedRoots)));
}

std::filesystem::path editor::Project::getAssetsDir() const{
    return assetsDir;
}

void editor::Project::setLuaDir(const std::filesystem::path& luaDir){
    this->luaDir = luaDir;
    refreshTrustedRoots();   // resolved now, when the setting is applied
}

std::filesystem::path editor::Project::getLuaDir() const{
    return luaDir;
}

void editor::Project::setScriptDirs(std::vector<std::filesystem::path> scriptDirs){
    this->scriptDirs = std::move(scriptDirs);
}

const std::vector<std::filesystem::path>& editor::Project::getScriptDirs() const{
    return scriptDirs;
}

std::vector<std::filesystem::path> editor::Project::getBuildScriptDirs() const{
    std::vector<std::filesystem::path> dirs = scriptDirs;
    const std::filesystem::path nativeCpp = NativePlugins::relativeRoot() / "cpp";
    std::error_code ec;
    if (!std::filesystem::is_directory(getProjectPath() / nativeCpp, ec)) return dirs;
    const std::filesystem::path nativeAbsolute = (getProjectPath() / nativeCpp).lexically_normal();
    for (const std::filesystem::path& dir : dirs) {
        const std::filesystem::path absolute = (dir.is_absolute() ? dir : getProjectPath() / dir).lexically_normal();
        // a user root that is (or holds) the folder compiles it already
        const std::filesystem::path inside = nativeAbsolute.lexically_relative(absolute);
        if (!inside.empty() && *inside.begin() != "..") return dirs;
    }
    dirs.push_back(nativeCpp);
    return dirs;
}

bool editor::Project::hasNativeCppPlugins() const{
    return !NativePlugins::scan(getProjectPath()).filesWith("cpp", {".cpp", ".cc", ".cxx", ".c"}).empty();
}

void editor::Project::setCxxStandard(int standard){
    this->cxxStandard = sanitizeCxxStandard(standard);
}

int editor::Project::getCxxStandard() const{
    return cxxStandard;
}

void editor::Project::setPhysics2DEnabled(bool enabled){
    this->physics2DEnabled = enabled;
}

bool editor::Project::isPhysics2DEnabled() const{
    return physics2DEnabled;
}

void editor::Project::setPhysics3DEnabled(bool enabled){
    this->physics3DEnabled = enabled;
}

bool editor::Project::isPhysics3DEnabled() const{
    return physics3DEnabled;
}

void editor::Project::setRuntimeShadersEnabled(bool enabled){
    this->runtimeShadersEnabled = enabled;
}

bool editor::Project::isRuntimeShadersEnabled() const{
    return runtimeShadersEnabled;
}

void editor::Project::setMobilePbrTier(PbrTier tier){
    this->mobilePbrTier = tier;
}

PbrTier editor::Project::getMobilePbrTier() const{
    return mobilePbrTier;
}

void editor::Project::setTagsAndLayers(const std::vector<std::string>& tags, const std::vector<std::string>& layerNames){
    TagManager::setTags(tags);
    TagManager::setLayerNames(layerNames);

    this->tags = TagManager::getTags();
    this->layerNames.assign(MAX_LAYERS, std::string());
    for (int layer = 0; layer < MAX_LAYERS; layer++){
        if (!TagManager::isBuiltinLayer(layer)){
            this->layerNames[layer] = TagManager::layerToName(layer);
        }
    }
}

const std::vector<std::string>& editor::Project::getTags() const{
    return tags;
}

const std::vector<std::string>& editor::Project::getLayerNames() const{
    return layerNames;
}

static editor::LayerCollisionRows symmetricLayerCollisions(const editor::LayerCollisionRows& rows){
    editor::LayerCollisionRows result;
    for (int layer = 0; layer < MAX_LAYERS; layer++){
        uint32_t row = 0;
        for (int other = 0; other < MAX_LAYERS; other++){
            if (((rows[layer] >> other) & 1u) && ((rows[other] >> layer) & 1u)){
                row |= 1u << other;
            }
        }
        result[layer] = row;
    }
    return result;
}

void editor::Project::setLayerCollisionMatrices(const LayerCollisionRows& matrix3D, const LayerCollisionRows& matrix2D){
    layerCollisionMatrix3D = symmetricLayerCollisions(matrix3D);
    layerCollisionMatrix2D = symmetricLayerCollisions(matrix2D);
    applyLayerCollisionMatrices();
}

const editor::LayerCollisionRows& editor::Project::getLayerCollisionMatrix3D() const{
    return layerCollisionMatrix3D;
}

const editor::LayerCollisionRows& editor::Project::getLayerCollisionMatrix2D() const{
    return layerCollisionMatrix2D;
}

void editor::Project::applyLayerCollisionMatrices() const{
    Physics::getLayerCollisionMatrix().setRows(layerCollisionMatrix3D.data());
    Physics2D::getLayerCollisionMatrix().setRows(layerCollisionMatrix2D.data());
}

void editor::Project::setPhysics3DWorldSettings(const Physics3DWorldSettings& settings){
    physics3DWorld = settings;
    PhysicsSystem::setWorldCapacity3D(settings.maxBodies, settings.maxBodyPairs, settings.maxContactConstraints);
    // what the engine kept (clamped)
    physics3DWorld.maxBodies = PhysicsSystem::getDefaultMaxBodies3D();
    physics3DWorld.maxBodyPairs = PhysicsSystem::getDefaultMaxBodyPairs3D();
    physics3DWorld.maxContactConstraints = PhysicsSystem::getDefaultMaxContactConstraints3D();
}

const editor::Physics3DWorldSettings& editor::Project::getPhysics3DWorldSettings() const{
    return physics3DWorld;
}

std::vector<jasperengine::InputManagerAction> editor::Project::getInputManagerActions() const{
    if (inputActionsAsset.empty())
        return InputManagerFormat::defaults();
    std::string text;
    InputActionAssetDesc asset;
    std::string error;
    if (!readInputActionsAsset(text) || !InputActionAssetDesc::parse(text, asset, error))
        return {};
    const InputActionMapDesc* map = asset.findMap(InputManagerFormat::MAP_NAME);
    return map ? InputManagerFormat::fromMap(*map) : std::vector<InputManagerAction>();
}

bool editor::Project::readInputActionsAsset(std::string& text) const{
    text.clear();
    if (inputActionsAsset.empty())
        return false;
    if (inputMigration.pending && inputMigration.asset == inputActionsAsset){
        text = inputMigration.text;
        return true;
    }
    std::ifstream file(getProjectPath() / inputActionsAsset, std::ios::binary);
    if (!file)
        return false;
    text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

void editor::Project::setInputActionsMigration(const InputActionsMigration& migration){
    inputMigration = migration;
}

const editor::InputActionsMigration& editor::Project::getInputActionsMigration() const{
    return inputMigration;
}

bool editor::Project::writeInputActionsMigration(){
    if (!inputMigration.pending)
        return true;
    if (getProjectPath().empty())
        return false;
    const std::filesystem::path file = getProjectPath() / inputMigration.asset;
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const io::WriteResult result = io::writeFileAtomic(file, inputMigration.text);
    if (!result.ok()){
        Out::error("Project Settings > Input: the Input Manager actions of project.yaml could not be written to %s (%s); "
                   "Play uses them from memory until they can", inputMigration.asset.generic_string().c_str(), result.message.c_str());
        return false;
    }
    inputMigration.pending = false;
    Out::info("Project Settings > Input: the %zu Input Manager actions of project.yaml are the \"%s\" map of %s now "
              "(project.yaml points to it once the project is saved)",
              inputMigration.actions, InputManagerFormat::MAP_NAME, inputMigration.asset.generic_string().c_str());
    return true;
}

void editor::Project::setUnreadableInputNode(const std::string& yaml){
    unreadableInputNode = yaml;
}

const std::string& editor::Project::getUnreadableInputNode() const{
    return unreadableInputNode;
}

void editor::Project::resetPlayInput() const{
    // no held key, touch, tap or text of the last Play (or of the editor's own use), and none of
    // its Input Manager rebinding, disabled actions or simulated sources
    Input::reset();
    InputActions::resetRuntimeState();
}

void editor::Project::setShaderGlobals(const std::vector<ShaderGlobalDecl>& decls){
    shaderGlobals = decls;
    applyShaderGlobals();
}

const std::vector<ShaderGlobalDecl>& editor::Project::getShaderGlobals() const{
    return shaderGlobals;
}

void editor::Project::applyShaderGlobals() const{
    ShaderGlobals::declare(shaderGlobals);
}

void editor::Project::resetPlayShaderGlobals() const{
    ShaderGlobals::resetToDefaults();
}

// `path` as spelled, when it is under `root`: below a folder linked in from out of the project,
// where std::filesystem::relative (which resolves links) leaves the root (S68 R7)
static std::filesystem::path spelledBelowRoot(const std::filesystem::path& path, const std::filesystem::path& root){
    const std::filesystem::path spelled = path.lexically_normal().lexically_relative(root.lexically_normal());
    return spelled.empty() || *spelled.begin() == ".." ? std::filesystem::path() : spelled;
}

static bool isInsideRoot(const std::filesystem::path& path, const std::filesystem::path& root){
    std::error_code ec;
    std::filesystem::path relativePath = std::filesystem::relative(path, root, ec);
    if (ec || relativePath.empty()){
        return false;
    }

    return *relativePath.begin() != ".." || !spelledBelowRoot(path, root).empty();
}

static std::filesystem::path resolveRootDir(const std::filesystem::path& projectPath, const std::filesystem::path& dir){
    if (dir.empty() || dir == "."){
        return projectPath;
    }
    if (dir.is_absolute()){
        return dir.lexically_normal();
    }
    return (projectPath / dir).lexically_normal();
}

static std::filesystem::path resolveAgainstRoot(const std::filesystem::path& root, const std::filesystem::path& relative){
    if (relative.empty()){
        return {};
    }
    if (relative.is_absolute()){
        return relative.lexically_normal();
    }
    return (root / relative).lexically_normal();
}

static std::filesystem::path normalizeAgainstRoot(const std::filesystem::path& root, const std::filesystem::path& path){
    if (path.empty()){
        return {};
    }

    std::filesystem::path normalizedPath = path.lexically_normal();
    if (!normalizedPath.is_absolute()){
        return normalizedPath;
    }

    std::error_code ec;
    std::filesystem::path relativePath = std::filesystem::relative(normalizedPath, root, ec);
    if (ec || relativePath.empty()){
        return normalizedPath;
    }
    if (*relativePath.begin() == ".."){
        if (std::filesystem::path spelled = spelledBelowRoot(normalizedPath, root); !spelled.empty()) return spelled;
    }

    return relativePath.lexically_normal();
}

void editor::Project::openAssetDatabase() {
    waitForAssetScan();
    assetIndex.close();
    fs::path relativeAssets = normalizeToProjectRelative(getAssetsPath());
    if (relativeAssets == ".") {
        relativeAssets.clear();
    }
    assetDatabase.setReadOnly(readOnlyAssets);
    assetDatabase.open(projectPath, relativeAssets);
    // the dependency index builds in the background; documents with unsaved edits answer from memory
    assetIndex.setOverlayProvider([this](std::unordered_map<Guid, std::vector<DepEdge>>& edges) {
        collectMemoryEdges(edges);
    });
    assetIndex.open(&assetDatabase, projectPath, getAssetRoots());
    AssetMap::setResolver(&assetResolver);
    // S25: the labels are read again (the assets may have moved roots), the search index
    // follows, and this user's organisation file belongs to the project's root
    assetLabels.open(this);
    assetSearchIndex.attach(this);
    if (!organisation.isOpen() || organisation.projectRoot() != projectPath) {
        organisation.open(projectPath);
    }
}

editor::AssetSearchIndex& editor::Project::getAssetSearchIndex() {
    assetLabels.update();
    assetSearchIndex.refresh();
    return assetSearchIndex;
}

std::filesystem::path editor::Project::getAssetsPath() const{
    return resolveRootDir(projectPath, assetsDir);
}

std::filesystem::path editor::Project::getLuaPath() const{
    return resolveRootDir(projectPath, luaDir);
}

std::filesystem::path editor::Project::resolveAssetPath(const std::filesystem::path& assetRelative) const{
    return resolveAgainstRoot(getAssetsPath(), assetRelative);
}

std::filesystem::path editor::Project::normalizeToAssetsRelative(const std::filesystem::path& path) const{
    return normalizeAgainstRoot(getAssetsPath(), path);
}

bool editor::Project::isInsideAssetsPath(const std::filesystem::path& path) const{
    return isInsideRoot(resolveAssetPath(path), getAssetsPath());
}

std::filesystem::path editor::Project::resolveLuaPath(const std::filesystem::path& luaRelative) const{
    return resolveAgainstRoot(getLuaPath(), luaRelative);
}

std::filesystem::path editor::Project::normalizeToLuaRelative(const std::filesystem::path& path) const{
    return normalizeAgainstRoot(getLuaPath(), path);
}

std::filesystem::path editor::Project::findShaderFile(const std::filesystem::path& projectRoot, const std::filesystem::path& assetsRoot,
                                                      const std::string& relative){
    if (!ShaderVariantCompiler::isSafeRelativePath(relative)){
        return {};
    }
    std::error_code ec;
    for (const std::filesystem::path& root : {projectRoot, assetsRoot}){
        if (root.empty()){
            continue;
        }
        const std::filesystem::path file = (root / relative).lexically_normal();
        if (std::filesystem::is_regular_file(file, ec)){
            return file;
        }
    }
    return {};
}

std::string editor::Project::resolveShaderFile(const std::string& relative) const{
    const std::filesystem::path file = findShaderFile(projectPath, getAssetsPath(), relative);
    if (file.empty()){
        return relative;
    }
    return normalizeToProjectRelative(file).generic_string();
}

editor::Util::CustomShaderPaths editor::Project::resolveCustomShaderFiles(const std::string& customShader) const{
    Util::CustomShaderPaths paths = Util::resolveCustomShaderPaths(customShader);
    if (!paths.vert.empty()){
        paths.vert = resolveShaderFile(paths.vert);
    }
    if (!paths.frag.empty()){
        paths.frag = resolveShaderFile(paths.frag);
    }
    return paths;
}

bool editor::Project::readShaderFile(const std::string& relative, std::string& content) const{
    const std::filesystem::path file = findShaderFile(projectPath, getAssetsPath(), relative);
    if (file.empty()){
        return false;
    }
    std::ifstream stream(file, std::ios::binary);
    if (!stream){
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    content = buffer.str();
    return true;
}

// Only a directory without project files can be moved as a whole: those are referenced
// from the project root and would need their own remap.
static bool holdsOnlyAssets(const std::filesystem::path& directory){
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(directory, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); ++it){
        const auto& entry = *it;
        if (!entry.is_regular_file()){
            continue;
        }

        const std::string path = entry.path().string();
        if (editor::Util::isSceneFile(path) || editor::Util::isPrefabFile(path) || editor::Util::isMaterialFile(path) ||
            editor::Util::isScriptFile(path) || editor::Util::isShaderFile(path)){
            return false;
        }
    }

    return !ec;
}

bool editor::Project::visitAssetPathsInMaterialFiles(const std::function<bool(std::string&)>& transform){
    return visitMaterialFiles([&](Material& material) {
        // the textures (paths relative to the assets directory)
        return AssetRefSchema::forEachMaterialRef(material, std::string(), [&](AssetRef& ref, const AssetRefSite& site) {
            if (site.base != AssetPathBase::Assets || ref.path.empty()) {
                return;
            }
            std::string path = ref.path;
            if (transform(path)) {
                ref.path = path;
            }
        });
    });
}

bool editor::Project::visitMaterialFiles(const std::function<bool(Material&)>& visit){
    if (projectPath.empty()){
        return false;
    }

    std::error_code ec;
    bool changed = false;

    for (watch::LinkedDirectoryIterator it(projectPath, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)){
        const auto& entry = *it;

        if (entry.is_directory() && isNonAssetDir(entry.path())){
            it.disable_recursion_pending();
            continue;
        }

        if (!entry.is_regular_file() || !Util::isMaterialFile(entry.path().string())){
            continue;
        }
        // this walk goes through links, and nobody named this file: only where the editor may write
        if (!assetDatabase.allowsWritesAt(entry.path())){
            continue;
        }

        Material material;
        try {
            material = Stream::decodeMaterial(DocumentFiles::read(entry.path(), DocumentKind::Material).root);
        } catch (const std::exception& e) {
            Out::warning("Could not read material %s: %s", entry.path().string().c_str(), e.what());
            continue;
        }

        if (!visit(material)){
            continue;
        }

        // a material this editor could not read completely is left as it is (the gate)
        if (writeMaterialFile(normalizeToProjectRelative(entry.path()), material) != SaveResult::Saved){
            continue;
        }
        changed = true;
    }

    return changed;
}

void editor::Project::changeAssetRoots(const std::filesystem::path& newAssetsDir, const std::filesystem::path& newLuaDir){
    const fs::path oldAssetsRoot = getAssetsPath();
    const fs::path oldLuaRoot = getLuaPath();
    const fs::path newAssetsRoot = resolveRootDir(projectPath, newAssetsDir);
    const fs::path newLuaRoot = resolveRootDir(projectPath, newLuaDir);

    if (projectPath.empty() || (oldAssetsRoot == newAssetsRoot && oldLuaRoot == newLuaRoot)){
        assetsDir = newAssetsDir;
        luaDir = newLuaDir;
        refreshTrustedRoots();
        return;
    }

    // Buffers must reach disk before their files move, as a play session does
    editor::getEditorHost().saveAllCodeEditors();

    const bool assetsChanged = oldAssetsRoot != newAssetsRoot;
    const bool luaChanged = oldLuaRoot != newLuaRoot;

    std::error_code ec;
    FileUtils::createDirectories(newAssetsRoot, ec);
    FileUtils::createDirectories(newLuaRoot, ec);

    // Scenes that are not open still hold references, so they are loaded and unloaded again
    std::vector<uint32_t> temporarilyLoaded;
    for (size_t i = 0; i < scenes.size(); i++){
        if (scenes[i].filepath.empty() || scenes[i].scene){
            continue;
        }
        const uint32_t sceneId = scenes[i].id;
        loadScene(scenes[i].filepath, false, false, true);
        temporarilyLoaded.push_back(sceneId);
    }

    auto unloadTemporaryScenes = [&]() {
        for (uint32_t sceneId : temporarilyLoaded){
            if (SceneProject* sceneProject = getScene(sceneId)){
                deleteSceneProject(sceneProject);
            }
        }
    };

    std::vector<std::pair<fs::path, fs::path>> completedMoves;
    std::vector<std::pair<fs::path, fs::path>> keptExisting;
    bool relocationFailed = false;

    // Merges into an existing destination, so a folder whose files were relocated one
    // by one still ends up whole.
    std::function<bool(const fs::path&, const fs::path&)> moveEntry =
        [&](const fs::path& source, const fs::path& destination) {
        std::error_code moveEc;
        if (!fs::exists(source, moveEc)){
            return false;
        }

        if (fs::is_directory(source, moveEc)){
            FileUtils::createDirectories(destination, moveEc);
            bool moved = false;
            for (const auto& entry : fs::directory_iterator(source, moveEc)){
                moved |= moveEntry(entry.path(), destination / entry.path().filename());
                // Reporting a move here would point the reference at what stayed behind.
                if (relocationFailed){
                    return false;
                }
            }
            // Only removes the source when everything left it.
            FileUtils::removePath(source, moveEc);
            return moved;
        }

        if (fs::exists(destination, moveEc)){
            Out::warning("Kept the existing %s: the reference points at it and %s is no longer used",
                         destination.string().c_str(), source.string().c_str());
            keptExisting.emplace_back(source, destination);
            return false;
        }

        FileUtils::createDirectories(destination.parent_path(), moveEc);
        // beneath an MCP call's write scope the scoped rename; otherwise with the Windows retries
        if (FileUtils::scopedOps()) FileUtils::renamePath(source, destination, moveEc);
        else io::renameWithRetry(source, destination, moveEc);
        if (moveEc){
            // A root on another volume rejects rename, so the file is copied and only then dropped.
            std::error_code copyEc;
            FileUtils::copyPath(source, destination, false, copyEc);
            if (copyEc){
                // A partial copy would pass as an already relocated file on the next attempt.
                std::error_code cleanupEc;
                FileUtils::removePath(destination, cleanupEc);

                Out::error("Failed to copy %s: %s", source.string().c_str(), copyEc.message().c_str());
                relocationFailed = true;
                return false;
            }
            if (!FileUtils::removePath(source, copyEc)){
                Out::warning("Copied %s but could not remove the original", source.string().c_str());
            }
        }

        completedMoves.emplace_back(source, destination);
        // the asset's meta goes with it, so it keeps its GUID (S04)
        const fs::path sourceMeta = AssetTypes::metaPathFor(source);
        const fs::path destinationMeta = AssetTypes::metaPathFor(destination);
        if (fs::is_regular_file(sourceMeta, moveEc) && !fs::exists(destinationMeta, moveEc)){
            std::string metaError;
            if (assetio::renameFile(sourceMeta, destinationMeta, &metaError)){
                completedMoves.emplace_back(sourceMeta, destinationMeta);
            } else {
                Out::warning("Could not move %s: %s", sourceMeta.string().c_str(), metaError.c_str());
            }
        }
        return true;
    };

    // Keeps the project on its previous roots when a relocation could not finish.
    auto undoMoves = [&]() {
        for (size_t i = completedMoves.size(); i > 0; i--){
            const auto& [source, destination] = completedMoves[i - 1];
            std::error_code undoEc;

            // A directory emptied by the move is gone by now.
            FileUtils::createDirectories(source.parent_path(), undoEc);

            // The copy fallback may have left the original in place.
            if (fs::exists(source, undoEc)){
                FileUtils::removePath(destination, undoEc);
                continue;
            }

            if (FileUtils::scopedOps()) FileUtils::renamePath(destination, source, undoEc);
            else io::renameWithRetry(destination, source, undoEc);
            if (!undoEc){
                continue;
            }

            FileUtils::copyPath(destination, source, false, undoEc);
            if (undoEc){
                // References still point here, so a partial copy would pass as the file itself.
                std::error_code cleanupEc;
                FileUtils::removePath(source, cleanupEc);

                Out::error("Could not restore %s, the file is at %s: %s", source.string().c_str(),
                           destination.string().c_str(), undoEc.message().c_str());
                continue;
            }
            FileUtils::removePath(destination, undoEc);
        }
    };

    std::map<fs::path, fs::path> relocations;

    // Files outside the new root are moved into it, keeping their layout relative to
    // the old root so references between them (a model and its textures) still match.
    auto relocate = [&](const fs::path& absolutePath, const fs::path& oldRoot, const fs::path& newRoot) {
        if (relocationFailed || isInsideRoot(absolutePath, newRoot) || relocations.count(absolutePath)){
            return;
        }

        std::error_code relocateEc;
        const fs::path relativeToOld = fs::relative(absolutePath, oldRoot, relocateEc);
        if (relocateEc || relativeToOld.empty() || *relativeToOld.begin() == ".."){
            Out::warning("Reference outside both asset directories was kept as is: %s", absolutePath.string().c_str());
            relocations[absolutePath] = absolutePath;
            return;
        }

        const fs::path destination = (newRoot / relativeToOld).lexically_normal();
        relocations[absolutePath] = destination;

        // Models carry sidecar files referenced from inside the model, so their folder
        // moves along when it holds nothing else.
        fs::path source = absolutePath;
        fs::path sourceDestination = destination;
        if (Util::isModelFile(absolutePath.string()) && relativeToOld.has_parent_path()){
            const fs::path modelDir = (oldRoot / relativeToOld.parent_path()).lexically_normal();
            if (!isInsideRoot(newRoot, modelDir) && holdsOnlyAssets(modelDir)){
                source = modelDir;
                sourceDestination = (newRoot / relativeToOld.parent_path()).lexically_normal();
            }
        }

        if (moveEntry(source, sourceDestination)){
            Out::info("Moved %s to %s", source.string().c_str(), sourceDestination.string().c_str());
            return;
        }

        // Nothing moved: the reference only follows when the file is already there.
        if (fs::exists(destination, relocateEc)){
            return;
        }

        relocations[absolutePath] = absolutePath;
    };

    // Resolved before anything moves: a file that cannot be relocated aborts the whole change.
    std::vector<fs::path> assetSources;
    std::vector<fs::path> luaSources;

    const std::function<bool(std::string&)> assetCollect = [&](std::string& stored) {
        if (!stored.empty()){
            assetSources.push_back(resolveAgainstRoot(oldAssetsRoot, fs::path(stored)));
        }
        return false;
    };
    const std::function<bool(std::string&)> luaCollect = [&](std::string& stored) {
        if (!stored.empty()){
            luaSources.push_back(resolveAgainstRoot(oldLuaRoot, fs::path(stored)));
        }
        return false;
    };

    auto rebase = [&](std::string& stored, const fs::path& oldRoot, const fs::path& newRoot) {
        if (stored.empty()){
            return false;
        }

        const fs::path source = resolveAgainstRoot(oldRoot, fs::path(stored));
        const auto relocated = relocations.find(source);
        const fs::path absolutePath = relocated != relocations.end() ? relocated->second : source;

        // A file left outside the new root keeps its stored path: re-basing it would
        // only turn it into a "../" reference the runtime cannot resolve.
        if (!isInsideRoot(absolutePath, newRoot)){
            return false;
        }

        std::error_code rebaseEc;
        const fs::path updated = fs::relative(absolutePath, newRoot, rebaseEc);
        if (rebaseEc || updated.empty()){
            return false;
        }

        const std::string updatedPath = updated.lexically_normal().generic_string();
        if (updatedPath == stored){
            return false;
        }

        stored = updatedPath;
        return true;
    };

    const std::function<bool(std::string&)> assetTransform = [&](std::string& stored) {
        return rebase(stored, oldAssetsRoot, newAssetsRoot);
    };
    const std::function<bool(std::string&)> luaTransform = [&](std::string& stored) {
        return rebase(stored, oldLuaRoot, newLuaRoot);
    };

    // Walks every reference the project holds, writing back only what a visitor changed.
    auto visitReferences = [&](const std::function<bool(std::string&)>& assetVisitor,
                               const std::function<bool(std::string&)>& luaVisitor) {
        for (auto& sceneProject : scenes){
            if (!sceneProject.scene){
                continue;
            }

            bool sceneChanged = assetsChanged && visitAssetPathsInRegistry(sceneProject.scene, assetVisitor);
            sceneChanged |= luaChanged && visitLuaPathsInRegistry(sceneProject.scene, luaVisitor);

            if (!sceneChanged){
                continue;
            }

            sceneProject.isModified = true;
            sceneProject.needUpdateRender = true;
            if (!sceneProject.filepath.empty()){
                saveSceneToPath(sceneProject.id, sceneProject.filepath);
            }
        }

        for (auto& [prefabPath, prefab] : prefabAssets){
            if (!prefab.registry){
                continue;
            }

            bool prefabChanged = assetsChanged && visitAssetPathsInRegistry(prefab.registry.get(), assetVisitor);
            prefabChanged |= luaChanged && visitLuaPathsInRegistry(prefab.registry.get(), luaVisitor);

            if (!prefabChanged){
                continue;
            }

            prefab.isModified = true;
            savePrefabToDisk(prefabPath);
        }

        if (assetsChanged){
            visitAssetPathsInMaterialFiles(assetVisitor);
        }
    };

    visitReferences(assetCollect, luaCollect);

    // Generated maps follow the assets root, moved before references resolve against it
    const fs::path oldTerrainMaps = oldAssetsRoot / "terrain_maps";
    if (assetsChanged && fs::is_directory(oldTerrainMaps, ec) &&
        !isInsideRoot(oldTerrainMaps, newAssetsRoot) && !isInsideRoot(newAssetsRoot, oldTerrainMaps)){
        moveEntry(oldTerrainMaps, newAssetsRoot / "terrain_maps");
    }

    for (const fs::path& source : assetSources){
        relocate(source, oldAssetsRoot, newAssetsRoot);
    }
    for (const fs::path& source : luaSources){
        relocate(source, oldLuaRoot, newLuaRoot);
    }

    if (relocationFailed){
        undoMoves();
        unloadTemporaryScenes();

        Out::error("Asset directories not changed: a referenced file could not be moved");
        editor::getEditorHost().registerAlert("Error", "Failed to move files to the new directories!");
        return;
    }

    assetsDir = newAssetsDir;
    luaDir = newLuaDir;
    refreshTrustedRoots();

    visitReferences(assetTransform, luaTransform);

    // Open tabs still hold the old path, where a later save would recreate the script.
    if (CodeEditor* codeEditor = editor::getEditorHost().getCodeEditor()){
        for (const auto& [source, destination] : completedMoves){
            codeEditor->handleFileRename(source, destination);
        }

        // Nothing moved here, so the tab is reopened on the file the reference took.
        // One that could not be saved is left alone, its content would be discarded.
        for (const auto& [source, destination] : keptExisting){
            if (codeEditor->isFileOpen(source.string()) && !codeEditor->isFileModified(source.string())){
                codeEditor->closeFile(source.string());
                codeEditor->openFile(destination.string());
            }
        }
    }

    if (ImageViewerWindow* imageViewer = editor::getEditorHost().getImageViewerWindow()){
        for (const auto& [source, destination] : completedMoves){
            imageViewer->handleFileRename(source, destination);
        }
    }

    unloadTemporaryScenes();

    // the scan follows the new roots (data files are assets only in the assets directory)
    openAssetDatabase();

    saveProjectFile();
}

void editor::Project::setPackNativeResources(bool enabled){
    packNativeResources = enabled;
}

void editor::Project::setVcsProjectSettings(const VcsProjectSettings& settings, bool apply){
    if (settings == vcsProjectSettings) return;
    if (!apply) {
        vcsProjectSettings = settings;
        return;
    }
    const bool providerChanged = settings.provider != vcsProjectSettings.provider;
    const bool attributesChanged = settings.lfs != vcsProjectSettings.lfs || settings.lockScenes != vcsProjectSettings.lockScenes;
    vcsProjectSettings = settings;
    if (attributesChanged && versionControlMetadata && !projectPath.empty()) writeVersionControlMetadata();
    if (providerChanged && vcsService->state() != vcs::VcsState::Off) startVersionControl();
}

editor::vcs::VcsService& editor::Project::getVcs(){
    return *vcsService;
}

editor::vcs::SceneDiffTracker& editor::Project::getSceneDiffs(){
    return *sceneDiffs;
}

const editor::vcs::VcsService& editor::Project::getVcs() const{
    return *vcsService;
}

editor::vcs::VcsSettings editor::Project::versionControlServiceSettings() const{
    const VersionControlSettings user = AppSettings::getVersionControlSettings();
    vcs::VcsSettings settings;
    settings.provider = vcsProjectSettings.provider;
    settings.gitPath = FileUtils::pathFromUtf8(user.gitPath);
    settings.svnPath = FileUtils::pathFromUtf8(user.svnPath);
    settings.pollSeconds = user.pollSeconds;
    settings.lockPollSeconds = user.lockPollSeconds;
    settings.autoFetchMinutes = user.autoFetchMinutes;
    // the svn lock comment: who holds it, as the OS knows them
    for (const char* variable : {"USER", "USERNAME", "LOGNAME"}) {
        if (const char* value = std::getenv(variable); value && *value) {
            settings.userName = value;
            break;
        }
    }
    return settings;
}

void editor::Project::startVersionControl(){
    if (projectPath.empty()) {
        vcsService->close();
        return;
    }
    vcsService->open(projectPath, getProjectInternalPath() / "vcs", versionControlServiceSettings(), watchFiles);
}

std::filesystem::path editor::Project::getVcsBackupDir() const{
    return getProjectInternalPath() / "vcs-backup";
}

void editor::Project::removeVcsBackups(){
    if (projectPath.empty()) return;
    std::error_code ec;
    std::filesystem::remove_all(getVcsBackupDir(), ec);
}

void editor::Project::setVersionControlMetadata(bool enabled){
    versionControlMetadata = enabled;
}

bool editor::Project::hasVersionControlMetadata() const{
    return versionControlMetadata;
}

bool editor::Project::shouldPackNativeResources() const{
    return packNativeResources;
}

editor::ShaderOverrides& editor::Project::getShaderOverrides(){
    return shaderOverrides;
}

const editor::ShaderOverrides& editor::Project::getShaderOverrides() const{
    return shaderOverrides;
}

editor::SourceCodeExportSettings& editor::Project::getSourceCodeExportSettings(){
    return sourceCodeExportSettings;
}

const editor::SourceCodeExportSettings& editor::Project::getSourceCodeExportSettings() const{
    return sourceCodeExportSettings;
}

editor::DesktopExportSettings& editor::Project::getDesktopExportSettings(){
    return desktopExportSettings;
}

const editor::DesktopExportSettings& editor::Project::getDesktopExportSettings() const{
    return desktopExportSettings;
}

// Leading numeric parts of a version, stopping at the first part that is not a
// number. Saturates instead of dropping digits, so date-stamped build numbers
// keep every digit that separates one release from the next.
static std::vector<unsigned int> versionParts(const std::string& source, size_t maxParts) {
    std::vector<unsigned int> parts;
    size_t start = 0;
    while (parts.size() < maxParts && start <= source.size()) {
        const size_t dot = source.find('.', start);
        const size_t end = dot == std::string::npos ? source.size() : dot;

        unsigned long long part = 0;
        size_t digits = 0;
        for (size_t i = start; i < end && std::isdigit(static_cast<unsigned char>(source[i])); i++) {
            part = std::min(part * 10 + static_cast<unsigned>(source[i] - '0'), 0xFFFFFFFFull);
            digits++;
        }
        if (digits == 0) break;

        parts.push_back(static_cast<unsigned int>(part));
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return parts;
}

editor::ApplicationSettings& editor::Project::getApplicationSettings(){
    return applicationSettings;
}

uint8_t editor::PresentationSettings::effectiveAllowedOrientations() const{
    uint8_t mask = allowedOrientations & PlatformRules::ALL_ORIENTATIONS;
    if (PlatformRules::isFixedOrientation(defaultOrientation)){
        mask |= PlatformRules::orientationBit(defaultOrientation);
    }
    return mask ? mask : PlatformRules::DEFAULT_ALLOWED_ORIENTATIONS;
}

editor::PresentationSettings& editor::Project::getPresentationSettings(){
    return presentationSettings;
}

const editor::PresentationSettings& editor::Project::getPresentationSettings() const{
    return presentationSettings;
}

editor::PlayerSettings editor::Project::getPlayerSettings() const{
    PlayerSettings settings;
    settings.defaultOrientation = presentationSettings.defaultOrientation;
    settings.allowedOrientations = presentationSettings.effectiveAllowedOrientations();
    settings.keepScreenOn = presentationSettings.keepScreenOn;
    settings.targetFrameRate = presentationSettings.targetFrameRate;
    auto app = [this](const std::string& name, const std::string& identifier, const std::string& version, const std::string& build) {
        ApplicationInfo info;
        info.name = getApplicationName(name);
        info.identifier = getApplicationIdentifier(identifier);
        info.version = getApplicationVersion(version);
        info.build = OsRules::parseBuildNumber(getApplicationBuild(build));
        return info;
    };
    settings.linuxApp = app(linuxProjectSettings.applicationName, "", "", "");
    settings.windowsApp = app(windowsProjectSettings.productName, "", windowsProjectSettings.productVersion, "");
    settings.macosApp = app(macOSProjectSettings.applicationName, macOSProjectSettings.bundleIdentifier,
                            macOSProjectSettings.versionName, macOSProjectSettings.buildNumber);
    settings.webApp = app(webProjectSettings.applicationName, "", "", "");
    return settings;
}

void editor::Project::applyPresentationSettings(){
    // what an earlier Play changed (focus, pause, frame rate) does not carry over
    Engine::resetApplicationState();
    editor::Platform::resetPlayRecords();
    Engine::setVSyncCount(vsyncEnabled ? 1 : 0);
    Engine::setTargetFrameRate(presentationSettings.targetFrameRate);
    Screen::initialize(presentationSettings.defaultOrientation, presentationSettings.effectiveAllowedOrientations(),
                       presentationSettings.keepScreenOn);
    // a simulated phone or tablet keeps the screen on as the device would (Keep Screen On
    // applies on Android and iOS only)
    if (presentationSettings.keepScreenOn && editor::Platform::getSimulation().device) {
        Screen::setSleepTimeout(SleepTimeout::NeverSleep);
    }
    ApplicationInfo info;
    info.name = getApplicationName();
    info.identifier = getApplicationIdentifier();
    info.version = getApplicationVersion();
    info.build = getApplicationVersionCode();
    System::setApplicationInfo(info);
}

const editor::ApplicationSettings& editor::Project::getApplicationSettings() const{
    return applicationSettings;
}

std::string editor::Project::getApplicationName(const std::string& platformOverride) const{
    if (!platformOverride.empty()) return platformOverride;
    if (!applicationSettings.name.empty()) return applicationSettings.name;
    if (!name.empty()) return name;
    return "JasperEngine";
}

std::string editor::Project::getApplicationIdentifier(const std::string& platformOverride) const{
    if (!platformOverride.empty()) return platformOverride;
    if (!applicationSettings.identifier.empty()) return applicationSettings.identifier;
    return ApplicationSettings{}.identifier;
}

std::string editor::Project::getApplicationVersion(const std::string& platformOverride) const{
    if (!platformOverride.empty()) return platformOverride;
    if (!applicationSettings.version.empty()) return applicationSettings.version;
    return ApplicationSettings{}.version;
}

std::string editor::Project::getApplicationBuild(const std::string& platformOverride) const{
    if (!platformOverride.empty()) return platformOverride;
    return std::to_string(std::max(1u, applicationSettings.build));
}

unsigned int editor::Project::getApplicationVersionCode(unsigned int platformOverride) const{
    if (platformOverride > 0) return platformOverride;
    return std::max(1u, applicationSettings.build);
}

std::string editor::Project::getApplicationFileVersion(const std::string& platformOverride) const{
    return toFourPartVersion(platformOverride.empty() ? getApplicationVersion() : platformOverride);
}

std::string editor::Project::toAppleVersion(const std::string& source){
    const std::vector<unsigned int> parts = versionParts(source, 3);
    if (parts.empty()) return "1";

    std::string result = std::to_string(parts[0]);
    for (size_t i = 1; i < parts.size(); i++) {
        result += "." + std::to_string(parts[i]);
    }
    return result;
}

std::string editor::Project::toFourPartVersion(const std::string& source){
    const std::vector<unsigned int> parts = versionParts(source, 4);

    std::string result;
    for (size_t i = 0; i < 4; i++) {
        if (i > 0) result += ".";
        result += std::to_string(i < parts.size() ? std::min(65535u, parts[i]) : 0u);
    }
    return result;
}

bool editor::isAdMobAppIdValid(const std::string& value){
    const std::string prefix = "ca-app-pub-";
    if (value.compare(0, prefix.size(), prefix) != 0) return false;

    const size_t tilde = value.find('~', prefix.size());
    if (tilde == std::string::npos || tilde == prefix.size() || tilde + 1 == value.size()) return false;

    for (size_t i = prefix.size(); i < value.size(); i++) {
        if (i != tilde && !std::isdigit(static_cast<unsigned char>(value[i]))) return false;
    }
    return true;
}

editor::WebProjectSettings& editor::Project::getWebProjectSettings(){
    return webProjectSettings;
}

const editor::WebProjectSettings& editor::Project::getWebProjectSettings() const{
    return webProjectSettings;
}

editor::LinuxProjectSettings& editor::Project::getLinuxProjectSettings(){
    return linuxProjectSettings;
}

const editor::LinuxProjectSettings& editor::Project::getLinuxProjectSettings() const{
    return linuxProjectSettings;
}

editor::WindowsProjectSettings& editor::Project::getWindowsProjectSettings(){
    return windowsProjectSettings;
}

const editor::WindowsProjectSettings& editor::Project::getWindowsProjectSettings() const{
    return windowsProjectSettings;
}

editor::MacOSProjectSettings& editor::Project::getMacOSProjectSettings(){
    return macOSProjectSettings;
}

const editor::MacOSProjectSettings& editor::Project::getMacOSProjectSettings() const{
    return macOSProjectSettings;
}

editor::IOSProjectSettings& editor::Project::getIOSProjectSettings(){
    return iosProjectSettings;
}

const editor::IOSProjectSettings& editor::Project::getIOSProjectSettings() const{
    return iosProjectSettings;
}

editor::AndroidProjectSettings& editor::Project::getAndroidProjectSettings(){
    return androidProjectSettings;
}

const editor::AndroidProjectSettings& editor::Project::getAndroidProjectSettings() const{
    return androidProjectSettings;
}

uint32_t editor::Project::getStartSceneId() const{
    return startSceneId;
}

void editor::Project::setStartSceneId(uint32_t sceneId){
    startSceneId = sceneId;
}

editor::LoadingSettings& editor::Project::getLoadingSettings(){
    return loadingSettings;
}

const editor::AudioSettings& editor::Project::getAudioSettings() const{
    return audioSettings;
}

void editor::Project::setAudioSettings(const AudioSettings& settings){
    const bool mixerChanged = settings.mixer != audioSettings.mixer;
    audioSettings = settings;
    audioSettings.maxRealVoices = std::clamp(audioSettings.maxRealVoices, 1, 255);
    audioSettings.maxVoices = std::clamp(audioSettings.maxVoices, 1, 1000);
    if (audioSettings.sampleRate < 0) audioSettings.sampleRate = 0;
    AudioSystem::configure({audioSettings.maxRealVoices, audioSettings.maxVoices, audioSettings.sampleRate});
    AudioMixerDocument::setDefaultOutputGroup(audioSettings.defaultOutputGroup);
    if (mixerChanged || AudioMixer::getPath() != audioSettings.mixer) {
        applyAudioMixer();
    }
}

std::filesystem::path editor::Project::getAudioMixerFile() const{
    if (audioSettings.mixer.empty()) return {};
    return resolveAssetPath(FileUtils::pathFromUtf8(audioSettings.mixer)).lexically_normal();
}

bool editor::Project::applyAudioMixer(std::string* error){
    if (audioSettings.mixer.empty()) {
        if (AudioMixer::hasAsset()) AudioMixer::unload();
        return true;
    }
    const fs::path file = getAudioMixerFile();
    std::string text;
    std::error_code ec;
    auto readText = [](const fs::path& path, std::string& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return true;
    };
    if (!fs::is_regular_file(file, ec) || !readText(file, text)) {
        // created on first use (the Audio Mixer window, MCP audio_mixer edit)
        if (AudioMixer::hasAsset()) AudioMixer::unload();
        if (error) *error = "'" + audioSettings.mixer + "' does not exist";
        return false;
    }
    AudioMixerDef def;
    std::string parseError;
    std::vector<std::string> warnings;
    if (!AudioMixerIO::parse(text, def, &parseError, &warnings)) {
        Out::error("Audio mixer '%s': %s (the implicit Master plays)", audioSettings.mixer.c_str(), parseError.c_str());
        if (error) *error = parseError;
        return false;
    }
    for (const std::string& warning : warnings) {
        Out::warning("Audio mixer '%s': %s", audioSettings.mixer.c_str(), warning.c_str());
    }
    const bool fresh = AudioMixer::getPath() != audioSettings.mixer;
    AudioMixer::apply(def);
    AudioMixer::setAssetPath(audioSettings.mixer);
    // another mixer: the asset's values, not what the last one moved
    if (fresh && !isAnyScenePlaying()) AudioMixer::resetRuntime(false);
    return true;
}

void editor::Project::audioFilesChanged(const std::vector<std::filesystem::path>& changed){
    const fs::path mixer = getAudioMixerFile();
    if (mixer.empty()) return;
    for (const fs::path& path : changed) {
        std::error_code ec;
        if (path.lexically_normal() == mixer || fs::equivalent(path, mixer, ec)) {
            applyAudioMixer();
            return;
        }
    }
}

void editor::Project::resetPlayAudio(bool starting) const{
    AudioSystem::setListenerPaused(false);
    // AudioListener.volume / AudioSystem.globalVolume: SoLoud's global volume, which the editor's
    // preview and the next Play hear too
    AudioSystem::setGlobalVolume(1.0f);
    AudioMixer::setMasterPaused(false);
    AudioMixer::resetRuntime(starting);
    if (starting) AudioMixer::loadRemembered();
}

const editor::LoadingSettings& editor::Project::getLoadingSettings() const{
    return loadingSettings;
}

const editor::ThreadingProjectSettings& editor::Project::getThreadingSettings() const{
    return threadingSettings;
}

ThreadingSettings editor::Project::toEngineThreading(const ThreadingProjectSettings& settings){
    ThreadingSettings engine;
    engine.workers = settings.workers;
    engine.reservedFrameWorkers = settings.reservedFrameWorkers;
    engine.ioThreads = settings.ioThreads;
    engine.webPumpBudgetMs = settings.webPumpBudgetMs;
    return engine;
}

void editor::Project::setThreadingSettings(const ThreadingProjectSettings& settings){
    threadingSettings = settings;
    Engine::setThreadingSettings(toEngineThreading(settings));
    // the standalone build's main.cpp bakes them in; loading them (project.yaml, the defaults a
    // load starts from) is not a change: opening a project writes nothing
    if (modelLoaded){
        markGeneratedSourcesDirty();
    }
}

const editor::ShaderCreationProjectSettings& editor::Project::getShaderCreationSettings() const{
    return shaderCreationSettings;
}

void editor::Project::setShaderCreationSettings(const ShaderCreationProjectSettings& settings){
    const bool changed = settings != shaderCreationSettings;
    shaderCreationSettings = settings;
    Engine::setShaderCreationSettings(settings.engineSettings());
    if (changed && modelLoaded){
        markGeneratedSourcesDirty();
    }
}

std::filesystem::path editor::Project::getShaderWarmupPath() const{
    return getProjectPath() / "ShaderWarmup.json";
}

void editor::Project::finishShaderWarmupRecording(){
    if (!ShaderWarmup::isRecording())
        return;
    ShaderWarmup::stopRecording();
    const std::filesystem::path path = getShaderWarmupPath();
    // what earlier runs recorded stays: this run's entries are added to the file's
    std::ifstream file(path, std::ios::binary);
    if (file) {
        std::stringstream text;
        text << file.rdbuf();
        std::string error;
        if (!ShaderWarmup::mergeFromString(text.str(), &error))
            Out::warning("Shader warm-up list %s was not readable (%s): it is replaced", path.string().c_str(), error.c_str());
    }
    std::string error;
    if (ShaderWarmup::save(path.string(), &error)) {
        Out::info("Shader warm-up list: %zu shaders, %zu pipelines (%s)", ShaderWarmup::getShaderCount(),
                  ShaderWarmup::getPipelineCount(), path.filename().string().c_str());
    } else {
        Out::error("Shader warm-up list not saved: %s", error.c_str());
    }
    ShaderWarmup::clear();
}

bool editor::Project::clearShaderWarmup(){
    if (ShaderWarmup::isRecording()) {
        ShaderWarmup::stopRecording();
        ShaderWarmup::startRecording();
    }
    ShaderWarmup::clear();
    std::string error;
    const std::filesystem::path path = getShaderWarmupPath();
    if (!std::filesystem::exists(path))
        return true;
    if (!ShaderWarmup::save(path.string(), &error)) {
        Out::error("Shader warm-up list not cleared: %s", error.c_str());
        return false;
    }
    return true;
}

const editor::DebugSettings& editor::Project::getDebugSettings() const{
    return debugSettings;
}

void editor::Project::setDebugSettings(const DebugSettings& settings){
    const DebugSettings before = debugSettings;
    debugSettings = settings;
    // a running Play takes what changed at once (what the game set itself, Log.bufferSize, the
    // console open, stays unless that setting changed)
    if (debugSettingsInPlay && settings != before){
        if (settings.logBufferSize != before.logBufferSize)
            Log::setBufferSize(debugSettings.logBufferSize);
        if (settings.logStackTraces != before.logStackTraces)
            Log::setStackTraces(debugSettings.logStackTraces);
        DebugOverlay::updateSettings(debugSettings.toOverlaySettings(true));
    }
}

void editor::Project::applyDebugSettingsForPlay() const{
    // the editor's Play is a development build, as a Debug-type export
    Engine::setDevelopmentBuild(true);
    Log::setBufferSize(debugSettings.logBufferSize);
    Log::setStackTraces(debugSettings.logStackTraces);
    DebugOverlay::configure(debugSettings.toOverlaySettings(true));
    debugSettingsInPlay = true;
}

editor::TerrainEditorSettings& editor::Project::getTerrainEditorSettings(){
    return terrainEditorSettings;
}

const editor::TerrainEditorSettings& editor::Project::getTerrainEditorSettings() const{
    return terrainEditorSettings;
}

const editor::TextureProjectSettings& editor::Project::getTextureSettings() const{
    return textureSettings;
}

void editor::Project::setTextureSettings(const TextureProjectSettings& settings){
    if (settings == textureSettings){
        return;
    }
    textureSettings = settings;
    if (TextureCookQueue::get().getProject() == this){
        TextureCookQueue::get().projectSettingsChanged();
    }
    // the standalone build's main.cpp bakes the runtime quality and budgets in
    if (modelLoaded){
        markGeneratedSourcesDirty();
    }
}

editor::PhysicsPlacementSettings& editor::Project::getPhysicsPlacementSettings(){
    return physicsPlacementSettings;
}

const editor::PhysicsPlacementSettings& editor::Project::getPhysicsPlacementSettings() const{
    return physicsPlacementSettings;
}

const editor::NavigationProjectSettings& editor::Project::getNavigationSettings() const{
    return navigationSettings;
}

void editor::Project::setNavigationSettings(const NavigationProjectSettings& settings){
    const bool changed = settings != navigationSettings;
    navigationSettings = settings;
    navigationSettings.settings.sanitize();
    nav::setSettings(navigationSettings.settings);
    // the standalone build's main.cpp and CMake settings carry them
    if (modelLoaded && changed){
        markGeneratedSourcesDirty();
    }
}

editor::SpriteNormalsWindowSettings& editor::Project::getSpriteNormalsSettings(){
    return spriteNormalsSettings;
}

const editor::SpriteNormalsWindowSettings& editor::Project::getSpriteNormalsSettings() const{
    return spriteNormalsSettings;
}

bool editor::Project::isSimulateTouch() const{
    return simulateTouch;
}

const std::vector<editor::GameResolution>& editor::Project::getCustomResolutions() const{
    return customResolutions;
}

void editor::Project::setCustomResolutions(const std::vector<GameResolution>& resolutions){
    customResolutions = resolutions;
}

void editor::Project::setSimulateTouch(bool simulateTouch){
    this->simulateTouch = simulateTouch;
}

editor::CommandHistory* editor::Project::getProjectCommandHistory(){
    return &projectHistory;
}

uint32_t editor::Project::createNewScene(std::string sceneName, SceneType type){
    if (isAnyScenePlaying()){
        Out::warning("Cannot create a new scene while a scene is playing.");
        return NULL_PROJECT_SCENE;
    }

    uint32_t previousSceneId = getSelectedSceneId();

    checkUnsavedAndExecute(previousSceneId, [this, sceneName, type, previousSceneId]() {
        // answered later (the unsaved changes dialog): Play may have started meanwhile
        if (isAnyScenePlaying()) {
            Out::warning("Cannot create a new scene while a scene is playing.");
            return;
        }
        createNewSceneInternal(sceneName, type, previousSceneId);
    });

    return NULL_PROJECT_SCENE; // Scene may be created asynchronously
}

uint32_t editor::Project::createNewSceneInternal(std::string sceneName, SceneType type, uint32_t previousSceneId){
    uint32_t reusedSceneId = NULL_PROJECT_SCENE;
    if (previousSceneId != NULL_PROJECT_SCENE) {
        SceneProject* previousScene = getScene(previousSceneId);
        if (previousScene && previousScene->transientId) {
            reusedSceneId = previousSceneId;
            closeScene(previousSceneId, true);
        }
    }

    unsigned int nameCount = 2;
    std::string baseName = sceneName;
    bool foundName = true;
    while (foundName){
        foundName = false;
        for (auto& sceneProject : scenes) {
            std::string usedName = sceneProject.name;
            if (usedName == sceneName){
                sceneName = baseName + " " + std::to_string(nameCount);
                nameCount++;
                foundName = true;
            }
        }
    }

    SceneProject data;
    data.id = (reusedSceneId != NULL_PROJECT_SCENE) ? reusedSceneId : allocateSceneId();
    data.name = sceneName;
    data.transientId = true;
    data.scene = new Scene();
    data.sceneType = type;
    data.sceneRender = createSceneRender(data.sceneType, data.scene);
    data.defaultCamera = createDefaultCamera(data.sceneType, data.scene);
    data.isVisible = true;

    scenes.push_back(data);

    setSelectedSceneId(data.id);

    if (data.sceneType == SceneType::SCENE_3D){
        // Scene-appearance defaults for newly created 3D scenes. Loaded scenes keep
        // the values decoded from disk (see SceneRender3D constructor note).
        data.scene->setLightState(LightState::ON);
        data.scene->setGlobalIllumination(0.2);
        data.scene->setBackgroundColor(Vector4(0.25, 0.45, 0.65, 1.0));

        CreateEntityCmd sunCreator(this, data.id, "Directional Light", EntityCreationType::DIRECTIONAL_LIGHT);
        sunCreator.addProperty<Vector3>(ComponentType::Transform, "position", Vector3(0.0f, 10.0f, 0.0f));
        sunCreator.addProperty<float>(ComponentType::LightComponent, "intensity", 4.0f);
        sunCreator.addProperty<Vector3>(ComponentType::LightComponent, "direction", Vector3(-0.2f, -0.5f, 0.3f));
        sunCreator.addProperty<bool>(ComponentType::LightComponent, "shadows", true);
        sunCreator.addProperty<float>(ComponentType::LightComponent, "range", 100);
        sunCreator.execute();

        CreateEntityCmd skyCreator(this, data.id, "Skybox", EntityCreationType::SKYBOX);
        Texture defaultSky;
        ProjectUtils::setDefaultSkyTexture(defaultSky);
        skyCreator.addProperty<Texture>(ComponentType::SkyboxComponent, "texture", defaultSky);
        skyCreator.execute();

        clearSelectedEntities(data.id);
        getScene(data.id)->isModified = false; // New scene starts as unmodified
    } else if (data.sceneType == SceneType::SCENE_2D) {
        data.scene->setLightState(LightState::OFF);
        data.scene->setBackgroundColor(Vector4(0.231, 0.298, 0.475, 1.0));
    } else if (data.sceneType == SceneType::SCENE_UI) {
        data.scene->setLightState(LightState::OFF);
        data.scene->setBackgroundColor(Vector4(0.525, 0.525, 0.525, 1.0));
    }

    editor::getEditorHost().addNewSceneToDock(data.id);

    // Close the previous scene after the new one is created
    if (previousSceneId != NULL_PROJECT_SCENE && previousSceneId != reusedSceneId) {
        closeScene(previousSceneId, true);
    }

    return data.id;
}

editor::SceneProject* editor::Project::createRuntimeCloneFromSource(const SceneProject* source) {
    if (!source) {
        return nullptr;
    }

    fs::path fullPath = source->filepath;
    if (fullPath.is_relative()) {
        fullPath = getProjectPath() / fullPath;
    }

    YAML::Node sceneNode = DocumentFiles::read(fullPath, DocumentKind::Scene).root;
    return createRuntimeCloneFromNode(source, sceneNode);
}

editor::SceneProject* editor::Project::createRuntimeCloneFromNode(const SceneProject* source, const YAML::Node& sceneNode) {
    if (!source) {
        return nullptr;
    }

    std::unique_ptr<SceneProject> runtime(new SceneProject());
    runtime->opened = false;
    runtime->isModified = false;
    runtime->isVisible = false;
    runtime->needUpdateRender = false;
    runtime->filepath = source->filepath;

    Stream::decodeSceneProject(runtime.get(), sceneNode, true);
    // the file's id differs if loading replaced a duplicate
    runtime->id = source->id;
    runtime->defaultCamera = createDefaultCamera(runtime->sceneType, runtime->scene);
    Stream::decodeSceneProjectEntities(this, runtime.get(), sceneNode);
    // clip references written before a rename point at the file by GUID (the play startup
    // worker may run this: the database is only read)
    resolveRegistryClipRefs(runtime->scene, false);
    pauseEngineScene(runtime->scene, true);

    return runtime.release();
}

void editor::Project::updateSceneCppScripts(SceneProject* sceneProject) {
    if (!sceneProject || !sceneProject->scene) {
        return;
    }

    sceneProject->cppScripts.clear();

    std::unordered_set<std::string> uniqueScripts;
    auto scriptsArray = sceneProject->scene->getComponentArray<ScriptComponent>();

    for (int i = 0; i < scriptsArray->size(); i++) {
        const ScriptComponent& scriptComponent = scriptsArray->getComponentFromIndex(i);

        // disabled scripts too: they exist at runtime and can be enabled (Unity)
        for (const auto& scriptEntry : scriptComponent.scripts) {
            if (scriptEntry.type == ScriptType::LUA)
                continue;
            if (scriptEntry.path.empty())
                continue;

            fs::path fullPath = scriptEntry.path;
            if (fullPath.is_relative()) {
                fullPath = getProjectPath() / fullPath;
            }

            if (!std::filesystem::exists(fullPath)) {
                Out::error("Script file not found: %s", fullPath.string().c_str());
                continue;
            }

            std::string key = fullPath.lexically_normal().generic_string();
            if (!uniqueScripts.insert(key).second) {
                continue;
            }

            SceneScriptSource sceneScript;
            sceneScript.path = scriptEntry.path;
            sceneScript.headerPath = scriptEntry.headerPath;
            sceneScript.className = scriptEntry.className;
            sceneScript.properties = toScriptPropertyInfos(scriptEntry.properties);
            sceneProject->cppScripts.push_back(std::move(sceneScript));
        }
    }
}

// The JSON of a .gltf, or the JSON chunk of a .glb
static nlohmann::json readGltfJson(const fs::path& path, uintmax_t size) {
    std::ifstream file(path, std::ios::binary);
    // magic, version, length, then the first chunk's length and type
    uint32_t header[5];
    if (!file.read(reinterpret_cast<char*>(header), sizeof(header)) || header[0] != 0x46546C67u) {
        file.clear();
        file.seekg(0);
        return nlohmann::json::parse(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>(), nullptr, false);
    }
    if (header[4] != 0x4E4F534Au || sizeof(header) + header[3] > size) {
        return {};
    }
    std::string text(header[3], '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    return nlohmann::json::parse(text, nullptr, false);
}

// Each distinct buffer view a mesh component reads, plus a synthetic view per primitive for byte
// indices, integer VEC3 colors, sparse morphs and baked transforms
unsigned int editor::Project::countGltfExternalBuffers(const fs::path& path, bool merged) {
    // glTF componentType values
    constexpr int UNSIGNED_BYTE = 5121;
    constexpr int UNSIGNED_SHORT = 5123;
    constexpr int FLOAT = 5126;

    struct Count {
        uintmax_t size = 0;
        fs::file_time_type time;
        unsigned int separate = 0;
        unsigned int merged = 0;
    };
    static std::mutex mutex;
    static std::unordered_map<std::string, Count> cache;

    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    if (ec) {
        return 0;
    }
    const fs::file_time_type time = fs::last_write_time(path, ec);
    if (ec) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex);
    Count& count = cache[editor::FileUtils::pathToUtf8(path.lexically_normal())];
    if (count.size == size && count.time == time) {
        return merged ? count.merged : count.separate;
    }
    count = {size, time};

    const nlohmann::json gltf = readGltfJson(path, size);
    try {
        const nlohmann::json& nodes = gltf.at("nodes");
        const nlohmann::json& meshes = gltf.at("meshes");
        const nlohmann::json& accessors = gltf.at("accessors");

        // buffers of the given mesh nodes put on one MeshComponent
        auto countBuffers = [&](const std::vector<size_t>& meshNodes, bool baked) {
            std::set<size_t> views;
            unsigned int generated = 0;
            auto add = [&](const nlohmann::json& accessor, bool copied) {
                if (copied) {
                    generated++;
                } else if (accessor.contains("bufferView")) {
                    views.insert(accessor["bufferView"].get<size_t>());
                }
            };
            for (size_t node : meshNodes) {
                for (const nlohmann::json& primitive : meshes.at(nodes.at(node).at("mesh").get<size_t>()).at("primitives")) {
                    // skipped by the loader
                    if (!primitive.contains("indices")) {
                        continue;
                    }
                    const nlohmann::json& indices = accessors.at(primitive["indices"].get<size_t>());
                    add(indices, indices.value("componentType", 0) == UNSIGNED_BYTE);

                    for (const auto& attribute : primitive.at("attributes").items()) {
                        const nlohmann::json& accessor = accessors.at(attribute.value().get<size_t>());
                        const std::string& name = attribute.key();
                        const int component = accessor.value("componentType", 0);
                        const std::string type = accessor.value("type", "");
                        const bool color = name == "COLOR_0" && type == "VEC3" &&
                            (component == UNSIGNED_BYTE || component == UNSIGNED_SHORT);
                        const bool bake = baked && component == FLOAT &&
                            (((name == "POSITION" || name == "NORMAL") && type == "VEC3") || (name == "TANGENT" && type == "VEC4"));
                        add(accessor, color || bake);
                    }

                    for (const nlohmann::json& target : primitive.value("targets", nlohmann::json::array())) {
                        for (const nlohmann::json& index : target) {
                            const nlohmann::json& accessor = accessors.at(index.get<size_t>());
                            add(accessor, accessor.contains("sparse"));
                        }
                    }
                }
            }
            return static_cast<unsigned int>(views.size()) + generated;
        };

        std::vector<size_t> meshNodes;
        for (size_t i = 0; i < nodes.size(); i++) {
            if (nodes[i].contains("mesh") && nodes[i]["mesh"].get<size_t>() < meshes.size()) {
                meshNodes.push_back(i);
            }
        }

        if (meshNodes.size() == 1) {
            // the transform from the scene root down to a lone mesh node is baked
            size_t root = meshNodes[0];
            const size_t scene = gltf.value("scene", size_t(0));
            if (gltf.contains("scenes") && scene < gltf["scenes"].size()) {
                const nlohmann::json roots = gltf["scenes"][scene].value("nodes", nlohmann::json::array());
                if (!roots.empty()) {
                    root = roots[0].get<size_t>();
                }
            }
            count.separate = countBuffers(meshNodes, meshNodes[0] != root);
            // a foliage layer (counted as merged) bakes the node's whole global transform
            count.merged = countBuffers(meshNodes, true);
        } else {
            // a child mesh per node, or all of them baked onto the root when merged
            for (size_t node : meshNodes) {
                count.separate = std::max(count.separate, countBuffers({node}, false));
            }
            count.merged = countBuffers(meshNodes, true);
        }
    } catch (const nlohmann::json::exception&) {
        count.separate = 0;
        count.merged = 0;
    }
    return merged ? count.merged : count.separate;
}

void editor::Project::calculateSceneMaxValues(const SceneProject* sceneProject, SceneMaxValues& maxValues) const {
    if (!sceneProject || !sceneProject->scene) {
        return;
    }

    // what the splines build is counted as built: a scene saved right after it loaded (before a
    // frame ran SplineSystem) gets the same values as after
    if (std::shared_ptr<SplineSystem> splines = sceneProject->scene->findSystem<SplineSystem>()) {
        auto splineArray = sceneProject->scene->getComponentArray<SplineComponent>();
        std::vector<Entity> owners;
        for (size_t i = 0; i < splineArray->size(); ++i) {
            const Entity owner = splineArray->getEntity(i);
            if ((sceneProject->scene->findComponent<SplineExtrudeComponent>(owner) ||
                 sceneProject->scene->findComponent<SplineInstantiateComponent>(owner)) &&
                (splines->isRebuildPending(owner) || splines->getGeneratedEntities(owner).empty())) {
                owners.push_back(owner);
            }
        }
        for (Entity owner : owners) {
            splines->rebuildNow(owner);
        }
    }

    // The scene's own GameObjects only (user ids, and a GameObject an old file gave a system id):
    // the Scene view makes its icons (Sprites of one submesh), grid and gizmos in the scene's
    // registry as system entities when it draws, so a save would otherwise depend on whether the
    // scene was drawn
    const std::unordered_set<Entity> tracked(sceneProject->entities.begin(), sceneProject->entities.end());
    auto own = [&tracked](auto& array, size_t i) {
        const Entity entity = array->getEntity(i);
        return entity > EntityManager::lastSystemEntity() || tracked.count(entity) > 0;
    };

    auto meshes = sceneProject->scene->getComponentArray<MeshComponent>();
    for (size_t i = 0; i < meshes->size(); ++i) {
        if (!own(meshes, i)) continue;
        const MeshComponent& mesh = meshes->getComponentFromIndex(i);
        maxValues.maxSubmeshes = std::max(maxValues.maxSubmeshes, mesh.numSubmeshes);
        maxValues.maxExternalBuffers = std::max(maxValues.maxExternalBuffers, mesh.numExternalBuffers);
        maxValues.maxBones = std::max(maxValues.maxBones,
            static_cast<unsigned int>(mesh.bonesMatrix.size()));
    }

    // numExternalBuffers is filled by a load, and a command line export loads no model
    auto addModelFile = [&](const std::string& filename, bool merged) {
        if (filename.empty()) {
            return;
        }
        const fs::path path = resolveAssetPath(FileUtils::pathFromUtf8(filename));
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension == ".glb" || extension == ".gltf") {
            maxValues.maxExternalBuffers = std::max(maxValues.maxExternalBuffers, countGltfExternalBuffers(path, merged));
        } else if (FbxImporter::isFbxFile(path)) {
            // an FBX loads from its imported .glb
            maxValues.maxExternalBuffers = std::max(maxValues.maxExternalBuffers, countGltfExternalBuffers(getImportedModelPath(path), merged));
        }
    };

    auto models = sceneProject->scene->getComponentArray<ModelComponent>();
    for (size_t i = 0; i < models->size(); ++i) {
        if (!own(models, i)) continue;
        const ModelComponent& model = models->getComponentFromIndex(i);
        addModelFile(model.filename, model.mergeStaticMeshes);
    }

    // Spline Instantiate model pools (their pieces are merged models), also where none loaded
    auto instantiates = sceneProject->scene->getComponentArray<SplineInstantiateComponent>();
    for (size_t i = 0; i < instantiates->size(); ++i) {
        if (!own(instantiates, i)) continue;
        const SplineInstantiateComponent& instantiate = instantiates->getComponentFromIndex(i);
        for (const std::vector<SplinePoolEntry>* pool : {&instantiate.items, &instantiate.posts, &instantiate.spans,
                                                         &instantiate.gates, &instantiate.crests, &instantiate.caps}) {
            for (const SplinePoolEntry& entry : *pool) {
                addModelFile(entry.asset.path, true);
            }
        }
    }

    // foliage chunks are merged models
    auto terrains = sceneProject->scene->getComponentArray<TerrainComponent>();
    for (size_t i = 0; i < terrains->size(); ++i) {
        if (!own(terrains, i)) continue;
        for (const TerrainFoliageLayer& layer : terrains->getComponentFromIndex(i).foliageLayers) {
            addModelFile(layer.meshPath, true);
        }
    }

    auto sprites = sceneProject->scene->getComponentArray<SpriteComponent>();
    for (size_t i = 0; i < sprites->size(); ++i) {
        if (!own(sprites, i)) continue;
        const SpriteComponent& sprite = sprites->getComponentFromIndex(i);
        maxValues.maxSpriteFrames = std::max(maxValues.maxSpriteFrames, sprite.numFramesRect);
    }

    auto points = sceneProject->scene->getComponentArray<PointsComponent>();
    for (size_t i = 0; i < points->size(); ++i) {
        if (!own(points, i)) continue;
        const PointsComponent& pointsComponent = points->getComponentFromIndex(i);
        maxValues.maxSpriteFrames = std::max(maxValues.maxSpriteFrames, pointsComponent.numFramesRect);
    }

    // SpriteAnimationComponent.frames / framesTime are also HybridArray<int, MAX_SPRITE_FRAMES>.
    // Their counts are independent of (and can exceed) a sprite's numFramesRect (e.g. a ping-pong
    // sequence reuses frame indices), and the generated component code writes them with unchecked
    // operator[], so they must factor into MAX_SPRITE_FRAMES too.
    auto spriteAnims = sceneProject->scene->getComponentArray<SpriteAnimationComponent>();
    for (size_t i = 0; i < spriteAnims->size(); ++i) {
        if (!own(spriteAnims, i)) continue;
        const SpriteAnimationComponent& spriteAnim = spriteAnims->getComponentFromIndex(i);
        maxValues.maxSpriteFrames = std::max(maxValues.maxSpriteFrames, spriteAnim.framesSize);
        maxValues.maxSpriteFrames = std::max(maxValues.maxSpriteFrames, spriteAnim.framesTimeSize);
    }

    auto tilemaps = sceneProject->scene->getComponentArray<TilemapComponent>();
    for (size_t i = 0; i < tilemaps->size(); ++i) {
        if (!own(tilemaps, i)) continue;
        const TilemapComponent& tilemap = tilemaps->getComponentFromIndex(i);
        maxValues.maxTilemapTilesRect = std::max(maxValues.maxTilemapTilesRect, tilemap.numTilesRect);
        maxValues.maxTilemapTiles = std::max(maxValues.maxTilemapTiles, tilemap.numTiles);
    }

    // colliders have no caps (any size, in the editor and on device alike)
}

void editor::Project::collectSceneShaderKeys(const SceneProject* sceneProject, std::set<ShaderKey>& keys) const {
    if (!sceneProject || !sceneProject->scene) {
        return;
    }

    Scene* scene = sceneProject->scene;
    const bool hasReflectionProbes = scene->getComponentArray<ReflectionProbeComponent>()->size() > 0;

    // resolve the effective custom shader (component > scene default > built-in) from the
    // strings rather than the cached customShaderId, which lags a reload frame after a
    // scene-default change and is zeroed by the build-fail fallback
    auto effectiveShaderId = [&](const std::string& customShader, ShaderType type, const std::vector<std::string>& features,
                                 MaterialBlendMode blendMode) -> uint16_t {
        return ShaderPool::registerCustomShader(RenderSystem::getComponentShaderReference(
            customShader.empty() ? scene->getDefaultCustomShader(type) : customShader, features, blendMode));
    };

    // The colour variant of a submesh with the scene's reflection probes: the selection
    // RenderSystem::loadMesh makes (getSubmeshShaderInputs, getSubmeshShaderVariant), in the
    // render system's environment with the probes in it and its lights as its next update
    // counts them (RenderSystem::checkLightsAndShadow: a light added with the probe), from the
    // submesh's attributes of its last load
    MeshShaderEnvironment probeEnvironment;
    if (hasReflectionProbes) {
        if (std::shared_ptr<RenderSystem> renderSystem = scene->getSystem<RenderSystem>())
            probeEnvironment = renderSystem->getMeshShaderEnvironment();
        probeEnvironment.reflectionProbes = true;
        auto lights = scene->getComponentArray<LightComponent>();
        const LightState lightState = scene->getLightState();
        if (lightState == LightState::ON || (lightState == LightState::AUTO && lights->size() > 0))
            probeEnvironment.lights = true;
        for (size_t i = 0; i < lights->size(); ++i) {
            if (lights->getComponentFromIndex(i).shadows)
                probeEnvironment.shadows = true;
        }
    }
    auto probeMeshProperties = [&](Entity entity, const MeshComponent& mesh, const Submesh& submesh,
                                   const std::string& sceneMeshShader) -> uint64_t {
        const TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity);
        const InstancedMeshComponent* instmesh = scene->findComponent<InstancedMeshComponent>(entity);
        const bool mirror = scene->findComponent<MirrorComponent>(entity) != nullptr;
        const SubmeshShaderInputs inputs = RenderSystem::getSubmeshShaderInputs(mesh, submesh, terrain, instmesh, mirror, sceneMeshShader);
        return RenderSystem::getSubmeshShaderVariant(probeEnvironment, inputs).properties;
    };

    // the runtime falls back to the built-in variant when a custom shader fails to build,
    // so always collect the customId-0 key alongside a custom one
    auto insertKeys = [&](ShaderType type, uint64_t properties, uint16_t customShaderId) {
        keys.insert(ShaderPool::getShaderKey(type, properties, customShaderId));
        if (customShaderId != 0) {
            keys.insert(ShaderPool::getShaderKey(type, properties, 0));
        }
    };

    // Foliage entities are created at runtime and kept out of sceneProject->entities, so the
    // authored list alone would export the field without its instanced variant.
    std::vector<Entity> shaderEntities = sceneProject->entities;
    auto meshSystem = scene->getSystem<MeshSystem>();
    auto terrains = scene->getComponentArray<TerrainComponent>();
    for (size_t i = 0; i < terrains->size(); ++i) {
        const std::vector<Entity> foliage = meshSystem->getFoliageEntities(terrains->getEntity(i));
        shaderEntities.insert(shaderEntities.end(), foliage.begin(), foliage.end());
    }
    // so are the chunks a spline generates (their materials are their GameObject's Mesh's)
    if (std::shared_ptr<SplineSystem> splines = scene->findSystem<SplineSystem>()) {
        auto splineArray = scene->getComponentArray<SplineComponent>();
        for (size_t i = 0; i < splineArray->size(); ++i) {
            const std::vector<Entity> chunks = splines->getGeneratedEntities(splineArray->getEntity(i));
            shaderEntities.insert(shaderEntities.end(), chunks.begin(), chunks.end());
        }
    }

    for (Entity entity : shaderEntities) {
        Signature signature = scene->getSignature(entity);

        if (signature.test(scene->getComponentId<MeshComponent>())) {
            const MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
            if (mesh.loaded) {
                const std::string& sceneMeshShader = scene->getDefaultCustomShader(ShaderType::MESH);
                for (unsigned int s = 0; s < mesh.numSubmeshes; ++s) {
                    // each submesh's shader (its material's, the mesh's, the scene's) and features,
                    // with the built-in Lit's defines its variant was loaded with
                    uint16_t customShaderId = ShaderPool::registerCustomShader(
                        RenderSystem::getSubmeshShaderReference(mesh, mesh.submeshes[s], sceneMeshShader,
                                                                MaterialPbr::getDefinesFromBits(mesh.submeshes[s].litDefineBits)));
                    uint16_t customDepthShaderId = ShaderPool::registerCustomShader(
                        RenderSystem::getSubmeshDepthShaderReference(mesh, mesh.submeshes[s], sceneMeshShader));
                    uint64_t meshProperties = mesh.submeshes[s].shaderProperties;
                    // Saving/exporting can happen before RenderSystem has reloaded
                    // meshes after a probe was added. Derive the variant the reload will
                    // pick (the stored one may still be unlit) with the same selection as
                    // RenderSystem::loadMesh (IBL, normals, the normal map and tangents, SSAO,
                    // the UV sets and Stochastic Tiling of maps only a lit variant samples), so
                    // the exported shader set is still complete.
                    if (hasReflectionProbes && mesh.receiveLights && mesh.receiveIBL) {
                        meshProperties = probeMeshProperties(entity, mesh, mesh.submeshes[s], sceneMeshShader);
                    }
                    insertKeys(ShaderType::MESH, meshProperties, customShaderId);
                    // The runtime only computes depthShaderProperties while
                    // shadows or SSAO are active (or a depth fork is set), so the
                    // stored value can be a stale 0 for scenes whose shadow lights
                    // arrive at runtime (scripts spawning them). Derive the props
                    // from the same submesh flags RenderSystem::loadMesh uses, and
                    // keep the stored key too (identical when it was computed).
                    bool isTerrain = signature.test(scene->getComponentId<TerrainComponent>());
                    bool isInstanced = signature.test(scene->getComponentId<InstancedMeshComponent>());
                    // From the components, not the stored word, for the same reason.
                    bool instancedFade = isInstanced && scene->getComponent<InstancedMeshComponent>(entity).distanceFade;
                    const Material& material = mesh.submeshes[s].material;
                    bool depthAlphaMask = RenderSystem::usesAlphaMask(material, mesh.submeshes[s].textureShadow);
                    bool depthTexture = depthAlphaMask && mesh.submeshes[s].hasTexCoord1 && !material.baseColorTexture.empty();
                    // its alpha clip samples as the colour pass's Stochastic Tiling ('Sto', which
                    // the colour variant has when the material's maps take it); a mesh loaded with
                    // its lattice variant: its depth one too, with the vertex colour a lattice mask reads
                    bool depthStochastic = depthTexture && (meshProperties & shaderPropertyBit(5)) != 0;
                    bool lattice = mesh.lattice.variantActive && !isTerrain;
                    // and its Vertex Animation variant (the skinning and morph flags of a mesh
                    // loaded with one are off already)
                    const VatVariant vat = isTerrain ? VatVariant::NONE : mesh.vat.variantActive;
                    uint64_t depthProperties = ShaderPool::getDepthMeshProperties(
                        depthTexture, mesh.submeshes[s].hasSkinning,
                        mesh.submeshes[s].hasMorphTarget, mesh.submeshes[s].hasMorphNormal,
                        mesh.submeshes[s].hasMorphTangent, isTerrain, isInstanced, depthAlphaMask,
                        instancedFade, false, lattice && mesh.submeshes[s].hasVertexColor3,
                        lattice && mesh.submeshes[s].hasVertexColor4, false, false, depthStochastic, lattice,
                        vat != VatVariant::NONE, vat == VatVariant::BONES);
                    insertKeys(ShaderType::DEPTH, depthProperties, customDepthShaderId);
                    insertKeys(ShaderType::DEPTH, mesh.submeshes[s].depthShaderProperties, customDepthShaderId);
                    if (mesh.submeshes[s].gbufferShader) {
                        keys.insert(ShaderPool::getShaderKey(ShaderType::GBUFFER, mesh.submeshes[s].gbufferShaderProperties));
                    }
                    // the extra pass as loaded: its variant and the reference it was loaded with (the
                    // build-fail state does not matter; the material may name another pass by now
                    // if the mesh has not reloaded yet)
                    if (mesh.submeshes[s].extraPass && mesh.submeshes[s].extraPass->customShaderId != 0 &&
                            RenderSystem::hasExtraPass(mesh.submeshes[s], false)) {
                        keys.insert(ShaderPool::getShaderKey(ShaderType::MESH, mesh.submeshes[s].extraPass->shaderProperties,
                            mesh.submeshes[s].extraPass->customShaderId));
                    }
                }
            }
        }

        if (signature.test(scene->getComponentId<UIComponent>())) {
            const UIComponent& ui = scene->getComponent<UIComponent>(entity);
            if (ui.loaded) {
                insertKeys(ShaderType::UI, ui.shaderProperties, effectiveShaderId(ui.customShader, ShaderType::UI, ui.shaderFeatures, ui.blendMode));
            }
        }

        if (signature.test(scene->getComponentId<PointsComponent>())) {
            const PointsComponent& pts = scene->getComponent<PointsComponent>(entity);
            if (pts.loaded) {
                insertKeys(ShaderType::POINTS, pts.shaderProperties, effectiveShaderId(pts.customShader, ShaderType::POINTS, pts.shaderFeatures, pts.blendMode));
            }
        }

        if (signature.test(scene->getComponentId<LineRendererComponent>())) {
            const LineRendererComponent& ln = scene->getComponent<LineRendererComponent>(entity);
            if (ln.loaded) {
                insertKeys(ShaderType::LINES, ln.shaderProperties, effectiveShaderId(ln.customShader, ShaderType::LINES, ln.shaderFeatures, ln.blendMode));
            }
        }

        if (signature.test(scene->getComponentId<TrailRendererComponent>())) {
            const TrailRendererComponent& trail = scene->getComponent<TrailRendererComponent>(entity);
            if (trail.loaded) {
                insertKeys(ShaderType::LINES, trail.shaderProperties, effectiveShaderId(trail.customShader, ShaderType::LINES, trail.shaderFeatures, trail.blendMode));
            }
        }

        if (signature.test(scene->getComponentId<SkyboxComponent>())) {
            const SkyboxComponent& sky = scene->getComponent<SkyboxComponent>(entity);
            if (sky.loaded) {
                insertKeys(ShaderType::SKYBOX, 0, effectiveShaderId(sky.customShader, ShaderType::SKYBOX, sky.shaderFeatures, MaterialBlendMode::AUTO));
            }
        }
    }

    if (scene->isSSAOEnabled()) {
        keys.insert(ShaderPool::getShaderKey(ShaderType::SSAO, 0));
        keys.insert(ShaderPool::getShaderKey(ShaderType::SSAO_BLUR, 0));
    }

    if (scene->isSSREnabled()) {
        keys.insert(ShaderPool::getShaderKey(ShaderType::SSR, 0));
        keys.insert(ShaderPool::getShaderKey(ShaderType::SSR_BLUR, 0));
        keys.insert(ShaderPool::getShaderKey(ShaderType::COMPOSITE, 0));
    }

    // a reflection probe's runtime capture is prefiltered into roughness mips
    if (hasReflectionProbes) {
        keys.insert(ShaderPool::getShaderKey(ShaderType::PROBE_FILTER, 0));
    }

    // disabled passes ship too: the generated scene carries the whole chain
    for (const PostProcessPass& pass : scene->getPostProcessPasses()) {
        insertKeys(ShaderType::POSTPROCESS, 0, ShaderPool::registerCustomShader(pass.shader));
    }

    // fixed resolution upscales with it and a scene stack is presented with it
    keys.insert(ShaderPool::getShaderKey(ShaderType::BLIT, 0));

    // the atmosphere's passes: the scene's first Fog and Volumetric Clouds (RenderSystem's)
    {
        const FogComponent* fog = scene->findComponentFromIndex<FogComponent>(0);
        const bool heightFog = fog && fog->type == FogType::HEIGHT;
        const bool clouds = scene->getComponentArray<VolumetricCloudsComponent>()->size() > 0;
        for (ShaderType type : AtmosphereRenderer::passShaderTypes(heightFog, heightFog && fog->volumetric, clouds))
            keys.insert(ShaderPool::getShaderKey(type, 0));
    }

    // HTML UI documents draw geometry, gradients and layer/filter passes (RmlRenderer)
    if (scene->isComponentRegistered<UIDocumentComponent>() && scene->getComponentArray<UIDocumentComponent>()->size() > 0) {
        for (const auto& variant : ShaderPool::getHtmlUIVariants())
            insertKeys(variant.first, variant.second, 0);
    }
    // Canvas Groups composite their FLATTEN layers with the HTML UI passthrough
    if (scene->isComponentRegistered<CanvasGroupComponent>() && scene->getComponentArray<CanvasGroupComponent>()->size() > 0) {
        insertKeys(ShaderType::HTMLUI_POST, ShaderPool::getHtmlUIPostProperties(HtmlUIPostVariant::PASSTHROUGH), 0);
    }
    // Video Players convert their frames (HTMLUI_POST Yuv) and blit camera planes (Passthrough)
    if (scene->isComponentRegistered<VideoPlayerComponent>() && scene->getComponentArray<VideoPlayerComponent>()->size() > 0) {
        for (const auto& variant : ShaderPool::getVideoPlayerVariants())
            insertKeys(variant.first, variant.second, 0);
    }

    // a light casting 3D shadows copies the static shadow cache back and clears slots with the
    // shadow cache's program (RenderSystem::renderShadowMaps)
    {
        auto lights = scene->getComponentArray<LightComponent>();
        for (int i = 0; i < (int)lights->size(); i++) {
            if (lights->getComponentFromIndex(i).shadows) {
                keys.insert(ShaderPool::getShaderKey(ShaderType::SHADOW_CACHE, 0));
                break;
            }
        }
    }

    // 2D lights with shadows render occluder segments through the shadow2d
    // pass. The runtime additionally gates on an enabled ShadowCaster2D existing
    // (RenderSystem::loadLights2D), but scripts can spawn occluders at runtime,
    // so a shadow-casting light alone is enough to require the shader.
    {
        auto lights2d = scene->getComponentArray<Light2DComponent>();
        for (int i = 0; i < (int)lights2d->size(); i++) {
            if (lights2d->getComponentFromIndex(i).shadows) {
                keys.insert(ShaderPool::getShaderKey(ShaderType::SHADOW2D, 0));
                break;
            }
        }
    }
}

void editor::Project::collectSavedShaderKeys(const SceneProject* sceneProject, std::set<ShaderKey>& keys) const {
    // What the content needs (derived from it without drawing: ShaderVariantCollector) and the
    // keys the scene had: its file's, and what Play's run loaded (added at Stop, a recording the
    // user made). Never the variants its objects were loaded with in the editor: those depend on
    // what has drawn and when (a depth variant only while shadows or SSAO ran), so the same scene
    // would save a different list. An export adds them where they are loaded
    // (Exporter::resolveShaderKeys, collectSceneShaderKeys).
    collectRequiredShaderKeys(sceneProject, keys);
    if (sceneProject)
        keys.insert(sceneProject->shaderKeys.begin(), sceneProject->shaderKeys.end());
}

void editor::Project::collectRequiredShaderKeys(const SceneProject* sceneProject, std::set<ShaderKey>& keys) const {
    if (!sceneProject || !sceneProject->scene) {
        return;
    }

    ShaderVariantContent sceneContent;
    sceneContent.registry = sceneProject->scene;
    sceneContent.entities = sceneProject->entities;

    // The prefabs the build registers (Generator, registerPrefabManager): a script can spawn
    // any of them into this scene. One not loaded here is read from its file.
    std::vector<ShaderVariantContent> prefabContents;
    std::vector<std::unique_ptr<EntityRegistry>> fileRegistries;
    std::vector<std::unique_ptr<Scene>> previewScenes;
    for (const PrefabSceneInfo& info : collectAllPrefabs()) {
        ShaderVariantContent content;
        // A Prefab Variant, or a prefab holding instances of others: most of what an instance
        // draws is in the other prefabs and in what each level overrides of them, so its
        // content is an instance of it as the editor makes it (PREFABS.md 17, 18)
        const Prefab* loadedPrefab = getPrefab(info.prefabPath);
        bool layered = false;
        if (loadedPrefab && loadedPrefab->registry) {
            for (Entity entity : loadedPrefab->registryEntities) {
                if (loadedPrefab->isInstanceRoot(entity)) {
                    layered = true;
                    break;
                }
            }
        }
        if (layered) {
            auto previewScene = std::make_unique<Scene>();
            const Entity previewRoot = const_cast<Project*>(this)->instantiatePrefabPreview(previewScene.get(), info.prefabPath);
            if (previewRoot != NULL_ENTITY) {
                content.registry = previewScene.get();
                content.entities = previewScene->getEntityList();
                previewScenes.push_back(std::move(previewScene));
                prefabContents.push_back(std::move(content));
                continue;
            }
        }
        if (const Prefab* prefab = getPrefab(info.prefabPath); prefab && prefab->registry) {
            content.registry = prefab->registry.get();
            content.entities = prefab->registryEntities;
        } else {
            try {
                Prefab loaded;
                loaded.loadRegistry(DocumentFiles::read(getProjectPath() / info.prefabPath, DocumentKind::Prefab).root);
                content.entities = loaded.registryEntities;
                content.registry = loaded.registry.get();
                fileRegistries.push_back(std::move(loaded.registry));
            } catch (const std::exception& e) {
                Out::error("Failed to read prefab %s for its shaders: %s", info.prefabPath.string().c_str(), e.what());
                continue;
            }
        }
        prefabContents.push_back(std::move(content));
    }

    const bool scriptsPost = scriptsSetPostProcessing();
    ShaderVariantCollector::collectScene(sceneProject->scene, sceneContent, prefabContents, keys, getProjectPath(), scriptsSetLightCapacity(),
                                         scriptsPost, scriptsPost ? scriptsPostProcessingFields() : std::vector<std::string>(),
                                         scriptsSetShadowQuality(), scriptsMention("litQuality"), getMobilePbrTier());
    // a fluid a script makes (FluidSimulation in Lua or C++) needs its programs too
    if (scriptsMention("FluidSimulation"))
        ShaderVariantCollector::addFluidKeys(keys);
}

bool editor::Project::scriptsMention(const char* text) const {
    for (const fs::path& script : findProjectFiles([](const std::string& path) { return Util::isScriptFile(path); })) {
        std::ifstream file(getProjectPath() / script, std::ios::binary);
        if (!file) {
            continue;
        }
        const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (content.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool editor::Project::scriptsSetLightCapacity() const {
    // Scene.maxLightsPerObject (Lua) or Scene::setMaxLightsPerObject (C++): the one way the
    // light capacity tier of the lit variants changes at runtime
    return scriptsMention("axLightsPerObject");
}

bool editor::Project::scriptsSetShadowQuality() const {
    // scene.shadowQuality (Lua) or Scene::setShadowQuality (C++): the shadow tier, and with it
    // the High filters' variant bit, can change at runtime
    return scriptsMention("hadowQuality");
}

bool editor::Project::scriptsSetPostProcessing() const {
    // scene.postProcessing (Lua) or Scene::setPostProcessing (C++): the post-processing colour's
    // variant bit can change at runtime
    return scriptsMention("ostProcessing");
}

std::vector<std::string> editor::Project::scriptsPostProcessingFields() const {
    std::vector<std::string> named;
    std::vector<std::string> contents;
    for (const fs::path& script : findProjectFiles([](const std::string& path) { return Util::isScriptFile(path); })) {
        std::ifstream file(getProjectPath() / script, std::ios::binary);
        if (file)
            contents.emplace_back((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }
    for (const std::string& field : ShaderVariantCollector::getScriptableEffectFields()) {
        for (const std::string& content : contents) {
            if (content.find(field) != std::string::npos) {
                named.push_back(field);
                break;
            }
        }
    }
    return named;
}

std::set<ShaderKey> editor::Project::collectStandaloneShaderKeys() {
    // the generated main.cpp registers every scene stack, so any of them can be loaded
    std::set<ShaderKey> shaderKeys;
    for (const auto& sceneProject : getScenes()) {
        shaderKeys.insert(sceneProject.shaderKeys.begin(), sceneProject.shaderKeys.end());
        // stored keys are only as new as the last save of that scene
        collectSceneShaderKeys(&sceneProject, shaderKeys);
        collectRequiredShaderKeys(&sceneProject, shaderKeys);
    }
    return shaderKeys;
}

void editor::Project::buildStandaloneShaderCache(const std::set<ShaderKey>& shaderKeys) {
    ShaderBuilder builder;
    builder.buildMissingShaders(shaderKeys, this);
}

void editor::Project::invalidateCustomShaders() {
    invalidateCustomShaders([](const std::string&) { return true; });
}

void editor::Project::invalidateCustomShaders(const std::function<bool(const std::string& fork)>& which) {
    // Drop the editor's compiled cache for forked shaders, free their GPU handles, and
    // flag every renderable that uses one so RenderSystem recompiles it on the next draw.
    editor::ShaderBuilder::invalidateCustomShaders(which);
    ShaderPool::destroyCustomShaders(which);
    // fork manifests are read again from the edited files
    ShaderManifests::clearForks(which);

    auto fork = [&which](const std::string& shader) {
        return !shader.empty() && !ShaderReference::isLibrary(shader) && which(shader);
    };
    for (auto& sceneProject : getScenes()) {
        if (!sceneProject.scene)
            continue;
        Scene* scene = sceneProject.scene;
        // a component rides on a fork either via its own customShader or the scene default
        bool sceneMesh = fork(scene->getDefaultMeshShader());
        bool sceneUI = fork(scene->getDefaultUIShader());
        bool scenePoints = fork(scene->getDefaultPointsShader());
        bool sceneLines = fork(scene->getDefaultLinesShader());
        bool sceneSky = fork(scene->getDefaultSkyShader());

        // needReload is only consumed while the scene draws, and the idle loop skips
        // scenes that are not flagged, so the redraw has to be requested here. Saving a
        // shader file is not a command, so nothing else would request it.
        auto flagReload = [&sceneProject](bool& needReload, bool usesFork) {
            if (!usesFork)
                return;
            needReload = true;
            sceneProject.needUpdateRender = true;
        };

        for (Entity entity : sceneProject.entities) {
            if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)){
                // a material or its extra pass can name a fork too (library shaders do not change)
                bool materialFork = false;
                for (unsigned int s = 0; s < mesh->numSubmeshes; s++){
                    materialFork = materialFork || fork(mesh->submeshes[s].material.shader);
                    materialFork = materialFork || fork(mesh->submeshes[s].material.extraPass->shader);
                }
                flagReload(mesh->needReload, sceneMesh || materialFork || fork(mesh->customShader) || fork(mesh->customDepthShader));
            }
            if (UIComponent* ui = scene->findComponent<UIComponent>(entity))
                flagReload(ui->needReload, sceneUI || fork(ui->customShader));
            if (PointsComponent* pts = scene->findComponent<PointsComponent>(entity))
                flagReload(pts->needReload, scenePoints || fork(pts->customShader));
            if (LineRendererComponent* ln = scene->findComponent<LineRendererComponent>(entity))
                flagReload(ln->needReload, sceneLines || fork(ln->customShader));
            if (TrailRendererComponent* trail = scene->findComponent<TrailRendererComponent>(entity))
                flagReload(trail->needReload, sceneLines || fork(trail->customShader));
            if (SkyboxComponent* sky = scene->findComponent<SkyboxComponent>(entity))
                flagReload(sky->needReload, sceneSky || fork(sky->customShader));
        }

        // post-process passes hold their shader directly, so the chain is rebuilt whole
        bool postFork = false;
        for (const PostProcessPass& pass : scene->getPostProcessPasses())
            postFork = postFork || fork(pass.shader);
        if (postFork) {
            scene->getSystem<RenderSystem>()->needReloadPostProcess();
            sceneProject.needUpdateRender = true;
        }
    }
}

Entity editor::Project::getSceneCamera(const SceneProject* sceneProject) const {
    if (sceneProject->mainCamera != NULL_ENTITY && sceneProject->scene->isEntityCreated(sceneProject->mainCamera)) {
        return sceneProject->mainCamera;
    } else if (sceneProject->defaultCamera != NULL_ENTITY) {
        return sceneProject->defaultCamera;
    } else {
        return sceneProject->scene->getCamera();
    }
}

namespace {
    // Play reuses the edit-time Scene and Stop decodes the snapshot over it, both keeping each
    // fluid's simulator: restart them, so the game starts its fluids as an export does (the
    // seed, the start burst; seed 0 a new one each start), not from what an edit-mode preview
    // or eval_lua step() left, and Stop brings back the edit-mode start, not the game's last frame
    void restartFluids(Scene* scene) {
        if (!scene || !scene->isComponentRegistered<FluidSimulationComponent>()) return;
        auto fluids = scene->getComponentArray<FluidSimulationComponent>();
        for (size_t i = 0; i < fluids->size(); ++i) {
            FluidSimulationComponent& fluid = fluids->getComponentFromIndex(i);
            fluid.needReset = true;
            fluid.pendingSplats.clear();
            fluid.pendingBursts.clear();
            fluid.pendingSteps = 0;
            if (fluid.simulator) fluid.simulator->pointerCancelAll();
        }
    }
}

editor::SceneProject* editor::Project::runtimeOf(const PlayRuntimeScene& entry) {
    return entry.ownedRuntime ? entry.copy : getScene(entry.sourceSceneId);
}

Scene* editor::Project::runtimeSceneOf(const PlayRuntimeScene& entry) {
    SceneProject* runtime = runtimeOf(entry);
    return runtime ? runtime->scene : nullptr;
}

void editor::Project::prepareRuntimeScene(PlayRuntimeScene& entry) {
    SceneProject* runtime = runtimeOf(entry);
    if (!runtime || !runtime->scene) return;

    runtime->scene->getSystem<MeshSystem>()->setFoliagePreviewEntity(NULL_ENTITY);

    Entity camera = getSceneCamera(runtime);
    runtime->scene->setCamera(camera);

    // play reuses the edit-time Scene without a reload, so re-arm reflection
    // probes: "On Load" (and static capture-once) probes should capture the
    // game's starting state, not keep the edit-mode capture
    auto probes = runtime->scene->getComponentArray<ReflectionProbeComponent>();
    for (size_t i = 0; i < probes->size(); ++i) {
        ReflectionProbeComponent& probe = probes->getComponentFromIndex(i);
        probe.needUpdate = true;
        probe.captureRevision++;
    }

    restartFluids(runtime->scene);

    runtime->scene->getSystem<ActionSystem>()->resetRunningActions();
    // no animator reset here: the scripts' init already ran (onFinish, play, setAnimation...);
    // Stop resets them (finalizeStop), so play starts from their beginning

    runtime->scene->getSystem<UISystem>()->clearAnchorReferenceSize();
    runtime->scene->getSystem<UIDocumentSystem>()->clearReferenceSize();
    // a Play copy (a child scene, a scene loaded while playing) is only drawn by the game's
    // frames, which never show editor overlays (SceneRender::activateGame turns them off for
    // the played scene); with them on, its HTML UI documents would be skipped as in the Scene view
    if (entry.ownedRuntime){
        runtime->scene->getSystem<RenderSystem>()->setDrawEditorOverlays(false);
    }
    pauseEngineScene(runtime->scene, false);

    entry.initialized = true;

    // a Play copy runs as the played scene does: what the game makes in it is its own
    if (entry.ownedRuntime){
        runtime->playState = ScenePlayState::PLAYING;
    }

    if (runtime->sceneRender){
        runtime->sceneRender->setPlayMode(true);
    }
}

void editor::Project::cleanupPlaySession(const std::shared_ptr<PlaySession>& session) {
    if (!session) {
        return;
    }

    std::vector<PlayRuntimeScene> runtimeScenes;
    {
        std::scoped_lock lock(playSessionMutex);
        runtimeScenes.swap(session->runtimeScenes);
        runtimeScenes.insert(runtimeScenes.end(), session->retiredRuntimeScenes.begin(), session->retiredRuntimeScenes.end());
        session->retiredRuntimeScenes.clear();
    }

    // the DontDestroyOnLoad scene is about to go: SceneManager lets it go first
    for (const auto& entry : runtimeScenes) {
        if (entry.persistent && runtimeSceneOf(entry) && SceneManager::getPersistentScene() == runtimeSceneOf(entry)) {
            SceneManager::destroyPersistentScene();
        }
    }

    for (const auto& entry : runtimeScenes) {
        SceneManager::removeScenePtr(entry.sourceSceneId);

        if (entry.ownedRuntime && entry.copy) {
            SceneProject* runtime = entry.copy;
            const uint32_t runtimeId = runtime->id;
            releasePlayRuntimeId(runtime, entry.sourceSceneId);
            discardPlayRuntimeState(runtimeId, entry.sourceSceneId, session->mainSceneId);
            deleteSceneProject(runtime);
            delete runtime;
        }
    }
}

bool editor::Project::loadScene(fs::path filepath, bool opened, bool isNewScene, bool loadSceneData){
    // Both outside the try so a failure can undo exactly what was built. The file is
    // parsed before anything is appended, so only this call's own entry may be popped.
    SceneProject* targetScene = nullptr;
    bool appendedScene = false;
    try {
        fs::path fullPath = filepath;
        if (fullPath.is_relative()) {
            fullPath = getProjectPath() / fullPath;
        }

        editor::getEditorHost().reportLoadingProgress("Loading scenes...");
        // The report collects what the decode cannot read; it lives on the scene and gates
        // saves that would drop it (docs/data-safety.md)
        LoadReport loadReport;
        const fs::path shownPath = fullPath.lexically_relative(getProjectPath());
        loadReport.documentPath = FileUtils::pathToUtf8(shownPath.empty() ? fullPath : shownPath);
        // a crash backup of this file: restored, discarded or kept (R17-R19)
        std::string restoredBytes;
        const bool restoring = loadSceneData && opened && !shownPath.empty() && resolveSceneRecovery(shownPath.lexically_normal(), restoredBytes);
        std::unique_ptr<ScopedLoadReport> reportScope = std::make_unique<ScopedLoadReport>(loadReport);
        DocumentFile document = restoring ? DocumentFiles::fromBytes(std::move(restoredBytes), loadReport.documentPath, DocumentKind::Scene) :
                DocumentFiles::read(fullPath, DocumentKind::Scene);
        // the file's bytes are what a later backup and save build on, even when restoring
        const std::string diskSha1 = restoring ? lastRecovery.backup.baseSha1 : document.sha1;
        YAML::Node sceneNode = document.root;

        if (isNewScene) {
            scenes.emplace_back();
            appendedScene = true;
            targetScene = &scenes.back();
            std::error_code ec;
            fs::path relPath = fs::relative(fullPath, getProjectPath(), ec);
            if (ec || relPath.empty()) {
                scenes.pop_back();
                appendedScene = false;
                Out::error("Scene filepath must be relative to project path: %s", fullPath.string().c_str());
                return false;
            }
            targetScene->filepath = relPath;
        } else {
            auto it = std::find_if(scenes.begin(), scenes.end(),
                [this, &fullPath](const SceneProject& scene) { 
                    fs::path scenePath = scene.filepath;
                    if (scenePath.is_relative()) {
                        scenePath = getProjectPath() / scenePath;
                    }
                    return scenePath == fullPath; 
                });
            targetScene = &(*it);

            if (targetScene->scene != nullptr || targetScene->sceneRender != nullptr) {
                Out::error("Scene is already loaded");
                return false;
            }
        }

        Stream::decodeSceneProject(targetScene, sceneNode, loadSceneData);
        targetScene->diskSha1 = diskSha1;
        // the build's prefab list of a closed scene follows the prefabs' GUIDs too
        resolveScenePrefabInfos(*targetScene);

        // Before the renderer exists, so the camera restore below picks it up
        applyRetainedSceneState(*targetScene);

        if (loadSceneData){
            targetScene->sceneRender = createSceneRender(targetScene->sceneType, targetScene->scene);

            // Null when decoding produced no Scene, as a file that parses as YAML but
            // holds nothing usable does. Everything below assumes both exist.
            if (!targetScene->sceneRender) {
                Out::error("Scene file has no usable scene data: %s", fullPath.string().c_str());
                alertUser("Error", "Failed to open scene file!");
                discardHalfLoadedScene(targetScene, appendedScene);
                return false;
            }

            if (targetScene->editorCameraState.IsDefined()) {
                Camera* editorCam = targetScene->sceneRender->getCamera();
                if (editorCam) {
                    float zoom = 0.0f;
                    float walkSpeedOffset = 0.0f;
                    Stream::decodeEditorCamera(editorCam, targetScene->editorCameraState, zoom, walkSpeedOffset);
                    if ((targetScene->sceneType == SceneType::SCENE_2D || targetScene->sceneType == SceneType::SCENE_UI) && zoom > 0.0f) {
                        static_cast<SceneRender2D*>(targetScene->sceneRender)->setZoom(zoom);
                    }
                    if (targetScene->sceneType == SceneType::SCENE_3D) {
                        static_cast<SceneRender3D*>(targetScene->sceneRender)->setWalkSpeedOffset(walkSpeedOffset);
                    }
                }
            }

            loadSceneProjectData(targetScene, sceneNode);
        }
        reportScope.reset();
        if (loadSceneData) {
            targetScene->loadReport = std::move(loadReport);
            logLoadReport(targetScene->loadReport);
        }

        if (opened){
            // Sync linked materials with latest file contents on disk
            for (Entity entity : targetScene->entities) {
                editor::getEditorHost().reportLoadingProgress();
                MeshComponent* mesh = targetScene->scene->findComponent<MeshComponent>(entity);
                if (!mesh) continue;

                for (unsigned int submeshIndex = 0; submeshIndex < mesh->numSubmeshes; submeshIndex++) {
                    Material& material = mesh->submeshes[submeshIndex].material;
                    if (material.name.empty()) continue;

                    std::string normalizedPath = fs::path(material.name).lexically_normal().generic_string();

                    fs::path materialPath = projectPath / normalizedPath;
                    if (!fs::exists(materialPath) || fs::is_directory(materialPath)) continue;

                    try {
                        YAML::Node materialNode = DocumentFiles::read(materialPath, DocumentKind::Material).root;
                        Material fileMaterial = Stream::decodeMaterial(materialNode);
                        fileMaterial.name = AssetRef(normalizedPath, material.name.guid);

                        if (material != fileMaterial) {
                            const uint64_t updateFlags = Catalog::getMaterialUpdateFlags(material, fileMaterial);
                            material = fileMaterial;
                            mesh->submeshes[submeshIndex].needUpdateTexture = true;
                            Catalog::updateEntity(targetScene->scene, entity, updateFlags);
                            targetScene->needUpdateRender = true;
                        }
                    } catch (const std::exception& e) {
                        Out::error("Error loading linked material file '%s': %s", materialPath.string().c_str(), e.what());
                    }
                }
            }

            setSelectedSceneId(targetScene->id);

            editor::getEditorHost().addNewSceneToDock(targetScene->id);
        }

        targetScene->needUpdateRender = opened;
        // A prefab its file names by an old path (moved since, found by GUID) is saved
        // with the new one
        targetScene->isModified = loadSceneData && (targetScene->prefabRefsChanged || restoring);
        targetScene->opened = opened;

        if (opened) {
            addTab(TabType::SCENE, targetScene->filepath.string());
        }

        // Random ids make this unlikely, but a project merged under the old counter
        // can still arrive with two scenes claiming one id
        SceneProject* existing = getScene(targetScene->id);
        if (targetScene->id == NULL_PROJECT_SCENE || (existing && existing != targetScene)) {
            uint32_t oldId = targetScene->id;
            targetScene->id = allocateSceneId();
            if (oldId != NULL_PROJECT_SCENE) {
                Out::warning("Scene with ID '%u' already exists, using ID %u. References to the old id may need to be set again.", oldId, targetScene->id);
            } else {
                Out::warning("Scene has no ID, assigning ID %u", targetScene->id);
            }
        }

        for (LoadIssue& issue : targetScene->loadReport.issues) {
            issue.sceneId = targetScene->id;
        }

        if (opened && !isNewScene) {
            removeMissingChildSceneReferences(*targetScene);
        }

        // The path answers again, so it is no longer one the project only knows by
        // name. Reordering invalidates targetScene, which must not be used after.
        const fs::path loadedPath = targetScene->filepath;
        if (appendedScene) {
            restoreUnresolvedScenePosition(loadedPath);
        }
        clearUnresolvedScene(loadedPath);
        return true;

    } catch (const YAML::Exception& e) {
        discardHalfLoadedScene(targetScene, appendedScene);
        Out::error("Failed to open scene: %s", e.what());
        alertUser("Error", "Failed to open scene file!");
    } catch (const std::exception& e) {
        discardHalfLoadedScene(targetScene, appendedScene);
        Out::error("Failed to open scene: %s", e.what());
        alertUser("Error", "Failed to open scene file!");
    }

    return false;
}

uint32_t editor::Project::loadUntitledScene(const std::string& text, const std::string& displayName) {
    SceneProject* targetScene = nullptr;
    try {
        LoadReport loadReport;
        loadReport.documentPath = displayName;
        std::unique_ptr<ScopedLoadReport> reportScope = std::make_unique<ScopedLoadReport>(loadReport);
        DocumentFile document = DocumentFiles::fromBytes(text, displayName, DocumentKind::Scene);

        scenes.emplace_back();
        targetScene = &scenes.back();
        Stream::decodeSceneProject(targetScene, document.root, true);
        targetScene->sceneRender = createSceneRender(targetScene->sceneType, targetScene->scene);
        if (!targetScene->sceneRender) {
            throw std::runtime_error("no usable scene data");
        }
        loadSceneProjectData(targetScene, document.root);
        reportScope.reset();
        targetScene->loadReport = std::move(loadReport);
        logLoadReport(targetScene->loadReport);

        // never written: no path, a transient id unless the backup's is free
        SceneProject* existing = getScene(targetScene->id);
        if (targetScene->id == NULL_PROJECT_SCENE || (existing && existing != targetScene)) {
            targetScene->id = allocateSceneId();
        }
        targetScene->filepath.clear();
        targetScene->transientId = true;
        targetScene->opened = true;
        targetScene->isVisible = true;
        targetScene->needUpdateRender = true;
        targetScene->isModified = true;
        const uint32_t id = targetScene->id;
        setSelectedSceneId(id);
        editor::getEditorHost().addNewSceneToDock(id);
        Out::info("Restored the untitled scene \"%s\" from its auto-saved copy; save it to keep it.", targetScene->name.c_str());
        return id;
    } catch (const std::exception& e) {
        Out::error("Could not restore the untitled scene %s: %s", displayName.c_str(), e.what());
        if (targetScene) {
            discardHalfLoadedScene(targetScene, true);
        }
    }
    return NULL_PROJECT_SCENE;
}

// A scene loadScene() appended goes away; one already in the project is put back the
// way a closed scene looks, since half-initialised pointers leak and fail every retry
// with "Scene is already loaded".
void editor::Project::discardHalfLoadedScene(SceneProject* sceneProject, bool appended){
    if (!sceneProject) {
        return;
    }

    const uint32_t sceneId = sceneProject->id;
    deleteSceneProject(sceneProject);

    if (sceneId != NULL_PROJECT_SCENE) {
        unlinkAllMaterialFiles(sceneId);
        cleanupPrefabsForScene(sceneId);
    }

    if (appended && !scenes.empty()) {
        scenes.pop_back();
    } else {
        sceneProject->opened = false;
        sceneProject->expandedInline = false;
        sceneProject->isModified = false;
        sceneProject->needUpdateRender = false;
    }
}

std::function<bool(const std::filesystem::path&, std::string&)> editor::Project::documentOpenGuard;
std::function<void(const std::filesystem::path&, std::function<void()>)> editor::Project::readOnlySaveHandler;

void editor::Project::openScene(fs::path filepath, bool closePrevious){
    if (isAnyScenePlaying()){
        Out::warning("Cannot open a new scene while a scene is playing.");
        return;
    }
    if (documentOpenGuard) {
        std::string why;
        const fs::path absolute = filepath.is_relative() ? projectPath / filepath : filepath;
        if (!documentOpenGuard(absolute, why)) {
            Out::warning("%s", why.c_str());
            editor::getEditorHost().registerAlert("Open Scene", why);
            return;
        }
    }

    uint32_t sceneToClose = NULL_PROJECT_SCENE;
    if (closePrevious) {
        SceneProject* selectedScene = getSelectedScene();
        if (selectedScene) {
            sceneToClose = selectedScene->id;
        }
    }

    checkUnsavedAndExecute(sceneToClose, [this, filepath, sceneToClose]() {
        // answered later (the unsaved changes dialog): Play may have started meanwhile
        if (isAnyScenePlaying()) {
            Out::warning("Cannot open a new scene while a scene is playing.");
            return;
        }
        openSceneInternal(filepath, sceneToClose);
    });
}

void editor::Project::openSceneInternal(fs::path filepath, uint32_t sceneToClose){
    if (filepath.is_relative()) {
        filepath = getProjectPath() / filepath;
    }

    auto it = std::find_if(scenes.begin(), scenes.end(),
        [this, &filepath](const SceneProject& scene) { 
            fs::path scenePath = scene.filepath;
            if (scenePath.is_relative()) {
                scenePath = getProjectPath() / scenePath;
            }
            return scenePath == filepath; 
        });

    if (it != scenes.end()) {
        if (it->opened) {
            setSelectedSceneId(it->id);
            if (sceneToClose != NULL_PROJECT_SCENE && sceneToClose != it->id) {
                closeScene(sceneToClose, true);
            }
            return;
        }
        // If expanded inline, unload inline data first so loadScene can reload fully
        if (it->expandedInline) {
            unloadChildSceneInline(it->id);
        }
        // Scene exists in project but is closed
        if (sceneToClose != NULL_PROJECT_SCENE && sceneToClose != it->id) {
            closeScene(sceneToClose, true);
        }
        loadScene(filepath, true, false, true);
        // The project already lists this scene; only the open tabs changed.
        saveWorkspaceFile();
        return;
    }

    // Scene is not in project
    editor::getEditorHost().registerConfirmAlert(
        "Add Scene",
        "This scene is not part of the current project. Do you want to add it?",
        [this, filepath, sceneToClose]() {
            if (sceneToClose != NULL_PROJECT_SCENE) {
                closeScene(sceneToClose, true);
            }
            loadScene(filepath, true, true, true);
            saveProjectFile();
        },
        []() {
            // Do nothing
        }
    );
}

bool editor::Project::reloadSceneFromDisk(uint32_t sceneId, std::string* error) {
    auto fail = [error](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    if (isAnyScenePlaying()) return fail("A scene is playing: stop it to reload a file from disk.");
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || sceneProject->filepath.empty()) return fail("The scene has no file to reload.");
    fs::path fullPath = sceneProject->filepath;
    if (fullPath.is_relative()) fullPath = getProjectPath() / fullPath;

    if (getPrefabStage(sceneId)) {
        // Prefab Mode: the stage is left without saving and opened on the file again
        const fs::path prefab = fullPath;
        const size_t depth = getPrefabStageDepth(sceneId);
        closePrefabStages(depth > 0 ? depth - 1 : 0, [this, prefab](bool closed) {
            if (closed) openPrefabStage(prefab);
        }, 2);
        return true;
    }

    // read it first: a file that does not parse leaves the open copy as it is
    try {
        DocumentFiles::read(fullPath, DocumentKind::Scene);
    } catch (const std::exception& e) {
        return fail(std::string("Cannot reload ") + sceneProject->name + ": " + e.what());
    }
    const bool selected = selectedScene == sceneId;
    editor::getEditorHost().clearSceneWindowState(sceneId);
    deleteSceneProject(sceneProject);
    // the steps were made on the copy the file replaces (S24 R23, as when a scene closes)
    CommandHandle::remove(sceneId);
    if (!loadScene(fullPath, true, false)) return fail("Cannot reload " + fullPath.filename().string() + ": it did not load (see the Console).");
    sceneProject = getScene(sceneId);
    if (sceneProject) {
        sceneProject->isModified = false;
        sceneProject->needUpdateRender = true;
    }
    if (selected) setSelectedSceneId(sceneId);
    markParentScenesNeedUpdate(sceneId);
    Out::info("Reloaded \"%s\" from disk", fullPath.filename().string().c_str());
    return true;
}

bool editor::Project::refuseSceneRemovalWhilePlaying(const std::string& what, std::string* reason, bool alert) {
    if (!isAnyScenePlaying()) {
        return false;
    }
    const std::string message = "Cannot " + what + " while a scene is playing: stop Play mode first.";
    Out::warning("%s", message.c_str());
    if (reason) {
        *reason = message;
    }
    if (alert) {
        alertUser("Scene is playing", message);
    }
    return true;
}

void editor::Project::closeScene(uint32_t sceneId, bool systemClose) {
    auto it = std::find_if(scenes.begin(), scenes.end(),
        [sceneId](const SceneProject& scene) { return scene.id == sceneId; });

    if (it == scenes.end() || !it->opened) {
        return;
    }

    // Count opened scenes
    int openedCount = 0;
    for (const auto& scene : scenes) {
        if (scene.opened) {
            openedCount++;
        }
    }

    // a never-saved scene leaves the project as it closes, and the played one is in use
    if ((it->filepath.empty() || it->id == getPlaySessionSceneId()) &&
        refuseSceneRemovalWhilePlaying("close scene '" + it->name + "'")) {
        return;
    }

    if (!systemClose){
        if (openedCount == 1) {
            Out::error("Cannot close last scene");
            return;
        }

        if (selectedScene == sceneId) {
            Out::error("Scene is selected, cannot close it");
            return;
        }
    }

    // Debounced writes still need the live scene to encode the material from
    editor::getEditorHost().flushSceneMaterialWrites(sceneId);

    // closed with changes not saved: they were discarded, and so is their backup (R16)
    if (it->isModified) {
        deleteSceneBackup(*it, it->filepath);
    }

    deleteSceneProject(&(*it));

    cleanupPrefabsForScene(sceneId);

    removeTab(TabType::SCENE, it->filepath.string());

    // Notify parent scenes so they rebuild their layers without the deleted scene
    markParentScenesNeedUpdate(sceneId);

    // If the scene was never saved, remove it entirely
    const bool removedFromProject = it->filepath.empty();
    if (removedFromProject) {
        // Nothing may stay pointing at an id no scene answers to any more
        purgeReferencesToScene(it->id);
        unlinkAllMaterialFiles(it->id);
        CommandHandle::remove(it->id);

        // setSelectedSceneId() early-outs on an unchanged id, so drop it here to let
        // the replacement scene run through selection again
        if (selectedScene == it->id) {
            selectedScene = NULL_PROJECT_SCENE;
        }
        if (selectedSceneForProperties == it->id) {
            selectedSceneForProperties = NULL_PROJECT_SCENE;
        }

        scenes.erase(it);
    } else {
        it->opened = false;
        it->expandedInline = false;
        // a closed scene keeps no history (S24 R23)
        CommandHandle::remove(sceneId);
        // closed: its scripts are its file's again, as at project load (an unsaved edit may have
        // changed them), for Play, exports and script builds (collectAllSceneCppScripts)
        readSceneFileCppScripts(*it);
    }

    editor::getEditorHost().clearSceneWindowState(sceneId);

    if (!systemClose){
        if (removedFromProject) {
            saveProjectFile();
        } else {
            saveWorkspaceFile();
        }
    }
}

void editor::Project::removeScene(uint32_t sceneId) {
    auto it = std::find_if(scenes.begin(), scenes.end(),
        [sceneId](const SceneProject& scene) { return scene.id == sceneId; });

    if (it == scenes.end()) {
        return;
    }

    if (scenes.size() <= 1) {
        Out::error("Cannot remove last scene");
        return;
    }

    if (refuseSceneRemovalWhilePlaying("remove scene '" + it->name + "'")) {
        return;
    }

    // If selected, select another scene
    if (selectedScene == sceneId) {
         // Try to select the first one that is not this one
        for (const auto& scene : scenes) {
            if (scene.id != sceneId) {
                setSelectedSceneId(scene.id);
                break;
            }
        }
    }

    // Cleanup resources
    deleteSceneProject(&(*it));

    // Remove C++ source file
    generator.clearSceneSource(it->id, it->name, getProjectInternalPath());

    // Cleanup Prefabs
    cleanupPrefabsForScene(sceneId);

    removeTab(TabType::SCENE, it->filepath.string());
    editor::getEditorHost().clearSceneWindowState(sceneId);
    scenes.erase(it);

    if (startSceneId == sceneId) {
        startSceneId = NULL_PROJECT_SCENE;
    }
    if (loadingSettings.sceneId == sceneId) {
        loadingSettings.sceneId = NULL_PROJECT_SCENE;
    }

    // CMakeLists.txt still lists its source
    generatedSourcesDirty.store(true);
}

std::vector<std::filesystem::path> editor::Project::findSceneFiles() const {
    return findProjectFiles(&Util::isSceneFile);
}

uint32_t editor::Project::findSceneByPath(const std::filesystem::path& filepath) const {
    const fs::path normalized = normalizeToProjectRelative(filepath);
    if (normalized.empty()) {
        return NULL_PROJECT_SCENE;
    }

    for (const SceneProject& sceneProject : scenes) {
        if (sceneProject.filepath.lexically_normal() == normalized) {
            return sceneProject.id;
        }
    }

    return NULL_PROJECT_SCENE;
}

//=== Non-interactive scene and project APIs ===

namespace {

// Counts a non-interactive API in: Project::alertUser() only logs until it returns
struct NonInteractiveScope {
    int& depth;
    explicit NonInteractiveScope(int& depth) : depth(depth) { ++this->depth; }
    ~NonInteractiveScope() { --depth; }
};

// Logged as a warning: the caller (an agent, a script) gets the message back, and the
// Console's errors stay the project's own
bool failWith(std::string* error, const std::string& message) {
    editor::Out::warning(message);
    if (error) *error = message;
    return false;
}

// A failure of a callee that only logs: the errors it logged since the cursor
// (Out::lastLogId) are the message, else the fallback (logged)
bool failWithLogged(std::string* error, uint64_t logId, const std::string& fallback) {
    std::string text;
    for (const editor::LogEntry& entry : editor::Out::getLogsSince(logId, 0, true)) {
        if (entry.type != editor::LogType::Error) continue;
        if (!text.empty()) text += "\n";
        text += entry.message;
    }
    if (text.empty()) {
        return failWith(error, fallback);
    }
    if (error) *error = text;
    return false;
}

}

void editor::Project::alertUser(const std::string& title, const std::string& message) {
    if (nonInteractiveDepth > 0) {
        return;  // the caller reports it; the message was logged already
    }
    editor::getEditorHost().registerAlert(title, message);
}

fs::path editor::Project::resolveScenePath(const fs::path& path, bool withExtension, std::string& error) const {
    if (path.empty()) {
        error = "No scene path given";
        return {};
    }
    fs::path fullPath = path.is_relative() ? getProjectPath() / path : path;
    fullPath = fullPath.lexically_normal();
    if (withExtension && fullPath.extension() != ".scene") {
        fullPath += ".scene";
    }
    std::error_code ec;
    const fs::path relPath = fs::relative(fullPath, getProjectPath(), ec);
    if (ec || relPath.empty() || *relPath.begin() == ".." || relPath == ".") {
        error = "Scene path must be inside the project folder: " + path.generic_string();
        return {};
    }
    return fullPath;
}

bool editor::Project::settleUnsavedScene(uint32_t sceneId, UnsavedChoice unsaved, std::string& error) {
    const SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || !hasSceneUnsavedChanges(sceneId)) {
        return true;
    }
    switch (unsaved) {
        case UnsavedChoice::Save:
            return saveSceneNow(sceneId, {}, &error);
        case UnsavedChoice::Discard:
            return true;
        default:
            error = "Scene '" + sceneProject->name + "' has unsaved changes: save or discard them first";
            return false;
    }
}

uint32_t editor::Project::createSceneNow(const std::string& name, SceneType type, const fs::path& saveAs,
                                        bool closePrevious, UnsavedChoice unsaved, std::string* error) {
    NonInteractiveScope scope(nonInteractiveDepth);
    std::string message;
    if (isAnyScenePlaying()) {
        failWith(error, "Cannot create a scene while a scene is playing");
        return NULL_PROJECT_SCENE;
    }

    fs::path savePath;
    if (!saveAs.empty()) {
        savePath = resolveScenePath(saveAs, true, message);
        if (savePath.empty()) {
            failWith(error, message);
            return NULL_PROJECT_SCENE;
        }
        if (findSceneByPath(savePath) != NULL_PROJECT_SCENE) {
            failWith(error, "Another scene of the project is saved at " + normalizeToProjectRelative(savePath).generic_string());
            return NULL_PROJECT_SCENE;
        }
    }

    uint32_t previousSceneId = NULL_PROJECT_SCENE;
    if (closePrevious) {
        previousSceneId = getSceneBehindPrefabStage(getSelectedSceneId());
        if (!getScene(previousSceneId) || isPlayRuntimeScene(previousSceneId)) {
            previousSceneId = NULL_PROJECT_SCENE;
        }
        if (previousSceneId != NULL_PROJECT_SCENE && !settleUnsavedScene(previousSceneId, unsaved, message)) {
            failWith(error, message);
            return NULL_PROJECT_SCENE;
        }
    }

    std::string sceneName = name;
    if (sceneName.empty()) {
        sceneName = savePath.empty() ? std::string("New Scene") : savePath.stem().string();
    }

    // closes previousSceneId (discarding what settleUnsavedScene left unsaved)
    const uint32_t sceneId = createNewSceneInternal(sceneName, type, previousSceneId);
    if (sceneId == NULL_PROJECT_SCENE || !getScene(sceneId)) {
        failWith(error, "Scene '" + sceneName + "' could not be created");
        return NULL_PROJECT_SCENE;
    }
    if (previousSceneId != NULL_PROJECT_SCENE && previousSceneId != sceneId) {
        if (SceneProject* previous = getScene(previousSceneId)) {
            previous->isModified = false;  // reloads from its file when opened again
        }
    }
    saveWorkspaceFile();

    if (!savePath.empty()) {
        saveSceneNow(sceneId, savePath, error);
    }
    return sceneId;
}

uint32_t editor::Project::openSceneNow(const fs::path& path, bool closePrevious, UnsavedChoice unsaved, std::string* error) {
    NonInteractiveScope scope(nonInteractiveDepth);
    std::string message;
    if (isAnyScenePlaying()) {
        failWith(error, "Cannot open a scene while a scene is playing");
        return NULL_PROJECT_SCENE;
    }

    const fs::path fullPath = resolveScenePath(path, false, message);
    if (fullPath.empty()) {
        failWith(error, message);
        return NULL_PROJECT_SCENE;
    }
    if (documentOpenGuard && !documentOpenGuard(fullPath, message)) {
        failWith(error, message);
        return NULL_PROJECT_SCENE;
    }
    std::error_code ec;
    if (!fs::is_regular_file(fullPath, ec)) {
        failWith(error, "Scene file not found: " + normalizeToProjectRelative(fullPath).generic_string());
        return NULL_PROJECT_SCENE;
    }

    uint32_t previousSceneId = NULL_PROJECT_SCENE;
    if (closePrevious) {
        previousSceneId = getSceneBehindPrefabStage(getSelectedSceneId());
        if (!getScene(previousSceneId) || isPlayRuntimeScene(previousSceneId)) {
            previousSceneId = NULL_PROJECT_SCENE;
        }
    }

    uint32_t sceneId = findSceneByPath(fullPath);
    if (previousSceneId == sceneId) {
        previousSceneId = NULL_PROJECT_SCENE;
    }
    if (previousSceneId != NULL_PROJECT_SCENE && !settleUnsavedScene(previousSceneId, unsaved, message)) {
        failWith(error, message);
        return NULL_PROJECT_SCENE;
    }

    SceneProject* listed = getScene(sceneId);
    if (listed && listed->opened && listed->scene) {
        setSelectedSceneId(sceneId);
    } else {
        const uint64_t logId = Out::lastLogId();
        bool loaded = false;
        if (listed) {
            // a closed scene of the project: shown inline as a child, it reloads fully
            if (listed->expandedInline) {
                unloadChildSceneInline(sceneId);
            }
            loaded = loadScene(fullPath, true, false, true);
            if (loaded) saveWorkspaceFile();
        } else {
            loaded = loadScene(fullPath, true, true, true);
            if (loaded) saveProjectFile();
        }
        sceneId = findSceneByPath(fullPath);
        if (!loaded || sceneId == NULL_PROJECT_SCENE) {
            failWithLogged(error, logId, "Failed to open scene " + normalizeToProjectRelative(fullPath).generic_string());
            return NULL_PROJECT_SCENE;
        }
        setSelectedSceneId(sceneId);
    }

    if (previousSceneId != NULL_PROJECT_SCENE && previousSceneId != sceneId) {
        // what it had unsaved was settled above (saved, or Discard)
        closeScene(previousSceneId, true);
        if (SceneProject* previous = getScene(previousSceneId)) {
            previous->isModified = false;  // reloads from its file when opened again
        }
        saveWorkspaceFile();
    }
    return sceneId;
}

bool editor::Project::closeSceneNow(uint32_t sceneId, UnsavedChoice unsaved, std::string* error) {
    NonInteractiveScope scope(nonInteractiveDepth);
    std::string message;
    if (isAnyScenePlaying()) {
        return failWith(error, "Cannot close a scene while a scene is playing");
    }
    if (isPrefabStage(sceneId) || isPlayRuntimeScene(sceneId)) {
        return failWith(error, "Scene " + std::to_string(sceneId) + " is not a project scene (a prefab stage closes with Prefab Mode)");
    }
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject) {
        return failWith(error, "No scene with id " + std::to_string(sceneId));
    }
    if (!sceneProject->opened) {
        return true;
    }

    uint32_t replacement = NULL_PROJECT_SCENE;
    for (const SceneProject& other : scenes) {
        if (other.opened && other.id != sceneId) {
            replacement = other.id;
            break;
        }
    }
    if (replacement == NULL_PROJECT_SCENE) {
        return failWith(error, "Cannot close the last open scene: open or create another one first");
    }
    if (!settleUnsavedScene(sceneId, unsaved, message)) {
        return failWith(error, message);
    }

    if (getSceneBehindPrefabStage(getSelectedSceneId()) == sceneId || getSelectedSceneId() == sceneId) {
        setSelectedSceneId(replacement);
    }
    const bool untitled = sceneProject->filepath.empty();
    closeScene(sceneId, true);
    if (SceneProject* closed = getScene(sceneId)) {
        closed->isModified = false;  // reloads from its file when opened again
    }
    if (untitled) {
        saveProjectFile();
    } else {
        saveWorkspaceFile();
    }
    return true;
}

bool editor::Project::saveSceneNow(uint32_t sceneId, const fs::path& pathIfUntitled, std::string* error,
        DegradedSavePolicy policy, SaveResult* outcome) {
    NonInteractiveScope scope(nonInteractiveDepth);
    std::string message;
    if (isPrefabStage(sceneId)) {
        // decided here: an API save never meets the editor's modal
        if (policy == DegradedSavePolicy::Refuse && isPrefabStageDegraded(sceneId)) {
            if (outcome) *outcome = SaveResult::RefusedDegraded;
            return failWith(error, "The prefab of the stage did not load completely; saving it would permanently remove values "
                                   "this editor could not read");
        }
        const uint64_t logId = Out::lastLogId();
        if (!savePrefabStage(sceneId, policy)) {
            return failWithLogged(error, logId, "The prefab of the stage could not be saved");
        }
        if (outcome) *outcome = SaveResult::Saved;
        return true;
    }
    if (isPlayRuntimeScene(sceneId)) {
        return failWith(error, "A Play copy is not saved: Stop puts the scene back");
    }
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject) {
        return failWith(error, "No scene with id " + std::to_string(sceneId));
    }
    if (sceneProject->playState != ScenePlayState::STOPPED) {
        return failWith(error, "Cannot save scene '" + sceneProject->name + "' while it is playing or busy");
    }
    if (!sceneProject->scene) {
        return true;  // closed: nothing in memory to write
    }

    fs::path target;
    if (!sceneProject->filepath.empty()) {
        target = getProjectPath() / sceneProject->filepath;
        if (!pathIfUntitled.empty()) {
            const fs::path requested = resolveScenePath(pathIfUntitled, true, message);
            if (requested.empty()) {
                return failWith(error, message);
            }
            if (requested.lexically_normal() != target.lexically_normal()) {
                return failWith(error, "Scene '" + sceneProject->name + "' is saved at " + sceneProject->filepath.generic_string() +
                                       " already (there is no Save As)");
            }
        }
    } else {
        if (pathIfUntitled.empty()) {
            return failWith(error, "Scene '" + sceneProject->name + "' was never saved: give the path to save it at");
        }
        target = resolveScenePath(pathIfUntitled, true, message);
        if (target.empty()) {
            return failWith(error, message);
        }
        const uint32_t owner = findSceneByPath(target);
        if (owner != NULL_PROJECT_SCENE && owner != sceneId) {
            return failWith(error, "Another scene of the project is saved at " + normalizeToProjectRelative(target).generic_string());
        }
        std::error_code ec;
        // (in a write scope: beneath its root)
        FileUtils::createDirectories(target.parent_path(), ec);
        if (ec) {
            return failWith(error, "Could not create " + target.parent_path().generic_string() + ": " + ec.message());
        }
    }

    const uint64_t logId = Out::lastLogId();
    SaveResult result = SaveResult::Failed;
    if (!saveSceneToPath(sceneId, target, true, policy, &result)) {
        if (outcome) *outcome = result;
        if (result == SaveResult::RefusedDegraded) {
            return failWith(error, "\"" + normalizeToProjectRelative(target).generic_string() + "\" did not load completely; saving it would "
                                   "permanently remove " + std::to_string(sceneProject->loadReport.dropped()) + " value(s) this editor could not read");
        }
        return failWithLogged(error, logId, "Failed to save scene " + target.generic_string());
    }
    if (outcome) *outcome = SaveResult::Saved;

    // what Save does next: the child scenes with changes of their own
    std::vector<uint32_t> involvedIds;
    collectInvolvedScenes(sceneId, involvedIds);
    for (uint32_t childId : involvedIds) {
        if (childId == sceneId || !hasLocalUnsavedChanges(childId)) continue;
        if (!saveSceneNow(childId, {}, &message)) {
            return failWith(error, "Scene saved, but its child scene failed: " + message);
        }
    }
    return true;
}

bool editor::Project::isNonAssetTopLevelDir(const std::string& name) {
    return name.empty() || name[0] == '.' || name == "build" || name == "Recordings" || name == "EditorSettings";
}

bool editor::Project::isNonAssetDir(const fs::path& dir) const {
    const std::string name = FileUtils::pathToUtf8(dir.filename());
    if (name.empty() || name[0] == '.' || name == "build") return true;
    std::error_code ec;
    return (name == "Recordings" || name == "EditorSettings") && fs::equivalent(dir.parent_path(), projectPath, ec);
}

bool editor::Project::createProjectAt(const fs::path& dir, const std::string& name, std::string* error) {
    NonInteractiveScope scope(nonInteractiveDepth);
    if (isAnyScenePlaying()) {
        return failWith(error, "Cannot create a project while a scene is playing");
    }
    if (dir.empty()) {
        return failWith(error, "No project directory given");
    }
    std::error_code ec;
    const fs::path projectDir = fs::absolute(dir, ec).lexically_normal();
    if (ec) {
        return failWith(error, "Invalid project directory " + dir.string() + ": " + ec.message());
    }
    if (fs::exists(projectDir / "project.yaml", ec)) {
        return failWith(error, "A project exists in " + projectDir.string() + " already: open it instead");
    }
    if (fs::exists(projectDir, ec) && !fs::is_directory(projectDir, ec)) {
        return failWith(error, projectDir.string() + " is a file, not a directory");
    }

    std::string projectName = name;
    if (projectName.empty()) {
        projectName = projectDir.filename().string();
    }

    const uint64_t logId = Out::lastLogId();
    if (!createProjectInDirectory(projectDir, projectName, false) || getProjectPath() != projectDir) {
        return failWithLogged(error, logId, "Failed to create the project in " + projectDir.string());
    }
    return true;
}

bool editor::Project::openProjectNow(const fs::path& path, std::string* error) {
    NonInteractiveScope scope(nonInteractiveDepth);
    if (isAnyScenePlaying()) {
        return failWith(error, "Cannot open a project while a scene is playing");
    }
    std::error_code ec;
    fs::path projectDir = fs::absolute(path, ec).lexically_normal();
    if (ec || path.empty()) {
        return failWith(error, "Invalid project path " + path.string());
    }
    if (projectDir.filename() == "project.yaml") {
        projectDir = projectDir.parent_path();
    }
    if (!fs::exists(projectDir / "project.yaml", ec)) {
        return failWith(error, "No project.yaml in " + projectDir.string());
    }

    const uint64_t logId = Out::lastLogId();
    if (!loadProject(projectDir)) {
        return failWithLogged(error, logId, "Failed to open the project in " + projectDir.string());
    }
    return true;
}

void editor::Project::markParentScenesNeedUpdate(uint32_t childSceneId) {
    bool foundParent = false;

    for (auto& s : scenes) {
        auto& cs = s.childScenes;
        if (findChildScene(cs, childSceneId) != cs.end()) {
            s.needUpdateRender = true;
            foundParent = true;
        }
    }

    if (foundParent) {
        // The inline child layers feed SceneRender::activate(), so force a re-activation.
        editor::getEditorHost().resetLastActivatedScene();
    }
}

void editor::Project::loadSceneProjectData(SceneProject* sceneProject, const YAML::Node& sceneNode) {
    sceneProject->defaultCamera = createDefaultCamera(sceneProject->sceneType, sceneProject->scene);

    Stream::decodeSceneProjectEntities(this, sceneProject, sceneNode);
    // references by GUID (S04 R10): a hint that changed is saved with the new path
    if (resolveSceneAssetRefs(*sceneProject, true)) {
        sceneProject->prefabRefsChanged = true;
    }

    for (Entity entity : sceneProject->entities) {
        MeshComponent* mesh = sceneProject->scene->findComponent<MeshComponent>(entity);
        if (!mesh) continue;
        for (unsigned int submeshIndex = 0; submeshIndex < mesh->numSubmeshes; submeshIndex++) {
            Material& material = mesh->submeshes[submeshIndex].material;
            if (material.name.empty()) continue;
            std::string normalizedPath = fs::path(material.name).lexically_normal().generic_string();
            linkMaterialFile(sceneProject->id, entity, submeshIndex, normalizedPath);
        }
    }

    updateSceneCppScripts(sceneProject);
    updateScenePrefabs(sceneProject);

    // Animation Player clips follow their GUIDs (R8): a hint a rename left stale is repaired (the
    // scene is then modified, as for prefab references)
    if (resolveRegistryClipRefs(sceneProject->scene)) {
        sceneProject->prefabRefsChanged = true;
    }
}

bool editor::Project::loadChildSceneInline(uint32_t childSceneId) {
    SceneProject* childScene = getScene(childSceneId);
    if (!childScene) {
        Out::error("Child scene with ID %u not found", childSceneId);
        return false;
    }

    if (childScene->scene != nullptr) {
        // Already loaded (opened as tab or already expanded inline)
        childScene->expandedInline = true;
        markParentScenesNeedUpdate(childSceneId);
        return true;
    }

    if (childScene->filepath.empty()) {
        Out::error("Child scene has no filepath");
        return false;
    }

    try {
        fs::path fullPath = childScene->filepath;
        if (fullPath.is_relative()) {
            fullPath = getProjectPath() / fullPath;
        }

        LoadReport loadReport;
        loadReport.documentPath = FileUtils::pathToUtf8(childScene->filepath);
        {
            ScopedLoadReport reportScope(loadReport, childScene->id);
            DocumentFile document = DocumentFiles::read(fullPath, DocumentKind::Scene);
            YAML::Node sceneNode = document.root;

            Stream::decodeSceneProject(childScene, sceneNode, true);
            loadSceneProjectData(childScene, sceneNode);
            childScene->diskSha1 = document.sha1;
        }
        childScene->loadReport = std::move(loadReport);
        logLoadReport(childScene->loadReport);

        childScene->expandedInline = true;
        childScene->needUpdateRender = true;
        childScene->isModified = childScene->prefabRefsChanged;

        removeMissingChildSceneReferences(*childScene);

        markParentScenesNeedUpdate(childSceneId);

        Out::info("Loaded child scene '%s' inline", childScene->name.c_str());
        return true;

    } catch (const YAML::Exception& e) {
        Out::error("Failed to load child scene inline: %s", e.what());
        return false;
    } catch (const std::exception& e) {
        Out::error("Failed to load child scene inline: %s", e.what());
        return false;
    }
}

void editor::Project::unloadChildSceneInline(uint32_t childSceneId) {
    SceneProject* childScene = getScene(childSceneId);
    if (!childScene) {
        return;
    }

    if (childScene->opened) {
        // Scene is open as a tab, only clear the inline flag
        childScene->expandedInline = false;
        markParentScenesNeedUpdate(childSceneId);
        return;
    }

    deleteSceneProject(childScene);
    cleanupPrefabsForScene(childSceneId);
    childScene->expandedInline = false;
    markParentScenesNeedUpdate(childSceneId);

    Out::info("Unloaded child scene '%s' from inline", childScene->name.c_str());
}

bool editor::Project::addChildScene(uint32_t sceneId, uint32_t childSceneId, bool startActive) {
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject) {
        Out::error("Scene with ID %u not found", sceneId);
        return false;
    }

    // Prevent adding self as child
    if (sceneId == childSceneId) {
        Out::error("Cannot add a scene as its own child");
        return false;
    }

    // Check if child scene exists
    const SceneProject* childScene = getScene(childSceneId);
    if (!childScene) {
        Out::error("Child scene with ID %u not found", childSceneId);
        return false;
    }

    // A cycle would have every walk down the child scenes recurse forever
    if (wouldCreateChildSceneCycle(sceneId, childSceneId)) {
        Out::error("Cannot add '%s' to '%s': '%s' already contains '%s'", childScene->name.c_str(), sceneProject->name.c_str(), childScene->name.c_str(), sceneProject->name.c_str());
        return false;
    }

    // Check if already added
    auto& childScenes = sceneProject->childScenes;
    if (findChildScene(childScenes, childSceneId) != childScenes.end()) {
        Out::warning("Child scene '%s' already exists in scene '%s'", childScene->name.c_str(), sceneProject->name.c_str());
        return false;
    }

    childScenes.push_back({childSceneId, startActive});
    sceneProject->isModified = true;
    Out::info("Added child scene '%s' to scene '%s'", childScene->name.c_str(), sceneProject->name.c_str());
    return true;
}

void editor::Project::removeChildScene(uint32_t sceneId, uint32_t childSceneId) {
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject) {
        Out::error("Scene with ID %u not found", sceneId);
        return;
    }

    auto& childScenes = sceneProject->childScenes;
    auto it = findChildScene(childScenes, childSceneId);
    if (it != childScenes.end()) {
        childScenes.erase(it);
        sceneProject->isModified = true;
        sceneProject->needUpdateRender = true;
        // Dropped an inline child, so the Engine layers need rebuilding.
        editor::getEditorHost().resetLastActivatedScene();

        const SceneProject* childScene = getScene(childSceneId);
        if (childScene) {
            Out::info("Removed child scene '%s' from scene '%s'", childScene->name.c_str(), sceneProject->name.c_str());
        }
    }
}

bool editor::Project::hasChildScene(uint32_t sceneId, uint32_t childSceneId) const {
    const SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject) {
        return false;
    }

    const auto& childScenes = sceneProject->childScenes;
    return findChildScene(childScenes, childSceneId) != childScenes.end();
}

bool editor::Project::wouldCreateChildSceneCycle(uint32_t sceneId, uint32_t childSceneId) const {
    std::unordered_set<uint32_t> visited;
    std::vector<uint32_t> pending = {childSceneId};
    while (!pending.empty()) {
        const uint32_t id = pending.back();
        pending.pop_back();
        if (id == sceneId) {
            return true;
        }
        if (!visited.insert(id).second) {
            continue;
        }
        if (const SceneProject* scene = getScene(id)) {
            for (const ChildSceneRef& childSceneRef : scene->childScenes) {
                pending.push_back(childSceneRef.id);
            }
        }
    }
    return false;
}

bool editor::Project::isChildSceneStartActive(uint32_t sceneId, uint32_t childSceneId) const {
    const SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject) {
        return true;
    }

    auto it = findChildScene(sceneProject->childScenes, childSceneId);
    return it == sceneProject->childScenes.end() ? true : it->startActive;
}

void editor::Project::setChildSceneStartActive(uint32_t sceneId, uint32_t childSceneId, bool startActive) {
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject) {
        Out::error("Scene with ID %u not found", sceneId);
        return;
    }

    auto it = findChildScene(sceneProject->childScenes, childSceneId);
    if (it == sceneProject->childScenes.end()) {
        Out::error("Child scene with ID %u not found in scene '%s'", childSceneId, sceneProject->name.c_str());
        return;
    }

    if (it->startActive == startActive) {
        return;
    }

    it->startActive = startActive;
    sceneProject->isModified = true;
}

std::vector<uint32_t> editor::Project::getChildScenes(uint32_t sceneId) const {
    const SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject) {
        return {};
    }
    std::vector<uint32_t> childSceneIds;
    childSceneIds.reserve(sceneProject->childScenes.size());
    for (const ChildSceneRef& childScene : sceneProject->childScenes) {
        childSceneIds.push_back(childScene.id);
    }
    return childSceneIds;
}

void editor::Project::deleteSceneProject(SceneProject* sceneProject){
    if (sceneProject->scene) {
        editor::getEditorHost().sceneDeleting(sceneProject->id);
    }

    if (sceneProject->sceneRender) {
        Camera* editorCam = sceneProject->sceneRender->getCamera();
        if (editorCam) {
            float zoom = 0.0f;
            float walkSpeedOffset = 0.0f;
            if (sceneProject->sceneType == SceneType::SCENE_2D || sceneProject->sceneType == SceneType::SCENE_UI) {
                zoom = static_cast<SceneRender2D*>(sceneProject->sceneRender)->getZoom();
            } else if (sceneProject->sceneType == SceneType::SCENE_3D) {
                walkSpeedOffset = static_cast<SceneRender3D*>(sceneProject->sceneRender)->getWalkSpeedOffset();
            }
            sceneProject->editorCameraState = Stream::encodeEditorCamera(editorCam, zoom, walkSpeedOffset);
        }
    }

    if (sceneProject->scene) {
        if (auto meshSystem = sceneProject->scene->getSystem<MeshSystem>())
            meshSystem->cancelAsyncModelLoads();
    }

    if (sceneProject->sceneRender)
        delete sceneProject->sceneRender;
    if (sceneProject->scene)
        delete sceneProject->scene;

    sceneProject->sceneRender = nullptr;
    sceneProject->scene = nullptr;
    // the pose rigs and animation builds of its models, now that their model data is freed
    pose::PoseRigCache::shared().prune();
    pose::PoseBuildCache::shared().prune();

    sceneProject->mainCamera = NULL_ENTITY;
    sceneProject->defaultCamera = NULL_ENTITY;

    sceneProject->entities.clear();
    sceneProject->selectedEntities.clear();
    sceneProject->missingPrefabs.clear();
}

void editor::Project::resetEngineConfigs(bool executeViewChanged) {
    Engine::setScalingMode(Scaling::NATIVE);
    // edit-mode scenes on the project default resolve it through the Engine
    Engine::setDefaultCanvasScaler(uiScale);
    Engine::setTextureStrategy(textureStrategy);
    Engine::setMouseCursor(CursorType::ARROW);
    Engine::setMouseMode(MouseMode::NORMAL);
    // a game (or the MCP server) may have changed the game clock
    Engine::setTimeScale(1.0f);
    Engine::setFixedDeltatime(0.0);
    Engine::setSimulationHeld(false);
    // a game may have turned it off
    Engine::setAsyncLoading(true);
    // Input is static: no level, edge or delta of the last Play reaches the next
    Input::resetFrameState();
    InputActions::unload();

    if (executeViewChanged) {
        Engine::systemViewChanged();
    }
}

void editor::Project::releaseScenes() {
    resetConfigs();
}

void editor::Project::resetConfigs() {
    // pose animation and clip documents and rigs belong to the project
    PoseAnimationDocument::closeAll();
    AnimationClipDocument::closeAll();
    VatDocument::closeAll();
    // the Inspector's asset was a file of the old project
    inspectedAsset.clear();
    PoseRigProvider::clear();
    // and the offscreen renderers render_pose_animation keeps of its models
    PoseSheetRenderer::clearShared();

    // Prefab Mode ends with the project (what it had unsaved was offered for saving before)
    while (!prefabStages.empty()) {
        popPrefabStage();
    }

    // Clear existing scenes
    for (auto& sceneProject : scenes) {
        deleteSceneProject(&sceneProject);
    }
    scenes.clear();
    unresolvedScenes.clear();
    unresolvedSceneStates.clear();
    prefabAssets.clear();
    standalonePrefabs.clear();
    editor::getEditorHost().resetLastActivatedScene();

    // A project may be closed, edited externally, then reopened in the same editor
    // process. Do not carry its dependency snapshots or compiled custom data across
    // that boundary.
    editor::ShaderBuilder::invalidateCustomShaders();
    // the pose rigs of its models (the next project may have files of the same names)
    pose::PoseRigCache::shared().clear();
    // and the animation builds shared on them
    pose::PoseBuildCache::shared().clear();

    // Scenes are gone: drop unreferenced pool assets from the previous project.
    // Keep entries still held by the editor/engine (fonts, active shaders, etc.).
    Engine::clearUnusedPools();

    // Reset state
    name = "";
    canvasWidth = defaultCanvasWidth;
    canvasHeight = defaultCanvasHeight;
    scalingMode = defaultScalingMode;
    uiScale = CanvasScalerSettings();
    Engine::setDefaultCanvasScaler(uiScale);
    textureStrategy = defaultTextureStrategy;
    vsyncEnabled = defaultVSyncEnabled;
    setAtmosphereQuality(defaultAtmosphereQuality);
    playerLogging = PlayerLoggingSettings();
    inputActionsAsset.clear();
    setInputDeadzone(defaultInputDeadzone);
    playerCrash = PlayerCrashSettings();
    buildMethod = "auto";
    packContent = false;
    stripEngineCode = true;
    setHtmlUiMsaaSamples(defaultHtmlUiMsaaSamples);
    setHtmlUiRenderSettings(HtmlUiRenderSettings());
    for (int platform = 0; platform < ShadowTiers::PLATFORM_COUNT; platform++){
        setShadowTier(platform, ShadowTiers::defaultTier(platform));
    }
    setThreadingSettings(ThreadingProjectSettings());
    setShaderCreationSettings(ShaderCreationProjectSettings());
    windowMode = defaultWindowMode;
    windowWidth = defaultWindowWidth;
    windowHeight = defaultWindowHeight;
    windowResizable = defaultWindowResizable;
    windowTitle = defaultWindowTitle;
    windowIcon.clear();
    assetsDir = defaultAssetsDir;
    luaDir = defaultLuaDir;
    refreshTrustedRoots();
    scriptDirs.clear();
    cxxStandard = defaultCxxStandard;
    physics2DEnabled = defaultPhysics2DEnabled;
    physics3DEnabled = defaultPhysics3DEnabled;
    runtimeShadersEnabled = defaultRuntimeShadersEnabled;
    mobilePbrTier = defaultMobilePbrTier;
    setTagsAndLayers({}, {});
    setLayerCollisionMatrices(allLayersCollide(), allLayersCollide());
    setPhysics3DWorldSettings(Physics3DWorldSettings());
    unreadableInputNode.clear();
    inputMigration = InputActionsMigration();
    setShaderGlobals({});
    packNativeResources = defaultPackNativeResources;
    versionControlMetadata = defaultVersionControlMetadata;
    // the project's version control ends with it (R11: its revert backups too)
    if (vcsService) {
        vcsService->close();
        removeVcsBackups();
    }
    if (sceneDiffs) sceneDiffs->clear();
    vcsProjectSettings = VcsProjectSettings();
    shaderOverrides = {};
    sourceCodeExportSettings = {};
    desktopExportSettings = {};
    applicationSettings = {};
    presentationSettings = {};
    webProjectSettings = {};
    linuxProjectSettings = {};
    windowsProjectSettings = {};
    macOSProjectSettings = {};
    iosProjectSettings = {};
    androidProjectSettings = {};
    workspaceMigrationPending = false;
    modelLoaded = false;
    generatedSourcesDirty.store(false);
    selectedScene = NULL_PROJECT_SCENE;
    selectedSceneForProperties = NULL_PROJECT_SCENE;
    startSceneId = NULL_PROJECT_SCENE;
    loadingSettings = {};
    debugSettings = {};
    terrainEditorSettings = {};
    physicsPlacementSettings = {};
    setNavigationSettings(NavigationProjectSettings());
    spriteNormalsSettings = {};
    simulateTouch = false;
    // nothing reloads into a project that is going (S05), nothing cooks for it (S14)
    HotReloadService::get().stopWatching();
    if (TextureCookQueue::get().getProject() == this){
        TextureCookQueue::get().attach(nullptr);
    }
    textureSettings = {};
    inspectedAssets.clear();
    customResolutions.clear();
    projectPath.clear();
    AssetMap::setResolver(nullptr);
    waitForAssetScan();
    assetSearchIndex.detach();
    assetLabels.close();
    organisation.close();
    assetIndex.close();
    assetDatabase.close();
    pendingAssetChanges.clear();
    materialFileLinks.clear();
    tabs.clear();
    projectHistory.clear();
    PlayChangeReview::get().discardAll();

    editor::getEditorHost().updateWindowTitle(name);

    resetEngineConfigs(false);
}

std::vector<editor::SceneScriptSource> editor::Project::collectAllSceneCppScripts() const {
    std::unordered_set<std::string> uniquePaths;
    std::vector<SceneScriptSource> mergedScripts;

    for (const auto& sceneProject : scenes) {
        for (const auto& script : sceneProject.cppScripts) {
            std::string pathKey = script.path.lexically_normal().generic_string();
            if (!uniquePaths.insert(pathKey).second) {
                continue;
            }

            fs::path sourcePath = script.path;
            if (sourcePath.is_relative()) {
                sourcePath = getProjectPath() / sourcePath;
            }
            if (sourcePath.empty() || !fs::exists(sourcePath)) {
                continue;
            }

            SceneScriptSource merged = script;

            // scene data can predate the last header edit, and only one scene contributes each script
            fs::path headerPath = merged.headerPath;
            if (headerPath.is_relative()) {
                headerPath = getProjectPath() / headerPath;
            }
            if (!merged.headerPath.empty()) {
                if (!fs::exists(headerPath)) {
                    continue;
                }
                merged.properties = toScriptPropertyInfos(ScriptParser::parseScriptProperties(headerPath));
            }

            mergedScripts.push_back(std::move(merged));
        }
    }

    // and the ones of prefabs only scripts instantiate
    appendPrefabCppScripts(mergedScripts, uniquePaths);

    return mergedScripts;
}

void editor::Project::pauseEngineScene(Scene* scene, bool pause) const{
    scene->getSystem<PhysicsSystem>()->setPaused(pause);
    scene->getSystem<ActionSystem>()->setPaused(pause);
    scene->getSystem<PoseAnimatorSystem>()->setPaused(pause);
    scene->getSystem<AudioSystem>()->setPaused(pause);
    // Edit mode: only the previewed (selected) Particle Systems simulate, driven by the editor
    scene->getSystem<ParticleSubsystem>()->setPaused(pause);
    // the fluids hold still in edit mode (the Inspector's preview steps one: FluidSystem::setPreview)
    if (auto fluids = scene->findSystem<FluidSystem>()) fluids->setPaused(pause);
    // lattice tweens and jiggle (followers and offsets still show in edit mode)
    scene->getSystem<LatticeSystem>()->setPaused(pause);
    // Spline Animate followers hold still; roads and pieces keep regenerating
    if (auto splines = scene->findSystem<SplineSystem>()) splines->setPaused(pause);
    // Vertex Animators hold still in edit mode unless their Preview is on
    scene->getSystem<VertexAnimatorSystem>()->setPaused(pause);
    // and Text Effects unless the Inspector previews them
    scene->getSystem<UISystem>()->setTextEffectsHeld(pause);
    // Video Players play nothing in edit mode (playOnAwake waits for Play)
    if (auto videos = scene->findSystem<VideoPlayerSystem>()) videos->setPaused(pause);
    // trails keep no point in edit mode but a dragged GameObject's (TrailSystem's preview)
    if (auto trails = scene->findSystem<TrailSystem>()){
        trails->setPaused(pause);
        if (!pause) trails->clearPreviews();
    }
    // Camera Follow and the Camera Shakers hold in edit mode (the Inspector's shake preview runs)
    scene->getSystem<CameraControlSystem>()->setPaused(pause);
}

void editor::Project::stopCameraShakePreviews() {
    for (SceneProject* sceneProject : getScenesAndPrefabStages()) {
        if (sceneProject && sceneProject->scene && sceneProject->playState == ScenePlayState::STOPPED) {
            sceneProject->scene->getSystem<CameraControlSystem>()->stopPreview();
        }
    }
}

void editor::Project::stopVertexAnimatorPreviews() {
    for (SceneProject* sceneProject : getScenesAndPrefabStages()) {
        if (sceneProject && sceneProject->scene && sceneProject->playState == ScenePlayState::STOPPED) {
            sceneProject->scene->getSystem<VertexAnimatorSystem>()->stopPreviews();
        }
    }
}

void editor::Project::copyEngineApiToProject() {
    // the engine API snapshot and tooling live in the editor's internal folder (an MCP call's
    // deny-by-default guard refuses it otherwise: util/SafeWrite.h, EditorInternal)
    safewrite::EditorInternal ownInternal(getProjectInternalPath());
    // Lua API stubs and LSP configuration (lua-language-server, clangd) next to the snapshot.
    // Written even when the engine folder is missing or the copy fails: the stubs are built
    // into the editor.
    auto writeTooling = [this]() {
        Generator::writeEditorTooling(getProjectPath(), getProjectInternalPath(), getCxxStandard(), getBuildScriptDirs());
        Generator::writeLuaScriptStubs(getProjectPath(), getProjectInternalPath());
    };
    try {
        std::filesystem::path engineApiSource = FileUtils::getEngineDir();

        if (!std::filesystem::exists(engineApiSource)) {
            Out::warning("engine folder not found at: %s", engineApiSource.string().c_str());
            writeTooling();
            return;
        }

        std::filesystem::path engineApiDest = getProjectInternalPath() / "engine-api";

        // Create internal path if it doesn't exist
        if (!std::filesystem::exists(getProjectInternalPath())) {
            std::error_code internalError;
            FileUtils::createDirectories(getProjectInternalPath(), internalError);
        }

        int updatedFiles = 0;

        editor::getEditorHost().reportLoadingProgress("Syncing engine API...");

        // Sync only files whose contents actually changed so reopening the editor
        // does not touch header timestamps and force a rebuild of game scripts.
        for (const auto& dirEntry : std::filesystem::recursive_directory_iterator(engineApiSource)) {
            editor::getEditorHost().reportLoadingProgress();

            if (dirEntry.is_regular_file()) {
                auto ext = dirEntry.path().extension().string();
                auto relPath = std::filesystem::relative(dirEntry.path(), engineApiSource);

                // The native application backends are compiled by the project's
                // own build (see Generator::getPlatformCMakeConfig), so their
                // implementations travel with the snapshot; everything else is
                // header-only API.
                const bool isPlatformSource =
                    !relPath.empty() && relPath.begin()->string() == "platform" &&
                    (ext == ".cpp" || ext == ".mm" || ext == ".m");

                if (ext == ".h" || ext == ".hpp" || ext == ".inl" || ext == ".glsl" || ext == ".frag" || ext == ".vert" || isPlatformSource) {
                    auto destPath = engineApiDest / relPath;
                    std::error_code folderError;
                    FileUtils::createDirectories(destPath.parent_path(), folderError);

                    std::ifstream sourceFile(dirEntry.path(), std::ios::in | std::ios::binary);
                    if (!sourceFile) {
                        Out::warning("Failed to read engine API source file: %s", dirEntry.path().string().c_str());
                        continue;
                    }

                    std::string sourceContent(
                        (std::istreambuf_iterator<char>(sourceFile)),
                        std::istreambuf_iterator<char>());

                    if (FileUtils::writeFile(destPath, sourceContent) == FileUtils::WriteResult::Written) {
                        updatedFiles++;
                    }
                }
            }
        }

        Out::info("Synced engine API in project: %s (%d files updated)", engineApiDest.string().c_str(), updatedFiles);

    } catch (const std::exception& e) {
        Out::error("Failed to copy engine API: %s", e.what());
    }
    writeTooling();
}

void editor::Project::finalizeStart(SceneProject* mainSceneProject, std::vector<PlayRuntimeScene>& runtimeScenes) {
    std::vector<uint32_t> activeSceneIds;
    if (mainSceneProject) {
        collectStartActiveScenes(mainSceneProject->id, activeSceneIds);
    }

    for (auto& entry : runtimeScenes) {
        SceneProject* sceneProject = runtimeOf(entry);
        if (!sceneProject || !sceneProject->scene) {
            continue;
        }

        if (!entry.initialized) {
            prepareRuntimeScene(entry);
        }

        bool isActiveScene = std::find(activeSceneIds.begin(), activeSceneIds.end(), entry.sourceSceneId) != activeSceneIds.end();
        if (sceneProject != mainSceneProject && isActiveScene) {
            Engine::addSceneLayer(sceneProject->scene);
        }
    }

    // A GameObject of a child scene selected when Play starts stays selected, now the
    // running copy of it, as Unity keeps the selection into Play mode
    for (const auto& entry : runtimeScenes) {
        SceneProject* runtime = runtimeOf(entry);
        if (!entry.ownedRuntime || !runtime || !runtime->scene || runtime->id == entry.sourceSceneId) {
            continue;
        }
        if (selectedSceneForProperties != entry.sourceSceneId || !Engine::isSceneRunning(runtime->scene)) {
            continue;
        }
        SceneProject* sourceScene = getScene(entry.sourceSceneId);
        if (!sourceScene) {
            continue;
        }
        std::vector<Entity> carried;
        for (Entity entity : sourceScene->selectedEntities) {
            if (std::find(runtime->entities.begin(), runtime->entities.end(), entity) != runtime->entities.end()) {
                carried.push_back(entity);
            }
        }
        if (carried.empty()) {
            continue;
        }
        sourceScene->selectedEntities.clear();
        sourceScene->needUpdateRender = true;
        runtime->selectedEntities = carried;
        runtime->needUpdateRender = true;
        selectedSceneForProperties = runtime->id;
    }

    Engine::pauseGameEvents(false);
    Engine::onViewLoaded.call();
    Engine::systemViewChanged();

    if (mainSceneProject) {
        mainSceneProject->playState = ScenePlayState::PLAYING;
        mainSceneProject->needUpdateRender = true;
        editor::getEditorHost().requestScenePlayFocus(mainSceneProject->id);
        editor::getEditorHost().resetLastActivatedScene();
        Out::success("Scene '%s' started", mainSceneProject->name.c_str());
    }
}

// Authored, alive before Play, or foliage chunks that MeshSystem streams in while playing
static std::set<Entity> getPlayKeptEntities(const editor::SceneProject* sceneProject) {
    Scene* scene = sceneProject->scene;

    std::set<Entity> keep = sceneProject->playStateEntities;
    keep.insert(sceneProject->entities.begin(), sceneProject->entities.end());

    auto meshSystem = scene->getSystem<MeshSystem>();
    auto terrains = scene->getComponentArray<TerrainComponent>();
    for (size_t i = 0; i < terrains->size(); i++) {
        const std::vector<Entity> foliage = meshSystem->getFoliageEntities(terrains->getEntity(i));
        keep.insert(foliage.begin(), foliage.end());
    }
    // what splines generate is SplineSystem's: it creates and destroys it (also in Play)
    if (std::shared_ptr<SplineSystem> splines = scene->findSystem<SplineSystem>()) {
        auto splineArray = scene->getComponentArray<SplineComponent>();
        for (size_t i = 0; i < splineArray->size(); i++) {
            const std::vector<Entity> generated = splines->getGeneratedEntities(splineArray->getEntity(i));
            keep.insert(generated.begin(), generated.end());
        }
    }

    // The Scene view's own gizmos and icons (SceneRender::overlay) are made when first needed,
    // also while the game plays or between the play snapshot and the first frame; they are the
    // editor's, which keeps pointers to them
    auto renderSystem = scene->getSystem<RenderSystem>();
    for (Entity entity : scene->getEntityList()) {
        if (renderSystem->isEditorOverlay(entity)) {
            keep.insert(entity);
        }
    }

    return keep;
}

// Engine cameras live in the system range, so only user entities count as made by the game
std::vector<Entity> editor::Project::getPlayCreatedEntities(const SceneProject* sceneProject) const {
    if (!sceneProject || !sceneProject->scene ||
        (sceneProject->playState != ScenePlayState::PLAYING && sceneProject->playState != ScenePlayState::PAUSED)) {
        return {};
    }

    const std::set<Entity> keep = getPlayKeptEntities(sceneProject);

    // what its documents made for themselves (a <particle-system>'s prefab) is not the game's
    UIDocumentSystem* documents = sceneProject->scene->findSystem<UIDocumentSystem>().get();
    std::vector<Entity> created;
    for (Entity entity : sceneProject->scene->getEntityList()) {
        if (entity > EntityManager::lastSystemEntity() && keep.count(entity) == 0 && !(documents && documents->isRuntimeEntity(entity))) {
            created.push_back(entity);
        }
    }

    return created;
}

// Entities a script spawns while playing are not in the editor list, so without this they
// stay alive after Stop: drawn in the viewport but no longer reachable from the Hierarchy.
void editor::Project::destroyPlayCreatedEntities(SceneProject* sceneProject) {
    Scene* scene = sceneProject->scene;

    // Stop's restore: only what was alive before Play stays. GameObjects an edit made while
    // playing are tracked, yet go too (their steps go with the play session; Play Mode
    // Changes can make them again)
    std::set<Entity> keep = getPlayKeptEntities(sceneProject);
    if (!sceneProject->playStateEntities.empty()) {
        for (Entity entity : sceneProject->entities) {
            if (!sceneProject->playStateEntities.count(entity)) keep.erase(entity);
        }
    }

    for (Entity entity : scene->getEntityList()) {
        // an owner's removal can destroy other entities in this list
        if (keep.count(entity) == 0 && scene->isEntityCreated(entity)) {
            scene->destroyEntity(entity);
        }
    }
    sceneProject->entities.erase(std::remove_if(sceneProject->entities.begin(), sceneProject->entities.end(),
        [scene](Entity entity) { return !scene->isEntityCreated(entity); }), sceneProject->entities.end());
}

bool editor::Project::restorePlaySnapshot(SceneProject* sceneProject) {
    if (!sceneProject) {
        return false;
    }
    if (!sceneProject->scene || !sceneProject->playStateSnapshot || sceneProject->playStateSnapshot.IsNull()) {
        sceneProject->playPrefabState = PlayPrefabState();
        return false;
    }
    Stream::decodeScene(sceneProject->scene, sceneProject->playStateSnapshot["scene"]);

    // entities are still live, so the decode also removes components added during play
    auto entitiesNode = sceneProject->playStateSnapshot["entities"];
    size_t insertAt = 0;
    for (const auto& entityNode : entitiesNode) {
        // a Mesh Play left as it was, moved or not, is kept: decoding it reloads it at the next frame
        std::vector<Entity> restored = Stream::decodeEntity(entityNode, sceneProject->scene, nullptr, nullptr, sceneProject, NULL_ENTITY, false, true, nullptr, true);

        // A script can destroy entities while playing and the decode recreates them, but
        // only the tracked list makes them visible to the editor again
        for (Entity entity : restored) {
            auto it = std::find(sceneProject->entities.begin(), sceneProject->entities.end(), entity);
            if (it != sceneProject->entities.end()) {
                insertAt = std::distance(sceneProject->entities.begin(), it) + 1;
                continue;
            }

            insertAt = std::min(insertAt, sceneProject->entities.size());
            sceneProject->entities.insert(sceneProject->entities.begin() + insertAt, entity);
            insertAt++;
        }
    }

    // A GameObject a script destroyed (GameObject.destroy) comes back at the end of its
    // parent's branch: siblings go back to the snapshot's order
    Scene* scene = sceneProject->scene;
    std::function<void(const YAML::Node&)> restoreOrder = [&](const YAML::Node& list) {
        if (!list || !list.IsSequence()) {
            return;
        }
        Entity previous = NULL_ENTITY;
        for (const auto& node : list) {
            if (!node.IsMap() || !node["entity"]) {
                continue;
            }
            const Entity entity = node["entity"].as<Entity>();
            if (!scene->isEntityCreated(entity) || !scene->findComponent<Transform>(entity)) {
                continue;
            }
            if (previous != NULL_ENTITY) {
                auto transforms = scene->getComponentArray<Transform>();
                if (transforms->getIndex(entity) < transforms->getIndex(previous)) {
                    scene->moveChildToIndex(entity, scene->findBranchLastIndex(previous) + 1);
                }
            }
            previous = entity;
            restoreOrder(node["children"]);
        }
    };
    restoreOrder(entitiesNode);

    destroyPlayCreatedEntities(sceneProject);

    deselectDestroyedEntities(sceneProject);

    // snapshot decode leaves camera-linked textures unresolved (no framebuffer)
    CameraTextureLink::resolve(sceneProject->scene);

    // Clear the snapshot
    sceneProject->playStateSnapshot = YAML::Node();
    sceneProject->playStateEntities.clear();

    // The prefab instances' bookkeeping goes back with them: an override recorded, a
    // GameObject removed or added while playing does not outlive Play
    restorePlayPrefabState(sceneProject);
    return true;
}

void editor::Project::finalizeStop(SceneProject* mainSceneProject, std::vector<PlayRuntimeScene> runtimeScenes) {
    bool saveMainOnStop = false;
    bool keepMainModified = false;

    for (const auto& entry : runtimeScenes) {
        SceneProject* sceneProject = runtimeOf(entry);
        if (!sceneProject || !sceneProject->scene) {
            continue;
        }

        if (entry.ownedRuntime) {
            // What was selected in the copy is selected again in the scene it copied, as
            // far as that scene has it loaded and the GameObject existed before Play (like
            // Unity after Play mode)
            const uint32_t runtimeId = sceneProject->id;
            if (runtimeId != entry.sourceSceneId && selectedSceneForProperties == runtimeId) {
                SceneProject* sourceScene = findScene<SceneProject>(entry.sourceSceneId);
                std::vector<Entity> restored;
                if (sourceScene && sourceScene->scene) {
                    for (Entity entity : sceneProject->selectedEntities) {
                        if (std::find(sourceScene->entities.begin(), sourceScene->entities.end(), entity) != sourceScene->entities.end()) {
                            restored.push_back(entity);
                        }
                    }
                }
                if (!restored.empty()) {
                    sourceScene->selectedEntities = restored;
                    sourceScene->needUpdateRender = true;
                    selectedSceneForProperties = entry.sourceSceneId;
                }
            }
            sceneProject->selectedEntities.clear();
        }

        bool saveOnStop = false;
        bool keepModified = false;

        // What the scene got while playing goes back with the snapshot below, and with it
        // the unsaved mark those edits put on it (as Unity leaves no Play mode change)
        if (!entry.ownedRuntime && sceneProject->playStateSnapshot && !sceneProject->playStateSnapshot.IsNull()) {
            sceneProject->isModified = sceneProject->playStateModified;
        }

        pauseEngineScene(sceneProject->scene, true);
        applyEditCanvasScale(sceneProject->scene, sceneProject->sceneType, sceneProject->sceneRender);

        // GameObject.destroy calls still pending; what the game made goes with the snapshot
        ObjectManager::forgetScene(sceneProject->scene);

        // Stop scene audio before restoring snapshot to prevent stale SoLoud handles on the next play
        sceneProject->scene->getSystem<AudioSystem>()->stopSceneSounds();
        // what the animators played (and the bone poses they remember) goes with Play
        sceneProject->scene->getSystem<PoseAnimatorSystem>()->resetAnimators();
        // and the Animation Players (animation clips spec 3.3)
        sceneProject->scene->getSystem<ActionSystem>()->resetPlayers();
        // and the Vertex Animators (vertex animation spec 3.4)
        sceneProject->scene->getSystem<VertexAnimatorSystem>()->resetAnimators();
        // and the camera shakes (their view offsets) and the followers' motion
        sceneProject->scene->getSystem<CameraControlSystem>()->resetAll();
        // and the Video Players: their sources close, playOnAwake runs again at the next Play
        if (auto videos = sceneProject->scene->findSystem<VideoPlayerSystem>()) videos->resetPlayers();

        // The copy goes with Stop, so its id does too: after the prefab instances the game
        // made in it went (their destroyer finds the copy by that id), before the copy is
        // saved or deleted as its source scene
        if (entry.ownedRuntime) {
            const uint32_t runtimeId = sceneProject->id;
            releasePlayRuntimeId(sceneProject, entry.sourceSceneId);
            discardPlayRuntimeState(runtimeId, entry.sourceSceneId, mainSceneProject ? mainSceneProject->id : NULL_PROJECT_SCENE);
        }

        // Collect shader keys while components are still loaded (before snapshot restore wipes loaded state).
        // Merge newly discovered keys into the editor-side scene and mark it modified so the file is saved.
        if (entry.sourceSceneId != NULL_PROJECT_SCENE) {
            std::set<ShaderKey> playKeys;
            collectSceneShaderKeys(sceneProject, playKeys);
            if (!playKeys.empty()) {
                SceneProject* editorScene = entry.ownedRuntime ? getScene(entry.sourceSceneId) : sceneProject;
                if (editorScene) {
                    const bool wasModified = editorScene->isModified;
                    const size_t prevSize = editorScene->shaderKeys.size();
                    editorScene->shaderKeys.insert(playKeys.begin(), playKeys.end());
                    if (editorScene->shaderKeys.size() > prevSize) {
                        editorScene->isModified = true;
                        saveOnStop = true;
                        keepModified = wasModified;

                        if (mainSceneProject && entry.sourceSceneId == mainSceneProject->id) {
                            saveMainOnStop = true;
                            keepMainModified = wasModified;
                        }
                    }
                }
            }
        }

        // Play Mode Changes: what was edited while playing, read before the snapshot puts the
        // edit-time scene back (runtime copies have no history)
        if (!entry.ownedRuntime) {
            PlayChangeReview::get().capture(this, sceneProject);
        }

        // Restore snapshot if present
        const bool restoredFromSnapshot = restorePlaySnapshot(sceneProject);
        // the restored scene's own Canvas Scaler (a script may have changed it while playing)
        applyEditCanvasScale(sceneProject->scene, sceneProject->sceneType, sceneProject->sceneRender);
        // the fluids too: back to the edit-mode start, not the game's last frame
        restartFluids(sceneProject->scene);

        sceneProject->playState = ScenePlayState::STOPPED;
        if (!entry.ownedRuntime) {
            // the edit-time scene itself played (runtime copies have no history)
            CommandHandle::get(sceneProject->id)->endPlaySession(restoredFromSnapshot);
            if (restoredFromSnapshot) {
                PlayChangeReview::get().afterRestore(this, sceneProject);
            } else {
                // nothing was rolled back: the play edits stayed
                PlayChangeReview::get().discard(sceneProject->id);
            }
        }

        Entity cameraEntity = sceneProject->scene->getCamera();
        if (cameraEntity != NULL_ENTITY && sceneProject->scene->isEntityCreated(cameraEntity)) {
            if (CameraComponent* cameraComponent = sceneProject->scene->findComponent<CameraComponent>(cameraEntity)) {
                cameraComponent->needUpdate = true;
            }
        }

        if (sceneProject->sceneRender){
            sceneProject->sceneRender->setPlayMode(false);
        }

        if (sceneProject != mainSceneProject) {
            editor::getEditorHost().enqueueMainThreadTask([this, sceneProject, entry, saveOnStop, keepModified]() {
                SceneProject* editorScene = entry.ownedRuntime ? getScene(entry.sourceSceneId) : sceneProject;
                if (saveOnStop && editorScene && !editorScene->filepath.empty()) {
                    saveLoadedSceneOnStop(sceneProject, editorScene, keepModified);
                }

                Engine::removeScene(sceneProject->scene);

                // Delete runtime clones that aren't opened in the editor.
                if (entry.ownedRuntime) {
                    deleteSceneProject(sceneProject);
                    delete sceneProject;
                }
            });
        }
    }

    editor::getEditorHost().enqueueMainThreadTask([this]() {
        SceneManager::clearAll();
        PrefabManager::clearAll();
        ObjectManager::clearAll();
        clearPlayPrefabTemplates();
        editor::getEditorHost().resetLastActivatedScene();
    });

    resetEngineConfigs(true);

    if (mainSceneProject) {
        Out::success("Scene '%s' stopped", mainSceneProject->name.c_str());
        if (saveMainOnStop && !mainSceneProject->filepath.empty()) {
            uint32_t mainSceneId = mainSceneProject->id;
            editor::getEditorHost().enqueueMainThreadTask([this, mainSceneId, keepMainModified]() {
                SceneProject* sceneProject = getScene(mainSceneId);
                if (sceneProject) {
                    saveLoadedSceneOnStop(sceneProject, sceneProject, keepMainModified);
                }
            });
        }
    }
}

bool editor::Project::createTempProject(std::string projectName, bool deleteIfExists) {
    return createProjectInDirectory(std::filesystem::temp_directory_path() / projectName, projectName, deleteIfExists);
}

// A temporary project (createTempProject, until Save Project moves it) or one at its
// final place (createProjectAt): the same files either way
bool editor::Project::createProjectInDirectory(const std::filesystem::path& path, const std::string& projectName, bool deleteIfExists) {
    if (isAnyScenePlaying()) {
        Out::warning("Cannot create a new project while a scene is running or stopping.");
        return false;
    }

    try {
        // The workspace is written from the live model, which resetConfigs() is about
        // to drop, so the outgoing project is flushed first
        if (modelLoaded && !projectPath.empty()) {
            saveWorkspaceFile();
        }
        // the outgoing session ended cleanly (R16): its backups and its lock go
        if (modelLoaded) {
            endProjectSession();
        }

        editor::getEditorHost().prepareForProjectSwitch();

        resetConfigs();

        // Clear the last project path in settings when creating a new temp project (a
        // project elsewhere becomes it when saved below)
        AppSettings::setLastProjectPath(std::filesystem::path());

        projectPath = path;
        refreshTrustedRoots();
        // PlayerPrefs are the project's (.jasperengine/user/rundata): read from the new one
        UserSettingsStore::instance().unload();
        fs::path projectFile = projectPath / "project.yaml";

        // Set the project name so libName (used for the generated CMake target
        // and project()) is populated. Without this a freshly created temp
        // project has an empty name, producing an invalid CMakeLists.txt.
        setName(projectName);

        if (deleteIfExists && fs::exists(projectPath)) {
            fs::remove_all(projectPath);
        }

        if (!std::filesystem::exists(projectFile)) {
            if (!std::filesystem::exists(projectPath)) {
                std::filesystem::create_directories(projectPath);
            }
            Out::info("Created project directory: \"%s\"", projectPath.string().c_str());
            refreshTrustedRoots();   // the folder exists now

            // a new project is locked like an opened one (R10); the editor's own temporary
            // project is not (each editor remakes it)
            if (!isEditorTempProject() && !takeProjectLock(projectPath)) {
                return false;
            }

            // Inherit the compiler chosen in a previous session so new temp projects
            // don't silently fall back to the Default toolchain every time. Drop a
            // stale compiler path that no longer exists on disk.
            {
                LocalBuildSettings build;
                build.cCompiler = AppSettings::getLastCMakeCCompiler();
                build.cxxCompiler = AppSettings::getLastCMakeCxxCompiler();
                build.generator = AppSettings::getLastCMakeGenerator();
                if (!build.cxxCompiler.empty() && !fs::exists(build.cxxCompiler)) {
                    build = LocalBuildSettings();
                }
                if (!build.cCompiler.empty() || !build.cxxCompiler.empty() || !build.generator.empty()) {
                    AppSettings::setBuildSettings(projectFile, build);
                }
            }

            // Fresh projects resolve fork manifests through the same project-relative
            // reader as opened projects, including after the assets directory changes.
            ShaderManifests::setForkReader([this](const std::string& relativePath, std::string& content) {
                return readShaderFile(relativePath, content);
            });

            modelLoaded = true;
            // a project this version creates keeps its binaries in Git LFS (R21)
            vcsProjectSettings.lfs = true;
            openAssetDatabase();
            if (watchFiles) HotReloadService::get().startWatching(*this);
        // the project's editor menus (a text scan of its Editor folders: no project code runs)
        EditorScripts::get().scan(this);
            saveProject();
            if (watchFiles) startVersionControl();
            createNewScene("New Scene", SceneType::SCENE_3D);
            copyEngineApiToProject();
        } else {
            Out::info("Project directory already exists: \"%s\"", projectPath.string().c_str());
            // the temporary project is one directory for all editors: another may hold its lock
            if (!loadProject(projectPath)) {
                editor::getEditorHost().updateResourcesPath();
                return false;
            }
        }

        editor::getEditorHost().updateResourcesPath();

    } catch (const std::exception& e) {
        Out::error("Failed to create project: %s", e.what());
        // prepareForProjectSwitch() suspends background resource work.
        // Always release that suspension even when project creation fails.
        editor::getEditorHost().updateResourcesPath();
        return false;
    }

    return true;
}

bool editor::Project::saveProject(bool userCalled, std::function<void()> callback) {
    if (isTempProject() && userCalled) {
        editor::getEditorHost().registerProjectSaveDialog(callback);
        return true;
    }

    bool saveret = saveProjectToPath(projectPath);

    if (callback) {
        callback();
    }

    return saveret;
}

bool editor::Project::saveProjectFile(DegradedSavePolicy policy, SaveResult* outcome) {
    if (outcome) {
        *outcome = SaveResult::Failed;
    }
    if (projectPath.empty()) {
        return false;
    }

    // project.yaml read with values this editor could not keep (a newer format): not written
    // over unless the loss was accepted
    if (projectLoadReport.dropped() > 0 && policy == DegradedSavePolicy::Refuse) {
        Out::warning("project.yaml not saved: it did not load completely and saving it would remove %zu value(s) "
                     "this editor could not read (save_project allow_data_loss writes it anyway).", projectLoadReport.dropped());
        if (outcome) {
            *outcome = SaveResult::RefusedDegraded;
        }
        return false;
    }

    // project.yaml is about to point to the asset an older editor's Input Manager actions became:
    // it exists first
    if (inputMigration.pending && !writeInputActionsMigration()) {
        Out::error("Project not saved: the actions asset project.yaml would point to (%s) could not be written",
                   inputMigration.asset.generic_string().c_str());
        return false;
    }

    if (!saveWorkspaceFile()) {
        // encodeProject() no longer emits the per-user keys, so while project.yaml
        // still holds the only copy of them, writing it now would discard them
        if (workspaceMigrationPending) {
            Out::error("Project not saved: its editor state has not reached \"%s\" yet",
                       Workspace::getUserDirectory(projectPath).string().c_str());
            return false;
        }
        Out::warning("Editor workspace could not be written, saving the project anyway. This session's tab layout and viewport cameras are lost.");
    }

    if (versionControlMetadata) {
        writeVersionControlMetadata();
    }

    try {
        std::filesystem::path projectFile = projectPath / "project.yaml";
        YAML::Node root = Stream::encodeProject(this);
        // the keys of the file this editor does not know stay (R28)
        DocumentFiles::overlayUnknownRootKeys(root, projectFile, RootKind::Project);
        // a file of a Windows checkout keeps its \r\n
        // the editor writes the project's own project.yaml (an MCP call's deny-by-default guard
        // refuses every other writer: util/SafeWrite.h, EditorInternal)
        safewrite::EditorInternal ownProjectFile(projectFile);
        const io::WriteResult written = DocumentFiles::write(projectFile, FileUtils::withLineEndings(projectFile, DocumentFiles::text(root)));
        if (!written.ok()) {
            const std::string message = DocumentFiles::saveFailureMessage("project.yaml", written);
            Out::error(message);
            alertUser("Error", message);
            return false;
        }
        if (projectLoadReport.dropped() > 0) {
            Out::warning("Saved project.yaml without %zu value(s) this editor could not read.", projectLoadReport.dropped());
            projectLoadReport.clearDropped();
        }

        if (outcome) {
            *outcome = SaveResult::Saved;
        }
        return true;
    } catch (const std::exception& e) {
        Out::error("Failed to save project file: \"%s\"", e.what());
        return false;
    }
}

bool editor::Project::saveWorkspaceFile() {
    if (projectPath.empty()) {
        return false;
    }
    // the editor's own per-user workspace file, .jasperengine/user/workspace.yaml (a hidden folder the
    // guard refuses to agents): the editor names that one file (util/SafeWrite.h); its folder is made
    // first when missing, under a token for that folder alone
    const fs::path userDir = Workspace::getUserDirectory(projectPath);
    {
        std::error_code ec;
        if (!fs::is_directory(userDir, ec)) {
            safewrite::EditorInternal makeUserDir(userDir);
            FileUtils::createDirectories(userDir, ec);
        }
    }
    safewrite::EditorInternal workspaceFile(userDir / "workspace.yaml");
    if (!Workspace::save(this)) {
        return false;
    }
    workspaceMigrationPending = false;
    return true;
}

namespace {

const char* versionControlMarker = "# Generated by JasperEngine Editor.";

// What the editor regenerates, and what belongs to one machine or one developer
const char* versionControlIgnore =
    "# Generated by JasperEngine Editor.\n"
    "\n"
    "# Engine API snapshot, generated C++, build trees, this machine's build\n"
    "# settings and this user's editor workspace.\n"
    ".jasperengine/\n"
    "\n"
    "# Regenerated on every build.\n"
    "/CMakeLists.txt\n"
    "\n"
    "# Deleted assets, until the trash is emptied.\n"
    ".trash/\n"
    "\n"
    "# Build output.\n"
    "build/\n"
    "\n"
    "# The Recorder's movies and image sequences.\n"
    "Recordings/\n"
    "\n"
    "# Per-developer IDE configuration.\n"
    ".vscode/\n"
    ".idea/\n"
    "*.user\n"
    "\n"
    "# Written by the game while it runs, Play mode included.\n"
    "UserSettings.xml\n"
    "\n"
    "# The editor's own settings and layout, left here by an editor started in this\n"
    "# folder (older versions; they now go to the user's config directory).\n"
    "/settings.yaml\n"
    "/editor_layout_v2.ini\n"
    "\n"
    "*.log\n"
    ".DS_Store\n"
    "Thumbs.db\n"
    "\n"
    "# What a conflicted merge leaves next to a file.\n"
    "*.orig\n";

// Scenes and project.yaml only diff and merge while every checkout agrees on the
// line ending, so it is not left to core.autocrlf. Scenes and prefabs merge with the editor's
// scene merge driver once it is set up (git config, "Set Up Smart Merge"); without it git's text
// merge runs. With LFS the binaries are LFS objects and lockable (git lfs lock); scenes, prefabs
// and meta files never are LFS objects (docs/specs/S29-version-control.md R21).
const char* const versionControlBinaryPatterns[] = {
    "*.png", "*.jpg", "*.jpeg", "*.tga", "*.ktx", "*.dds", "*.glb", "*.gltf", "*.fbx", "*.obj",
    "*.ogg", "*.wav", "*.mp3", "*.ttf", "*.otf"
};
// binaries LFS adds (art sources, HDR images, cooked textures, video)
const char* const versionControlLfsOnlyPatterns[] = {
    "*.psd", "*.exr", "*.hdr", "*.ktx2", "*.basis", "*.blend", "*.mp4", "*.webm"
};

std::string versionControlAttributes(bool lfs, bool lockScenes) {
    auto line = [](const char* pattern, const std::string& attributes) {
        std::string text = pattern;
        text.append(text.size() < 10 ? 10 - text.size() : 1, ' ');
        return text + attributes + "\n";
    };
    const std::string sceneLock = lockScenes ? " lockable" : "";
    std::string text =
        "# Generated by JasperEngine Editor.\n"
        "\n"
        "* text=auto eol=lf\n"
        "\n";
    text += line("*.scene", "text eol=lf merge=jasperengine-scene" + sceneLock);
    text += line("*.prefab", "text eol=lf merge=jasperengine-scene" + sceneLock);
    text += line("*.meta", "text eol=lf");
    for (const char* pattern : {"*.yaml", "*.lua", "*.cmake", "*.h", "*.cpp"}) text += line(pattern, "text eol=lf");
    text += "\n";
    const std::string binary = lfs ? "filter=lfs diff=lfs merge=lfs -text lockable" : "binary";
    for (const char* pattern : versionControlBinaryPatterns) text += line(pattern, binary);
    if (lfs) {
        for (const char* pattern : versionControlLfsOnlyPatterns) text += line(pattern, binary);
    }
    return text;
}

} // namespace

bool editor::Project::writeVersionControlMetadata() {
    if (projectPath.empty()) {
        return false;
    }

    const std::string attributes = versionControlAttributes(vcsProjectSettings.lfs, vcsProjectSettings.lockScenes);
    const std::pair<const char*, const char*> files[] = {
        {".gitignore", versionControlIgnore},
        {".gitattributes", attributes.c_str()},
    };

    bool success = true;
    for (const auto& [name, content] : files) {
        const fs::path target = projectPath / name;

        // Rules the project wrote itself are worth more than the editor's, so only a
        // file carrying the marker is refreshed
        std::ifstream fin(target);
        if (fin) {
            std::string firstLine;
            std::getline(fin, firstLine);
            if (firstLine.rfind(versionControlMarker, 0) != 0) {
                continue;
            }
        }
        fin.close();

        // the editor's own .gitignore / .gitattributes at the project root (dot-files the guard
        // refuses to agents): named so the editor's own refresh goes through
        safewrite::EditorInternal ownMetadata(target);
        if (!FileUtils::writeIfChanged(target, content)) {
            Out::error("Failed to write %s", target.string().c_str());
            success = false;
        }
    }

    return success;
}

void editor::Project::clearTrash() {
    if (projectPath.empty())
        return;

    std::filesystem::path trashPath = projectPath / ".trash";
    if (std::filesystem::exists(trashPath)) {
        // never through a link: a trash folder resolving outside the project is left alone, and
        // the removal goes beneath the project (util/SafeWrite.h)
        if (!safewrite::isInsideResolved(trashPath, projectPath)) {
            Out::warning("Not clearing %s: it resolves outside the project (a symbolic link); remove the link.", FileUtils::pathToGenericUtf8(trashPath).c_str());
            return;
        }
        try {
            std::string error;
            if (!safewrite::removeTreeResolvedBeneath(projectPath, trashPath, error)) throw std::runtime_error(error);
            Out::info("Cleared trash directory: %s", trashPath.string().c_str());
        } catch (const std::exception& e) {
            Out::error("Failed to clear trash directory: %s", e.what());
        }
    }
}

bool editor::Project::saveProjectToPath(const std::filesystem::path& path) {
    // Try to create the directory if it doesn't exist
    if (!std::filesystem::exists(path)) {
        try {
            std::filesystem::create_directory(path);
        } catch (const std::exception& e) {
            Out::error("Failed to create project directory: %s", e.what());
            alertUser("Error", "Failed to create project directory!");
            return false;
        }
    }

    // Check if we're moving from temp location
    bool wasTemp = isTempProject();
    std::filesystem::path oldPath = projectPath;

    projectPath = path;
    refreshTrustedRoots();
    // PlayerPrefs are the project's (.jasperengine/user/rundata): read from the new one
    UserSettingsStore::instance().unload();

    // If we're moving from a temp path, handle the file transfers
    if (wasTemp && oldPath != path) {
        try {
            std::filesystem::path oldBuildPath = getProjectInternalPath() / "build";
            if (std::filesystem::exists(oldBuildPath)) {
                std::filesystem::remove_all(oldBuildPath);
            }

            std::filesystem::path oldExtBuildPath = getProjectInternalPath() / "externalbuild";
            if (std::filesystem::exists(oldExtBuildPath)) {
                std::filesystem::remove_all(oldExtBuildPath);
            }

            // Copy all project files from temp dir to the new location
            for (const auto& entry : std::filesystem::directory_iterator(oldPath)) {
                std::filesystem::path destPath = path / entry.path().filename();
                std::filesystem::copy(entry.path(), destPath, 
                                     std::filesystem::copy_options::recursive);
            }

            // Scene filepaths are stored as relative paths within the project,
            // and the directory structure is preserved by the copy, so they
            // remain valid without modification.

            // Delete the temp directory after moving all files
            std::filesystem::remove_all(oldPath);

            // Build settings and the workspace live under .jasperengine, which the copy
            // above carried across, so nothing has to be re-keyed

        } catch (const std::exception& e) {
            Out::error("Failed to move project files: %s", e.what());
            alertUser("Error", "Failed to move project files to the new location!");
            return false;
        }
    }

    // The asset files (and their GUIDs) are under the new root now
    if (oldPath != path) {
        refreshTrustedRoots();   // the folders exist now
        openAssetDatabase();
        if (watchFiles) HotReloadService::get().startWatching(*this);
        if (watchFiles) startVersionControl();
        // the project's editor menus (a text scan of its Editor folders: no project code runs)
        EditorScripts::get().scan(this);
    }

    // the temporary project saved somewhere of its own is locked there (R10)
    if (wasTemp && oldPath != path && !isEditorTempProject()) {
        takeProjectLock(projectPath);
    }

    if (!saveProjectFile()) {
        return false;
    }

    if (!isTempProject()){
        AppSettings::addToRecentProjects(path, false, getName());
        AppSettings::setLastProjectPath(path);
    }
    editor::getEditorHost().updateResourcesPath();

    return true;
}

bool editor::Project::loadProject(const std::filesystem::path requestedPath, bool updateLastOpened) {
    // the load pumps the loading UI (App::reportLoadingProgress beats); its budget covers the gaps
    const diag::BusyScope busy("Loading project", 120'000);
    if (isAnyScenePlaying()) {
        Out::warning("Cannot load a project while a scene is running or stopping.");
        return false;
    }

    try {
        // Absolute: scene and asset paths are project-relative and a relative path is joined to the
        // project directory, so a relative project directory would be joined twice (the CLI makes its
        // --project absolute already, against the directory it started in)
        const std::filesystem::path path =
            requestedPath.empty() || requestedPath.is_absolute() ? requestedPath : std::filesystem::absolute(requestedPath);
        if (!std::filesystem::exists(path)) {
            Out::error("Project directory does not exist: \"%s\"", path.string().c_str());
            return false;
        }

        std::filesystem::path projectFile = path / "project.yaml";
        if (!std::filesystem::exists(projectFile)) {
            Out::error("Project file does not exist: %s", projectFile.string().c_str());
            return false;
        }

        // Load and parse project file
        editor::getEditorHost().reportLoadingProgress("Loading project...");
        LoadReport projectReport;
        projectReport.documentPath = "project.yaml";
        std::unique_ptr<ScopedLoadReport> projectReportScope = std::make_unique<ScopedLoadReport>(projectReport);
        YAML::Node projectNode = DocumentFiles::read(projectFile, DocumentKind::Project).root;

        // One editor per project (R10): another live editor holding it stops the load
        // unless the user (or --ignore-lock) says otherwise
        const std::filesystem::path previousProject = projectPath;
        if (!takeProjectLock(path)) {
            return false;
        }

        // The workspace is written from the live model, which resetConfigs() is about
        // to drop, so the outgoing project is flushed first
        if (modelLoaded && !projectPath.empty()) {
            saveWorkspaceFile();
        }
        // the outgoing session ended cleanly: its backups go (R16)
        if (!previousProject.empty() && previousProject != path) {
            SceneBackups::removeOwnedBy(previousProject, io::currentProcessId());
            autosaveScenes.clear();
        }

        editor::getEditorHost().prepareForProjectSwitch();

        resetConfigs();
        projectPath = path;
        refreshTrustedRoots();
        // PlayerPrefs are the project's (.jasperengine/user/rundata): read from the new one
        UserSettingsStore::instance().unload();

        // shader manifests of forks are read from this project's files
        // (the project root, else the assets directory: findShaderFile, read when asked, so
        // the assets directory loaded with the project and any later change apply)
        ShaderManifests::setForkReader([this](const std::string& relativePath, std::string& content) {
            return readShaderFile(relativePath, content);
        });

        // Before any scene or prefab loads: every prefab file gets its GUID (the one-shot
        // upgrade of files written before them) and references resolve through it
        editor::getEditorHost().reportLoadingProgress("Scanning assets...");
        // the assets directory decides which data files are assets
        if (projectNode["assetsDir"]) {
            setAssetsDir(projectNode["assetsDir"].as<std::string>());
        }
        openAssetDatabase();
        // what changes on disk from now on reloads (S05)
        if (watchFiles) HotReloadService::get().startWatching(*this);
        // the project's editor menus (a text scan of its Editor folders: no project code runs)
        EditorScripts::get().scan(this);
        // the temps of saves a crash interrupted, found by that walk (R12)
        sweepOrphanedTemporaryFiles();

        // Per-user state comes from .jasperengine/user/, falling back to the copy an older
        // editor left in project.yaml
        Workspace workspace;
        workspace.load(projectPath);
        workspace.adoptLegacyProjectState(projectNode);
        workspaceMigrationPending = workspace.hasAdoptedLegacyState();

        Stream::decodeProject(this, projectNode, workspace);
        // an older editor's Input Manager actions became an actions asset: a new file in the
        // assets directory (project.yaml keeps its node until the user saves)
        writeInputActionsMigration();
        projectReportScope.reset();
        projectLoadReport = std::move(projectReport);
        logLoadReport(projectLoadReport);
        setUnresolvedSceneStates(workspace.statesForUnresolvedScenes(this));

        // unlike the other settings it applies while editing too, textures are shared with Play
        Engine::setTextureStrategy(textureStrategy);
        editor::getEditorHost().reportLoadingProgress("Finishing project load...");

        // Guarantee a non-empty name: project.yaml files without a "name" field
        // (older or hand-authored projects) would otherwise leave libName empty
        // and generate an invalid CMakeLists.txt. Fall back to the folder name.
        if (getName().empty()) {
            setName(path.filename().string());
        }

        // Create a default scene if no scenes were loaded
        if (scenes.empty()) {
            createNewScene("New Scene", SceneType::SCENE_3D);
        }

        // the repository it is in, polled from now on (S29)
        if (watchFiles) startVersionControl();

        for (auto& sceneProject : scenes) {
            if (sceneProject.opened) {
                removeMissingChildSceneReferences(sceneProject);
            }
        }

        if (editor::getEditorHost().shouldSyncEngineApi()) {
            copyEngineApiToProject();
        }

        editor::getEditorHost().updateResourcesPath();

        // Save this as the last opened project (the Project Hub's record gets its name, S27 R21)
        if (updateLastOpened && !isTempProject()) {
            AppSettings::addToRecentProjects(projectPath, false, getName());
            AppSettings::setLastProjectPath(projectPath);
        }

        clearTrash();
        modelLoaded = true;

        // The whole migration: project.yaml keeps the keys until the user saves for
        // a reason of their own, so opening a project never rewrites a tracked file
        if (workspaceMigrationPending && !saveWorkspaceFile()) {
            Out::warning("This project's editor state could not be written to \"%s\"; project.yaml still holds the only copy",
                         Workspace::getUserDirectory(projectPath).string().c_str());
        }

        // untitled scenes a crashed session was editing, offered once (R17)
        offerUntitledBackups();

        Out::info("Project loaded successfully: \"%s\"", projectPath.string().c_str());
        return true;

    } catch (const YAML::Exception& e) {
        Out::error("Failed to load project YAML: \"%s\"", e.what());
        alertUser("Error", "Failed to load project file!");
        editor::getEditorHost().updateResourcesPath();
        return false;
    } catch (const std::exception& e) {
        Out::error("Failed to load project: \"%s\"", e.what());
        alertUser("Error", "Failed to load project!");
        editor::getEditorHost().updateResourcesPath();
        return false;
    }
}

bool editor::Project::saveLoadedSceneOnStop(SceneProject* loadedSceneProject, SceneProject* editorScene, bool keepModified) {
    if (!loadedSceneProject || !editorScene || editorScene->filepath.empty()) {
        return true;
    }

    bool success = false;
    if (loadedSceneProject == editorScene) {
        success = saveSceneFile(editorScene, editorScene->filepath, false);
    } else {
        // A Play copy has no snapshot to go back to and holds the played state, so only the
        // shader keys it found reach the file
        success = saveSceneShaderKeys(editorScene);
    }

    if (success) {
        editorScene->isModified = keepModified;
        Out::info("Auto-saved scene '%s' on stop", editorScene->name.c_str());
    }

    return success;
}

// The scene's shaderKeys written into its file as it is on disk, the rest of the file untouched
bool editor::Project::saveSceneShaderKeys(SceneProject* sceneProject) {
    fs::path fullPath = sceneProject->filepath;
    if (fullPath.is_relative()) {
        fullPath = getProjectPath() / fullPath;
    }

    try {
        YAML::Node root = YAML::LoadFile(fullPath.string());
        if (!root.IsMap()) {
            Out::error("Failed to save shader keys of scene '%s': %s is not a scene file", sceneProject->name.c_str(), fullPath.string().c_str());
            return false;
        }

        const YAML::Node& onDisk = root;

        // The file's keys stay, as a full save keeps the ones saved before
        for (ShaderKey key : Stream::decodeShaderKeys(onDisk["shaderKeys"])) {
            sceneProject->shaderKeys.insert(key);
        }
        const YAML::Node shaderKeysNode = Stream::encodeShaderKeys(sceneProject->shaderKeys);

        std::string content;
        if (!shaderKeysNode.size()) {
            root.remove("shaderKeys");
            content = YAML::Dump(root);
        } else if (onDisk["shaderKeys"] || !onDisk["maxValues"]) {
            root["shaderKeys"] = shaderKeysNode;
            content = YAML::Dump(root);
        } else {
            // After maxValues, where encodeSceneProject() puts them, so the next full save moves nothing
            YAML::Node ordered(YAML::NodeType::Map);
            for (const auto& item : onDisk) {
                const std::string key = item.first.as<std::string>();
                ordered[key] = item.second;
                if (key == "maxValues") {
                    ordered["shaderKeys"] = shaderKeysNode;
                }
            }
            content = YAML::Dump(ordered);
        }

        // a Document write with the file's line endings; the hash the crash backups compare
        // against follows the bytes on disk (S03 R13)
        const std::string text = FileUtils::withLineEndings(fullPath, content + "\n");
        const io::WriteResult written = DocumentFiles::write(fullPath, text);
        if (!written.ok()) {
            Out::error(DocumentFiles::saveFailureMessage(fullPath, written));
            return false;
        }
        sceneProject->diskSha1 = DocumentFiles::sha1(text);
    } catch (const std::exception& e) {
        Out::error("Failed to save shader keys of scene '%s': %s", sceneProject->name.c_str(), e.what());
        return false;
    }

    return true;
}

void editor::Project::saveScene(uint32_t sceneId, std::function<void(bool)> callback) {
    // Prefab Mode: saving its scene saves the prefab asset
    if (isPrefabStage(sceneId)) {
        const bool saved = savePrefabStage(sceneId);
        if (callback) callback(saved);
        return;
    }

    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject) {
        Out::error("Cannot save scene - invalid scene ID: %u", sceneId);
        if (callback) callback(false);
        return;
    }

    if (sceneProject->playState != ScenePlayState::STOPPED) {
        Out::warning("Cannot save scene '%s' while it is busy.", sceneProject->name.c_str());
        if (callback) callback(false);
        return;
    }

    auto finishWithChildren = [this, sceneId, callback](bool success) {
        if (!success) {
            if (callback) callback(false);
            return;
        }

        saveModifiedChildScenes(sceneId, callback);
    };

    if (!sceneProject->scene) {
        finishWithChildren(true);
        return;
    }

    refusedSaves.erase(sceneId);
    if (!sceneProject->filepath.empty()) {
        if (isSceneSaveGated(sceneProject)) {
            // never through the default alerts: the host decides (a modal in the editor,
            // Refuse when headless)
            DegradedSaveRequest request;
            request.documentName = sceneProject->filepath.filename().string();
            request.path = sceneProject->filepath;
            for (const LoadIssue& issue : sceneProject->loadReport.issues) {
                if (issue.fate == LoadIssueFate::Dropped) {
                    request.dropped.push_back(issue);
                }
            }
            request.saveAnyway = [this, sceneId, finishWithChildren]() {
                if (SceneProject* scene = getScene(sceneId)) {
                    saveSceneToPathAsync(sceneId, scene->filepath, finishWithChildren, DegradedSavePolicy::SaveAnyway);
                }
            };
            request.saveAs = [sceneId]() {
                editor::getEditorHost().registerSaveSceneDialog(sceneId);
            };
            if (editor::getEditorHost().confirmDegradedSave(request) == DegradedSavePolicy::SaveAnyway) {
                saveSceneToPathAsync(sceneId, sceneProject->filepath, finishWithChildren, DegradedSavePolicy::SaveAnyway);
            } else {
                refusedSaves.insert(sceneId);
                if (callback) callback(false);
            }
            return;
        }
        saveSceneToPathAsync(sceneId, sceneProject->filepath, finishWithChildren);
    } else {
        editor::getEditorHost().registerSaveSceneDialog(sceneId, [finishWithChildren]() {
            finishWithChildren(true);
        });
    }
}

void editor::Project::saveSceneListSequentially(std::vector<uint32_t> sceneIds, std::function<void(bool)> callback) {
    auto ids = std::make_shared<std::vector<uint32_t>>(std::move(sceneIds));
    auto index = std::make_shared<size_t>(0);
    auto anyRefused = std::make_shared<bool>(false);
    auto saveNext = std::make_shared<std::function<void(bool)>>();
    // (it holds itself weakly: the save in flight holds it, a capture of itself was a cycle
    // that leaked every chain, LeakSanitizer)
    std::weak_ptr<std::function<void(bool)>> weakNext = saveNext;
    *saveNext = [this, ids, index, anyRefused, callback, weakNext](bool previousSuccess) {
        if (!previousSuccess) {
            // a document the save gate refused does not stop the others (Save All asks about
            // each degraded one, after saving the clean ones)
            const uint32_t previous = *index > 0 ? (*ids)[*index - 1] : NULL_PROJECT_SCENE;
            if (previous == NULL_PROJECT_SCENE || !refusedSaves.count(previous)) {
                if (callback) callback(false);
                return;
            }
            *anyRefused = true;
        }

        while (*index < ids->size()) {
            uint32_t id = (*ids)[(*index)++];
            if (hasSceneUnsavedChanges(id)) {
                if (auto next = weakNext.lock()) {
                    saveScene(id, [next](bool ok) { (*next)(ok); });
                }
                return;
            }
        }

        if (callback) callback(!*anyRefused);
    };

    (*saveNext)(true);
}

void editor::Project::saveModifiedChildScenes(uint32_t sceneId, std::function<void(bool)> callback) {
    std::vector<uint32_t> involvedIds;
    collectInvolvedScenes(sceneId, involvedIds);

    std::vector<uint32_t> childIds;
    for (uint32_t id : involvedIds) {
        if (id == sceneId) continue;
        const SceneProject* childScene = getScene(id);
        if (childScene && hasLocalUnsavedChanges(id)) {
            childIds.push_back(id);
        }
    }

    saveSceneListSequentially(std::move(childIds), std::move(callback));
}

bool editor::Project::isSceneSaveGated(const SceneProject* sceneProject, const fs::path& path) const {
    if (!sceneProject || sceneProject->filepath.empty() || sceneProject->loadReport.dropped() == 0) {
        return false;
    }
    // Save As to another file is never gated: the original keeps the data
    fs::path target = path.empty() ? sceneProject->filepath : path;
    if (target.is_relative()) {
        target = getProjectPath() / target;
    }
    return target.lexically_normal() == (getProjectPath() / sceneProject->filepath).lexically_normal();
}

bool editor::Project::saveSceneFile(SceneProject* sceneProject, const std::filesystem::path& path, bool stopTransientPreviews,
        DegradedSavePolicy policy, SaveResult* outcome) {
    if (outcome) {
        *outcome = SaveResult::Failed;
    }
    if (!sceneProject || path.empty()) {
        return false;
    }
    const diag::BusyScope busy("Saving scene " + sceneProject->name, 120'000);

    // A file that did not load completely is not written over unless the loss was accepted
    const bool ownFile = isSceneSaveGated(sceneProject, path);
    if (ownFile && policy == DegradedSavePolicy::Refuse) {
        Out::warning("Not saved: \"%s\" did not load completely and saving it would remove %zu value(s) this editor could not read. "
                     "Save Anyway (allow_data_loss) or Save As keeps the original.",
                     sceneProject->filepath.filename().string().c_str(), sceneProject->loadReport.dropped());
        if (outcome) {
            *outcome = SaveResult::RefusedDegraded;
        }
        return false;
    }

    if (stopTransientPreviews) {
        editor::getEditorHost().stopTransientPreviews();
    }

    fs::path fullPath = path;
    if (fullPath.is_relative()) {
        fullPath = getProjectPath() / fullPath;
    }

    std::error_code ec;
    fs::path relPath = fs::relative(fullPath, getProjectPath(), ec);
    if (ec || relPath.empty()) {
        Out::error("Scene filepath must be relative to project path: %s", path.string().c_str());
        return false;
    }
    const fs::path previousPath = sceneProject->filepath;

    // Its prefabs with changes, and the ones read from a file of an older version (upgraded)
    for (const auto& [filepath, prefab] : prefabAssets) {
        if (prefab.instances.find(sceneProject->id) != prefab.instances.end() && (prefab.isModified || prefab.needsUpgrade)) {
            savePrefabToDisk(filepath);
        }
    }

    SceneMaxValues maxValues;
    maxValues.maxBones = sceneProject->maxValues.maxBones;
    maxValues.legacyMaxColliders2D = sceneProject->maxValues.legacyMaxColliders2D;
    maxValues.legacyMaxColliderPoints2D = sceneProject->maxValues.legacyMaxColliderPoints2D;
    maxValues.legacyMaxColliders3D = sceneProject->maxValues.legacyMaxColliders3D;
    maxValues.legacyMaxColliderVertices3D = sceneProject->maxValues.legacyMaxColliderVertices3D;
    maxValues.legacyMaxColliderIndices3D = sceneProject->maxValues.legacyMaxColliderIndices3D;
    calculateSceneMaxValues(sceneProject, maxValues);
    sceneProject->maxValues = maxValues;

    updateSceneCppScripts(sceneProject);
    updateScenePrefabs(sceneProject);
    updateSceneImports(sceneProject);

    std::set<ShaderKey> shaderKeys;
    collectSavedShaderKeys(sceneProject, shaderKeys);
    sceneProject->shaderKeys = std::move(shaderKeys);

    // paths assigned since the last save get their assets' GUIDs (S04); a background save
    // had it done on the main thread (resolving may reload what a moved path feeds)
    if (sceneProject->playState != ScenePlayState::SAVING) {
        // the grids' changed cells go to their .navgrid files first (S19 R35)
        NavigationEditor::get().saveSceneAssets(this, *sceneProject);
        resolveSceneAssetRefs(*sceneProject, false);
    }

    try {
        YAML::Node root = Stream::encodeSceneProject(this, sceneProject);
        const Guid guid = prepareDocumentIdentity(root, relPath);
        // a file of a Windows checkout keeps its \r\n (what is hashed is what is on disk)
        const std::string text = FileUtils::withLineEndings(fullPath, DocumentFiles::text(root));
        const io::WriteResult written = DocumentFiles::write(fullPath, text);
        if (!written.ok()) {
            const std::string message = DocumentFiles::saveFailureMessage(relPath, written);
            Out::error(message);
            // an unlocked lockable file is read only: version control offers to lock it (S29 R12)
            if (written.error == io::WriteError::ReadOnlyTarget && readOnlySaveHandler) {
                const uint32_t sceneId = sceneProject->id;
                const fs::path file = fullPath;
                editor::getEditorHost().enqueueMainThreadTask([this, sceneId, file]() {
                    readOnlySaveHandler(file, [this, sceneId]() { saveScene(sceneId); });
                });
                return false;
            }
            editor::getEditorHost().enqueueMainThreadTask([this, message]() {
                alertUser("Error", message);
            });
            return false;
        }
        assetDatabase.registerAsset(relPath, guid.str());
        assetIndex.updateReferrer(relPath.generic_string());
        sceneProject->diskSha1 = DocumentFiles::sha1(text);
        const size_t lost = sceneProject->loadReport.dropped();
        if (lost > 0) {
            if (ownFile) {
                std::string list;
                for (const LoadIssue& issue : sceneProject->loadReport.issues) {
                    if (issue.fate == LoadIssueFate::Dropped) {
                        list += "\n  " + issue.describe();
                    }
                }
                Out::warning("Saved \"%s\" without %zu value(s) this editor could not read:%s",
                             relPath.generic_string().c_str(), lost, list.c_str());
            } else if (!previousPath.empty()) {
                Out::warning("The original %s still holds %zu value(s) this save did not keep.",
                             previousPath.filename().string().c_str(), lost);
            }
            // the file written no longer has them
            sceneProject->loadReport.clearDropped();
        }
        sceneProject->loadReport.documentPath = FileUtils::pathToUtf8(relPath);
        // the file has these changes now: the backup protects nothing more (R16)
        deleteSceneBackup(*sceneProject, previousPath);

        // Every id this file carries has reached disk and can no longer be reclaimed
        sceneProject->transientId = false;
        for (const ChildSceneRef& childSceneRef : sceneProject->childScenes) {
            if (SceneProject* childScene = getScene(childSceneRef.id)) {
                childScene->transientId = false;
            }
        }
        markReferencedSceneIdsPersisted(sceneProject->scene);

        sceneProject->filepath = relPath;
        // Saving into a path the project only knew as missing resolves it
        clearUnresolvedScene(relPath);
        // Terrain map writes are asynchronous; when any failed to reach disk the
        // save is incomplete even though the scene YAML is written: keep the
        // scene dirty so quit/open flows still prompt, and report failure so
        // save-and-quit flows don't proceed to close while sculpt data exists
        // only in memory. The next save retries the writes.
        if (!TerrainEditWindow::cleanUnusedTerrainMaps(this)){
            sceneProject->isModified = true;
            return false;
        }
        markSceneSaved(sceneProject);
    } catch (const Stream::IncompleteSave& e) {
        // the encode would have lost a GameObject: nothing written, the scene stays modified
        const std::string message = "Couldn't save \"" + FileUtils::pathToUtf8(relPath) + "\": " + e.what() +
            ". Your changes are still in the editor.";
        Out::error(message);
        editor::getEditorHost().enqueueMainThreadTask([this, message]() {
            alertUser("Error", message);
        });
        return false;
    } catch (const std::exception& e) {
        Out::error("Failed to save scene file: %s", e.what());
        return false;
    }

    if (outcome) {
        *outcome = SaveResult::Saved;
    }
    return true;
}

void editor::Project::markSceneSaved(SceneProject* sceneProject) {
    sceneProject->isModified = false;
    sceneProject->savedAt = CommandHistory::stampSave();
    // the history is the main thread's (a background save tells it from there)
    const uint32_t sceneId = sceneProject->id;
    auto tell = [sceneId]() {
        if (CommandHistory* history = CommandHandle::find(sceneId)) {
            history->markSaved();
        }
    };
    if (editor::getEditorHost().isMainThread()) {
        tell();
    } else {
        editor::getEditorHost().enqueueMainThreadTask(tell);
    }
}

bool editor::Project::saveSceneToPath(uint32_t sceneId, const std::filesystem::path& path, bool stopTransientPreviews,
        DegradedSavePolicy policy, SaveResult* outcome) {
    if (!writeSceneToPath(sceneId, path, stopTransientPreviews, policy, outcome)) {
        return false;
    }

    return saveProject();
}

bool editor::Project::writeSceneToPath(uint32_t sceneId, const std::filesystem::path& path, bool stopTransientPreviews,
        DegradedSavePolicy policy, SaveResult* outcome) {
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject) {
        return false;
    }

    fs::path fullPath = path;
    if (fullPath.is_relative()) {
        fullPath = getProjectPath() / fullPath;
    }

    std::string oldFilepath = sceneProject->filepath.string();
    if (!saveSceneFile(sceneProject, path, stopTransientPreviews, policy, outcome)) {
        return false;
    }

    // Update tabs: if filepath changed, update existing tab entry
    if (sceneProject->opened) {
        if (!oldFilepath.empty() && oldFilepath != sceneProject->filepath.string()) {
            removeTab(TabType::SCENE, oldFilepath);
        }
        addTab(TabType::SCENE, sceneProject->filepath.string());
    }

    writeSceneSource(sceneProject);
    configureGenerator();

    // Written on the main thread, where closed scenes can be read
    if (hasMissingSceneSources()) {
        generatedSourcesDirty.store(true);
    }

    Out::info("Scene saved to: \"%s\"", fullPath.string().c_str());

    return true;
}

void editor::Project::writeSceneSource(const SceneProject* sceneProject) {
    // prefabs only scripts reference or other prefabs nest get their sources too
    loadBuildPrefabs();
    const auto baked = bakePrefabsForExport();
    std::vector<PrefabInstanceInfo> prefabInstances = generator.writePrefabSources(prefabAssets, sceneProject->id, getProjectPath(), getProjectInternalPath(),
        [this](const fs::path& path) { return getPrefabGuid(path); }, baked);
    generator.writeSceneSource(sceneProject->scene, sceneProject->id, sceneProject->name, sceneProject->entities, getSceneCamera(sceneProject), getProjectPath(), getProjectInternalPath(), prefabInstances, collectResourcesInRegistry(sceneProject->scene));
}

bool editor::Project::hasMissingSceneSources() const {
    return std::any_of(scenes.begin(), scenes.end(), [&](const SceneProject& sceneProject){
        return !generator.hasSceneSource(sceneProject.id, sceneProject.name, getProjectInternalPath());
    });
}

// Scenes the editor never saved or played, like one created outside it
void editor::Project::writeMissingSceneSources() {
    for (const SceneProject& sceneProject : scenes) {
        if (generator.hasSceneSource(sceneProject.id, sceneProject.name, getProjectInternalPath())) {
            continue;
        }

        if (sceneProject.scene) {
            writeSceneSource(&sceneProject);
            continue;
        }

        std::unique_ptr<SceneProject> probe;
        try {
            probe.reset(createRuntimeCloneFromSource(&sceneProject));
            writeSceneSource(probe.get());
        } catch (const std::exception& e) {
            Out::warning("Source of scene '%s' not generated: %s", sceneProject.name.c_str(), e.what());
        }
        if (probe) {
            deleteSceneProject(probe.get());
        }
        // forget the prefab instances the probe registered
        cleanupPrefabsForScene(sceneProject.id);
    }
}

void editor::Project::configureGenerator() {
    // the standalone build starts the selected scene (in Prefab Mode, the scene behind it)
    const uint32_t selectedRegular = getSceneBehindPrefabStage(selectedScene);
    const uint32_t startId = getScene(selectedRegular) ? selectedRegular : startSceneId;

    std::vector<editor::SceneBuildInfo> scenesToConfig;
    for (SceneProject& sceneConf : scenes) {
        bool isMain = (startId == sceneConf.id);
        std::vector<uint32_t> involvedSceneIds;
        std::vector<uint32_t> activeSceneIds;
        collectInvolvedScenes(sceneConf.id, involvedSceneIds);
        collectStartActiveScenes(sceneConf.id, activeSceneIds);

        scenesToConfig.push_back({sceneConf.id, sceneConf.name, involvedSceneIds, activeSceneIds, isMain});
    }

    std::vector<SceneScriptSource> mergedCppScripts = collectAllSceneCppScripts();
    std::vector<PrefabSceneInfo> prefabBuildInfos = collectAllPrefabs();
    generator.setAtmosphereQuality(atmosphereQuality);
    generator.setInputSettings(getInputActionsEnginePath(), inputDeadzone);
    generator.setNavigationSettingsCode(generateNavigationSettingsCode(navigationSettings.settings, "    "));
    generator.setTextureDefaults(textureSettings.textureQuality, textureSettings.memoryBudgetMB, textureSettings.lowMemoryFallback);
    generator.setMacOSFrameworks(macOSProjectSettings.frameworks);
    generator.setUIScale(uiScale);
    generator.configure(scenesToConfig, libName, mergedCppScripts, prefabBuildInfos, getProjectPath(), getProjectInternalPath(), getAssetsPath(), getLuaPath(), getBuildScriptDirs(), getCxxStandard(), scalingMode, textureStrategy, canvasWidth, canvasHeight, vsyncEnabled, getWindowSettings(), loadingSettings, TagsAndLayers{tags, layerNames, layerCollisionMatrix3D, layerCollisionMatrix2D, physics3DWorld.maxBodies, physics3DWorld.maxBodyPairs, physics3DWorld.maxContactConstraints}, htmlUiMsaaSamples, threadingSettings, htmlUiRender, mobilePbrTier, shaderCreationSettings, applicationSettings.allowInsecureHttp, getShaderGlobals(), getPlayerSettings(), getAudioSettings(), debugSettings);
}

std::vector<editor::SceneScriptSource> editor::Project::prepareScriptBuild() {
    for (SceneProject& sceneProject : scenes) {
        if (!sceneProject.scene) continue;
        updateSceneCppScripts(&sceneProject);
        writeSceneSource(&sceneProject);
    }
    writeMissingSceneSources();
    configureGenerator();
    generatedSourcesDirty.store(false);
    return collectAllSceneCppScripts();
}

void editor::Project::updateGeneratedSources() {
    if (!generatedSourcesDirty.load() || isAnyScenePlaying()) {
        return;
    }
    // An export clears, rewrites and copies the folder from its own thread: the flag stays set
    // until it has finished, so the change is regenerated after the build
    std::lock_guard<std::mutex> lock(generatedMutex);
    if (generatedExports > 0) {
        return;
    }
    generatedSourcesDirty.store(false);

    // nothing to update until a save or play creates the build
    std::error_code ec;
    if (!fs::exists(getProjectPath() / "CMakeLists.txt", ec)) {
        return;
    }

    try {
        writeMissingSceneSources();
        configureGenerator();
    } catch (const std::exception& e) {
        Out::error("Failed to update generated sources: %s", e.what());
    }
}

void editor::Project::markGeneratedSourcesDirty() {
    generatedSourcesDirty.store(true);
}

void editor::Project::beginGeneratedExport() {
    std::lock_guard<std::mutex> lock(generatedMutex);
    generatedExports++;
}

void editor::Project::endGeneratedExport() {
    std::lock_guard<std::mutex> lock(generatedMutex);
    if (generatedExports > 0) generatedExports--;
}

void editor::Project::saveSceneToPathAsync(uint32_t sceneId, const std::filesystem::path& path, std::function<void(bool)> callback,
        DegradedSavePolicy policy) {
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject) {
        if (callback) callback(false);
        return;
    }

    if (sceneProject->playState != ScenePlayState::STOPPED) {
        Out::warning("Cannot save scene '%s' while it is busy.", sceneProject->name.c_str());
        if (callback) callback(false);
        return;
    }

    editor::getEditorHost().stopTransientPreviews();
    stopVertexAnimatorPreviews();
    NavigationEditor::get().saveSceneAssets(this, *sceneProject);
    resolveSceneAssetRefs(*sceneProject, false);
    for (auto& [prefabPath, prefab] : prefabAssets) {
        if (prefab.registry && prefab.instances.count(sceneId) && (prefab.isModified || prefab.needsUpgrade)) {
            resolveRegistryAssetRefs(prefab.registry.get(), false);
        }
    }
    stopCameraShakePreviews();

    sceneProject->playState = ScenePlayState::SAVING;
    fs::path savePath = path;
    // the save reads component arrays (this scene's, the other open scenes' scripts, the
    // prefabs') while the frames go on: their holes are closed before it starts, by this thread
    compactAllComponents();

    // a long disk write: the io-write channel (one write at a time, in order)
    // the worker writes in the write scope of the thread that started it (an MCP call's project)
    const std::vector<fs::path> writeRoot = safewrite::Scope::currentRoots();
    const bool writeStrict = safewrite::Scope::currentStrict();
    jobs::SubmitOptions saveOptions;
    saveOptions.name = "scene-save";
    sceneSaveTask = jobs::system().ioWriteChannel().submit([this, sceneId, savePath, policy, writeRoot, writeStrict, callback = std::move(callback)]() mutable {
        safewrite::Scope scope(writeRoot, writeStrict);
        bool success = false;
        try {
            success = writeSceneToPath(sceneId, savePath, false, policy);
        } catch (const std::exception& e) {
            Out::error("Failed to save scene in background: %s", e.what());
        }

        editor::getEditorHost().enqueueMainThreadTask([this, sceneId, success, callback = std::move(callback)]() mutable {
            if (success) {
                success = saveProject();
            }

            if (SceneProject* sceneProject = getScene(sceneId)) {
                if (sceneProject->playState == ScenePlayState::SAVING) {
                    sceneProject->playState = ScenePlayState::STOPPED;
                }
            }

            if (callback) callback(success);
        });
    }, saveOptions);
}

void editor::Project::saveAllScenes(std::function<void(bool)> callback) {
    // first: a physics gesture that settles now commits, and its scene counts as changed below
    editor::getEditorHost().stopTransientPreviews();
    stopVertexAnimatorPreviews();
    stopCameraShakePreviews();

    std::vector<uint32_t> sceneIds;
    // the prefab stages first: the scenes saved after them hold their assets' new state
    for (const auto& stage : prefabStages) {
        if (hasUnsavedPrefabStage(stage->sceneProject.id)) {
            sceneIds.push_back(stage->sceneProject.id);
        }
    }
    for (auto& sceneProject : scenes) {
        if (hasSceneUnsavedChanges(sceneProject.id)) {
            sceneIds.push_back(sceneProject.id);
        }
    }

    saveSceneListSequentially(std::move(sceneIds), std::move(callback));
}

void editor::Project::saveLastSelectedScene(std::function<void(bool)> callback){
    saveScene(selectedScene, callback);
}

AABB editor::Project::getEntityWorldAABB(Scene* scene, Entity entity, Scene* mainScene) const{
    AABB aabb;
    Signature signature = scene->getSignature(entity);

    if (signature.test(scene->getComponentId<SplineComponent>())){
        // what the spline generated (the road picks and frames as its GameObject), and its knots
        if (std::shared_ptr<SplineSystem> splines = scene->findSystem<SplineSystem>()){
            aabb = splines->getGeneratedBounds(entity);
        }
        const Transform* transform = scene->findComponent<Transform>(entity);
        if (transform){
            for (const spline::SplineKnot& knot : scene->getComponent<SplineComponent>(entity).spline.knots()){
                aabb.merge(transform->modelMatrix * knot.position);
            }
        }
        if (!aabb.isNull()){
            return aabb;
        }
    }

    if (signature.test(scene->getComponentId<MeshComponent>())){
        aabb = scene->getComponent<MeshComponent>(entity).worldAABB;
    }else if (signature.test(scene->getComponentId<UIComponent>())){
        aabb = scene->getComponent<UIComponent>(entity).worldAABB;
        if (!signature.test(scene->getComponentId<PolygonComponent>()) && signature.test(scene->getComponentId<RectTransformComponent>())){
            RectTransformComponent& layout = scene->getComponent<RectTransformComponent>(entity);
            if (layout.width > 0 && layout.height > 0){
                Vector2 center = GraphicUtils::getRectTransformCenter(scene, entity, layout);
                Transform& transform = scene->getComponent<Transform>(entity);
                aabb = transform.modelMatrix * AABB(-center.x, -center.y, 0, layout.width-center.x, layout.height-center.y, 0);
            }
        }
    }else if (signature.test(scene->getComponentId<LightComponent>()) ||
              signature.test(scene->getComponentId<Light2DComponent>()) ||
              signature.test(scene->getComponentId<CameraComponent>()) ||
              signature.test(scene->getComponentId<AudioSourceComponent>())){
        Transform& transform = scene->getComponent<Transform>(entity);
        float size = 16.0f;
        SceneRender* editorRender = nullptr;
        for (SceneProject* sceneProject : const_cast<Project*>(this)->getScenesAndPrefabStages()){
            if (sceneProject->scene == mainScene){
                editorRender = sceneProject->sceneRender;
                break;
            }
        }
        if (editorRender){
            size = editorRender->billboardScreenScale(transform.worldPosition, 16.0f);
        }else{
            Transform& camtransform = mainScene->getComponent<Transform>(mainScene->getCamera());
            CameraComponent& camera = mainScene->getComponent<CameraComponent>(mainScene->getCamera());
            if (camera.type == CameraType::CAMERA_ORTHO){
                float zoom = (camera.topClip - camera.bottomClip) / std::max(1.0f, (float)Engine::getCanvasHeight());
                size = 16.0f * zoom;
            }else{
                float dist = (transform.worldPosition - camtransform.worldPosition).length();
                size = dist * tan(camera.yfov) * 0.01f;
            }
        }
        aabb = transform.modelMatrix * AABB(-size, -size, -size, size, size, size);
    }else if (signature.test(scene->getComponentId<ShadowCaster2DComponent>())){
        const ShadowCaster2DComponent& occluder = scene->getComponent<ShadowCaster2DComponent>(entity);
        Transform& transform = scene->getComponent<Transform>(entity);

        // hug the polygon points; the entity origin is only the fallback when
        // there is nothing else to bound
        AABB localAABB(Vector3(0, 0, -1), Vector3(0, 0, 1));
        if (occluder.shape == ShadowCaster2DShape::POLYGON && occluder.points.size() > 0){
            localAABB.setNull();
            for (const Vector2& point : occluder.points){
                localAABB.merge(Vector3(point.x, point.y, 0.0f));
            }
            Vector3 mn = localAABB.getMinimum(); mn.z = -1.0f;
            Vector3 mx = localAABB.getMaximum(); mx.z = 1.0f;
            localAABB.setExtents(mn, mx);
        }
        aabb = transform.modelMatrix * localAABB;
    }else if (signature.test(scene->getComponentId<PointsComponent>()) || signature.test(scene->getComponentId<LineRendererComponent>())){
        if (signature.test(scene->getComponentId<Transform>())) {
            const Transform& transform = scene->getComponent<Transform>(entity);
            const Transform& camtransform = mainScene->getComponent<Transform>(mainScene->getCamera());
            const CameraComponent& camera = mainScene->getComponent<CameraComponent>(mainScene->getCamera());

            auto mergeWorldPoint = [&](const Vector3& worldPos) {
                float size;
                if (camera.type == CameraType::CAMERA_PERSPECTIVE) {
                    float dist = (worldPos - camtransform.worldPosition).length();
                    size = dist * std::tan(camera.yfov) * 0.01f;
                } else {
                    size = (camera.topClip - camera.bottomClip) * 0.02f;
                }
                if (size < 0.001f) size = 0.001f;
                aabb.merge(AABB(worldPos.x - size, worldPos.y - size, worldPos.z - size,
                                worldPos.x + size, worldPos.y + size, worldPos.z + size));
            };

            // Always include the entity origin
            mergeWorldPoint(transform.worldPosition);

            if (signature.test(scene->getComponentId<PointsComponent>())){
                const PointsComponent& pts = scene->getComponent<PointsComponent>(entity);
                for (const PointData& pt : pts.points) {
                    if (!pt.visible) continue;
                    mergeWorldPoint(transform.modelMatrix * pt.position);
                }
            }

            if (signature.test(scene->getComponentId<LineRendererComponent>())){
                // either mode, world or local space points; a ribbon of world widths around them
                const LineRendererComponent& lines = scene->getComponent<LineRendererComponent>(entity);
                const float half = (lines.loadedRibbon && lines.widthUnit == LineWidthUnit::World)
                    ? lines.ribbon.maxHalfWidth * LineRibbonBuilder::boundsGrowth(lines.numCornerVertices) : 0.0f;
                for (const Vector3& point : SceneRender::getLinePointWorldPositions(lines, transform)) {
                    mergeWorldPoint(point);
                    if (half > 0.0f) aabb.merge(AABB(point.x - half, point.y - half, point.z - half, point.x + half, point.y + half, point.z + half));
                }
            }
        }
    }

    return aabb;
}

AABB editor::Project::getEntityLocalAABB(Scene* scene, Entity entity) const{
    AABB aabb;
    Signature signature = scene->getSignature(entity);

    if (signature.test(scene->getComponentId<SplineComponent>())){
        for (const spline::SplineKnot& knot : scene->getComponent<SplineComponent>(entity).spline.knots()){
            aabb.merge(knot.position);
        }
        const Transform* transform = scene->findComponent<Transform>(entity);
        std::shared_ptr<SplineSystem> splines = scene->findSystem<SplineSystem>();
        if (transform && splines){
            const AABB generated = splines->getGeneratedBounds(entity);
            if (!generated.isNull()){
                aabb.merge(transform->modelMatrix.inverse() * generated);
            }
        }
        if (!aabb.isNull()){
            return aabb;
        }
    }

    if (signature.test(scene->getComponentId<MeshComponent>())){
        aabb = SceneRender::getMeshLocalAABB(scene->getComponent<MeshComponent>(entity));
    }else if (signature.test(scene->getComponentId<UIComponent>())){
        aabb = scene->getComponent<UIComponent>(entity).aabb;
        if (!signature.test(scene->getComponentId<PolygonComponent>()) && signature.test(scene->getComponentId<RectTransformComponent>())){
            RectTransformComponent& layout = scene->getComponent<RectTransformComponent>(entity);
            if (layout.width > 0 && layout.height > 0){
                Vector2 center = GraphicUtils::getRectTransformCenter(scene, entity, layout);
                aabb = AABB(-center.x, -center.y, 0, layout.width-center.x, layout.height-center.y, 0);
            }
        }
    }else if (signature.test(scene->getComponentId<LightComponent>()) ||
              signature.test(scene->getComponentId<Light2DComponent>()) ||
              signature.test(scene->getComponentId<CameraComponent>()) ||
              signature.test(scene->getComponentId<AudioSourceComponent>())){
        aabb = AABB::ZERO;
    }else if (signature.test(scene->getComponentId<ShadowCaster2DComponent>())){
        const ShadowCaster2DComponent& occluder = scene->getComponent<ShadowCaster2DComponent>(entity);
        aabb.setNull();
        if (occluder.shape == ShadowCaster2DShape::POLYGON && occluder.points.size() > 0){
            // hug the polygon points only
            for (const Vector2& point : occluder.points){
                aabb.merge(Vector3(point.x, point.y, 0.0f));
            }
        }else{
            aabb.merge(Vector3::ZERO); // fallback: the entity origin
        }
    }else if (signature.test(scene->getComponentId<PointsComponent>()) || signature.test(scene->getComponentId<LineRendererComponent>())){
        aabb.setNull();
        // Always include the entity origin (local space zero)
        aabb.merge(Vector3::ZERO);
        if (signature.test(scene->getComponentId<PointsComponent>())){
            const PointsComponent& pts = scene->getComponent<PointsComponent>(entity);
            for (const PointData& pt : pts.points) {
                if (!pt.visible) continue;
                aabb.merge(pt.position);
            }
        }
        if (signature.test(scene->getComponentId<LineRendererComponent>()) && signature.test(scene->getComponentId<Transform>())){
            SceneRender::mergeLineLocalPoints(aabb, scene->getComponent<LineRendererComponent>(entity), scene->getComponent<Transform>(entity));
        }
    }

    return aabb;
}

Entity editor::Project::findBestEntityByRay(const std::vector<Entity>& entities, Scene* scene, const Ray& ray, Scene* sizeScene, SceneType sceneType, float& distance, size_t& index, bool iconsDrawn) const{
    // triangle-exact through the scene's picking BVH (editor/picking/ScenePicker.h)
    return ScenePicker(const_cast<Project*>(this)).bestOnRay(entities, scene, ray, sizeScene, sceneType, distance, index, iconsDrawn);
}

bool editor::Project::selectEntitiesInRect(uint32_t sceneId, const std::vector<Entity>& entities, Scene* scene, const Matrix4& vpMatrix, Vector2 start, Vector2 end, const SceneProject* viewScene){
    Vector2 minRect = Vector2(std::min(start.x, end.x), std::min(start.y, end.y));
    Vector2 maxRect = Vector2(std::max(start.x, end.x), std::max(start.y, end.y));

    // the BVH's frustum of the rectangle finds what may be inside (R15), the corner rule decides
    const SceneProject* sceneProject = getScene(sceneId);
    ScenePicker picker(this);
    PickFilter filter = picker.makeFilter(sceneProject, viewScene, true);
    auto listed = std::make_shared<std::unordered_set<Entity>>(entities.begin(), entities.end());
    filter.accept = [listed](Entity entity){ return listed->count(entity) > 0; };
    Plane planes[6];
    ScenePicker::rectPlanes(vpMatrix, minRect, maxRect, planes);
    std::vector<Entity> candidates;
    scene->getPickBvh().frustum(planes, filter, candidates);
    // icons and handles have no leaf of their own (only bounds the view sizes)
    for (Entity entity : entities){
        if (!scene->isEntityCreated(entity)) continue;
        const Signature signature = scene->getSignature(entity);
        if (signature.test(scene->getComponentId<MeshComponent>()) || signature.test(scene->getComponentId<UIComponent>())) continue;
        if (scene->getEntityEditorFlags(entity) & filter.excludeEditorFlags) continue;
        if (!(filter.layerMask & (1u << (scene->getEntityLayer(entity) & 31)))) continue;
        if (std::find(candidates.begin(), candidates.end(), entity) == candidates.end()){
            candidates.push_back(entity);
        }
    }
    // in the order of the list, as before
    std::unordered_set<Entity> inFrustum(candidates.begin(), candidates.end());

    bool found = false;
    for (auto& entity : entities) {
        if (!inFrustum.count(entity)) continue;
        if (!scene->isEntityCreated(entity)) continue;
        if (!scene->getSignature(entity).test(scene->getComponentId<Transform>())) continue;
        const Transform* drawnTransform = scene->findComponent<Transform>(entity);
        if (!drawnTransform || !drawnTransform->visible) continue;
        if (scene->hasInactiveEntities() && !scene->isEntityActiveInHierarchy(entity)) continue;

        AABB aabb = getEntityLocalAABB(scene, entity);

        if (!aabb.isNull() && !aabb.isInfinite()){
            Transform& transform = scene->getComponent<Transform>(entity);
            const Vector3* corners = aabb.getCorners();

            bool inside = true;
            for (int c = 0; c < 8; c++){
                Vector4 clipCorner = vpMatrix * transform.modelMatrix * Vector4(corners[c], 1.0);
                Vector3 ndcCorner = Vector3(clipCorner) / clipCorner.w;

                if (!(ndcCorner.x >= minRect.x && ndcCorner.x <= maxRect.x && ndcCorner.y >= minRect.y && ndcCorner.y <= maxRect.y)){
                    inside = false;
                    break;
                }
            }

            if (inside){
                addSelectedEntity(sceneId, entity);
                found = true;
            }
        }
    }
    return found;
}

std::vector<Entity> editor::Project::getPickableEntities(const SceneProject* sceneProject) const{
    std::vector<Entity> entities = sceneProject->entities;
    // what a running scene made for itself is drawn too (and selectable, read-only)
    const std::vector<Entity> playCreated = getPlayCreatedEntities(sceneProject);
    entities.insert(entities.end(), playCreated.begin(), playCreated.end());
    return entities;
}

std::vector<editor::Project::SceneViewLayer> editor::Project::getSceneViewLayers(uint32_t sceneId){
    std::vector<SceneViewLayer> layers;

    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || !sceneProject->sceneRender){
        return layers;
    }

    SceneViewLayer base;
    base.sceneId = sceneId;
    base.sceneProject = sceneProject;
    base.base = true;
    base.running = sceneProject->playState == ScenePlayState::PLAYING || sceneProject->playState == ScenePlayState::PAUSED;
    layers.push_back(base);

    const float width = (float)sceneProject->sceneRender->getViewWidth();
    const float height = (float)sceneProject->sceneRender->getViewHeight();
    if (width <= 0.0f || height <= 0.0f){
        return layers;
    }

    // as SceneRender draws them: through the layer scene's camera, over rect
    auto addLayer = [&](uint32_t layerId, SceneProject* layerScene, const Rect& rect, float canvasWidth, float canvasHeight, bool running){
        if (!layerScene || !layerScene->scene){
            return;
        }
        const Entity cameraEntity = layerScene->scene->getCamera();
        const CameraComponent* camera = layerScene->scene->findComponent<CameraComponent>(cameraEntity);
        const Transform* cameraTransform = layerScene->scene->findComponent<Transform>(cameraEntity);
        if (cameraEntity == NULL_ENTITY || !camera || !cameraTransform){
            return;
        }

        SceneViewLayer layer;
        layer.sceneId = layerId;
        layer.sceneProject = layerScene;
        layer.running = running;
        layer.view = makeLayerView(*camera, *cameraTransform, rect, width, height, canvasWidth, canvasHeight);
        layers.push_back(layer);
    };

    if (sceneProject->playState == ScenePlayState::STOPPED){
        // inline child scenes cover the view, their auto-resize cameras sized to it
        const Rect full(0.0f, 0.0f, width, height);
        for (const ChildSceneRef& childSceneRef : sceneProject->childScenes){
            SceneProject* childScene = getScene(childSceneRef.id);
            if (childScene && childScene->expandedInline && childScene->scene){
                addLayer(childSceneRef.id, childScene, full, width, height, false);
            }
        }
    }else if (getPlaySessionSceneId() == sceneId){
        // the running game's layer scenes over its canvas, their cameras as the game has them
        const Rect gameRect = sceneProject->sceneRender->getGameLayerRect((int)width, (int)height);
        for (SceneProject* runtime : getRunningRuntimeLayerScenes(sceneId)){
            addLayer(runtime->id, runtime, gameRect, 0.0f, 0.0f, true);
        }
    }

    return layers;
}

bool editor::Project::isSceneViewLayer(uint32_t viewSceneId, uint32_t sceneId){
    if (sceneId == NULL_PROJECT_SCENE || sceneId == viewSceneId){
        return false;
    }
    const SceneProject* viewScene = getScene(viewSceneId);
    if (!viewScene){
        return false;
    }

    if (viewScene->playState == ScenePlayState::STOPPED){
        if (!hasChildScene(viewSceneId, sceneId)){
            return false;
        }
        const SceneProject* childScene = getScene(sceneId);
        return childScene && childScene->expandedInline && childScene->scene;
    }

    for (SceneProject* runtime : getRunningRuntimeLayerScenes(viewSceneId)){
        if (runtime->id == sceneId){
            return true;
        }
    }
    return false;
}

Entity editor::Project::findObjectByRay(uint32_t sceneId, float x, float y, uint32_t* outSceneId){
    if (outSceneId) *outSceneId = sceneId;

    SceneProject* scenedata = getScene(sceneId);
    if (!scenedata || !scenedata->scene || !scenedata->sceneRender){
        return NULL_ENTITY;
    }

    if (!outSceneId){
        Ray ray = scenedata->sceneRender->screenToRay(x, y);
        float distance = FLT_MAX;
        size_t index = 0;
        return findBestEntityByRay(getPickableEntities(scenedata), scenedata->scene, ray, scenedata->scene, scenedata->sceneType, distance, index);
    }

    // What is drawn on top is picked first: the layers in reverse drawing order, the
    // scene itself last. Inside a layer, the nearest (3D) or the last drawn (2D) wins.
    const std::vector<SceneViewLayer> layers = getSceneViewLayers(sceneId);
    for (size_t i = layers.size(); i-- > 0;){
        const SceneViewLayer& layer = layers[i];

        Ray ray;
        if (layer.base){
            ray = scenedata->sceneRender->screenToRay(x, y);
        }else{
            // outside its rect a layer draws nothing
            if (!layer.view.contains(x, y)) continue;
            ray = layer.view.ray(x, y);
        }

        Scene* layerScene = layer.sceneProject->scene;
        float distance = FLT_MAX;
        size_t index = 0;
        Entity hit = findBestEntityByRay(getPickableEntities(layer.sceneProject), layerScene, ray,
            layer.base ? scenedata->scene : layerScene, layer.sceneProject->sceneType, distance, index, layer.base);
        if (hit != NULL_ENTITY){
            *outSceneId = layer.sceneId;
            return hit;
        }
    }

    return NULL_ENTITY;
}

bool editor::Project::isEntityPickable(uint32_t sceneId, Entity entity) const{
    const SceneProject* scenedata = getScene(sceneId);
    if (!scenedata || !scenedata->scene) return false;

    Scene* scene = scenedata->scene;
    if (!scene->isEntityCreated(entity)) return false;
    if (!scene->getSignature(entity).test(scene->getComponentId<Transform>())) return false;

    AABB aabb = getEntityLocalAABB(scene, entity);
    return !aabb.isNull() && !aabb.isInfinite();
}

bool editor::Project::selectObjectByRay(uint32_t sceneId, float x, float y, bool shiftPressed){
    SceneProject* scenedata = getScene(sceneId);
    if (!scenedata || !scenedata->sceneRender){
        return false;
    }

    uint32_t hitSceneId = sceneId;
    Entity selEntity = findObjectByRay(sceneId, x, y, &hitSceneId);

    if (!scenedata->sceneRender->isAnyGizmoSideSelected()){
        if (selEntity != NULL_ENTITY){
            // a selection is of one scene: Shift adds to it only within that scene
            if (!shiftPressed || getSelectedSceneForProperties() != hitSceneId){
                clearAllSelections(sceneId);
            }
            addSelectedEntity(hitSceneId, selEntity);
            setSelectedSceneForProperties(hitSceneId);
            return true;
        }

        clearAllSelections(sceneId);
        setSelectedSceneForProperties(sceneId);
    }

    return false;
}

bool editor::Project::selectObjectsByRect(uint32_t sceneId, Vector2 start, Vector2 end){
    SceneProject* scenedata = getScene(sceneId);
    if (!scenedata || !scenedata->sceneRender){
        return false;
    }

    clearAllSelections(sceneId);

    // The topmost scene with GameObjects inside the box takes the selection, as a click
    // would: a selection is of one scene
    const std::vector<SceneViewLayer> layers = getSceneViewLayers(sceneId);
    for (size_t i = layers.size(); i-- > 0;){
        const SceneViewLayer& layer = layers[i];
        const Matrix4 vpMatrix = layer.base
            ? scenedata->sceneRender->getCamera()->getViewProjectionMatrix()
            : layer.view.viewProjectionMatrix;

        if (selectEntitiesInRect(layer.sceneId, getPickableEntities(layer.sceneProject), layer.sceneProject->scene, vpMatrix, start, end,
                                 layer.base ? scenedata : nullptr)){
            setSelectedSceneForProperties(layer.sceneId);
            return true;
        }
    }

    setSelectedSceneForProperties(sceneId);
    return false;
}

uint32_t editor::Project::syncSceneRenderEditLayer(uint32_t sceneId){
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || !sceneProject->sceneRender){
        return sceneId;
    }

    const uint32_t propertiesSceneId = selectedSceneForProperties;
    if (propertiesSceneId != sceneId && isSceneViewLayer(sceneId, propertiesSceneId)){
        SceneProject* layerScene = getScene(propertiesSceneId);
        if (layerScene && layerScene->scene && !layerScene->selectedEntities.empty()){
            for (const SceneViewLayer& layer : getSceneViewLayers(sceneId)){
                if (!layer.base && layer.sceneId == propertiesSceneId){
                    sceneProject->sceneRender->setEditLayer(propertiesSceneId, layerScene->scene, layer.view,
                        layerScene->selectedEntities, layerScene->entities, layer.running);
                    return propertiesSceneId;
                }
            }
        }
    }

    sceneProject->sceneRender->clearEditLayer();
    return sceneId;
}

void editor::Project::updateSceneRender(uint32_t sceneId){
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || !sceneProject->sceneRender){
        return;
    }

    syncSceneRenderEditLayer(sceneId);
    // what the Scene view hides (R22, R23) has no icon or gizmo there either
    sceneProject->sceneRender->update(sceneProject->selectedEntities, sceneview::visibleEntities(*sceneProject, sceneProject->entities),
                                      sceneProject->mainCamera, sceneProject->displaySettings);
}

std::vector<editor::SceneProject>& editor::Project::getScenes(){
    return scenes;
}

const std::vector<editor::SceneProject>& editor::Project::getScenes() const{
    return scenes;
}

std::vector<editor::TabEntry>& editor::Project::getTabs(){
    return tabs;
}

const std::vector<editor::TabEntry>& editor::Project::getTabs() const{
    return tabs;
}

void editor::Project::addTab(TabType type, const std::string& filepath){
    if (!hasTab(type, filepath)){
        tabs.push_back({type, filepath});
    }
}

void editor::Project::removeTab(TabType type, const std::string& filepath){
    tabs.erase(
        std::remove_if(tabs.begin(), tabs.end(), [&](const TabEntry& t){
            return t.type == type && t.filepath == filepath;
        }),
        tabs.end()
    );
}

bool editor::Project::hasTab(TabType type, const std::string& filepath) const{
    return std::any_of(tabs.begin(), tabs.end(), [&](const TabEntry& t){
        return t.type == type && t.filepath == filepath;
    });
}

template<typename T>
T* editor::Project::findScene(uint32_t sceneId) const {
    for (int i = 0; i < scenes.size(); i++) {
        if (scenes[i].id == sceneId) {
            return const_cast<T*>(&scenes[i]);
        }
    }
    return nullptr;
}

// Non-const version
editor::SceneProject* editor::Project::getScene(uint32_t sceneId) {
    if (SceneProject* sceneProject = findScene<editor::SceneProject>(sceneId)) {
        return sceneProject;
    }
    // a prefab stage's scene (Prefab Mode) answers to its id like any scene
    if (SceneProject* stageScene = findPrefabStageScene(sceneId)) {
        return stageScene;
    }
    return findPlayRuntimeScene(sceneId);
}

// Const version
const editor::SceneProject* editor::Project::getScene(uint32_t sceneId) const {
    if (const SceneProject* sceneProject = findScene<const editor::SceneProject>(sceneId)) {
        return sceneProject;
    }
    if (const SceneProject* stageScene = findPrefabStageScene(sceneId)) {
        return stageScene;
    }
    return findPlayRuntimeScene(sceneId);
}

editor::SceneProject* editor::Project::findPlayRuntimeScene(uint32_t sceneId) const {
    if (sceneId == NULL_PROJECT_SCENE || !hasPlayRuntimeIds.load(std::memory_order_acquire)) {
        return nullptr;
    }
    std::scoped_lock lock(playRuntimeIdMutex);
    auto it = playRuntimeIds.find(sceneId);
    return it != playRuntimeIds.end() ? it->second : nullptr;
}

void editor::Project::assignPlayRuntimeId(SceneProject* runtime) {
    if (!runtime) {
        return;
    }

    // What the copy holds before it runs is its own, as for the played scene at Play:
    // anything else is what the running game made (getPlayCreatedEntities)
    if (runtime->scene) {
        const std::vector<Entity> alive = runtime->scene->getEntityList();
        runtime->playStateEntities = std::set<Entity>(alive.begin(), alive.end());
    }

    static std::mutex generatorMutex;
    static std::mt19937 generator{std::random_device{}()};
    std::uniform_int_distribution<uint32_t> distribution(1, std::numeric_limits<uint32_t>::max());

    for (int attempt = 0; attempt < 1000; attempt++) {
        uint32_t candidate;
        {
            std::scoped_lock generatorLock(generatorMutex);
            candidate = distribution(generator);
        }
        if (findScene<SceneProject>(candidate)) {
            continue;
        }

        std::scoped_lock lock(playRuntimeIdMutex);
        if (playRuntimeIds.count(candidate)) {
            continue;
        }
        playRuntimeIds[candidate] = runtime;
        runtime->id = candidate;
        hasPlayRuntimeIds.store(true, std::memory_order_release);
        return;
    }

    Out::error("Could not allocate an id for the Play copy of scene '%s'", runtime->name.c_str());
}

void editor::Project::releasePlayRuntimeId(SceneProject* runtime, uint32_t sourceSceneId) {
    if (!runtime) {
        return;
    }

    std::scoped_lock lock(playRuntimeIdMutex);
    auto it = playRuntimeIds.find(runtime->id);
    if (it != playRuntimeIds.end() && it->second == runtime) {
        playRuntimeIds.erase(it);
    }
    runtime->id = sourceSceneId;
    hasPlayRuntimeIds.store(!playRuntimeIds.empty(), std::memory_order_release);
}

void editor::Project::discardPlayRuntimeState(uint32_t runtimeId, uint32_t sourceSceneId, uint32_t mainSceneId) {
    if (runtimeId == NULL_PROJECT_SCENE || runtimeId == sourceSceneId) {
        return;
    }

    // prefab instances the running game made in the copy, commands kept for it and
    // what the windows remember of it
    cleanupPrefabsForScene(runtimeId);
    CommandHandle::remove(runtimeId);
    editor::getEditorHost().clearSceneWindowState(runtimeId);

    if (selectedSceneForProperties == runtimeId) {
        selectedSceneForProperties = getScene(mainSceneId) ? mainSceneId : selectedScene;
    }
}

std::vector<editor::SceneProject*> editor::Project::getPlayRuntimeScenesOf(uint32_t sceneId) {
    std::vector<SceneProject*> runtimes;

    std::scoped_lock lock(playSessionMutex);
    if (!activePlaySession || activePlaySession->mainSceneId != sceneId) {
        return runtimes;
    }
    for (const PlayRuntimeScene& entry : activePlaySession->runtimeScenes) {
        if (entry.ownedRuntime && entry.copy && entry.copy->id != entry.sourceSceneId) {
            runtimes.push_back(entry.copy);
        }
    }
    return runtimes;
}


// ------------------------------------------------------------------ C++ reload during Play (S23)

namespace {

    // what a script's generated binding depends on: its class and DPROPERTYs
    std::string scriptSourceSignature(const std::vector<editor::SceneScriptSource>& scripts) {
        std::vector<std::string> lines;
        for (const editor::SceneScriptSource& script : scripts) {
            std::string line = script.className + "|" + script.headerPath.generic_string();
            for (const editor::ScriptPropertyInfo& property : script.properties) {
                line += "|" + property.name + (property.isPtr ? "*" + property.ptrTypeName : std::string());
            }
            lines.push_back(line);
        }
        std::sort(lines.begin(), lines.end());
        std::string signature;
        for (const std::string& line : lines) signature += line + "\n";
        return signature;
    }

}

bool editor::Project::startCppReload(const std::vector<fs::path>& changed, std::chrono::steady_clock::time_point firstSeen, std::string& error) {
    if (!conector.isLibraryConnected()) {
        error = "this Play loaded no C++ script library (no C++ script is attached): the change is built at the next Play";
        return false;
    }
    if (cppReload.running || generator.isBuildInProgress()) {
        error = "a C++ build is running";
        return false;
    }
    if (conector.generationCount() >= Conector::MAX_GENERATIONS) {
        error = "this Play has reloaded the C++ scripts " + std::to_string(Conector::MAX_GENERATIONS - 1) + " times: stop Play to reload again";
        getEditorHost().registerConfirmAlert("Stop Play?", "The C++ scripts were reloaded " + std::to_string(Conector::MAX_GENERATIONS - 1) +
            " times in this Play, the most one Play keeps loaded. Stop Play now to load the latest code?", [this]() { stopActivePlay(); });
        return false;
    }

    // the DPROPERTYs as the headers declare them now; the generated binding is written again
    // only when a class or a property changed (Play reuses the edit-time scene: the scene
    // factories are never regenerated mid-Play)
    std::vector<SceneScriptSource> scripts = generator.lastScriptSources();
    cppReload.properties.clear();
    for (SceneScriptSource& script : scripts) {
        if (script.headerPath.empty()) continue;
        fs::path header = script.headerPath;
        if (header.is_relative()) header = getProjectPath() / header;
        std::error_code ec;
        if (!fs::is_regular_file(header, ec)) continue;
        std::vector<ScriptProperty> parsed = ScriptParser::parseScriptProperties(header);
        script.properties = toScriptPropertyInfos(parsed);
        cppReload.properties[script.className] = std::move(parsed);
    }
    if (scriptSourceSignature(scripts) != scriptSourceSignature(generator.lastScriptSources())) {
        if (!generator.rewriteScriptSources(scripts)) {
            error = "the script sources of this Play are not known";
            return false;
        }
        Out::info("C++ reload: the script bindings changed (a class or DPROPERTY), their sources are written again");
    }

    cppReload.running = true;
    cppReload.firstSeen = firstSeen;
    cppReload.started = std::chrono::steady_clock::now();
    cppReload.changed = changed;
    const fs::path buildPath = getProjectInternalPath() / "build";
    FileUtils::writeIfChanged(getProjectInternalPath() / "PluginGeneration.cmake", Generator::pluginGenerationCMake(conector.generationCount()));
    generator.build(getProjectPath(), buildPath, playBuildKit.cCompiler, playBuildKit.cxxCompiler, playBuildKit.generator, playBuildJobs);
    Out::info("C++ reload: building the scripts (the running code stays until the build is done)");
    return true;
}

void editor::Project::updateCppReload() {
    if (!cppReload.running || generator.isBuildInProgress()) return;
    cppReload.running = false;
    // the build's own events (diagnostics) first
    ScriptStatus::get().update();

    ScriptReloadService::Record record;
    record.language = "cpp";
    if (!cppReload.changed.empty()) {
        const fs::path rel = cppReload.changed.front().lexically_normal().lexically_relative(getProjectPath().lexically_normal());
        record.path = rel.empty() || *rel.begin() == ".." ? cppReload.changed.front().generic_string() : rel.generic_string();
    }
    if (!isAnyScenePlaying()) return;

    if (!generator.didLastBuildSucceed()) {
        record.ok = false;
        record.findings = ScriptStatus::get().state().cppRow.findings;
        record.cppState = "failed";
        Out::error("C++ reload: the build failed \xE2\x80\x94 the last good build keeps running");
        ScriptReloadService::get().cppReloadFinished(record);
        return;
    }

    const fs::path buildPath = getProjectInternalPath() / "build";
    if (!conector.connectNext(buildPath, libName)) {
        record.ok = false;
        const Conector::LoadCheck& check = conector.lastLoadCheck();
        record.findings.push_back({record.path, 0, 0, check.message});
        if (check.refused) {
            ScriptStatus::get().setCpp(CppScriptState::Refused, {}, check.message);
            record.cppState = "refused";
        } else {
            ScriptStatus::get().setCpp(CppScriptState::Failed, record.findings, "C++ library not loaded \xE2\x80\x94 using last good build");
            record.cppState = "failed";
        }
        ScriptReloadService::get().cppReloadFinished(record);
        return;
    }
    applyCppReload();
}

void editor::Project::applyCppReload() {
    const auto swapStart = std::chrono::steady_clock::now();
    ScriptReloadService::Record record;
    record.language = "cpp";
    if (!cppReload.changed.empty()) {
        const fs::path rel = cppReload.changed.front().lexically_normal().lexically_relative(getProjectPath().lexically_normal());
        record.path = rel.empty() || *rel.begin() == ".." ? cppReload.changed.front().generic_string() : rel.generic_string();
    }

    using ScriptBaseFn = ScriptBase* (*)(const char*, void*);
    const size_t oldGeneration = conector.currentGeneration();
    const size_t newGeneration = conector.generationCount() - 1;
    auto oldBase = reinterpret_cast<ScriptBaseFn>(conector.symbol(oldGeneration, "jasper_scriptBase"));
    auto newBase = reinterpret_cast<ScriptBaseFn>(conector.symbol(newGeneration, "jasper_scriptBase"));
    auto oldCleanup = reinterpret_cast<ObjectManager::ScriptHook>(conector.symbol(oldGeneration, "cleanupEntityScripts"));
    auto newInit = reinterpret_cast<ObjectManager::ScriptHook>(conector.symbol(newGeneration, "initEntityScripts"));
    if (!oldCleanup || !newInit) {
        record.ok = false;
        record.findings.push_back({record.path, 0, 0, "the script library has no initEntityScripts/cleanupEntityScripts"});
        record.cppState = "failed";
        ScriptStatus::get().setCpp(CppScriptState::Failed, record.findings, "C++ reload failed \xE2\x80\x94 using last good build");
        ScriptReloadService::get().cppReloadFinished(record);
        return;
    }

    int instances = 0;
    for (Scene* scene : getPlayingScenes()) {
        // 1. the C++ instances' state: their DPROPERTYs, then their own hook
        struct Saved {
            Entity entity;
            size_t index;
            ScriptReloadState state;
        };
        std::vector<Saved> saved;
        std::vector<Entity> entities;
        auto scripts = scene->getComponentArray<ScriptComponent>();
        for (size_t i = 0; i < scripts->size(); i++) {
            const Entity entity = scripts->getEntity(i);
            ScriptComponent& component = scripts->getComponentFromIndex(i);
            bool any = false;
            for (size_t s = 0; s < component.scripts.size(); s++) {
                ScriptEntry& entry = component.scripts[s];
                if (entry.type != ScriptType::CPP || !entry.instance) continue;
                any = true;
                Saved item{entity, s, {}};
                for (ScriptProperty& property : entry.properties) {
                    property.syncFromMember();
                    item.state.values[property.name] = property.value;
                }
                if (ScriptBase* base = oldBase ? oldBase(entry.className.c_str(), entry.instance) : nullptr) {
                    try {
                        base->saveReloadState(item.state);
                    } catch (const std::exception& e) {
                        Out::error("%s::saveReloadState threw: %s", entry.className.c_str(), e.what());
                    }
                }
                saved.push_back(std::move(item));
            }
            if (any) entities.push_back(entity);
        }
        if (entities.empty()) continue;

        // 2. the old library's destructors (and the owner release of their subscriptions)
        oldCleanup(scene, entities.data(), entities.size());

        // the members the properties pointed at are gone; the new DPROPERTYs by name
        for (Entity entity : entities) {
            ScriptComponent* component = scene->findComponent<ScriptComponent>(entity);
            if (!component) continue;
            for (ScriptEntry& entry : component->scripts) {
                if (entry.type != ScriptType::CPP) continue;
                for (ScriptProperty& property : entry.properties) {
                    property.memberPtr = nullptr;
                    property.ownedInstance = nullptr;
                }
                auto parsed = cppReload.properties.find(entry.className);
                if (parsed != cppReload.properties.end()) mergeScriptProperties(entry.properties, parsed->second);
            }
        }

        // 3. the new library's constructors (isReloading), bound to the carried-over values
        ScriptBase::setReloading(true);
        try {
            newInit(scene, entities.data(), entities.size());
        } catch (const std::exception& e) {
            Out::error("A C++ script constructor threw during the reload: %s", e.what());
        } catch (...) {
            Out::error("A C++ script constructor threw during the reload");
        }
        ScriptBase::setReloading(false);

        // 4. their own state back
        for (Saved& item : saved) {
            ScriptComponent* component = scene->findComponent<ScriptComponent>(item.entity);
            if (!component || item.index >= component->scripts.size()) continue;
            ScriptEntry& entry = component->scripts[item.index];
            if (!entry.instance) continue;
            instances++;
            if (ScriptBase* base = newBase ? newBase(entry.className.c_str(), entry.instance) : nullptr) {
                try {
                    base->loadReloadState(item.state);
                } catch (const std::exception& e) {
                    Out::error("%s::loadReloadState threw: %s", entry.className.c_str(), e.what());
                }
            }
        }
    }

    // 5. what instantiates or destroys from now on uses the new library
    conector.makeLatestCurrent();

    record.ok = true;
    record.instances = instances;
    record.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - swapStart).count();
    record.sinceSaveMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cppReload.firstSeen).count();
    record.cppState = "up_to_date";
    for (const SceneScriptSource& script : generator.lastScriptSources()) record.classes.push_back(script.className);
    ScriptStatus::get().setCpp(CppScriptState::UpToDate);
    Out::success("[Script Reload] Reloaded the C++ scripts (%d instance%s, library generation %zu of this Play; %.1f ms, %.0f ms after the save)",
                 instances, instances == 1 ? "" : "s", conector.generationCount(), record.ms, record.sinceSaveMs);
    ScriptReloadService::get().cppReloadFinished(record);
}

std::vector<Scene*> editor::Project::getPlayingScenes() const {
    std::vector<Scene*> scenes;
    std::scoped_lock lock(playSessionMutex);
    if (!activePlaySession || activePlaySession->cancelled.load(std::memory_order_acquire) ||
        !activePlaySession->startupSucceeded.load(std::memory_order_acquire)) {
        return scenes;
    }
    for (const PlayRuntimeScene& entry : activePlaySession->runtimeScenes) {
        // the played scene by its id (runtimeOf): the scenes vector may have moved it
        const SceneProject* runtime = entry.ownedRuntime ? entry.copy : getScene(entry.sourceSceneId);
        if (runtime && runtime->scene && entry.initialized &&
            std::find(scenes.begin(), scenes.end(), runtime->scene) == scenes.end()) {
            scenes.push_back(runtime->scene);
        }
    }
    return scenes;
}

uint32_t editor::Project::getPlayRuntimeSceneId(uint32_t sourceSceneId) const {
    std::scoped_lock lock(playSessionMutex);
    if (!activePlaySession || activePlaySession->cancelled.load(std::memory_order_acquire) ||
        !activePlaySession->startupSucceeded.load(std::memory_order_acquire)) {
        return NULL_PROJECT_SCENE;
    }
    for (const PlayRuntimeScene& entry : activePlaySession->runtimeScenes) {
        if (entry.ownedRuntime && entry.sourceSceneId == sourceSceneId && entry.copy && entry.copy->id != sourceSceneId) {
            return entry.copy->id;
        }
    }
    return NULL_PROJECT_SCENE;
}

uint32_t editor::Project::getPlaySourceSceneId(uint32_t sceneId) const {
    if (!findPlayRuntimeScene(sceneId)) {
        return sceneId;
    }
    std::scoped_lock lock(playSessionMutex);
    if (activePlaySession) {
        for (const PlayRuntimeScene& entry : activePlaySession->runtimeScenes) {
            if (entry.ownedRuntime && entry.copy && entry.copy->id == sceneId) {
                return entry.sourceSceneId;
            }
        }
    }
    return sceneId;
}

bool editor::Project::isPlayRuntimeScene(uint32_t sceneId) const {
    return findPlayRuntimeScene(sceneId) != nullptr;
}

void editor::Project::syncPlayRuntimeSelections() {
    if (!hasPlayRuntimeIds.load(std::memory_order_acquire)) {
        return;
    }

    std::vector<std::pair<SceneProject*, bool>> runtimes;
    {
        std::scoped_lock lock(playSessionMutex);
        // before startup nothing shows the copies, and Stop hands their selection back itself
        if (!activePlaySession || !activePlaySession->startupSucceeded.load(std::memory_order_acquire) ||
            activePlaySession->cancelled.load(std::memory_order_acquire)) {
            return;
        }
        for (const PlayRuntimeScene& entry : activePlaySession->runtimeScenes) {
            if (entry.ownedRuntime && entry.copy && entry.copy->scene && entry.copy->id != entry.sourceSceneId) {
                runtimes.push_back({entry.copy, Engine::isSceneRunning(entry.copy->scene)});
            }
        }
    }

    for (const auto& [runtime, running] : runtimes) {
        if (running) {
            deselectDestroyedEntities(runtime);
            continue;
        }
        // a scene the game took off is gone, as in Unity, and so is its selection
        if (!runtime->selectedEntities.empty()) {
            runtime->selectedEntities.clear();
            runtime->needUpdateRender = true;
        }
        if (selectedSceneForProperties == runtime->id) {
            selectedSceneForProperties = selectedScene;
        }
    }
}

editor::SceneProject* editor::Project::getSelectedScene(){
    return getScene(selectedScene);
}

const editor::SceneProject* editor::Project::getSelectedScene() const{
    return getScene(selectedScene);
}

uint32_t editor::Project::allocateSceneId() const{
    static std::mt19937 generator{std::random_device{}()};
    std::uniform_int_distribution<uint32_t> distribution(1, std::numeric_limits<uint32_t>::max());

    // The loop covers collisions inside this project, the size of the space those
    // against a branch not merged yet
    for (int attempt = 0; attempt < 1000; attempt++){
        const uint32_t candidate = distribution(generator);
        if (!getScene(candidate)){
            return candidate;
        }
    }

    Out::error("Could not allocate a free scene id");
    return NULL_PROJECT_SCENE;
}

void editor::Project::setSelectedSceneId(uint32_t selectedScene){
    if (this->selectedScene != selectedScene){
        if (SceneProject* previousScene = getScene(this->selectedScene)){
            if (previousScene->sceneRender){
                previousScene->sceneRender->getUILayer()->setCameraGaugeVisible(false);
            }
        }

        this->selectedScene = selectedScene;
        this->selectedSceneForProperties = selectedScene;

        // The standalone build starts the selected scene, but opening a project writes nothing.
        if (modelLoaded){
            generatedSourcesDirty.store(true);
        }

        // A newly selected scene must draw at least once.
        if (SceneProject* sceneProject = getScene(selectedScene)){
            sceneProject->needUpdateRender = true;
        }
    }
}

uint32_t editor::Project::getSelectedSceneId() const{
    return selectedScene;
}

void editor::Project::setSelectedSceneForProperties(uint32_t selectedScene){
    this->selectedSceneForProperties = selectedScene;
}

uint32_t editor::Project::getSelectedSceneForProperties() const{
    return selectedSceneForProperties;
}

bool editor::Project::isTempProject() const{
    // The menu asks every frame, and relative() opens both paths
    if (projectPath == tempCheckPath) {
        return tempCheckResult;
    }
    tempCheckPath = projectPath;

    std::error_code ec;
    auto relPath = std::filesystem::relative(projectPath, std::filesystem::temp_directory_path(), ec);

    // relative() returns empty across different roots, like a project on D: while temp is on C:
    tempCheckResult = !ec && !relPath.empty() && *relPath.begin() != "..";
    return tempCheckResult;
}

bool editor::Project::isTempUnsavedProject() const{
    bool isTemp = isTempProject();

    if (isTemp){
        for (auto& scene : scenes){
            if (!scene.filepath.empty() || scene.isModified){
                return true;
            }
        }
    }

    return false;
}

std::filesystem::path editor::Project::getProjectPath() const{
    return projectPath;
}

std::filesystem::path editor::Project::getProjectInternalPath() const{
    return projectPath / ".jasperengine";
}

std::filesystem::path editor::Project::getImportPath() const{
    return getProjectInternalPath() / "imported";
}

fs::path editor::Project::getUserDataPath() const{
    // One player's progress, not project content. At the assets root, as a standalone
    // build has it, every playtest would leave a modified file in the working tree.
    const fs::path path = getProjectInternalPath() / "user" / "rundata";
    std::error_code ec;
    safewrite::EditorInternal ownRunData(path);   // Play's data folder, under .jasperengine (util/SafeWrite.h)
    FileUtils::createDirectories(path, ec);
    if (ec) {
        Out::error("Failed to create the Play mode data directory: %s", ec.message().c_str());
        return getAssetsPath();
    }
    return path;
}

void editor::Project::addUnresolvedScene(size_t listPosition, const fs::path& filepath){
    const fs::path normalized = filepath.lexically_normal();
    auto it = std::find_if(unresolvedScenes.begin(), unresolvedScenes.end(),
        [&normalized](const std::pair<size_t, fs::path>& entry) { return entry.second == normalized; });
    if (it == unresolvedScenes.end()) {
        unresolvedScenes.emplace_back(listPosition, normalized);
    }
}

// Without this, restoring a file mid-session opens the scene with defaults and the
// next save writes those over what was kept for it.
void editor::Project::applyRetainedSceneState(SceneProject& sceneProject){
    if (sceneProject.filepath.empty()) {
        return;
    }

    auto it = unresolvedSceneStates.find(sceneProject.filepath.lexically_normal().generic_string());
    if (it == unresolvedSceneStates.end()) {
        return;
    }

    Stream::decodeSceneDisplaySettings(it->second, sceneProject.displaySettings);
    if (it->second["editorCamera"]) {
        sceneProject.editorCameraState = YAML::Clone(it->second["editorCamera"]);
    }
}

// project.yaml lists scenes in model order, so one left at the end where loadScene()
// appended it would reorder the shared file
void editor::Project::restoreUnresolvedScenePosition(const fs::path& filepath){
    const fs::path normalized = filepath.lexically_normal();
    if (scenes.size() < 2 || scenes.back().filepath.lexically_normal() != normalized) {
        return;
    }

    size_t listPosition = 0;
    bool wasUnresolved = false;
    for (const auto& [position, path] : unresolvedScenes) {
        if (path == normalized) {
            listPosition = position;
            wasUnresolved = true;
        }
    }
    if (!wasUnresolved) {
        return;
    }

    // Its recorded position counts entries still missing ahead of it, which hold no
    // place in scenes
    size_t missingAhead = 0;
    for (const auto& [position, path] : unresolvedScenes) {
        if (path != normalized && position < listPosition) {
            missingAhead++;
        }
    }

    const size_t target = std::min(listPosition - std::min(listPosition, missingAhead), scenes.size() - 1);
    std::rotate(scenes.begin() + target, scenes.end() - 1, scenes.end());
}

bool editor::Project::clearUnresolvedScene(const fs::path& filepath){
    const fs::path normalized = filepath.lexically_normal();
    unresolvedSceneStates.erase(normalized.generic_string());

    const size_t before = unresolvedScenes.size();
    unresolvedScenes.erase(
        std::remove_if(unresolvedScenes.begin(), unresolvedScenes.end(),
            [&normalized](const std::pair<size_t, fs::path>& entry) { return entry.second == normalized; }),
        unresolvedScenes.end());
    return unresolvedScenes.size() != before;
}

const std::vector<std::pair<size_t, fs::path>>& editor::Project::getUnresolvedScenes() const{
    return unresolvedScenes;
}

void editor::Project::setUnresolvedSceneStates(std::map<std::string, YAML::Node> states){
    unresolvedSceneStates = std::move(states);
}

const std::map<std::string, YAML::Node>& editor::Project::getUnresolvedSceneStates() const{
    return unresolvedSceneStates;
}

fs::path editor::Project::getTerrainMapsDir() const{
    // Painted maps are referenced like any other texture, so they live under the
    // assets root and ship with it.
    return getAssetsPath() / "terrain_maps";
}

fs::path editor::Project::getThumbsDir() const{
    return getProjectInternalPath() / "thumbs";
}

fs::path editor::Project::getThumbnailPath(const fs::path& originalPath) const {
    fs::path thumbsDir = getThumbsDir();
    fs::path resolvedPath = originalPath;

    if (resolvedPath.is_relative() && !projectPath.empty()) {
        resolvedPath = projectPath / resolvedPath;
    }

    resolvedPath = resolvedPath.lexically_normal();

    fs::path relativePath;
    if (!projectPath.empty()) {
        relativePath = resolvedPath.lexically_relative(projectPath.lexically_normal());
    }

    // keyed by the asset's GUID and its content fingerprint (S04 R18): a rename or a move keeps
    // the thumbnail, an edit makes a new one; a file without a GUID by its path
    const Guid guid = relativePath.empty() ? Guid() : assetDatabase.getAssetGuid(relativePath);
    std::string hashInput = !guid.isNull() ? "guid:" + guid.str()
                          : relativePath.empty() ? resolvedPath.generic_string() : relativePath.generic_string();

    std::error_code ec;
    const auto fileSize = fs::file_size(resolvedPath, ec);
    if (!ec) {
        hashInput += "_" + std::to_string(static_cast<uint64_t>(fileSize));
    }

    ec.clear();
    const auto modTime = fs::last_write_time(resolvedPath, ec);
    if (!ec) {
        hashInput += "_" + std::to_string(static_cast<int64_t>(modTime.time_since_epoch().count()));
    }

    // Hash the combined string
    std::string hash = SHA1::hash(hashInput);

    std::string thumbFilename = hash + ".thumb.png";
    return thumbsDir / thumbFilename;
}

// The selection outline and gizmo are drawn, so a selection change needs a redraw: of the
// scene, and of the scenes that show it as an inline child scene (their tools edit it)
void editor::Project::markSelectionChanged(SceneProject* sceneProject){
    sceneProject->needUpdateRender = true;
    for (SceneProject& other : scenes){
        if (findChildScene(other.childScenes, sceneProject->id) != other.childScenes.end()){
            other.needUpdateRender = true;
        }
    }
}

void editor::Project::setSelection(uint32_t sceneId, const SelectionState& state, SelectionCause cause){
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject){
        return;
    }
    // a GameObject takes the Inspector back from the Project window's files
    if (!state.entities.empty()){
        inspectedAssets.clear();
    }
    // the active GameObject last; no copy when the state already is that way, and none at
    // all when nothing changes (a click on what is selected)
    const bool ordered = state.active == NULL_ENTITY || (!state.entities.empty() && state.entities.back() == state.active) ||
        std::find(state.entities.begin(), state.entities.end(), state.active) == state.entities.end();
    if (ordered && sceneProject->selectedEntities == state.entities){
        return;
    }
    std::vector<Entity> entities = state.entities;
    if (!ordered){
        entities.erase(std::find(entities.begin(), entities.end(), state.active));
        entities.push_back(state.active);
        if (sceneProject->selectedEntities == entities){
            return;
        }
    }

    const bool record = cause == SelectionCause::User && recordSelectionInUndo && !CommandHistory::isApplying() &&
        !isPlayRuntimeScene(sceneId) && !UndoTimeline::canRedo(sceneId);
    if (record){
        SelectionState after;
        after.entities = std::move(entities);
        after.active = after.entities.empty() ? NULL_ENTITY : after.entities.back();
        // its execute writes the selection; a refused push still selects
        if (CommandHandle::get(sceneId)->addCommand(new SelectionCmd(this, sceneId, getSelection(sceneId), after, selectionGesture))){
            return;
        }
        entities = after.entities;
    }

    markSelectionChanged(sceneProject);
    sceneProject->selectedEntities = std::move(entities);
}

editor::SelectionState editor::Project::getSelection(uint32_t sceneId) const{
    SelectionState state;
    if (const SceneProject* sceneProject = getScene(sceneId)){
        state.entities = sceneProject->selectedEntities;
        state.active = state.entities.empty() ? NULL_ENTITY : state.entities.back();
    }
    return state;
}

void editor::Project::beginSelectionGesture(){
    selectionGesture++;
    selectionGestureHeld = true;
}

void editor::Project::endSelectionGesture(){
    selectionGestureHeld = false;
    selectionGesture++;
}

void editor::Project::nextSelectionFrame(){
    if (!selectionGestureHeld){
        selectionGesture++;
    }
}

void editor::Project::setInspectedAssets(std::vector<std::filesystem::path> assets){
    inspectedAssets = std::move(assets);
}

const std::vector<std::filesystem::path>& editor::Project::getInspectedAssets() const{
    return inspectedAssets;
}

void editor::Project::replaceSelectedEntities(uint32_t sceneId, std::vector<Entity> selectedEntities){
    // Ctrl-click deselect reaches the selection only through here.
    SelectionState state;
    state.entities = std::move(selectedEntities);
    setSelection(sceneId, state);
}

void editor::Project::setSelectedEntity(uint32_t sceneId, Entity selectedEntity){
    SelectionState state;
    if (selectedEntity != NULL_ENTITY){
        state.entities.push_back(selectedEntity);
    }
    setSelection(sceneId, state);
}

void editor::Project::addSelectedEntity(uint32_t sceneId, Entity selectedEntity){
    SceneProject* sceneProject = getScene(sceneId);
    std::vector<Entity> entities = sceneProject->selectedEntities;
    Scene* scene = sceneProject->scene;
    auto transforms = scene->getComponentArray<Transform>();

    if (selectedEntity != NULL_ENTITY){
        if (std::find(entities.begin(), entities.end(), selectedEntity) == entities.end()) {
            entities.push_back(selectedEntity);
        }
    }

    // removing childs of selected entities
    std::vector<Entity> removeChilds;
    for (auto& entity: entities){
        if (scene->getSignature(entity).test(scene->getComponentId<Transform>())){
            size_t firstIndex = transforms->getIndex(entity);
            size_t branchIndex = scene->findBranchLastIndex(entity);
            for (int t = (firstIndex+1); t <= branchIndex; t++){
                Entity childEntity = transforms->getEntity(t);
                if (std::find(entities.begin(), entities.end(), childEntity) != entities.end()) {
                    removeChilds.push_back(childEntity);
                    #ifdef _DEBUG
                    printf("DEBUG: Removed entity %u from selection\n", childEntity);
                    #endif
                }
            }
        }
    }
    entities.erase(
        std::remove_if(entities.begin(), entities.end(), [&removeChilds](Entity value) {
            return std::find(removeChilds.begin(), removeChilds.end(), value) != removeChilds.end();
        }),
        entities.end()
    );

    SelectionState state;
    state.entities = std::move(entities);
    setSelection(sceneId, state);
}

bool editor::Project::isSelectedEntity(uint32_t sceneId, Entity selectedEntity){
    const SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject) {
        return false;
    }
    const std::vector<Entity>& entities = sceneProject->selectedEntities;

    if (std::find(entities.begin(), entities.end(), selectedEntity) != entities.end()) {
        return true;
    }

    return false;
}

void editor::Project::deselectDestroyedEntities(SceneProject* sceneProject){
    const std::vector<Entity>& selected = sceneProject->selectedEntities;
    auto destroyed = std::find_if(selected.begin(), selected.end(), [sceneProject](Entity entity) {
        return !sceneProject->scene->isEntityCreated(entity);
    });
    if (destroyed == selected.end()){
        return;
    }
    SelectionState state;
    for (Entity entity : selected){
        if (sceneProject->scene->isEntityCreated(entity)){
            state.entities.push_back(entity);
        }
    }
    // not a step of its own: what destroyed them is
    setSelection(sceneProject->id, state, SelectionCause::Forced);
}

void editor::Project::clearSelectedEntities(uint32_t sceneId){
    setSelection(sceneId, SelectionState());
}

void editor::Project::clearAllSelections(uint32_t sceneId){
    clearSelectedEntities(sceneId);
    for (uint32_t cId : getChildScenes(sceneId)) {
        clearSelectedEntities(cId);
    }
    // while it plays, its game's layer scenes are what its Scene view and Hierarchy select
    for (SceneProject* runtime : getPlayRuntimeScenesOf(sceneId)) {
        if (!runtime->selectedEntities.empty()) {
            runtime->selectedEntities.clear();
            runtime->needUpdateRender = true;
        }
    }
}

// sceneId may name no scene: an editor whose project did not open has none, and still draws
std::vector<Entity> editor::Project::getSelectedEntities(uint32_t sceneId) const{
    const SceneProject* sceneProject = getScene(sceneId);
    return sceneProject ? sceneProject->selectedEntities : std::vector<Entity>();
}

bool editor::Project::hasSelectedEntities(uint32_t sceneId) const{
    const SceneProject* sceneProject = getScene(sceneId);
    return sceneProject && !sceneProject->selectedEntities.empty();
}

bool editor::Project::hasSelectedSceneUnsavedChanges() const{
    return hasSceneUnsavedChanges(selectedScene);
}

bool editor::Project::hasLocalUnsavedChanges(uint32_t sceneId) const{
    const SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject){
        return false;
    }

    if (sceneProject->isModified){
        return true;
    }

    if (hasUnsavedPrefabs(sceneId)){
        return true;
    }

    return false;
}

bool editor::Project::hasSceneUnsavedChangesImpl(uint32_t sceneId, std::unordered_set<uint32_t>& visited) const{
    if (!visited.insert(sceneId).second){
        return false;
    }

    const SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject){
        return false;
    }

    if (hasLocalUnsavedChanges(sceneId)){
        return true;
    }

    for (const ChildSceneRef& childSceneRef : sceneProject->childScenes) {
        if (hasSceneUnsavedChangesImpl(childSceneRef.id, visited)) {
            return true;
        }
    }

    return false;
}

bool editor::Project::hasSceneUnsavedChanges(uint32_t sceneId) const{
    std::unordered_set<uint32_t> visited;
    return hasSceneUnsavedChangesImpl(sceneId, visited);
}

bool editor::Project::hasUnsavedPrefabs(uint32_t sceneId) const{
    for (const auto& [filepath, prefab] : prefabAssets) {
        if (prefab.isModified && prefab.hasInstances(sceneId)) {
            return true;
        }
    }

    return false;
}

bool editor::Project::hasScenesUnsavedChanges() const{
    for (auto& scene: scenes){
        if (scene.isModified){
            return true;
        }
    }

    // Prefab Mode with Auto Save off
    if (hasUnsavedPrefabStages()){
        return true;
    }

    if (hasUnsavedPrefabs()){
         return true;
    }

    return false;
}

bool editor::Project::hasUnsavedPrefabs() const{
    for (const auto& [filepath, prefab] : prefabAssets) {
        if (prefab.isModified) {
            return true;
        }
    }

    return false;
}


void editor::Project::updateAllScriptsProperties(uint32_t sceneId){
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject) return;

    for (Entity entity : sceneProject->entities) {
        Signature signature = sceneProject->scene->getSignature(entity);
        if (signature.test(sceneProject->scene->getComponentId<ScriptComponent>())) {
            ScriptComponent& scriptComponent = sceneProject->scene->getComponent<ScriptComponent>(entity);
            updateScriptProperties(sceneProject, entity, scriptComponent.scripts);
        }
    }
}

bool editor::Project::updateScriptProperties(SceneProject* sceneProject, Entity entity, std::vector<ScriptEntry>& scripts, const std::string& inMemoryContent, const std::string& inMemoryPath) {
    bool hasChanges = false;

    // Update properties for each script in the component
    for (auto& scriptEntry : scripts) {
        // C++ scripts: keep existing behavior
        if (scriptEntry.type == ScriptType::CPP) {

            if (scriptEntry.headerPath.empty()) continue;

            fs::path fullPath = scriptEntry.headerPath;
            if (fullPath.is_relative()) {
                fullPath = getProjectPath() / fullPath;
            }

            std::vector<ScriptProperty> parsedProperties;
            if (!inMemoryContent.empty() && fullPath.string() == fs::path(inMemoryPath).string()) {
                parsedProperties = ScriptParser::parseScriptPropertiesFromString(inMemoryContent, fullPath.string());
            } else {
                parsedProperties = ScriptParser::parseScriptProperties(fullPath);
            }

            hasChanges |= mergeScriptProperties(scriptEntry.properties, parsedProperties);

            continue;
        }

        // Lua scripts: load properties from Lua file
        if (scriptEntry.type == ScriptType::LUA) {
            if (scriptEntry.path.empty()) continue;

            fs::path fullPath = resolveLuaPath(scriptEntry.path);

            ScriptEntry parsedEntry = scriptEntry;

            if (!inMemoryContent.empty() && fullPath.string() == fs::path(inMemoryPath).string()) {
                ProjectUtils::loadLuaScriptPropertiesFromString(parsedEntry, inMemoryContent, fullPath.string());
            } else {
                ProjectUtils::loadLuaScriptProperties(parsedEntry, fullPath.string());
            }

            hasChanges |= mergeScriptProperties(scriptEntry.properties, parsedEntry.properties);

            continue;
        }
    }

    if (hasChanges) {
        sceneProject->isModified = true;
    }

    return hasChanges;
}

std::vector<std::filesystem::path> editor::Project::findProjectFiles(const std::function<bool(const std::string&)>& matches) const {
    std::vector<fs::path> files;
    if (projectPath.empty()) {
        return files;
    }

    std::error_code ec;

    for (watch::LinkedDirectoryIterator it(projectPath, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
        const fs::directory_entry& entry = *it;

        // '.jasperengine' holds the engine copy and the generated resources, 'build' the artifacts,
        // Recordings and EditorSettings no assets either (S27 R31)
        const std::string filename = entry.path().filename().string();
        if (filename.rfind('.', 0) == 0 || filename == "build" || (entry.is_directory(ec) && isNonAssetDir(entry.path()))) {
            if (entry.is_directory(ec)) it.disable_recursion_pending();
            continue;
        }

        if (!entry.is_regular_file(ec) || !matches(entry.path().string())) {
            continue;
        }

        fs::path relativePath = normalizeToProjectRelative(entry.path());
        if (!relativePath.empty() && !relativePath.is_absolute()) {
            files.push_back(std::move(relativePath));
        }
    }

    std::sort(files.begin(), files.end());
    // a file link to a project file is that file (normalizeToProjectRelative resolves it)
    files.erase(std::unique(files.begin(), files.end()), files.end());

    return files;
}

void editor::Project::collectInvolvedScenes(uint32_t sceneId, std::vector<uint32_t>& involvedSceneIds) {
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject) return;

    // Skip if already visited - also guards against cycles in the child-scene graph
    if (std::find(involvedSceneIds.begin(), involvedSceneIds.end(), sceneId) != involvedSceneIds.end()) {
        return;
    }
    involvedSceneIds.push_back(sceneId);

    for (const ChildSceneRef& childSceneRef : sceneProject->childScenes) {
        collectInvolvedScenes(childSceneRef.id, involvedSceneIds);
    }
}

void editor::Project::collectStartActiveScenes(uint32_t sceneId, std::vector<uint32_t>& activeSceneIds) {
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject) return;

    // Skip if already visited - also guards against cycles in the child-scene graph
    if (std::find(activeSceneIds.begin(), activeSceneIds.end(), sceneId) != activeSceneIds.end()) {
        return;
    }
    activeSceneIds.push_back(sceneId);

    for (const ChildSceneRef& childSceneRef : sceneProject->childScenes) {
        if (childSceneRef.startActive) {
            collectStartActiveScenes(childSceneRef.id, activeSceneIds);
        }
    }
}

bool editor::Project::isAnyScenePlaying() const{
    if (isPlaySessionActive()) {
        return true;
    }

    for (const auto& sceneProject : scenes) {
        if (sceneProject.playState != ScenePlayState::STOPPED) {
            return true;
        }
    }
    return false;
}

bool editor::Project::isPlaySessionActive() const{
    std::scoped_lock lock(playSessionMutex);
    return activePlaySession != nullptr;
}

bool editor::Project::isMainScenePlaying() const{
    uint32_t mainSceneId;
    {
        std::scoped_lock lock(playSessionMutex);
        if (!activePlaySession) {
            return false;
        }
        mainSceneId = activePlaySession->mainSceneId;
    }
    for (const auto& sceneProject : scenes) {
        if (sceneProject.id == mainSceneId) {
            return sceneProject.playState == ScenePlayState::PLAYING;
        }
    }
    return false;
}

bool editor::Project::isPlayStarting() const{
    for (const auto& sceneProject : scenes) {
        if (sceneProject.playState == ScenePlayState::LOADING) {
            return true;
        }
    }
    return false;
}

void editor::Project::readSceneFileCppScripts(SceneProject& sceneProject) {
    sceneProject.cppScripts.clear();
    sceneProject.cppScriptsRead = false;
    if (sceneProject.filepath.empty()) {
        return;
    }
    fs::path fullPath = sceneProject.filepath;
    if (fullPath.is_relative()) {
        fullPath = getProjectPath() / fullPath;
    }
    try {
        Stream::decodeSceneCppScripts(&sceneProject, YAML::LoadFile(fullPath.string()));
    } catch (const std::exception& e) {
        Out::warning("Scripts of scene '%s' not read from its file: %s", sceneProject.name.c_str(), e.what());
    }
}

bool editor::Project::collectClosedSceneCppScripts(SceneProject& sceneProject) {
    if (sceneProject.scene || sceneProject.filepath.empty()) {
        return false;
    }
    fs::path fullPath = sceneProject.filepath;
    if (fullPath.is_relative()) {
        fullPath = getProjectPath() / fullPath;
    }
    // the size too: a copy that keeps the time, or two writes within the clock's tick
    std::error_code ec;
    const fs::file_time_type stamp = fs::last_write_time(fullPath, ec);
    const std::uintmax_t size = ec ? 0 : fs::file_size(fullPath, ec);
    if (ec || (sceneProject.cppScriptsRead && stamp == sceneProject.cppScriptsReadStamp && size == sceneProject.cppScriptsReadSize)) {
        return false;
    }

    closedSceneScriptReads++;
    // a file that does not decode warns once per version too
    sceneProject.cppScriptsRead = true;
    sceneProject.cppScriptsReadStamp = stamp;
    sceneProject.cppScriptsReadSize = size;
    std::unique_ptr<SceneProject> probe;
    try {
        probe.reset(createRuntimeCloneFromSource(&sceneProject));
        updateSceneCppScripts(probe.get());
        sceneProject.cppScripts = probe->cppScripts;
    } catch (const std::exception& e) {
        Out::warning("Scripts of scene '%s' not collected: %s", sceneProject.name.c_str(), e.what());
    }
    if (probe) {
        deleteSceneProject(probe.get());
    }
    return true;
}

bool editor::Project::isAnySceneSaving() const{
    for (const auto& sceneProject : scenes) {
        if (sceneProject.playState == ScenePlayState::SAVING) {
            return true;
        }
    }
    return false;
}

void editor::Project::failPlayStartup(const std::shared_ptr<PlaySession>& session, uint32_t sceneId, const std::string& message,
                                      const std::string& alertTitle, const std::string& alertMessage) {
    editor::getEditorHost().enqueueMainThreadTask([this, session, sceneId, message, alertTitle, alertMessage]() {
        if (session->cancelled.load(std::memory_order_acquire)) {
            session->startupThreadDone.store(true, std::memory_order_release);
            return;
        }

        if (!message.empty()) {
            Out::error("%s", message.c_str());
        }

        cleanupPlaySession(session);
        // what the scripts that ran started (Coroutine.start, Timer.after)
        ScriptScheduler::clear();
        SceneManager::clearAll();
        PrefabManager::clearAll();
        ObjectManager::clearAll();
        clearPlayPrefabTemplates();
        Scheduler::game().clear();
        jasperengine::CrashHook::set(nullptr);

        if (SceneProject* sceneProject = getScene(sceneId)) {
            sceneProject->playState = ScenePlayState::STOPPED;
            CommandHandle::get(sceneId)->endPlaySession(false);
        }

        {
            std::scoped_lock lock(playSessionMutex);
            if (activePlaySession == session) {
                activePlaySession.reset();
            }
        }

        session->startupThreadDone.store(true, std::memory_order_release);

        if (!alertTitle.empty()) {
            editor::getEditorHost().registerAlert(alertTitle, alertMessage);
        }
    });
}

bool editor::Project::saveSceneForPlayStartup(SceneProject* sceneProject) {
    if (!sceneProject || sceneProject->filepath.empty()) {
        return false;
    }

    return saveSceneFile(sceneProject, sceneProject->filepath, false);
}

// The scenes as Play starts from them, read and written on the main thread at one moment
editor::Project::PlayStartupPrepared editor::Project::preparePlayStartup(const std::shared_ptr<PlaySession>& session, uint32_t sceneId) {
    PlayStartupPrepared result;
    // Stop runs on this thread too: stopped before this ran, or not at all while it runs
    if (session->cancelled.load(std::memory_order_acquire)) {
        result.status = PlayStartupPrepared::Status::Cancelled;
        return result;
    }

    SceneProject* mainSceneProject = getScene(sceneId);
    if (!mainSceneProject || !mainSceneProject->scene) {
        result.message = "Failed to find scene to start";
        return result;
    }

    std::vector<uint32_t> involvedMainSceneIds;
    collectInvolvedScenes(sceneId, involvedMainSceneIds);

    // Each scene's scripts and saves first, so the prefab bake and the runtime copies below see
    // every scene as saved
    for (SceneProject& currentSceneProject : scenes) {
        bool isInvolvedScene = std::find(involvedMainSceneIds.begin(), involvedMainSceneIds.end(), currentSceneProject.id) != involvedMainSceneIds.end();

        if (currentSceneProject.opened || (isInvolvedScene && currentSceneProject.scene)) {
            updateAllScriptsProperties(currentSceneProject.id);

            if (isInvolvedScene) {
                warnPreservedNotInBuild(currentSceneProject);
            }
            if (currentSceneProject.isModified && !currentSceneProject.filepath.empty()) {
                if (isSceneSaveGated(&currentSceneProject)) {
                    // Play saves modified scenes first: never by dropping what a file holds
                    const std::string file = currentSceneProject.filepath.filename().string();
                    result.message = "\"" + file + "\" did not load completely and Play would save it without " +
                        std::to_string(currentSceneProject.loadReport.dropped()) + " value(s). Save it first (Save Anyway or Save As).";
                    result.alertTitle = "Cannot Play";
                    result.alertMessage = "\"" + file + "\" did not load completely. "
                        "Save it first: Save Anyway drops what this editor could not read, Save As keeps the original.";
                    return result;
                }
                if (!saveSceneForPlayStartup(&currentSceneProject)) {
                    result.message = "Failed to save modified scene before play startup";
                    return result;
                }
                // involved scenes are written below
                if (!isInvolvedScene) {
                    writeSceneSource(&currentSceneProject);
                }
            } else {
                updateSceneCppScripts(&currentSceneProject);
                updateScenePrefabs(&currentSceneProject);
            }
        } else if (!currentSceneProject.scene && !currentSceneProject.filepath.empty() &&
                   (currentSceneProject.cppScripts.empty() || currentSceneProject.cppScriptsRead)) {
            // SceneManager can load a closed scene while playing, so its scripts must be built too;
            // a scene never saved by the editor has no script list yet, read it from the entities
            // (once per version of its file)
            collectClosedSceneCppScripts(currentSceneProject);
        }
    }

    // prefabs only scripts reference or other prefabs nest get their sources too (one bake for
    // every involved scene)
    loadBuildPrefabs();
    const auto baked = bakePrefabsForExport();

    std::vector<editor::SceneBuildInfo> scenesToGenerate;
    for (SceneProject& currentSceneProject : scenes) {
        bool isInvolvedScene = std::find(involvedMainSceneIds.begin(), involvedMainSceneIds.end(), currentSceneProject.id) != involvedMainSceneIds.end();

        if (isInvolvedScene) {
            PlayRuntimeScene entry;
            entry.sourceSceneId = currentSceneProject.id;

            if (currentSceneProject.id == sceneId) {
                // found by its id at each use (runtimeOf): the scenes vector may move it
                entry.ownedRuntime = false;
            } else {
                entry.copy = createRuntimeCloneFromSource(&currentSceneProject);
                entry.ownedRuntime = true;
                assignPlayRuntimeId(entry.copy);
            }

            SceneProject* runtime = runtimeOf(entry);
            if (!runtime) {
                result.message = "Failed to create runtime scene for play startup";
                return result;
            }

            std::vector<PrefabInstanceInfo> prefabInstances = generator.writePrefabSources(prefabAssets, currentSceneProject.id, getProjectPath(), getProjectInternalPath(),
                [this](const fs::path& path) { return getPrefabGuid(path); }, baked);
            generator.writeSceneSource(runtime->scene, runtime->id, runtime->name, runtime->entities, getSceneCamera(runtime), getProjectPath(), getProjectInternalPath(), prefabInstances, collectResourcesInRegistry(runtime->scene));

            {
                std::scoped_lock lock(playSessionMutex);
                session->runtimeScenes.push_back(entry);
            }
        }

        bool isMain = (sceneId == currentSceneProject.id);
        std::vector<uint32_t> involvedSceneIds;
        std::vector<uint32_t> activeSceneIds;
        collectInvolvedScenes(currentSceneProject.id, involvedSceneIds);
        collectStartActiveScenes(currentSceneProject.id, activeSceneIds);
        scenesToGenerate.push_back({currentSceneProject.id, currentSceneProject.name, involvedSceneIds, activeSceneIds, isMain});
    }

    registerSceneManager();
    registerPrefabManager();

    result.snapshot = Stream::encodeSceneProject(nullptr, mainSceneProject);
    // with what describes its prefab instances, which Stop puts back with it
    result.prefabState = capturePlayPrefabState(mainSceneProject);

    // Play reuses the edit-time Scene, so anything alive now is authored or engine
    // owned. Whatever appears later belongs to the play session.
    const std::vector<Entity> aliveBeforePlay = mainSceneProject->scene->getEntityList();
    result.entities = std::set<Entity>(aliveBeforePlay.begin(), aliveBeforePlay.end());

    std::vector<SceneScriptSource> mergedCppScripts = collectAllSceneCppScripts();
    std::vector<PrefabSceneInfo> prefabBuildInfos = collectAllPrefabs();
    generator.setAtmosphereQuality(atmosphereQuality);
    generator.setInputSettings(getInputActionsEnginePath(), inputDeadzone);
    generator.setNavigationSettingsCode(generateNavigationSettingsCode(navigationSettings.settings, "    "));
    generator.setTextureDefaults(textureSettings.textureQuality, textureSettings.memoryBudgetMB, textureSettings.lowMemoryFallback);
    generator.setMacOSFrameworks(macOSProjectSettings.frameworks);
    generator.setUIScale(uiScale);
    generator.configure(scenesToGenerate, libName, mergedCppScripts, prefabBuildInfos, getProjectPath(), getProjectInternalPath(), getAssetsPath(), getLuaPath(), getBuildScriptDirs(), getCxxStandard(), scalingMode, textureStrategy, canvasWidth, canvasHeight, vsyncEnabled, getWindowSettings(), loadingSettings, TagsAndLayers{tags, layerNames, layerCollisionMatrix3D, layerCollisionMatrix2D, physics3DWorld.maxBodies, physics3DWorld.maxBodyPairs, physics3DWorld.maxContactConstraints}, htmlUiMsaaSamples, threadingSettings, htmlUiRender, mobilePbrTier, shaderCreationSettings, applicationSettings.allowInsecureHttp, getShaderGlobals(), getPlayerSettings(), getAudioSettings(), debugSettings);
    if (hasMissingSceneSources()) {
        generatedSourcesDirty.store(true);
    }

    result.shaderKeys = collectStandaloneShaderKeys();
    // the C++ native plugins (plugins/native/cpp) live in the Play library too
    result.hasCppScripts = !mergedCppScripts.empty() || hasNativeCppPlugins();
    result.status = PlayStartupPrepared::Status::Ready;
    return result;
}

void editor::Project::runPlayStartup(const std::shared_ptr<PlaySession>& session, uint32_t sceneId, const LocalBuildSettings& buildSettings) {
    auto isCancelled = [session]() {
        return session->cancelled.load(std::memory_order_acquire);
    };

    auto markStartupDone = [session]() {
        session->startupThreadDone.store(true, std::memory_order_release);
    };

    try {
        // Scripts can load any FBX by path. Its reloads land before the scenes are captured for Play,
        // once the loads in flight end, since those would bring the old file back.
        importFbxModels();
        const auto reloadDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        fs::path stillLoading;
        do {
            auto reloaded = std::make_shared<std::promise<fs::path>>();
            std::future<fs::path> reloadedFuture = reloaded->get_future();
            editor::getEditorHost().enqueueMainThreadTask([this, reloaded]() {
                reloaded->set_value(applyModelReloads(true));
            });
            while (reloadedFuture.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready) {
                if (isCancelled()) {
                    markStartupDone();
                    return;
                }
            }
            stillLoading = reloadedFuture.get();
        } while (!stillLoading.empty() && std::chrono::steady_clock::now() < reloadDeadline);
        if (!stillLoading.empty()) {
            failPlayStartup(session, sceneId, "Cannot play: " + FileUtils::pathToUtf8(stillLoading.filename()) +
                " is still loading. Play again once it finishes.");
            return;
        }

        // What reads or changes the scenes runs on the main thread, as one task: there MCP calls,
        // the other scenes' views (their gizmos) and opening or creating a scene change them,
        // and the scenes vector may grow under a reader on this thread
        auto prepared = std::make_shared<std::promise<PlayStartupPrepared>>();
        std::future<PlayStartupPrepared> preparedFuture = prepared->get_future();
        editor::getEditorHost().enqueueMainThreadTask([this, session, sceneId, prepared]() {
            PlayStartupPrepared result;
            try {
                result = preparePlayStartup(session, sceneId);
            } catch (const std::exception& e) {
                result = PlayStartupPrepared();
                result.message = std::string("Failed to start scene: ") + e.what();
            } catch (...) {
                result = PlayStartupPrepared();
                result.message = "Failed to start scene: unknown error";
            }
            prepared->set_value(std::move(result));
        });
        // Stopped meanwhile: the task does nothing if it has not run yet, and what it made
        // for the session (runtime copies) goes with the session's cleanup
        while (preparedFuture.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready) {
            if (isCancelled()) {
                markStartupDone();
                return;
            }
        }
        PlayStartupPrepared ready = preparedFuture.get();
        if (ready.status == PlayStartupPrepared::Status::Cancelled || isCancelled()) {
            markStartupDone();
            return;
        }
        if (ready.status == PlayStartupPrepared::Status::Failed) {
            failPlayStartup(session, sceneId, ready.message, ready.alertTitle, ready.alertMessage);
            return;
        }
        YAML::Node playSnapshot = std::move(ready.snapshot);
        PlayPrefabState playPrefabState = std::move(ready.prefabState);
        std::set<Entity> playEntities = std::move(ready.entities);

        // play regenerates the standalone project, so its shaders are ensured here too
        buildStandaloneShaderCache(ready.shaderKeys);

        if (isCancelled()) { markStartupDone(); return; }

        const bool hasCppScripts = ready.hasCppScripts;

        if (hasCppScripts) {
            // Default needs a discoverable compatible kit; explicit kits may
            // live outside PATH and are ABI-checked by the generated CMake.
            const bool useDefaultKit = buildSettings.cCompiler.empty() && buildSettings.cxxCompiler.empty() && buildSettings.generator.empty();
            CMakeKit playKit;
            playKit.cCompiler = buildSettings.cCompiler;
            playKit.cxxCompiler = buildSettings.cxxCompiler;
            playKit.generator = buildSettings.generator;
            std::string missingTools = Generator::checkBuildTools(useDefaultKit, &playKit);
            if (!missingTools.empty()) {
                failPlayStartup(session, sceneId, "Cannot build C++ scripts: missing build tools", "Missing Build Tools",
                    "C++ scripts require build tools that were not found on your system:\n\n" + missingTools +
                    "\nPlease install the missing tools and try again.");
                return;
            }

            fs::path buildPath = getProjectInternalPath() / "build";
            // a C++ reload during this Play builds the same way
            playBuildKit = playKit;
            playBuildJobs = buildSettings.buildJobs;
            generator.build(getProjectPath(), buildPath, playKit.cCompiler, playKit.cxxCompiler, playKit.generator, buildSettings.buildJobs);
            generator.waitForBuildToComplete();

            if (isCancelled()) { markStartupDone(); return; }

            if (!generator.didLastBuildSucceed()) {
                failPlayStartup(session, sceneId, "C++ script build failed");
                return;
            }

            if (!conector.connect(buildPath, libName, getProjectPath())) {
                failPlayStartup(session, sceneId, "Failed to connect to library");
                return;
            }
        }

        if (isCancelled()) { markStartupDone(); return; }

        // Final initialization runs on the main/GL thread so that script
        // constructors observing a current GL context behave correctly,
        // and so that finalizeStart is invoked from the UI thread.
        editor::getEditorHost().enqueueMainThreadTask(
            [this, session, sceneId, hasCppScripts, playSnapshot = std::move(playSnapshot), playEntities = std::move(playEntities),
             playPrefabState = std::move(playPrefabState)]() mutable {
                if (session->cancelled.load(std::memory_order_acquire)) {
                    session->startupThreadDone.store(true, std::memory_order_release);
                    return;
                }

                SceneProject* sceneProject = getScene(sceneId);
                if (!sceneProject) {
                    cleanupPlaySession(session);
                    {
                        std::scoped_lock lock(playSessionMutex);
                        if (activePlaySession == session) activePlaySession.reset();
                    }
                    session->startupThreadDone.store(true, std::memory_order_release);
                    return;
                }

                sceneProject->playStateSnapshot = std::move(playSnapshot);
                sceneProject->playStateEntities = std::move(playEntities);
                sceneProject->playPrefabState = std::move(playPrefabState);
                sceneProject->playStateModified = sceneProject->isModified;

                std::vector<PlayRuntimeScene> runtimeScenesToInitialize;
                {
                    std::scoped_lock lock(playSessionMutex);
                    runtimeScenesToInitialize = session->runtimeScenes;
                }

                for (const auto& entry : runtimeScenesToInitialize) {
                    if (runtimeSceneOf(entry)) {
                        SceneManager::setScenePtr(entry.sourceSceneId, runtimeSceneOf(entry));
                    }
                }
                // C++ projects run Lua scripts too, through the plugin's initScripts
                LuaBinding::clearLoadedProjectModules();

                // Frame pacing, orientation, screen sleep and application info, before any
                // script runs (an exported game's generated main sets them before its scripts too)
                applyPresentationSettings();
                // the plain-HTTP policy of the exported game (its generated main sets it before
                // any scene), before the first script's init can send a request
                Http::setInsecureHttpAllowed(applicationSettings.allowInsecureHttp);
                // native plugins of other platforms fail with "unavailable" here (the generated
                // main sets the same per platform)
                NativeBridge::setPlatformOnlyPlugins(NativePlugins::platformOnlyPlugins(
                    NativePlugins::listPlugins(NativePlugins::scan(getProjectPath())), NativePlugins::editorPlatform()));
                // the init hooks of native plugin code the Play library brought (Objective-C on a Mac)
                NativeBridge::initPlugins();
                NativePlugins::warnStaleObjectiveC(getProjectPath());

                // Apply project display settings before scripts read the canvas size.
                Engine::setCanvasSize(canvasWidth, canvasHeight);
                Engine::setScalingMode(scalingMode);
                Engine::setDefaultCanvasScaler(uiScale);
                Engine::setTextureStrategy(textureStrategy);
                Engine::systemViewChanged();

                // asyncLoading is only for exports, the editor always loads asynchronously
                if (getScene(loadingSettings.sceneId)) {
                    SceneManager::setLoadingScene(loadingSettings.sceneId);
                }
                SceneManager::setLoadingDelay(loadingSettings.delay);
                SceneManager::setLoadingTimeout(loadingSettings.timeout);
                // Project Settings > Debug: the overlay of a development build, before the scripts start
                applyDebugSettingsForPlay();
                // so Engine::onSceneLoaded fires, as for the exported start scene
                SceneManager::setCurrentScene(sceneId);

                // By index, and guarded: a script here can call addChildScene(), which
                // initializes another entry and appends to runtimeScenes. Initializing from
                // the snapshot above would then run a scene's scripts a second time.
                // Each scene's start ends at a safe point for DontDestroyOnLoad (before its
                // documents start), as in the exported game's first load.
                SceneManager::beginStartingScenes();
                for (size_t i = 0; i < session->runtimeScenes.size(); i++) {
                    PlayRuntimeScene& entry = session->runtimeScenes[i];
                    if (entry.initialized || entry.initializing) continue;
                    if (!runtimeSceneOf(entry)) continue;

                    entry.initializing = true;
                    entry.scriptsRan = true;

                    Scene* scene = runtimeSceneOf(entry);
                    // the edit-time scene plays in place: its Particle Systems start as loaded
                    // ones (the Scene view's preview stopped or played some), before any script
                    scene->getSystem<ParticleSubsystem>()->resetForPlay();
                    if (hasCppScripts) {
                        conector.init(scene);
                    } else {
                        LuaBinding::initializeLuaScripts(scene);
                    }

                    session->runtimeScenes[i].initializing = false;
                }
                SceneManager::endStartingScenes();

                finalizeStart(sceneProject, session->runtimeScenes);
                session->startupSucceeded.store(true, std::memory_order_release);
                session->startupThreadDone.store(true, std::memory_order_release);
            });
    } catch (const std::exception& e) {
        failPlayStartup(session, sceneId, std::string("Failed to start scene: ") + e.what());
    }
}

std::shared_ptr<editor::Project::PlaySession> editor::Project::buildRuntimeSceneStack(uint32_t sceneId, std::vector<size_t>& stackIndices) {
    std::shared_ptr<PlaySession> session;
    {
        std::scoped_lock lock(playSessionMutex);
        session = activePlaySession;
    }
    if (!session || session->cancelled.load(std::memory_order_acquire)) return nullptr;

    std::vector<uint32_t> involvedSceneIds;
    collectInvolvedScenes(sceneId, involvedSceneIds);

    for (uint32_t invSceneId : involvedSceneIds) {
        size_t entryIndex = (size_t)-1;
        {
            std::scoped_lock lock(playSessionMutex);
            auto it = std::find_if(session->runtimeScenes.begin(), session->runtimeScenes.end(),
                [invSceneId](const PlayRuntimeScene& entry) {
                    return entry.sourceSceneId == invSceneId;
                });

            if (it != session->runtimeScenes.end()) {
                entryIndex = std::distance(session->runtimeScenes.begin(), it);
            } else {
                SceneProject* sourceScene = getScene(invSceneId);
                if (sourceScene) {
                    PlayRuntimeScene newEntry;
                    newEntry.sourceSceneId = invSceneId;
                    // the played scene loaded again (it was retired): from its state at Play
                    // start, which an unsaved scene has no file for
                    const bool played = invSceneId == session->mainSceneId && sourceScene->playStateSnapshot &&
                                        !sourceScene->playStateSnapshot.IsNull();
                    newEntry.copy = played ? createRuntimeCloneFromNode(sourceScene, sourceScene->playStateSnapshot)
                                           : createRuntimeCloneFromSource(sourceScene);
                    newEntry.ownedRuntime = true;

                    if (newEntry.copy) {
                        assignPlayRuntimeId(newEntry.copy);
                        session->runtimeScenes.push_back(newEntry);
                        entryIndex = session->runtimeScenes.size() - 1;
                    }
                }
            }
        }

        if (entryIndex != (size_t)-1) {
            stackIndices.push_back(entryIndex);
        }
    }

    // Register all runtime scenes so cross-scene entity references can resolve
    for (size_t entryIndex : stackIndices) {
        PlayRuntimeScene& entry = session->runtimeScenes[entryIndex];
        if (runtimeSceneOf(entry)) {
            SceneManager::setScenePtr(entry.sourceSceneId, runtimeSceneOf(entry));
        }
    }

    for (size_t entryIndex : stackIndices) {
        PlayRuntimeScene& entry = session->runtimeScenes[entryIndex];
        if (entry.initialized || entry.initializing || !runtimeSceneOf(entry)) {
            continue;
        }

        // Scripts can add another stack, which re-enters this function and pushes into
        // runtimeScenes. The flag stops it initializing this scene a second time, and the
        // entry is read again by index because the push invalidates references into it.
        entry.initializing = true;
        entry.scriptsRan = true;

        Scene* scene = runtimeSceneOf(entry);
        if (conector.isLibraryConnected()) {
            conector.init(scene);
        }else{
            LuaBinding::initializeLuaScripts(scene);
        }

        session->runtimeScenes[entryIndex].initializing = false;
        prepareRuntimeScene(session->runtimeScenes[entryIndex]);
    }

    return session;
}

void editor::Project::retireReplacedRuntimeScenes() {
    std::shared_ptr<PlaySession> session;
    {
        std::scoped_lock lock(playSessionMutex);
        session = activePlaySession;
    }
    if (!session || session->cancelled.load(std::memory_order_acquire)) return;

    const std::vector<Scene*>& unloading = SceneManager::getUnloadingScenes();
    std::vector<PlayRuntimeScene> retired;
    {
        std::scoped_lock lock(playSessionMutex);
        // a scene's scripts are starting (a load requested from inside them runs later, but
        // never take the entries from under that loop)
        for (const PlayRuntimeScene& entry : session->runtimeScenes) {
            if (entry.initializing) return;
        }
        for (size_t i = 0; i < session->runtimeScenes.size();) {
            const PlayRuntimeScene& entry = session->runtimeScenes[i];
            Scene* scene = runtimeSceneOf(entry);
            // unloaded by this load, or by an earlier one that left it out of its stack (never
            // the persistent scene)
            const bool replaced = scene && !entry.persistent && entry.scriptsRan && !Engine::isSceneRunning(scene) &&
                (std::find(unloading.begin(), unloading.end(), scene) != unloading.end() || !entry.initialized);
            if (replaced) {
                retired.push_back(entry);
                session->runtimeScenes.erase(session->runtimeScenes.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                i++;
            }
        }
    }

    // outside the lock: a C++ script destructor can call back into the editor
    for (PlayRuntimeScene& entry : retired) {
        Scene* scene = runtimeSceneOf(entry);
        if (scene) {
            // as the exported load function deletes it: no pointer, no sounds, no scripts
            if (SceneManager::getScenePtr(entry.sourceSceneId) == scene) {
                SceneManager::removeScenePtr(entry.sourceSceneId);
            }
            scene->getSystem<AudioSystem>()->stopSceneSounds();
            ObjectManager::forgetScene(scene);
            // the unload ran their onDisable and onDestroy: this releases the instances
            if (conector.isLibraryConnected()) {
                conector.cleanup(scene);
            } else {
                LuaBinding::cleanupLuaScripts(scene);
            }
            Engine::clearComponentSubscriptions(scene);
        }
        entry.initialized = false;
        std::scoped_lock lock(playSessionMutex);
        session->retiredRuntimeScenes.push_back(entry);
    }
}

void editor::Project::registerSceneManager() {
    SceneManager::clearAll();
    // DontDestroyOnLoad: the persistent scene is a runtime scene of the session, which deletes
    // it at Stop (SceneManager only lets it go)
    SceneManager::setPersistentSceneFactory([this]() { return createPersistentRuntimeScene(); }, [](Scene*) {});
    for (SceneProject& sceneProject : scenes) {
        std::vector<uint32_t> stackSceneIds;
        collectStartActiveScenes(sceneProject.id, stackSceneIds);

        SceneManager::registerScene(sceneProject.id, sceneProject.name, [this, sceneId = sceneProject.id]() {
            // what this load replaced goes, so a scene loaded again starts anew (as exported)
            retireReplacedRuntimeScenes();

            std::vector<size_t> currentStackIndices;
            std::shared_ptr<PlaySession> session = buildRuntimeSceneStack(sceneId, currentStackIndices);
            if (!session) return;

            std::vector<uint32_t> activeSceneIds;
            collectStartActiveScenes(sceneId, activeSceneIds);

            std::vector<size_t> activeStackIndices;
            for (uint32_t activeSceneId : activeSceneIds) {
                auto it = std::find_if(session->runtimeScenes.begin(), session->runtimeScenes.end(),
                    [activeSceneId](const PlayRuntimeScene& entry) {
                        return entry.sourceSceneId == activeSceneId;
                    });

                if (it != session->runtimeScenes.end()) {
                    activeStackIndices.push_back(std::distance(session->runtimeScenes.begin(), it));
                }
            }

            for (size_t entryIndex : activeStackIndices) {
                PlayRuntimeScene& entry = session->runtimeScenes[entryIndex];
                if (!runtimeSceneOf(entry)) {
                    continue;
                }

                if (entry.sourceSceneId == sceneId) {
                    Engine::setScene(runtimeSceneOf(entry));
                } else {
                    Engine::addSceneLayer(runtimeSceneOf(entry));
                }
            }

            // Scenes no longer in the current stack
            std::vector<uint32_t> preservedSceneIds;
            std::vector<size_t> staleIndices;
            {
                std::scoped_lock lock(playSessionMutex);
                for (const PlayRuntimeScene& entry : session->runtimeScenes) {
                    if (runtimeSceneOf(entry) && Engine::isSceneRunning(runtimeSceneOf(entry))) {
                        collectInvolvedScenes(entry.sourceSceneId, preservedSceneIds);
                    }
                }

                for (size_t i = 0; i < session->runtimeScenes.size(); i++) {
                    PlayRuntimeScene& entry = session->runtimeScenes[i];
                    // the persistent scene outlives every load
                    if (!entry.initialized || entry.persistent || !runtimeSceneOf(entry)) continue;
                    if (std::find(currentStackIndices.begin(), currentStackIndices.end(), i) != currentStackIndices.end()) continue;

                    // a script added this scene's stack while this one was loading, so it
                    // stays - along with the involved scenes that stack did not display
                    if (std::find(preservedSceneIds.begin(), preservedSceneIds.end(), entry.sourceSceneId) != preservedSceneIds.end()) continue;

                    entry.initialized = false;
                    staleIndices.push_back(i);
                }
            }

            // Outside the lock: a C++ script destructor can call back into the editor and
            // would deadlock on it. Nothing is held across the call for the same reason
            // runtimeScenes is re-read by index above.
            for (size_t entryIndex : staleIndices) {
                // a destructor in an earlier pass may have added this stack back
                if (session->runtimeScenes[entryIndex].initialized) continue;

                uint32_t sourceSceneId = session->runtimeScenes[entryIndex].sourceSceneId;
                Scene* scene = runtimeSceneOf(session->runtimeScenes[entryIndex]);

                // The exported load function deletes these scenes and drops their pointers,
                // so getScenePtr() must not resolve them here either, and their sounds stop
                SceneManager::removeScenePtr(sourceSceneId);
                scene->getSystem<AudioSystem>()->stopSceneSounds();

                if (conector.isLibraryConnected()) {
                    conector.cleanup(scene);
                } else {
                    LuaBinding::cleanupLuaScripts(scene);
                }
            }
        }, [this, sceneId = sceneProject.id]() {
            // SceneManager::addChildScene() puts the scenes on screen
            std::vector<size_t> stackIndices;
            buildRuntimeSceneStack(sceneId, stackIndices);
        }, stackSceneIds, [this, sceneId = sceneProject.id]() {
            return collectPlaySceneResources(sceneId);
        });
    }
}

// The Play copy when the session built one, else the open scene, else a probe of its file
SceneResources editor::Project::collectPlaySceneResources(uint32_t sceneId) {
    Scene* runtimeScene = nullptr;
    {
        std::scoped_lock lock(playSessionMutex);
        if (activePlaySession) {
            for (const PlayRuntimeScene& entry : activePlaySession->runtimeScenes) {
                if (entry.sourceSceneId == sceneId && runtimeSceneOf(entry)) {
                    runtimeScene = runtimeSceneOf(entry);
                    break;
                }
            }
        }
    }
    if (runtimeScene) {
        return collectResourcesInRegistry(runtimeScene);
    }

    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject) {
        return {};
    }
    if (sceneProject->scene) {
        return collectResourcesInRegistry(sceneProject->scene);
    }

    SceneResources resources;
    std::unique_ptr<SceneProject> probe;
    try {
        probe.reset(createRuntimeCloneFromSource(sceneProject));
        resources = collectResourcesInRegistry(probe->scene);
    } catch (const std::exception& e) {
        Out::warning("Files of scene '%s' not preloaded: %s", sceneProject->name.c_str(), e.what());
    }
    if (probe) {
        deleteSceneProject(probe.get());
    }
    // forget the prefab instances the probe registered
    cleanupPrefabsForScene(sceneId);

    return resources;
}

editor::SceneProject* editor::Project::findSceneProjectByScene(Scene* scene) {
    for (auto& sp : scenes) {
        if (sp.scene == scene) return &sp;
    }
    std::scoped_lock lock(playSessionMutex);
    if (activePlaySession) {
        for (auto& entry : activePlaySession->runtimeScenes) {
            if (runtimeSceneOf(entry) == scene) {
                return runtimeOf(entry);
            }
        }
    }
    return nullptr;
}

void editor::Project::start(uint32_t sceneId) {
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject) {
        Out::error("Failed to find scene %u to start", sceneId);
        return;
    }
    editor::getEditorHost().playStarting(sceneId);
    // Play's main-thread startup steps (saving, the script build hand-off, scene copies)
    const diag::BusyScope busy("Starting Play", 120'000);

    {
        std::scoped_lock lock(playSessionMutex);
        if (activePlaySession) {
            Out::warning("Cannot start: a play session is already active");
            return;
        }
    }
    // Record During Play (spec S32 R10): every pipeline Play uses joins the warm-up list at Stop
    if (shaderCreationSettings.recordDuringPlay && !ShaderWarmup::isRecording()) {
        ShaderWarmup::clear();
        ShaderWarmup::startRecording();
    }

    // An MCP agent's export or C++ script build would race Play's own script build
    const std::string busyReason = editor::getEditorHost().backgroundWorkConflict("play");
    if (!busyReason.empty()) {
        Out::warning("Cannot start Play: %s", busyReason.c_str());
        editor::getEditorHost().registerAlert("Cannot Play", busyReason);
        return;
    }

    // Play's Lua may now be interrupted when it runs away (Preferences > Diagnostics)
    EditorDiagnostics::setPlaying(true, sceneProject->name);
    jasperengine::CrashHook::set(
        [this, sceneId](const std::string& tag, const std::string& errorInfo) {
            // Log the scene and entity context
            SceneProject* sceneProject = getScene(sceneId);
            std::string sceneName = sceneProject ? sceneProject->name : "Unknown";

            Out::error("Script crash in scene '%s' (ID: %u)\nLocation: %s\nError: %s", sceneName.c_str(), sceneId, tag.c_str(), errorInfo.c_str());

            // 1. Pause immediately.
            // This sets a flag to prevent the Engine from starting the NEXT frame update.
            pause(sceneId);

            // 2. Stop asynchronously, on the main thread once the CURRENT frame is done and its
            // stack unwound: destroying the scene (via stop) while the main thread still runs
            // code inside it would crash. Not from another thread: stop runs the scripts'
            // onShutdown and releases their callbacks, all in the one Lua state, which is not
            // thread-safe.
            std::weak_ptr<PlaySession> crashedSession;
            {
                std::scoped_lock lock(playSessionMutex);
                crashedSession = activePlaySession;
            }
            editor::getEditorHost().enqueueMainThreadTask([this, sceneId, crashedSession]() {
                std::shared_ptr<PlaySession> session = crashedSession.lock();
                {
                    std::scoped_lock lock(playSessionMutex);
                    // stopped meanwhile (or another session started)
                    if (!session || activePlaySession != session) return;
                }
                if (!session->cancelled.load(std::memory_order_acquire)) {
                    stop(sceneId);
                }
            });
        });

    auto session = std::make_shared<PlaySession>();
    session->mainSceneId = sceneId;

    {
        std::scoped_lock lock(playSessionMutex);
        activePlaySession = session;
    }

    editor::getEditorHost().stopTransientPreviews();
    // a previewing Vertex Animator plays from its start
    stopVertexAnimatorPreviews();
    // a shake previewed in the Game view does not carry into the game
    stopCameraShakePreviews();

    // the edges and deltas since the last Play (or of the editor's own use) are not the game's
    Input::resetFrameState();
    // Project Settings > Input: the default actions asset, its maps as the asset has them
    InputActions::unload();
    InputActions::setDefaultDeadzone(inputDeadzone);
    if (const std::string actions = getInputActionsEnginePath(); !actions.empty()) {
        // an older editor's Input Manager actions not written yet: from memory
        if (inputMigration.pending && inputMigration.asset == inputActionsAsset)
            InputActions::loadFromText(actions, inputMigration.text);
        else
            InputActions::loadDefault(actions);
    } else if (!inputActionsAsset.empty()) {
        Out::warning("Project Settings > Input: %s is not in the assets directory, Play starts without it",
                     inputActionsAsset.generic_string().c_str());
    }

    // Exit any editor camera preview before starting: covers the owned-runtime path
    // where the source scene's render never enters play mode, so stopping wouldn't
    // otherwise drop the user back out of the preview view.
    if (sceneProject->sceneRender){
        sceneProject->sceneRender->clearPreviewCamera();
    }

    sceneProject->playState = ScenePlayState::LOADING;
    // Play starts from the project's layer collision matrices, whatever an earlier Play's
    // scripts did to them
    applyLayerCollisionMatrices();
    // a review left from the last Play of this scene goes with the new one
    PlayChangeReview::get().discard(sceneId);
    // and from clean input (Project Settings > Input, no runtime rebinding)
    resetPlayInput();
    // and audio: the mixer's start snapshot and remembered values, the listener unpaused
    resetPlayAudio(true);
    // and from the declared global shader values (Project Settings > Graphics > Shader Globals)
    resetPlayShaderGlobals();
    CommandHandle::get(sceneId)->beginPlaySession();
    editor::getEditorHost().saveAllCodeEditors();
    editor::getEditorHost().requestScenePlayFocus(sceneId);

    // Read on the UI thread: the startup thread must not race the settings dialog
    const LocalBuildSettings buildSettings = AppSettings::getBuildSettings(getProjectPath() / "project.yaml");

    // the startup writes generated sources in the write scope of the thread that started Play
    const std::vector<fs::path> writeRoot = safewrite::Scope::currentRoots();
    const bool writeStrict = safewrite::Scope::currentStrict();
    // waits on the C++ build and the library load: the process channel; the project holds
    // the task, so closing it waits for the startup instead of leaving it on a freed project
    jobs::SubmitOptions startupOptions;
    startupOptions.name = "play-startup";
    playStartupTask = jobs::system().processChannel().submit([this, session, sceneId, buildSettings, writeRoot, writeStrict]() {
        safewrite::Scope scope(writeRoot, writeStrict);
        runPlayStartup(session, sceneId, buildSettings);
    }, startupOptions);
}

void editor::Project::pause(uint32_t sceneId) {
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject) {
        Out::error("Failed to find scene %u to pause", sceneId);
        return;
    }

    std::shared_ptr<PlaySession> session;
    {
        std::scoped_lock lock(playSessionMutex);
        session = activePlaySession;
    }

    if (session && session->mainSceneId == sceneId) {
        if (sceneProject->playState == ScenePlayState::PLAYING) {
            Engine::onPause.call();

            std::vector<PlayRuntimeScene> runtimeScenesCopy;
            {
                std::scoped_lock lock(playSessionMutex);
                runtimeScenesCopy = session->runtimeScenes;
            }

            for (const auto& entry : runtimeScenesCopy) {
                if (!runtimeSceneOf(entry)) continue;
                pauseEngineScene(runtimeSceneOf(entry), true);
            }
            Engine::pauseGameEvents(true);
            sceneProject->playState = ScenePlayState::PAUSED;
        }
        return;
    }

    Engine::onPause.call();
}

void editor::Project::resume(uint32_t sceneId) {
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject) {
        Out::error("Failed to find scene %u to resume", sceneId);
        return;
    }

    std::shared_ptr<PlaySession> session;
    {
        std::scoped_lock lock(playSessionMutex);
        session = activePlaySession;
    }

    if (session && session->mainSceneId == sceneId) {
        if (sceneProject->playState == ScenePlayState::PAUSED) {
            Engine::onResume.call();

            std::vector<PlayRuntimeScene> runtimeScenesCopy;
            {
                std::scoped_lock lock(playSessionMutex);
                runtimeScenesCopy = session->runtimeScenes;
            }

            for (const auto& entry : runtimeScenesCopy) {
                if (!runtimeSceneOf(entry)) continue;
                pauseEngineScene(runtimeSceneOf(entry), false);
            }
            Engine::pauseGameEvents(false);
            sceneProject->playState = ScenePlayState::PLAYING;
            editor::getEditorHost().requestScenePlayFocus(sceneId);
        }
        return;
    }

    Engine::onResume.call();
}

void editor::Project::stop(uint32_t sceneId) {
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject) {
        Out::error("Failed to find scene %u to stop", sceneId);
        return;
    }
    const diag::BusyScope busy("Stopping Play", 120'000);
    EditorDiagnostics::setPlaying(false, sceneProject->name);
    finishShaderWarmupRecording();

    if (sceneProject->playState == ScenePlayState::STOPPED) {
        Out::warning("Scene %u is not currently playing", sceneId);
        return;
    }

    if (sceneProject->playState == ScenePlayState::CANCELLING) {
        return;
    }

    std::shared_ptr<PlaySession> session;
    {
        std::scoped_lock lock(playSessionMutex);
        session = activePlaySession;
    }

    if (!session) {
        Out::warning("No active play session for scene %u", sceneId);
        return;
    }

    if (session->mainSceneId != sceneId) {
        Out::warning("Scene %u is not the main scene of the active play session", sceneId);
        return;
    }

    session->cancelled.store(true, std::memory_order_release);
    sceneProject->playState = ScenePlayState::CANCELLING;

    const bool startupSucceededAtStop = session->startupSucceeded.load(std::memory_order_acquire);
    if (startupSucceededAtStop) {
        // Notify scripts/engine before tearing the runtime down so that
        // shutdown handlers still see a valid engine state.
        Engine::onViewDestroyed.call();
        // Play stop is the application quit (Unity): onApplicationQuit, onDisable, onDestroy
        ScriptRuntime::quitAll();
        Engine::onShutdown.call();
        Engine::pauseGameEvents(true);
        // the actions' script callbacks go while the Lua state is there
        InputActions::unload();
        Engine::clearAllSubscriptions(true);
        // the play's timers and coroutines (Lua's Timer): none runs after Stop
        Scheduler::game().clear();
    }

    // Clear crash handler when stopping
    jasperengine::CrashHook::set(nullptr);

    // Request cancellation without blocking; the stop job waits for the build to end
    generator.cancelBuild();

    const std::vector<fs::path> stopWriteRoot = safewrite::Scope::currentRoots();
    const bool stopWriteStrict = safewrite::Scope::currentStrict();
    // waits on processes (the build, the startup job), so it runs on the process channel
    jobs::SubmitOptions stopOptions;
    stopOptions.name = "play-stop";
    playStopTask = jobs::system().processChannel().submit([this, session, sceneId, startupSucceededAtStop, stopWriteRoot, stopWriteStrict]() mutable {
        safewrite::Scope scope(stopWriteRoot, stopWriteStrict);
        generator.waitForBuildToComplete();

        // Wait for the connect/startup thread to finish to avoid races
        while (!session->startupThreadDone.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // All teardown happens on the main/GL thread so that script
        // destructors and engine resource cleanup run with a valid GL context.
        editor::getEditorHost().enqueueMainThreadTask([this, session, sceneId, startupSucceededAtStop]() {
            SceneProject* mainSceneProject = getScene(sceneId);
            const bool startupSucceeded = session->startupSucceeded.load(std::memory_order_acquire);

            if (startupSucceeded && !startupSucceededAtStop) {
                Engine::onViewDestroyed.call();
                ScriptRuntime::quitAll();
                Engine::onShutdown.call();
                Engine::pauseGameEvents(true);
                // the actions' script callbacks go while the Lua state is there
                InputActions::unload();
                Engine::clearAllSubscriptions(true);
            }

            std::vector<PlayRuntimeScene> runtimeScenes;
            {
                std::scoped_lock lock(playSessionMutex);
                runtimeScenes = session->runtimeScenes;
                // the scenes a load replaced: copies deleted, the played scene restored
                runtimeScenes.insert(runtimeScenes.end(), session->retiredRuntimeScenes.begin(), session->retiredRuntimeScenes.end());
            }

            // anime: every animation of the play, and its script callbacks, before the scripts go
            AnimEngine::instance().reset();
            // and timers the scripts' onShutdown made
            Scheduler::game().clear();
            // every coroutine and timer of the play (unowned ones too), before the scripts go
            ScriptScheduler::clear();
            // the debug overlay and the console commands of the scripts, before the scripts go
            DebugOverlay::shutdown();
            debugSettingsInPlay = false;
            // HTTP requests of the play end without their callbacks (nothing waits for a
            // transfer); what worker threads posted for the game is dropped
            Http::shutdown();
            // the URL videos' downloads of the play (their .part files stay for a resume)
            MediaCache::shutdown();
            // native calls end without their callbacks, Native.on handlers and mocks go (the
            // C++ plugins of the Play library go with it, Conector::disconnect)
            NativeBridge::shutdown();
            MainThreadQueue::clear();

            const bool libraryConnected = conector.isLibraryConnected();
            if (libraryConnected) {
                for (const auto& entry : runtimeScenes) {
                    if (runtimeSceneOf(entry)) conector.cleanup(runtimeSceneOf(entry));
                }
            } else {
                // a startup that failed half way may have started some scripts too
                for (const auto& entry : runtimeScenes) {
                    if (runtimeSceneOf(entry)) LuaBinding::cleanupLuaScripts(runtimeSceneOf(entry));
                }
            }
            // the DontDestroyOnLoad scene (its scripts released above) leaves SceneManager; the
            // session deletes it with its other copies. Nothing persists into edit mode.
            SceneManager::destroyPersistentScene();
            refusePersistentScenes();
            // nothing of the session's lifecycle outlives it (the played scene is edited again)
            for (const auto& entry : runtimeScenes) {
                if (runtimeSceneOf(entry)) ScriptRuntime::forgetScene(runtimeSceneOf(entry));
            }

            // What the scripts registered and did not remove is dropped here, while the code
            // behind those callbacks is still loaded
            for (const auto& entry : runtimeScenes) {
                if (runtimeSceneOf(entry)) Engine::clearComponentSubscriptions(runtimeSceneOf(entry));
            }

            if (libraryConnected) {
                conector.disconnect();
            }

            // the Systems window's and set_system_options' switches are Play-mode debug state
            for (const auto& entry : runtimeScenes) {
                if (Scene* scene = runtimeSceneOf(entry)) scene->getScheduler().resetDebugOptions();
            }
            if (mainSceneProject && mainSceneProject->scene) {
                mainSceneProject->scene->getScheduler().resetDebugOptions();
            }

            if (startupSucceeded) {
                finalizeStop(mainSceneProject, runtimeScenes);
            } else {
                cleanupPlaySession(session);
                SceneManager::clearAll();
                PrefabManager::clearAll();
                ObjectManager::clearAll();
                clearPlayPrefabTemplates();
                editor::getEditorHost().resetLastActivatedScene();
                if (mainSceneProject) {
                    mainSceneProject->playState = ScenePlayState::STOPPED;
                    CommandHandle::get(mainSceneProject->id)->endPlaySession(false);
                }
            }

            // matrix changes made by scripts last only for the Play, never reaching project.yaml
            applyLayerCollisionMatrices();
            // so do held input, rebinding overrides and simulated input sources
            resetPlayInput();
            // and the mixer values scripts set (the remembered ones stay in UserSettings)
            resetPlayAudio(false);
            // and the global shader values scripts set (Shader.setGlobal*)
            resetPlayShaderGlobals();

            // what the game stored in UserSettings / PlayerPrefs is on disk when Play has stopped
            UserSettings::save();

            // simulated focus, pause and rotation end with the Play, as the frame rate a script set
            Engine::resetApplicationState();
            editor::Platform::endPlaySimulation();

            {
                std::scoped_lock lock(playSessionMutex);
                if (activePlaySession == session) activePlaySession.reset();
            }
        });
    }, stopOptions);
}

void editor::Project::stopActivePlay() {
    uint32_t mainSceneId;
    {
        std::scoped_lock lock(playSessionMutex);
        if (!activePlaySession) {
            return;
        }
        mainSceneId = activePlaySession->mainSceneId;
    }
    stop(mainSceneId);
}

void editor::Project::step(uint32_t sceneId) {
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject) {
        return;
    }

    if (sceneProject->playState == ScenePlayState::PLAYING) {
        pause(sceneId);
    }
    if (sceneProject->playState != ScenePlayState::PAUSED) {
        return;
    }

    stepSceneId = sceneId;
    stepPending = true;
    sceneProject->needUpdateRender = true;
}

bool editor::Project::isStepPending(uint32_t sceneId) const {
    return stepPending && stepSceneId == sceneId;
}

bool editor::Project::beginStepFrame(uint32_t sceneId) {
    if (!isStepPending(sceneId)) {
        return false;
    }
    stepPending = false;

    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || sceneProject->playState != ScenePlayState::PAUSED) {
        return false;
    }

    std::vector<PlayRuntimeScene> runtimeScenesCopy;
    {
        std::scoped_lock lock(playSessionMutex);
        if (!activePlaySession || activePlaySession->mainSceneId != sceneId) {
            return false;
        }
        runtimeScenesCopy = activePlaySession->runtimeScenes;
    }

    // Unpaused for this one draw only: no onResume/onPause, like a single frame
    // of a game that never stopped
    for (const auto& entry : runtimeScenesCopy) {
        if (runtimeSceneOf(entry)) pauseEngineScene(runtimeSceneOf(entry), false);
    }
    Engine::pauseGameEvents(false);

    // The draw measures the time since the last one, which while paused is long.
    // Capping it at the fixed step makes the frame advance exactly one update.
    float frameTime = Engine::getUpdateTime();
    if (frameTime <= 0.0f) frameTime = 1.0f / 60.0f;
    stepSavedMaxDeltatime = Engine::getMaxDeltatime();
    Engine::setMaxDeltatime(frameTime);

    return true;
}

void editor::Project::endStepFrame(uint32_t sceneId) {
    Engine::setMaxDeltatime(stepSavedMaxDeltatime);

    std::vector<PlayRuntimeScene> runtimeScenesCopy;
    {
        std::scoped_lock lock(playSessionMutex);
        if (activePlaySession && activePlaySession->mainSceneId == sceneId) {
            runtimeScenesCopy = activePlaySession->runtimeScenes;
        }
    }
    for (const auto& entry : runtimeScenesCopy) {
        if (runtimeSceneOf(entry)) pauseEngineScene(runtimeSceneOf(entry), true);
    }
    Engine::pauseGameEvents(true);
}

uint32_t editor::Project::getPlaySessionSceneId() const {
    std::scoped_lock lock(playSessionMutex);
    return activePlaySession ? activePlaySession->mainSceneId : NULL_PROJECT_SCENE;
}

Entity editor::Project::getGameCamera(uint32_t sceneId) const {
    const SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) {
        return NULL_ENTITY;
    }
    return getSceneCamera(sceneProject);
}

void editor::Project::waitForPlaySessionToFinish() {
    // Wait for the detached finalizeStop thread to complete
    // so that all cleanup finishes before the app tears down.
    // Also pump the main-thread task queue, because finalizeStop
    // is now enqueued there and the caller (closeWindow) is on
    // the main thread — without pumping we would deadlock.
    while (true) {
        {
            std::scoped_lock lock(playSessionMutex);
            if (!activePlaySession) {
                return;
            }
        }
        editor::getEditorHost().processMainThreadTasks();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool editor::Project::isPlayedSceneReplaced(uint32_t sceneId) {
    std::scoped_lock lock(playSessionMutex);
    if (!activePlaySession || activePlaySession->mainSceneId != sceneId) {
        return false;
    }
    for (const PlayRuntimeScene& entry : activePlaySession->retiredRuntimeScenes) {
        if (!entry.ownedRuntime && entry.sourceSceneId == sceneId) {
            return true;
        }
    }
    return false;
}

std::vector<Scene*> editor::Project::getRunningRuntimeLayers(uint32_t sceneId) {
    std::vector<Scene*> runningLayers;
    for (SceneProject* runtimeProject : getRunningRuntimeLayerScenes(sceneId)) {
        runningLayers.push_back(runtimeProject->scene);
    }
    return runningLayers;
}

std::vector<editor::SceneProject*> editor::Project::getRunningRuntimeLayerScenes(uint32_t sceneId) {
    std::vector<SceneProject*> runningLayers;

    std::scoped_lock lock(playSessionMutex);
    if (!activePlaySession || activePlaySession->mainSceneId != sceneId) {
        return runningLayers;
    }
    if (!activePlaySession->startupSucceeded.load(std::memory_order_acquire)) {
        return runningLayers;
    }

    for (const auto& entry : activePlaySession->runtimeScenes) {
        SceneProject* runtimeProject = runtimeOf(entry);
        // the played scene itself is the view's own; a copy of it (it was loaded again) is a layer
        if (!runtimeProject || !runtimeProject->scene || (!entry.ownedRuntime && entry.sourceSceneId == sceneId)) {
            continue;
        }
        if (Engine::isSceneRunning(runtimeProject->scene)) {
            runningLayers.push_back(runtimeProject);
        }
    }

    // as the game draws them (a scene added later can be below one added before: the
    // persistent scene stays above the loaded stack, the loading scene above it)
    const std::vector<Scene*> engineOrder = Engine::getSceneLayers();
    auto layerIndex = [&engineOrder](const SceneProject* layer) {
        return std::find(engineOrder.begin(), engineOrder.end(), layer->scene) - engineOrder.begin();
    };
    std::stable_sort(runningLayers.begin(), runningLayers.end(), [&layerIndex](const SceneProject* a, const SceneProject* b) {
        return layerIndex(a) < layerIndex(b);
    });

    return runningLayers;
}

uint32_t editor::Project::getPersistentPlaySceneId() const {
    std::scoped_lock lock(playSessionMutex);
    if (!activePlaySession) {
        return NULL_PROJECT_SCENE;
    }
    for (const auto& entry : activePlaySession->runtimeScenes) {
        if (entry.persistent && entry.copy) {
            return entry.copy->id;
        }
    }
    return NULL_PROJECT_SCENE;
}

bool editor::Project::isPersistentPlayScene(uint32_t sceneId) const {
    return sceneId != NULL_PROJECT_SCENE && getPersistentPlaySceneId() == sceneId;
}

void editor::Project::refusePersistentScenes() {
    // DontDestroyOnLoad outside Play: nothing to move to (the scene would vanish from the editor)
    SceneManager::setPersistentSceneFactory([]() -> Scene* { return nullptr; }, [](Scene*) {});
}

Scene* editor::Project::createPersistentRuntimeScene() {
    std::shared_ptr<PlaySession> session;
    {
        std::scoped_lock lock(playSessionMutex);
        session = activePlaySession;
    }
    if (!session || session->cancelled.load(std::memory_order_acquire)) {
        return nullptr;
    }
    SceneProject* mainSceneProject = getScene(session->mainSceneId);
    if (!mainSceneProject) {
        return nullptr;
    }

    // an empty UI scene of the session, like a Play copy: its Play id, the Hierarchy's row, MCP
    std::unique_ptr<SceneProject> runtime(new SceneProject());
    runtime->opened = false;
    runtime->isModified = false;
    runtime->isVisible = false;
    runtime->needUpdateRender = false;
    runtime->name = SceneManager::PERSISTENT_SCENE_NAME;
    runtime->sceneType = SceneType::SCENE_UI;
    runtime->id = SceneManager::PERSISTENT_SCENE_ID;
    runtime->scene = new Scene();
    runtime->defaultCamera = createDefaultCamera(runtime->sceneType, runtime->scene);

    PlayRuntimeScene entry;
    entry.sourceSceneId = SceneManager::PERSISTENT_SCENE_ID;
    entry.copy = runtime.release();
    entry.ownedRuntime = true;
    entry.persistent = true;
    entry.scriptsRan = true;
    assignPlayRuntimeId(entry.copy);
    // the camera, the game's flags (initialized, PLAYING), running systems
    prepareRuntimeScene(entry);
    if (mainSceneProject->playState == ScenePlayState::PAUSED) {
        pauseEngineScene(entry.copy->scene, true);
    }
    {
        std::scoped_lock lock(playSessionMutex);
        session->runtimeScenes.push_back(entry);
    }
    mainSceneProject->structureVersion++;
    return entry.copy->scene;
}

void editor::Project::onPlayObjectsMoved(Scene* from, Scene* to, const std::unordered_map<Entity, Entity>& moved) {
    SceneProject* source = findSceneProjectByScene(from);
    SceneProject* target = findSceneProjectByScene(to);

    // authored GameObjects stay listed (editable in the Inspector), the selection follows
    std::vector<Entity> listed;
    std::vector<Entity> selected;
    if (source) {
        for (Entity entity : source->entities) {
            auto it = moved.find(entity);
            if (it != moved.end()) listed.push_back(it->second);
        }
        for (Entity entity : source->selectedEntities) {
            auto it = moved.find(entity);
            if (it != moved.end()) selected.push_back(it->second);
        }
        auto gone = [&moved](Entity entity) { return moved.count(entity) != 0; };
        source->entities.erase(std::remove_if(source->entities.begin(), source->entities.end(), gone), source->entities.end());
        source->selectedEntities.erase(std::remove_if(source->selectedEntities.begin(), source->selectedEntities.end(), gone),
                                       source->selectedEntities.end());
        source->structureVersion++;
        markSelectionChanged(source);
    }
    if (target) {
        for (Entity entity : listed) {
            if (std::find(target->entities.begin(), target->entities.end(), entity) == target->entities.end()) {
                target->entities.push_back(entity);
            }
        }
        if (!selected.empty()) {
            target->selectedEntities = selected;
            if (source && selectedSceneForProperties == source->id) {
                selectedSceneForProperties = target->id;
            }
        }
        target->structureVersion++;
        markSelectionChanged(target);
    }
    // the Hierarchy of the played scene lists its running layers
    const uint32_t mainId = getPlaySessionSceneId();
    if (SceneProject* mainSceneProject = getScene(mainId)) {
        mainSceneProject->structureVersion++;
    }
}

void editor::Project::setInspectedAsset(const std::filesystem::path& absolutePath){
    inspectedAsset = absolutePath;
}

void editor::Project::clearInspectedAsset(){
    inspectedAsset.clear();
}

void editor::Project::remapInspectedAsset(const std::filesystem::path& oldPath, const std::filesystem::path& newPath){
    if (inspectedAsset.empty()) {
        return;
    }
    const std::filesystem::path relative = inspectedAsset.lexically_normal().lexically_relative(oldPath.lexically_normal());
    if (relative.empty() || *relative.begin() == "..") {
        return;
    }
    inspectedAsset = relative == "." ? newPath.lexically_normal() : (newPath / relative).lexically_normal();
}

void editor::Project::cleanupInspectedAsset(const std::filesystem::path& deletedPath){
    if (inspectedAsset.empty()) {
        return;
    }
    const std::filesystem::path relative = inspectedAsset.lexically_normal().lexically_relative(deletedPath.lexically_normal());
    if (!relative.empty() && *relative.begin() != "..") {
        inspectedAsset.clear();
    }
}

const std::filesystem::path& editor::Project::getInspectedAsset() const{
    return inspectedAsset;
}
