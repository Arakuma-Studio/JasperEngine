// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Project.h"
#include "Factory.h"
#include "Workspace.h"
#include "registry/PhysicsLayers.h"

#include "EditorHost.h"
#include "util/FileUtils.h"
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

#include "render/SceneRender2D.h"
#include "render/SceneRender3D.h"

#include "lua.hpp"
#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "AppSettings.h"
#include "Out.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/UISystem.h"
#include "subsystem/UIDocumentSystem.h"
#include "htmlui/anim/AnimEngine.h"
#include "command/CommandHandle.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/MoveEntityOrderCmd.h"
#include "Stream.h"
#include "util/FileDialogs.h"
#include "util/SHA1.h"
#include "util/GraphicUtils.h"
#include "util/CameraTextureLink.h"
#include "util/ProjectUtils.h"
#include "util/Util.h"

#include "texture/Texture.h"
#include "component/UIDocumentComponent.h"
#include "Engine.h"
#include "pool/ShaderPool.h"
#include "shader/ShaderBuilder.h"
#include "shader/ShaderVariantCollector.h"
#include "shader/ShaderManifest.h"
#include "shader/ShaderVariantCompiler.h"
#include "SceneManager.h"
#include "PrefabManager.h"
#include "ObjectManager.h"

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

editor::Project::Project(){
    resetConfigs();
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
            return relativePath.lexically_normal();
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

bool editor::Project::remapScriptEntryPaths(ScriptEntry& scriptEntry, const fs::path& oldRelative,
                                            const fs::path& newRelative) {
    bool changed = false;

    if (!scriptEntry.path.empty()) {
        std::string updatedPath;
        if (remapRelativeString(oldRelative, newRelative, scriptEntry.path, updatedPath)) {
            scriptEntry.path = updatedPath;
            changed = true;
        }
    }

    if (!scriptEntry.headerPath.empty()) {
        std::string updatedHeaderPath;
        if (remapRelativeString(oldRelative, newRelative, scriptEntry.headerPath, updatedHeaderPath)) {
            scriptEntry.headerPath = updatedHeaderPath;
            changed = true;
        }
    }

    return changed;
}

bool editor::Project::remapScriptPathsInRegistry(EntityRegistry* registry, const fs::path& oldPath,
                                                 const fs::path& newPath) {
    if (!registry) {
        return false;
    }

    const fs::path oldRelative = normalizeToProjectRelative(oldPath);
    const fs::path newRelative = normalizeToProjectRelative(newPath);
    const fs::path oldLuaRelative = normalizeToLuaRelative(oldPath);
    const fs::path newLuaRelative = normalizeToLuaRelative(newPath);

    auto scriptsArray = registry->getComponentArray<ScriptComponent>();
    bool changed = false;

    for (size_t i = 0; i < scriptsArray->size(); ++i) {
        ScriptComponent& scriptComponent = scriptsArray->getComponentFromIndex(i);
        for (auto& scriptEntry : scriptComponent.scripts) {
            const bool isLua = scriptEntry.type == ScriptType::LUA;
            changed |= remapScriptEntryPaths(scriptEntry,
                                             isLua ? oldLuaRelative : oldRelative,
                                             isLua ? newLuaRelative : newRelative);
        }
    }

    return changed;
}

bool editor::Project::cleanupScriptPathsInRegistry(EntityRegistry* registry, const fs::path& deletedPath) {
    if (!registry) {
        return false;
    }

    const fs::path deletedRelative = normalizeToProjectRelative(deletedPath);
    const fs::path deletedLuaRelative = normalizeToLuaRelative(deletedPath);

    auto scriptsArray = registry->getComponentArray<ScriptComponent>();
    bool changed = false;

    for (size_t i = 0; i < scriptsArray->size(); ++i) {
        ScriptComponent& scriptComponent = scriptsArray->getComponentFromIndex(i);
        const size_t originalSize = scriptComponent.scripts.size();

        scriptComponent.scripts.erase(
            std::remove_if(scriptComponent.scripts.begin(), scriptComponent.scripts.end(),
                [&deletedRelative, &deletedLuaRelative](const ScriptEntry& scriptEntry) {
                    if (scriptEntry.type == ScriptType::LUA) {
                        return matchesRelativeString(deletedLuaRelative, scriptEntry.path);
                    }
                    return matchesRelativeString(deletedRelative, scriptEntry.path) ||
                           matchesRelativeString(deletedRelative, scriptEntry.headerPath);
                }),
            scriptComponent.scripts.end());

        changed |= (scriptComponent.scripts.size() != originalSize);
    }

    return changed;
}

static bool transformTexturePaths(Texture& texture, const std::function<bool(std::string&)>& transform) {
    // Framebuffer and camera-linked textures carry an id instead of a file path.
    if (texture.isFramebuffer()) {
        return false;
    }

    // Per-face paths only exist on cubemaps, incomplete ones included
    bool hasFacePaths = false;
    for (size_t face = 1; face < 6; face++) {
        if (!texture.getPath(face).empty()) {
            hasFacePaths = true;
            break;
        }
    }

    if (hasFacePaths) {
        bool changed = false;
        for (size_t face = 0; face < 6; face++) {
            std::string path = texture.getPath(face);
            if (path.empty() || !transform(path)) {
                continue;
            }
            // One missing face leaves the whole cubemap unloadable
            if (path.empty()) {
                texture = Texture();
                return true;
            }
            texture.setCubePath(face, path);
            changed = true;
        }

        return changed;
    }

    std::string path = texture.getPath(0);
    if (path.empty() || !transform(path)) {
        return false;
    }

    // setPath("") would instead leave the texture waiting for a load that cannot happen
    if (path.empty()) {
        texture = Texture();
        return true;
    }

    if (texture.isCubeMap()) {
        // Single-file cubemap: setPath() would turn it into a 2D texture.
        texture.setCubeMap(path);
        return true;
    }

    const float svgScale = texture.getSvgScale();
    texture.setPath(path);
    texture.setSvgScale(svgScale);

    return true;
}

static bool visitTexturePaths(Texture& texture, const std::function<bool(std::string&)>& transform) {
    Texture updated(texture);
    if (!transformTexturePaths(updated, transform)) {
        return false;
    }

    // Renders keep drawing the old GPU texture until the new one exists, so the new path loads
    // before the old texture is released. Kept in the pool, it would show for a reused name.
    if (TexturePool::get(texture.getId())) {
        const bool asyncLoading = Engine::isAsyncLoading();
        Engine::setAsyncLoading(false);
        updated.getRender();
        Engine::setAsyncLoading(asyncLoading);
    }

    texture.destroy();
    texture = updated;
    return true;
}

bool editor::Project::visitAssetPathsInRegistry(EntityRegistry* registry, const std::function<bool(std::string&)>& transform) {
    if (!registry) {
        return false;
    }

    bool changed = false;

    auto visitComponents = [&](auto* componentArray, auto&& visitComponent) {
        for (size_t i = 0; i < componentArray->size(); i++) {
            visitComponent(componentArray->getComponentFromIndex(i));
        }
    };

    visitComponents(registry->getComponentArray<MeshComponent>(), [&](MeshComponent& mesh) {
        for (unsigned int s = 0; s < mesh.numSubmeshes; s++) {
            Material& material = mesh.submeshes[s].material;
            bool submeshChanged = visitTexturePaths(material.baseColorTexture, transform);
            submeshChanged |= visitTexturePaths(material.emissiveTexture, transform);
            submeshChanged |= visitTexturePaths(material.metallicRoughnessTexture, transform);
            submeshChanged |= visitTexturePaths(material.occlusionTexture, transform);
            submeshChanged |= visitTexturePaths(material.normalTexture, transform);
            for (auto& shaderTexture : material.shaderTextures) {
                submeshChanged |= visitTexturePaths(shaderTexture.second, transform);
            }
            if (MaterialPass* pass = material.extraPass.editIfSet()) {
                for (auto& shaderTexture : pass->shaderTextures) {
                    submeshChanged |= visitTexturePaths(shaderTexture.second, transform);
                }
            }

            if (submeshChanged) {
                mesh.submeshes[s].needUpdateTexture = true;
                changed = true;
            }
        }
    });

    // shader library textures of the components without a material
    auto visitShaderTextures = [&](std::vector<std::pair<std::string, Texture>>& textures, bool& needReload) {
        bool texturesChanged = false;
        for (auto& shaderTexture : textures) {
            texturesChanged |= visitTexturePaths(shaderTexture.second, transform);
        }
        if (texturesChanged) {
            needReload = true;
            changed = true;
        }
    };
    visitComponents(registry->getComponentArray<UIComponent>(), [&](UIComponent& ui) { visitShaderTextures(ui.shaderTextures, ui.needReload); });
    visitComponents(registry->getComponentArray<PointsComponent>(), [&](PointsComponent& points) { visitShaderTextures(points.shaderTextures, points.needReload); });
    visitComponents(registry->getComponentArray<LineRendererComponent>(), [&](LineRendererComponent& lines) { visitShaderTextures(lines.shaderTextures, lines.needReload); });
    visitComponents(registry->getComponentArray<SkyboxComponent>(), [&](SkyboxComponent& sky) { visitShaderTextures(sky.shaderTextures, sky.needReload); });

    visitComponents(registry->getComponentArray<UIComponent>(), [&](UIComponent& ui) {
        if (visitTexturePaths(ui.texture, transform)) {
            ui.needUpdateTexture = true;
            changed = true;
        }
    });

    visitComponents(registry->getComponentArray<ButtonComponent>(), [&](ButtonComponent& button) {
        bool buttonChanged = visitTexturePaths(button.textureNormal, transform);
        buttonChanged |= visitTexturePaths(button.textureHovered, transform);
        buttonChanged |= visitTexturePaths(button.texturePressed, transform);
        buttonChanged |= visitTexturePaths(button.textureDisabled, transform);

        if (buttonChanged) {
            button.needUpdateButton = true;
            changed = true;
        }
    });

    visitComponents(registry->getComponentArray<SkyboxComponent>(), [&](SkyboxComponent& sky) {
        if (visitTexturePaths(sky.texture, transform)) {
            sky.needUpdateTexture = true;
            changed = true;
        }
    });

    visitComponents(registry->getComponentArray<TerrainComponent>(), [&](TerrainComponent& terrain) {
        bool heightMapChanged = visitTexturePaths(terrain.heightMap, transform);
        bool terrainChanged = heightMapChanged;
        for (Texture& blendMap : terrain.blendMaps) {
            terrainChanged |= visitTexturePaths(blendMap, transform);
        }
        for (TerrainSurfaceLayer& layer : terrain.surfaceLayers) {
            forEachTerrainLayerTexture(layer, [&](Texture& texture) {
                terrainChanged |= visitTexturePaths(texture, transform);
            });
        }

        for (TerrainFoliageLayer& layer : terrain.foliageLayers) {
            if (visitTexturePaths(layer.densityMap, transform) ||
                (!layer.meshPath.empty() && transform(layer.meshPath))) {
                terrain.needUpdateFoliage = true;
                changed = true;
            }
        }

        // The node tree and the physics heightfield are built from the map, so they follow it
        if (heightMapChanged) {
            terrain.heightMapLoaded = false;
            terrain.needUpdateTerrain = true;
        }

        if (terrainChanged) {
            terrain.needUpdateTexture = true;
            changed = true;
        }
    });

    visitComponents(registry->getComponentArray<LightComponent>(), [&](LightComponent& light) {
        if (visitTexturePaths(light.spotMask, transform)) {
            light.needUpdateShadowCamera = true;
            changed = true;
        }
    });

    visitComponents(registry->getComponentArray<PointsComponent>(), [&](PointsComponent& points) {
        if (visitTexturePaths(points.texture, transform)) {
            points.needUpdateTexture = true;
            changed = true;
        }
    });

    visitComponents(registry->getComponentArray<ReflectionProbeComponent>(), [&](ReflectionProbeComponent& probe) {
        if (visitTexturePaths(probe.texture, transform)) {
            probe.needUpdate = true;
            changed = true;
        }
    });

    visitComponents(registry->getComponentArray<ModelComponent>(), [&](ModelComponent& model) {
        if (!model.filename.empty() && transform(model.filename)) {
            model.needUpdateModel = true;
            changed = true;
        }
    });

    visitComponents(registry->getComponentArray<AudioSourceComponent>(), [&](AudioSourceComponent& sound) {
        if (!sound.filename.empty() && transform(sound.filename)) {
            sound.loaded = false;
            sound.loadFailed = false;
            sound.needUpdate = true;
            changed = true;
        }
    });

    visitComponents(registry->getComponentArray<TextComponent>(), [&](TextComponent& text) {
        for (std::string& font : text.font) {
            if (!font.empty() && transform(font)) {
                text.needReloadAtlas = true;
                changed = true;
            }
        }
    });

    // the document itself; references inside its HTML/CSS are not rewritten (phase 4)
    visitComponents(registry->getComponentArray<UIDocumentComponent>(), [&](UIDocumentComponent& document) {
        if (!document.filename.empty() && transform(document.filename)) {
            document.needReload = true;
            changed = true;
        }
    });

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

        auto sameId = [&file](const Texture& other) { return other.getId() == file.getId(); };
        if (std::none_of(resources.textures.begin(), resources.textures.end(), sameId)) {
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
        addFile(resources.sounds, sound.filename);
    });

    // UIDocumentComponent: nothing to preload. RmlUi reads a document and its stylesheets when
    // it loads it, and its images through RmlRenderer::LoadTexture, not the texture pool

    return resources;
}

bool editor::Project::visitLuaPathsInRegistry(EntityRegistry* registry, const std::function<bool(std::string&)>& transform) {
    if (!registry) {
        return false;
    }

    auto scriptsArray = registry->getComponentArray<ScriptComponent>();
    bool changed = false;

    for (size_t i = 0; i < scriptsArray->size(); i++) {
        ScriptComponent& scriptComponent = scriptsArray->getComponentFromIndex(i);
        for (auto& scriptEntry : scriptComponent.scripts) {
            if (scriptEntry.type != ScriptType::LUA) {
                continue;
            }
            changed |= transform(scriptEntry.path);
        }
    }

    return changed;
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

    fs::path absolutePath = projectPath / entry.filePath;
    std::error_code ec;
    entry.lastWriteTime = fs::last_write_time(absolutePath, ec);

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

void editor::Project::remapMaterialFilePath(const std::filesystem::path& oldPath, const std::filesystem::path& newPath) {
    if (projectPath.empty()) {
        return;
    }

    fs::path oldRelative = normalizeToProjectRelative(oldPath);
    fs::path newRelative = normalizeToProjectRelative(newPath);

    if (oldRelative.empty() || newRelative.empty()) {
        return;
    }

    std::vector<std::pair<MaterialLinkKey, MaterialLinkEntry>> remappedEntries;

    for (auto it = materialFileLinks.begin(); it != materialFileLinks.end();) {
        std::string updatedFilePath;
        if (!remapRelativeString(oldRelative, newRelative, it->second.filePath, updatedFilePath)) {
            ++it;
            continue;
        }

        MaterialLinkEntry updatedEntry = it->second;
        const std::string oldFilePath = updatedEntry.filePath;
        updatedEntry.filePath = updatedFilePath;

        std::error_code ec;
        updatedEntry.lastWriteTime = fs::last_write_time(projectPath / updatedEntry.filePath, ec);

        const MaterialLinkKey key = it->first;
        const uint32_t sceneId = std::get<0>(key);
        const Entity entity = std::get<1>(key);
        const unsigned int submeshIndex = std::get<2>(key);

        SceneProject* sceneProject = getScene(sceneId);
        if (sceneProject && sceneProject->scene) {
            MeshComponent* mesh = sceneProject->scene->findComponent<MeshComponent>(entity);
            if (mesh && submeshIndex < mesh->numSubmeshes) {
                Material& material = mesh->submeshes[submeshIndex].material;
                std::string updatedMaterialName;
                if (remapRelativeString(oldRelative, newRelative, material.name, updatedMaterialName)) {
                    material.name = updatedMaterialName;
                    mesh->submeshes[submeshIndex].needUpdateTexture = true;
                    sceneProject->needUpdateRender = true;
                    sceneProject->isModified = true;
                }
            }
        }

        remappedEntries.emplace_back(key, updatedEntry);
        it = materialFileLinks.erase(it);
    }

    for (auto& [key, entry] : remappedEntries) {
        materialFileLinks[key] = entry;
    }
}

void editor::Project::remapSceneFilePath(const std::filesystem::path& oldPath, const std::filesystem::path& newPath) {
    if (projectPath.empty()) {
        return;
    }

    fs::path oldRelative = normalizeToProjectRelative(oldPath);
    fs::path newRelative = normalizeToProjectRelative(newPath);

    if (oldRelative.empty() || newRelative.empty()) {
        return;
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
}

void editor::Project::remapScriptFilePath(const std::filesystem::path& oldPath, const std::filesystem::path& newPath) {
    if (projectPath.empty() || oldPath.empty() || newPath.empty()) {
        return;
    }

    for (SceneProject* sceneProjectPtr : getScenesAndPrefabStages()) {
        SceneProject& sceneProject = *sceneProjectPtr;
        if (!sceneProject.scene) {
            continue;
        }

        if (remapScriptPathsInRegistry(sceneProject.scene, oldPath, newPath)) {
            sceneProject.isModified = true;
            updateSceneCppScripts(&sceneProject);
        }
    }

    for (auto& [prefabPath, prefab] : prefabAssets) {
        if (!remapScriptPathsInRegistry(prefab.registry.get(), oldPath, newPath)) {
            continue;
        }
        prefab.isModified = true;
        savePrefabToDisk(prefabPath);
    }
}

void editor::Project::cleanupMaterialFilePath(const std::filesystem::path& deletedPath) {
    if (projectPath.empty()) {
        return;
    }

    fs::path deletedRelative = normalizeToProjectRelative(deletedPath);
    if (deletedRelative.empty()) {
        return;
    }

    for (auto it = materialFileLinks.begin(); it != materialFileLinks.end();) {
        if (matchesRelativeString(deletedRelative, it->second.filePath)) {
            it = materialFileLinks.erase(it);
        } else {
            ++it;
        }
    }

    for (SceneProject* sceneProjectPtr : getScenesAndPrefabStages()) {
        SceneProject& sceneProject = *sceneProjectPtr;
        if (!sceneProject.scene) {
            continue;
        }

        auto meshes = sceneProject.scene->getComponentArray<MeshComponent>();
        bool sceneChanged = false;

        for (size_t i = 0; i < meshes->size(); ++i) {
            Entity entity = meshes->getEntity(i);
            MeshComponent& mesh = meshes->getComponentFromIndex(i);

            for (unsigned int submeshIndex = 0; submeshIndex < mesh.numSubmeshes; ++submeshIndex) {
                Material& material = mesh.submeshes[submeshIndex].material;
                if (!matchesRelativeString(deletedRelative, material.name)) {
                    continue;
                }

                material.name.clear();
                mesh.submeshes[submeshIndex].needUpdateTexture = true;
                sceneProject.needUpdateRender = true;
                sceneProject.isModified = true;
                sceneChanged = true;

                unlinkMaterialFile(sceneProject.id, entity, submeshIndex);
            }
        }

        if (sceneChanged) {
            sceneProject.needUpdateRender = true;
        }
    }
}

void editor::Project::cleanupSceneFilePath(const std::filesystem::path& deletedPath) {
    if (projectPath.empty()) {
        return;
    }

    fs::path deletedRelative = normalizeToProjectRelative(deletedPath);
    if (deletedRelative.empty()) {
        return;
    }

    // A scene kept listed while its file was missing has no SceneProject for the
    // Scenes dialog to offer, so deleting the file is the only way to drop it.
    bool changed = clearUnresolvedScene(deletedRelative);

    std::vector<uint32_t> scenesToRemove;
    std::set<uint32_t> removedChildSceneIds;

    for (auto& sceneProject : scenes) {
        if (sceneProject.filepath.empty()) {
            continue;
        }

        if (matchesRelativePath(deletedRelative, sceneProject.filepath)) {
            removedChildSceneIds.insert(sceneProject.id);
            if (sceneProject.opened) {
                sceneProject.filepath.clear();
                sceneProject.isModified = true;
                sceneProject.needUpdateRender = true;
            } else {
                scenesToRemove.push_back(sceneProject.id);
            }
            changed = true;
        }
    }

    for (uint32_t childSceneId : removedChildSceneIds) {
        if (detachChildSceneFromParents(childSceneId, removedChildSceneIds)) {
            changed = true;
        }
    }

    for (uint32_t sceneId : scenesToRemove) {
        auto it = std::find_if(scenes.begin(), scenes.end(),
            [sceneId](const SceneProject& s) { return s.id == sceneId; });
        if (it != scenes.end()) {
            deleteSceneProject(&(*it));
            cleanupPrefabsForScene(sceneId);
            removeTab(TabType::SCENE, it->filepath.string());
            markParentScenesNeedUpdate(sceneId);
            editor::getEditorHost().clearSceneWindowState(sceneId);
            scenes.erase(it);
        }
    }

    if (changed) {
        saveProject();
    }
}

void editor::Project::cleanupScriptFilePath(const std::filesystem::path& deletedPath) {
    if (projectPath.empty() || deletedPath.empty()) {
        return;
    }

    for (SceneProject* sceneProjectPtr : getScenesAndPrefabStages()) {
        SceneProject& sceneProject = *sceneProjectPtr;
        if (!sceneProject.scene) {
            continue;
        }

        if (cleanupScriptPathsInRegistry(sceneProject.scene, deletedPath)) {
            sceneProject.isModified = true;
            updateSceneCppScripts(&sceneProject);
        }
    }

    for (auto& [prefabPath, prefab] : prefabAssets) {
        if (!cleanupScriptPathsInRegistry(prefab.registry.get(), deletedPath)) {
            continue;
        }
        prefab.isModified = true;
        savePrefabToDisk(prefabPath);
    }
}

// the images the scene's settings name: its post-process passes' textures and the
// post-processing LUT strip (phase 4)
static bool visitSceneSettingsAssetPaths(Scene* scene, const std::function<bool(std::string&)>& transform) {
    std::vector<PostProcessPass> passes = scene->getPostProcessPasses();
    bool passesChanged = false;
    for (PostProcessPass& pass : passes) {
        for (auto& texture : pass.textures) {
            passesChanged |= visitTexturePaths(texture.second, transform);
        }
    }
    if (passesChanged) {
        scene->setPostProcessPasses(passes);
    }

    // the LUT is a plain path (assets-relative), read by the renderer when it changes
    PostProcessingSettings postProcessing = scene->getPostProcessing();
    bool lutChanged = false;
    if (!postProcessing.lutTexture.empty()) {
        std::string lut = postProcessing.lutTexture;
        if (transform(lut) && lut != postProcessing.lutTexture) {
            postProcessing.lutTexture = lut;
            scene->setPostProcessing(postProcessing);
            lutChanged = true;
        }
    }
    return passesChanged || lutChanged;
}

void editor::Project::applyAssetPathChange(const std::function<bool(std::string&)>& transform) {
    for (SceneProject* sceneProjectPtr : getScenesAndPrefabStages()) {
        SceneProject& sceneProject = *sceneProjectPtr;
        if (!sceneProject.scene)
            continue;
        const bool settingsChanged = visitSceneSettingsAssetPaths(sceneProject.scene, transform);
        if (!visitAssetPathsInRegistry(sceneProject.scene, transform) && !settingsChanged) {
            continue;
        }

        sceneProject.isModified = true;
        sceneProject.needUpdateRender = true;
    }

    for (auto& [prefabPath, prefab] : prefabAssets) {
        if (!prefab.registry || !visitAssetPathsInRegistry(prefab.registry.get(), transform)) {
            continue;
        }

        prefab.isModified = true;
        savePrefabToDisk(prefabPath);
    }

    visitAssetPathsInMaterialFiles(transform);
}

void editor::Project::remapAssetFilePath(const std::filesystem::path& oldPath, const std::filesystem::path& newPath) {
    if (projectPath.empty()) {
        return;
    }

    // A file that left the assets directory cannot be referenced, so it counts as deleted
    if (!isInsideAssetsPath(newPath)) {
        cleanupAssetFilePath(oldPath);
        return;
    }

    fs::path oldRelative = normalizeToAssetsRelative(oldPath);
    fs::path newRelative = normalizeToAssetsRelative(newPath);

    if (oldRelative.empty() || newRelative.empty()) {
        return;
    }

    applyAssetPathChange([&](std::string& assetPath) -> bool {
        std::string updated;
        if (!remapRelativeString(oldRelative, newRelative, assetPath, updated)) {
            return false;
        }
        assetPath = updated;
        return true;
    });
}

void editor::Project::cleanupAssetFilePath(const std::filesystem::path& deletedPath) {
    if (projectPath.empty()) {
        return;
    }

    fs::path deletedRelative = normalizeToAssetsRelative(deletedPath);
    if (deletedRelative.empty()) {
        return;
    }

    applyAssetPathChange([&](std::string& assetPath) -> bool {
        if (!matchesRelativeString(deletedRelative, assetPath)) {
            return false;
        }
        assetPath.clear();
        return true;
    });
}

void editor::Project::applyCustomShaderPathChange(const std::function<bool(std::string&)>& transform) {
    // customShader lives on every renderable component type (a mesh also has its depth
    // fork); visit each array in a registry, apply the transform, and flag a shader
    // reload on the entities that changed.
    auto applyToRegistry = [&](EntityRegistry* registry) -> bool {
        bool changed = false;
        auto applyToArray = [&](auto arr, auto shaderField) {
            for (size_t i = 0; i < arr->size(); ++i) {
                std::string& customShader = arr->getComponentFromIndex(i).*shaderField;
                if (customShader.empty())
                    continue;
                if (transform(customShader)) {
                    Catalog::updateEntity(registry, arr->getEntity(i), UpdateFlags_Shader_Reload);
                    changed = true;
                }
            }
        };
        applyToArray(registry->getComponentArray<MeshComponent>(), &MeshComponent::customShader);
        applyToArray(registry->getComponentArray<MeshComponent>(), &MeshComponent::customDepthShader);
        applyToArray(registry->getComponentArray<UIComponent>(), &UIComponent::customShader);
        applyToArray(registry->getComponentArray<PointsComponent>(), &PointsComponent::customShader);
        applyToArray(registry->getComponentArray<LineRendererComponent>(), &LineRendererComponent::customShader);
        applyToArray(registry->getComponentArray<SkyboxComponent>(), &SkyboxComponent::customShader);
        // a submesh material's own shader and its extra pass's (mesh reload)
        auto meshes = registry->getComponentArray<MeshComponent>();
        for (size_t i = 0; i < meshes->size(); ++i) {
            MeshComponent& mesh = meshes->getComponentFromIndex(i);
            bool meshChanged = false;
            for (unsigned int s = 0; s < mesh.numSubmeshes; s++) {
                meshChanged |= applyToMaterialShaders(mesh.submeshes[s].material, transform);
            }
            if (meshChanged) {
                Catalog::updateEntity(registry, meshes->getEntity(i), UpdateFlags_Mesh_Reload);
                changed = true;
            }
        }
        return changed;
    };

    // scene-level default shaders reference the same files; the setter flags the reload
    auto applyToSceneDefaults = [&](Scene* scene) -> bool {
        bool changed = false;
        for (ShaderType type : {ShaderType::MESH, ShaderType::UI, ShaderType::SKYBOX, ShaderType::POINTS, ShaderType::LINES}) {
            std::string defaultShader = scene->getDefaultCustomShader(type);
            if (!defaultShader.empty() && transform(defaultShader)) {
                scene->setDefaultCustomShader(type, defaultShader);
                changed = true;
            }
        }
        return changed;
    };

    for (SceneProject* sceneProjectPtr : getScenesAndPrefabStages()) {
        SceneProject& sceneProject = *sceneProjectPtr;
        if (!sceneProject.scene) {
            continue;
        }
        bool changed = applyToRegistry(sceneProject.scene);
        changed |= applyToSceneDefaults(sceneProject.scene);
        if (changed) {
            sceneProject.isModified = true;
            sceneProject.needUpdateRender = true;
        }
    }

    for (auto& [prefabPath, prefab] : prefabAssets) {
        if (!prefab.registry) {
            continue;
        }
        if (applyToRegistry(prefab.registry.get())) {
            prefab.isModified = true;
            savePrefabToDisk(prefabPath);
        }
    }

    // .material files (their linked submeshes were remapped above as well)
    visitMaterialFiles([&](Material& material) {
        return applyToMaterialShaders(material, transform);
    });
}

// The fork references of a material (its shader and its extra pass's); library references
// and empty ones name no file
bool editor::Project::applyToMaterialShaders(Material& material, const std::function<bool(std::string&)>& transform) {
    bool changed = false;
    MaterialPass* pass = material.extraPass.editIfSet();
    for (std::string* shader : {&material.shader, pass ? &pass->shader : nullptr}) {
        if (!shader || shader->empty() || ShaderReference::isLibrary(*shader)) {
            continue;
        }
        changed |= transform(*shader);
    }
    return changed;
}

// The project-relative files a fork path can name (findShaderFile): itself, and under the
// assets directory; whether they exist or not (a rename or delete has already happened)
std::vector<std::string> editor::Project::shaderFileCandidates(const std::string& relative) const {
    std::vector<std::string> candidates;
    if (relative.empty()) {
        return candidates;
    }
    candidates.push_back(relative);
    const fs::path assetsRelative = normalizeToProjectRelative(getAssetsPath());
    if (!assetsRelative.empty() && assetsRelative != "." && *assetsRelative.begin() != "..") {
        candidates.push_back((assetsRelative / relative).lexically_normal().generic_string());
    }
    return candidates;
}

void editor::Project::remapShaderFilePath(const std::filesystem::path& oldPath, const std::filesystem::path& newPath) {
    if (projectPath.empty()) {
        return;
    }

    fs::path oldRelative = normalizeToProjectRelative(oldPath);
    fs::path newRelative = normalizeToProjectRelative(newPath);

    if (oldRelative.empty() || newRelative.empty()) {
        return;
    }

    // customShader resolves to a .vert and a .frag entry point; remap each path against the
    // rename and rebuild the value. A single-file rename of a shared-base pair naturally
    // becomes the separate-file form; a directory rename remaps both via child match.
    // (an assets-relative entry point is matched at its project-relative path too, see
    // findShaderFile; the remapped value is project-relative)
    applyCustomShaderPathChange([&](std::string& customShader) -> bool {
        Util::CustomShaderPaths paths = Util::resolveCustomShaderPaths(customShader);
        std::string newVert = paths.vert;
        std::string newFrag = paths.frag;
        bool changed = false;
        std::string updated;
        for (const std::string& candidate : shaderFileCandidates(paths.vert)) {
            if (remapRelativeString(oldRelative, newRelative, candidate, updated)) {
                newVert = updated;
                changed = true;
                break;
            }
        }
        for (const std::string& candidate : shaderFileCandidates(paths.frag)) {
            if (remapRelativeString(oldRelative, newRelative, candidate, updated)) {
                newFrag = updated;
                changed = true;
                break;
            }
        }
        if (changed) {
            customShader = Util::makeCustomShader(newVert, newFrag);
        }
        return changed;
    });
}

void editor::Project::cleanupShaderFilePath(const std::filesystem::path& deletedPath) {
    if (projectPath.empty()) {
        return;
    }

    fs::path deletedRelative = normalizeToProjectRelative(deletedPath);
    if (deletedRelative.empty()) {
        return;
    }

    // A custom shader needs both its .vert and .frag; deleting either entry point (or the
    // containing folder) leaves the reference dangling, so reset it to built-in. Match the
    // deleted path against each resolved entry point (handles shared-base and separate forms).
    applyCustomShaderPathChange([&](std::string& customShader) -> bool {
        Util::CustomShaderPaths paths = Util::resolveCustomShaderPaths(customShader);
        for (const std::string& entry : {paths.vert, paths.frag}) {
            for (const std::string& candidate : shaderFileCandidates(entry)) {
                if (matchesRelativeString(deletedRelative, candidate)) {
                    customShader.clear();
                    return true;
                }
            }
        }
        return false;
    });
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
            if (entry.ownedRuntime && entry.runtime && entry.runtime->scene &&
                std::find(result.begin(), result.end(), entry.runtime) == result.end()) {
                result.push_back(entry.runtime);
            }
        }
    }
    return result;
}

editor::HtmlUIWatcher& editor::Project::getHtmlUIWatcher() {
    return htmlUIWatcher;
}

void editor::Project::notifyHtmlUIFileSaved(const std::filesystem::path& path) {
    (void)path;
    htmlUIWatcher.requestCheck();
}

void editor::Project::refreshLinkedMaterials(bool force) {
    if (materialFileLinks.empty()) {
        return;
    }

    // Throttle: only check every materialRefreshIntervalSec seconds
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - lastMaterialRefreshTime).count();
    if (!force && elapsed < materialRefreshIntervalSec) {
        return;
    }
    lastMaterialRefreshTime = now;

    if (projectPath.empty() || !fs::exists(projectPath)) {
        materialFileLinks.clear();
        return;
    }

    // Collect changed files and stale entries
    struct ChangedFile {
        std::string filePath;
        Material updatedMaterial;
    };
    std::unordered_map<std::string, ChangedFile> changedFiles;
    std::vector<MaterialLinkKey> staleKeys;

    for (auto& [key, linkEntry] : materialFileLinks) {
        uint32_t keySceneId = std::get<0>(key);
        Entity keyEntity = std::get<1>(key);
        unsigned int keySubmeshIndex = std::get<2>(key);

        // Find the scene
        SceneProject* sceneProject = nullptr;
        for (auto& sp : scenes) {
            if (sp.id == keySceneId) {
                sceneProject = &sp;
                break;
            }
        }
        if (!sceneProject || !sceneProject->scene) {
            staleKeys.push_back(key);
            continue;
        }

        MeshComponent* mesh = sceneProject->scene->findComponent<MeshComponent>(keyEntity);
        if (!mesh || keySubmeshIndex >= mesh->numSubmeshes) {
            staleKeys.push_back(key);
            continue;
        }

        // If the material name no longer matches the linked file (e.g. after undo), remove the link
        std::string currentName = fs::path(mesh->submeshes[keySubmeshIndex].material.name).lexically_normal().generic_string();
        if (currentName != linkEntry.filePath) {
            staleKeys.push_back(key);
            continue;
        }

        fs::path absolutePath = projectPath / linkEntry.filePath;
        if (!fs::exists(absolutePath)) {
            staleKeys.push_back(key);
            continue;
        }

        std::error_code ec;
        auto writeTime = fs::last_write_time(absolutePath, ec);
        if (ec) {
            continue;
        }

        if (writeTime != linkEntry.lastWriteTime) {
            linkEntry.lastWriteTime = writeTime;

            // Only load the file once per unique path
            if (changedFiles.find(linkEntry.filePath) == changedFiles.end()) {
                try {
                    YAML::Node materialNode = YAML::LoadFile(absolutePath.string());
                    Material updatedMaterial = Stream::decodeMaterial(materialNode);
                    updatedMaterial.name = linkEntry.filePath;
                    changedFiles[linkEntry.filePath] = {linkEntry.filePath, updatedMaterial};
                } catch (const std::exception& e) {
                    Out::error("Error reloading linked material file '%s': %s", absolutePath.string().c_str(), e.what());
                }
            }
        }
    }

    // Remove stale entries
    for (const auto& key : staleKeys) {
        materialFileLinks.erase(key);
    }

    // Apply changes
    for (auto& [key, linkEntry] : materialFileLinks) {
        uint32_t keySceneId = std::get<0>(key);
        Entity keyEntity = std::get<1>(key);
        unsigned int keySubmeshIndex = std::get<2>(key);

        auto changedIt = changedFiles.find(linkEntry.filePath);
        if (changedIt == changedFiles.end()) {
            continue;
        }

        SceneProject* sceneProject = nullptr;
        for (auto& sp : scenes) {
            if (sp.id == keySceneId) {
                sceneProject = &sp;
                break;
            }
        }
        if (!sceneProject || !sceneProject->scene) {
            continue;
        }

        MeshComponent* mesh = sceneProject->scene->findComponent<MeshComponent>(keyEntity);
        if (!mesh || keySubmeshIndex >= mesh->numSubmeshes) {
            continue;
        }

        const Material& updatedMaterial = changedIt->second.updatedMaterial;
        if (mesh->submeshes[keySubmeshIndex].material != updatedMaterial) {
            // a new shader, features or render state reload the mesh, parameters its uniforms
            const uint64_t updateFlags = Catalog::getMaterialUpdateFlags(mesh->submeshes[keySubmeshIndex].material, updatedMaterial);
            mesh->submeshes[keySubmeshIndex].material = updatedMaterial;
            mesh->submeshes[keySubmeshIndex].needUpdateTexture = true;
            Catalog::updateEntity(sceneProject->scene, keyEntity, updateFlags);
            sceneProject->needUpdateRender = true;
            sceneProject->isModified = true;
        }
    }
}

editor::SceneRender* editor::Project::createSceneRender(SceneType type, Scene* scene) const {
    if (!scene) {
        return nullptr;
    }

    pauseEngineScene(scene, true);
    scene->getSystem<UISystem>()->setAnchorReferenceSize(canvasWidth, canvasHeight);
    scene->getSystem<UIDocumentSystem>()->setReferenceSize(canvasWidth, canvasHeight);

    switch (type) {
        case SceneType::SCENE_3D:
            return new SceneRender3D(scene);
        case SceneType::SCENE_2D:
            return new SceneRender2D(scene, canvasWidth, canvasHeight, false);
        case SceneType::SCENE_UI:
            return new SceneRender2D(scene, canvasWidth, canvasHeight, true);
        default:
            return new SceneRender3D(scene);
    }
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

        sceneProject.scene->getSystem<UISystem>()->setAnchorReferenceSize(width, height);
        sceneProject.scene->getSystem<UIDocumentSystem>()->setReferenceSize(width, height);

        if (sceneProject.sceneRender && sceneProject.sceneType != SceneType::SCENE_3D) {
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

void editor::Project::setTextureStrategy(TextureStrategy textureStrategy){
    this->textureStrategy = textureStrategy;
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
}

std::filesystem::path editor::Project::getAssetsDir() const{
    return assetsDir;
}

void editor::Project::setLuaDir(const std::filesystem::path& luaDir){
    this->luaDir = luaDir;
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

static bool isInsideRoot(const std::filesystem::path& path, const std::filesystem::path& root){
    std::error_code ec;
    std::filesystem::path relativePath = std::filesystem::relative(path, root, ec);
    if (ec || relativePath.empty()){
        return false;
    }

    return *relativePath.begin() != "..";
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

    return relativePath.lexically_normal();
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
        bool materialChanged = visitTexturePaths(material.baseColorTexture, transform);
        materialChanged |= visitTexturePaths(material.emissiveTexture, transform);
        materialChanged |= visitTexturePaths(material.metallicRoughnessTexture, transform);
        materialChanged |= visitTexturePaths(material.occlusionTexture, transform);
        materialChanged |= visitTexturePaths(material.normalTexture, transform);
        for (auto& shaderTexture : material.shaderTextures){
            materialChanged |= visitTexturePaths(shaderTexture.second, transform);
        }
        if (MaterialPass* pass = material.extraPass.editIfSet()){
            for (auto& shaderTexture : pass->shaderTextures){
                materialChanged |= visitTexturePaths(shaderTexture.second, transform);
            }
        }
        return materialChanged;
    });
}

bool editor::Project::visitMaterialFiles(const std::function<bool(Material&)>& visit){
    if (projectPath.empty()){
        return false;
    }

    std::error_code ec;
    bool changed = false;

    for (auto it = fs::recursive_directory_iterator(projectPath, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); ++it){
        const auto& entry = *it;

        const std::string name = entry.path().filename().string();
        if (entry.is_directory() && !name.empty() && (name[0] == '.' || name == "build")){
            it.disable_recursion_pending();
            continue;
        }

        if (!entry.is_regular_file() || !Util::isMaterialFile(entry.path().string())){
            continue;
        }

        Material material;
        try {
            material = Stream::decodeMaterial(YAML::LoadFile(entry.path().string()));
        } catch (const std::exception& e) {
            Out::warning("Could not read material %s: %s", entry.path().string().c_str(), e.what());
            continue;
        }

        if (!visit(material)){
            continue;
        }

        if (!FileUtils::writeFileReplacing(entry.path(), YAML::Dump(Stream::encodeMaterial(material)), FileUtils::LineEndings::KeepExisting)){
            Out::error("Failed to write material: %s", entry.path().string().c_str());
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
        return;
    }

    // Buffers must reach disk before their files move, as a play session does
    editor::getEditorHost().saveAllCodeEditors();

    const bool assetsChanged = oldAssetsRoot != newAssetsRoot;
    const bool luaChanged = oldLuaRoot != newLuaRoot;

    std::error_code ec;
    fs::create_directories(newAssetsRoot, ec);
    fs::create_directories(newLuaRoot, ec);

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
            fs::create_directories(destination, moveEc);
            bool moved = false;
            for (const auto& entry : fs::directory_iterator(source, moveEc)){
                moved |= moveEntry(entry.path(), destination / entry.path().filename());
                // Reporting a move here would point the reference at what stayed behind.
                if (relocationFailed){
                    return false;
                }
            }
            // Only removes the source when everything left it.
            fs::remove(source, moveEc);
            return moved;
        }

        if (fs::exists(destination, moveEc)){
            Out::warning("Kept the existing %s: the reference points at it and %s is no longer used",
                         destination.string().c_str(), source.string().c_str());
            keptExisting.emplace_back(source, destination);
            return false;
        }

        fs::create_directories(destination.parent_path(), moveEc);
        fs::rename(source, destination, moveEc);
        if (moveEc){
            // A root on another volume rejects rename, so the file is copied and only then dropped.
            std::error_code copyEc;
            fs::copy_file(source, destination, copyEc);
            if (copyEc){
                // A partial copy would pass as an already relocated file on the next attempt.
                std::error_code cleanupEc;
                fs::remove(destination, cleanupEc);

                Out::error("Failed to copy %s: %s", source.string().c_str(), copyEc.message().c_str());
                relocationFailed = true;
                return false;
            }
            if (!fs::remove(source, copyEc)){
                Out::warning("Copied %s but could not remove the original", source.string().c_str());
            }
        }

        completedMoves.emplace_back(source, destination);
        return true;
    };

    // Keeps the project on its previous roots when a relocation could not finish.
    auto undoMoves = [&]() {
        for (size_t i = completedMoves.size(); i > 0; i--){
            const auto& [source, destination] = completedMoves[i - 1];
            std::error_code undoEc;

            // A directory emptied by the move is gone by now.
            fs::create_directories(source.parent_path(), undoEc);

            // The copy fallback may have left the original in place.
            if (fs::exists(source, undoEc)){
                fs::remove(destination, undoEc);
                continue;
            }

            fs::rename(destination, source, undoEc);
            if (!undoEc){
                continue;
            }

            fs::copy_file(destination, source, undoEc);
            if (undoEc){
                // References still point here, so a partial copy would pass as the file itself.
                std::error_code cleanupEc;
                fs::remove(source, cleanupEc);

                Out::error("Could not restore %s, the file is at %s: %s", source.string().c_str(),
                           destination.string().c_str(), undoEc.message().c_str());
                continue;
            }
            fs::remove(destination, undoEc);
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

    saveProjectFile();
}

void editor::Project::setPackNativeResources(bool enabled){
    packNativeResources = enabled;
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

const editor::LoadingSettings& editor::Project::getLoadingSettings() const{
    return loadingSettings;
}

editor::TerrainEditorSettings& editor::Project::getTerrainEditorSettings(){
    return terrainEditorSettings;
}

const editor::TerrainEditorSettings& editor::Project::getTerrainEditorSettings() const{
    return terrainEditorSettings;
}

bool editor::Project::isSimulateTouch() const{
    return simulateTouch;
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

    std::unique_ptr<SceneProject> runtime(new SceneProject());
    runtime->opened = false;
    runtime->isModified = false;
    runtime->isVisible = false;
    runtime->needUpdateRender = false;
    runtime->filepath = source->filepath;

    fs::path fullPath = runtime->filepath;
    if (fullPath.is_relative()) {
        fullPath = getProjectPath() / fullPath;
    }

    YAML::Node sceneNode = YAML::LoadFile(fullPath.string());
    Stream::decodeSceneProject(runtime.get(), sceneNode, true);
    runtime->defaultCamera = createDefaultCamera(runtime->sceneType, runtime->scene);
    Stream::decodeSceneProjectEntities(this, runtime.get(), sceneNode);
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

        for (const auto& scriptEntry : scriptComponent.scripts) {
            if (!scriptEntry.enabled)
                continue;
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

void editor::Project::calculateSceneMaxValues(const SceneProject* sceneProject, SceneMaxValues& maxValues) const {
    if (!sceneProject || !sceneProject->scene) {
        return;
    }

    auto meshes = sceneProject->scene->getComponentArray<MeshComponent>();
    for (size_t i = 0; i < meshes->size(); ++i) {
        const MeshComponent& mesh = meshes->getComponentFromIndex(i);
        maxValues.maxSubmeshes = std::max(maxValues.maxSubmeshes, mesh.numSubmeshes);
        maxValues.maxExternalBuffers = std::max(maxValues.maxExternalBuffers, mesh.numExternalBuffers);
        maxValues.maxBones = std::max(maxValues.maxBones,
            static_cast<unsigned int>(mesh.bonesMatrix.size()));
    }

    auto sprites = sceneProject->scene->getComponentArray<SpriteComponent>();
    for (size_t i = 0; i < sprites->size(); ++i) {
        const SpriteComponent& sprite = sprites->getComponentFromIndex(i);
        maxValues.maxSpriteFrames = std::max(maxValues.maxSpriteFrames, sprite.numFramesRect);
    }

    auto points = sceneProject->scene->getComponentArray<PointsComponent>();
    for (size_t i = 0; i < points->size(); ++i) {
        const PointsComponent& pointsComponent = points->getComponentFromIndex(i);
        maxValues.maxSpriteFrames = std::max(maxValues.maxSpriteFrames, pointsComponent.numFramesRect);
    }

    // SpriteAnimationComponent.frames / framesTime are also HybridArray<int, MAX_SPRITE_FRAMES>.
    // Their counts are independent of (and can exceed) a sprite's numFramesRect (e.g. a ping-pong
    // sequence reuses frame indices), and the generated component code writes them with unchecked
    // operator[], so they must factor into MAX_SPRITE_FRAMES too.
    auto spriteAnims = sceneProject->scene->getComponentArray<SpriteAnimationComponent>();
    for (size_t i = 0; i < spriteAnims->size(); ++i) {
        const SpriteAnimationComponent& spriteAnim = spriteAnims->getComponentFromIndex(i);
        maxValues.maxSpriteFrames = std::max(maxValues.maxSpriteFrames, spriteAnim.framesSize);
        maxValues.maxSpriteFrames = std::max(maxValues.maxSpriteFrames, spriteAnim.framesTimeSize);
    }

    auto tilemaps = sceneProject->scene->getComponentArray<TilemapComponent>();
    for (size_t i = 0; i < tilemaps->size(); ++i) {
        const TilemapComponent& tilemap = tilemaps->getComponentFromIndex(i);
        maxValues.maxTilemapTilesRect = std::max(maxValues.maxTilemapTilesRect, tilemap.numTilesRect);
        maxValues.maxTilemapTiles = std::max(maxValues.maxTilemapTiles, tilemap.numTiles);
    }

    auto bodies2d = sceneProject->scene->getComponentArray<Rigidbody2DComponent>();
    for (size_t i = 0; i < bodies2d->size(); ++i) {
        const Rigidbody2DComponent& body = bodies2d->getComponentFromIndex(i);
        maxValues.maxColliders2D = std::max(maxValues.maxColliders2D, static_cast<unsigned int>(body.numColliders));
        for (size_t s = 0; s < body.numColliders; ++s) {
            maxValues.maxColliderPoints2D = std::max(maxValues.maxColliderPoints2D, static_cast<unsigned int>(body.colliders[s].numVertices));
        }
    }

    auto bodies3d = sceneProject->scene->getComponentArray<RigidbodyComponent>();
    for (size_t i = 0; i < bodies3d->size(); ++i) {
        const RigidbodyComponent& body = bodies3d->getComponentFromIndex(i);
        maxValues.maxColliders3D = std::max(maxValues.maxColliders3D, static_cast<unsigned int>(body.numColliders));
        for (size_t s = 0; s < body.numColliders; ++s) {
            maxValues.maxColliderVertices3D = std::max(maxValues.maxColliderVertices3D, static_cast<unsigned int>(body.colliders[s].numVertices));
            maxValues.maxColliderIndices3D = std::max(maxValues.maxColliderIndices3D, static_cast<unsigned int>(body.colliders[s].numIndices));
        }
    }
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

    for (Entity entity : shaderEntities) {
        Signature signature = scene->getSignature(entity);

        if (signature.test(scene->getComponentId<MeshComponent>())) {
            const MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
            if (mesh.loaded) {
                const std::string& sceneMeshShader = scene->getDefaultCustomShader(ShaderType::MESH);
                for (unsigned int s = 0; s < mesh.numSubmeshes; ++s) {
                    // each submesh's shader (its material's, the mesh's, the scene's) and features
                    uint16_t customShaderId = ShaderPool::registerCustomShader(
                        RenderSystem::getSubmeshShaderReference(mesh, mesh.submeshes[s], sceneMeshShader));
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
                    // the colour variant has when the material's maps take it)
                    bool depthStochastic = depthTexture && (meshProperties & shaderPropertyBit(5)) != 0;
                    uint64_t depthProperties = ShaderPool::getDepthMeshProperties(
                        depthTexture, mesh.submeshes[s].hasSkinning,
                        mesh.submeshes[s].hasMorphTarget, mesh.submeshes[s].hasMorphNormal,
                        mesh.submeshes[s].hasMorphTangent, isTerrain, isInstanced, depthAlphaMask,
                        instancedFade, false, false, false, false, false, depthStochastic);
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

    // disabled passes ship too: the generated scene carries the whole chain
    for (const PostProcessPass& pass : scene->getPostProcessPasses()) {
        insertKeys(ShaderType::POSTPROCESS, 0, ShaderPool::registerCustomShader(pass.shader));
    }

    // fixed resolution upscales with it and a scene stack is presented with it
    keys.insert(ShaderPool::getShaderKey(ShaderType::BLIT, 0));

    // HTML UI documents draw geometry, gradients and layer/filter passes (RmlRenderer)
    if (scene->isComponentRegistered<UIDocumentComponent>() && scene->getComponentArray<UIDocumentComponent>()->size() > 0) {
        for (const auto& variant : ShaderPool::getHtmlUIVariants())
            insertKeys(variant.first, variant.second, 0);
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
                loaded.loadRegistry(YAML::LoadFile((getProjectPath() / info.prefabPath).string()));
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
                                         scriptsPost, scriptsPost ? scriptsPostProcessingFields() : std::vector<std::string>());
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

void editor::Project::buildStandaloneShaderCache() {
    // the generated main.cpp registers every scene stack, so any of them can be loaded
    std::set<ShaderKey> shaderKeys;
    for (const auto& sceneProject : getScenes()) {
        shaderKeys.insert(sceneProject.shaderKeys.begin(), sceneProject.shaderKeys.end());
        // stored keys are only as new as the last save of that scene
        collectSceneShaderKeys(&sceneProject, shaderKeys);
        collectRequiredShaderKeys(&sceneProject, shaderKeys);
    }

    ShaderBuilder builder;
    builder.buildMissingShaders(shaderKeys, this);
}

void editor::Project::invalidateCustomShaders() {
    // Drop the editor's compiled cache for forked shaders, free their GPU handles, and
    // flag every renderable that uses one so RenderSystem recompiles it on the next draw.
    editor::ShaderBuilder::invalidateCustomShaders();
    ShaderPool::destroyCustomShaders();
    // fork manifests are read again from the edited files
    ShaderManifests::clearForks();

    for (auto& sceneProject : getScenes()) {
        if (!sceneProject.scene)
            continue;
        Scene* scene = sceneProject.scene;
        // a component rides on a fork either via its own customShader or the scene default
        bool sceneMesh = !scene->getDefaultMeshShader().empty();
        bool sceneUI = !scene->getDefaultUIShader().empty();
        bool scenePoints = !scene->getDefaultPointsShader().empty();
        bool sceneLines = !scene->getDefaultLinesShader().empty();
        bool sceneSky = !scene->getDefaultSkyShader().empty();

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
                    const std::string& shader = mesh->submeshes[s].material.shader;
                    const std::string& pass = mesh->submeshes[s].material.extraPass->shader;
                    materialFork = materialFork || (!shader.empty() && !ShaderReference::isLibrary(shader));
                    materialFork = materialFork || (!pass.empty() && !ShaderReference::isLibrary(pass));
                }
                flagReload(mesh->needReload, sceneMesh || materialFork || !mesh->customShader.empty() || !mesh->customDepthShader.empty());
            }
            if (UIComponent* ui = scene->findComponent<UIComponent>(entity))
                flagReload(ui->needReload, sceneUI || !ui->customShader.empty());
            if (PointsComponent* pts = scene->findComponent<PointsComponent>(entity))
                flagReload(pts->needReload, scenePoints || !pts->customShader.empty());
            if (LineRendererComponent* ln = scene->findComponent<LineRendererComponent>(entity))
                flagReload(ln->needReload, sceneLines || !ln->customShader.empty());
            if (SkyboxComponent* sky = scene->findComponent<SkyboxComponent>(entity))
                flagReload(sky->needReload, sceneSky || !sky->customShader.empty());
        }

        // post-process passes hold their shader directly, so the chain is rebuilt whole
        if (!scene->getPostProcessPasses().empty()) {
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

void editor::Project::prepareRuntimeScene(PlayRuntimeScene& entry) {
    if (!entry.runtime || !entry.runtime->scene) return;

    entry.runtime->scene->getSystem<MeshSystem>()->setFoliagePreviewEntity(NULL_ENTITY);

    Entity camera = getSceneCamera(entry.runtime);
    entry.runtime->scene->setCamera(camera);

    // play reuses the edit-time Scene without a reload, so re-arm reflection
    // probes: "On Load" (and static capture-once) probes should capture the
    // game's starting state, not keep the edit-mode capture
    auto probes = entry.runtime->scene->getComponentArray<ReflectionProbeComponent>();
    for (size_t i = 0; i < probes->size(); ++i) {
        ReflectionProbeComponent& probe = probes->getComponentFromIndex(i);
        probe.needUpdate = true;
        probe.captureRevision++;
    }

    entry.runtime->scene->getSystem<ActionSystem>()->resetRunningActions();

    entry.runtime->scene->getSystem<UISystem>()->clearAnchorReferenceSize();
    entry.runtime->scene->getSystem<UIDocumentSystem>()->clearReferenceSize();
    // a Play copy (a child scene, a scene loaded while playing) is only drawn by the game's
    // frames, which never show editor overlays (SceneRender::activateGame turns them off for
    // the played scene); with them on, its HTML UI documents would be skipped as in the Scene view
    if (entry.ownedRuntime){
        entry.runtime->scene->getSystem<RenderSystem>()->setDrawEditorOverlays(false);
    }
    pauseEngineScene(entry.runtime->scene, false);

    entry.initialized = true;

    // a Play copy runs as the played scene does: what the game makes in it is its own
    if (entry.ownedRuntime){
        entry.runtime->playState = ScenePlayState::PLAYING;
    }

    if (entry.runtime->sceneRender){
        entry.runtime->sceneRender->setPlayMode(true);
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
    }

    for (const auto& entry : runtimeScenes) {
        SceneProject* runtime = entry.runtime;
        if (!runtime) {
            continue;
        }

        SceneManager::removeScenePtr(entry.sourceSceneId);

        if (entry.ownedRuntime) {
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
        YAML::Node sceneNode = YAML::LoadFile(fullPath.string());

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
                        YAML::Node materialNode = YAML::LoadFile(materialPath.string());
                        Material fileMaterial = Stream::decodeMaterial(materialNode);
                        fileMaterial.name = normalizedPath;

                        std::error_code ec;
                        materialFileLinks[MaterialLinkKey{targetScene->id, entity, submeshIndex}].lastWriteTime = fs::last_write_time(materialPath, ec);

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
        targetScene->isModified = loadSceneData && targetScene->prefabRefsChanged;
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

void editor::Project::openScene(fs::path filepath, bool closePrevious){
    if (isAnyScenePlaying()){
        Out::warning("Cannot open a new scene while a scene is playing.");
        return;
    }

    uint32_t sceneToClose = NULL_PROJECT_SCENE;
    if (closePrevious) {
        SceneProject* selectedScene = getSelectedScene();
        if (selectedScene) {
            sceneToClose = selectedScene->id;
        }
    }

    checkUnsavedAndExecute(sceneToClose, [this, filepath, sceneToClose]() {
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
    generator.clearSceneSource(it->name, getProjectInternalPath());

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

bool editor::Project::saveSceneNow(uint32_t sceneId, const fs::path& pathIfUntitled, std::string* error) {
    NonInteractiveScope scope(nonInteractiveDepth);
    std::string message;
    if (isPrefabStage(sceneId)) {
        const uint64_t logId = Out::lastLogId();
        if (!savePrefabStage(sceneId)) {
            return failWithLogged(error, logId, "The prefab of the stage could not be saved");
        }
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
        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            return failWith(error, "Could not create " + target.parent_path().generic_string() + ": " + ec.message());
        }
    }

    const uint64_t logId = Out::lastLogId();
    if (!saveSceneToPath(sceneId, target)) {
        return failWithLogged(error, logId, "Failed to save scene " + target.generic_string());
    }

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

        YAML::Node sceneNode = YAML::LoadFile(fullPath.string());

        Stream::decodeSceneProject(childScene, sceneNode, true);
        loadSceneProjectData(childScene, sceneNode);

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

    sceneProject->mainCamera = NULL_ENTITY;
    sceneProject->defaultCamera = NULL_ENTITY;

    sceneProject->entities.clear();
    sceneProject->selectedEntities.clear();
    sceneProject->missingPrefabs.clear();
}

void editor::Project::resetEngineConfigs(bool executeViewChanged) {
    Engine::setScalingMode(Scaling::NATIVE);
    Engine::setTextureStrategy(textureStrategy);
    Engine::setMouseCursor(CursorType::ARROW);
    Engine::setMouseMode(MouseMode::NORMAL);
    // a game (or the MCP server) may have changed the game clock
    Engine::setTimeScale(1.0f);
    Engine::setFixedDeltatime(0.0);
    Engine::setSimulationHeld(false);
    // a game may have turned it off
    Engine::setAsyncLoading(true);

    if (executeViewChanged) {
        Engine::systemViewChanged();
    }
}

void editor::Project::resetConfigs() {
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

    // Scenes are gone: drop unreferenced pool assets from the previous project.
    // Keep entries still held by the editor/engine (fonts, active shaders, etc.).
    Engine::clearUnusedPools();

    // Reset state
    name = "";
    canvasWidth = defaultCanvasWidth;
    canvasHeight = defaultCanvasHeight;
    scalingMode = defaultScalingMode;
    textureStrategy = defaultTextureStrategy;
    vsyncEnabled = defaultVSyncEnabled;
    setHtmlUiMsaaSamples(defaultHtmlUiMsaaSamples);
    windowMode = defaultWindowMode;
    windowWidth = defaultWindowWidth;
    windowHeight = defaultWindowHeight;
    windowResizable = defaultWindowResizable;
    windowTitle = defaultWindowTitle;
    windowIcon.clear();
    assetsDir = defaultAssetsDir;
    luaDir = defaultLuaDir;
    scriptDirs.clear();
    cxxStandard = defaultCxxStandard;
    physics2DEnabled = defaultPhysics2DEnabled;
    physics3DEnabled = defaultPhysics3DEnabled;
    runtimeShadersEnabled = defaultRuntimeShadersEnabled;
    setTagsAndLayers({}, {});
    setLayerCollisionMatrices(allLayersCollide(), allLayersCollide());
    packNativeResources = defaultPackNativeResources;
    versionControlMetadata = defaultVersionControlMetadata;
    shaderOverrides = {};
    sourceCodeExportSettings = {};
    desktopExportSettings = {};
    applicationSettings = {};
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
    terrainEditorSettings = {};
    simulateTouch = false;
    projectPath.clear();
    assetDatabase.close();
    pendingAssetChanges.clear();
    materialFileLinks.clear();
    tabs.clear();
    lastMaterialRefreshTime = std::chrono::steady_clock::time_point{};
    projectHistory.clear();

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
    scene->getSystem<AudioSystem>()->setPaused(pause);
}

void editor::Project::copyEngineApiToProject() {
    // Lua API stubs and LSP configuration (lua-language-server, clangd) next to the snapshot.
    // Written even when the engine folder is missing or the copy fails: the stubs are built
    // into the editor.
    auto writeTooling = [this]() {
        Generator::writeEditorTooling(getProjectPath(), getProjectInternalPath(), getCxxStandard(), getScriptDirs());
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
            std::filesystem::create_directories(getProjectInternalPath());
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
                    std::filesystem::create_directories(destPath.parent_path());

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
        SceneProject* sceneProject = entry.runtime;
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
        SceneProject* runtime = entry.runtime;
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

    return keep;
}

// Engine cameras live in the system range, so only user entities count as made by the game
std::vector<Entity> editor::Project::getPlayCreatedEntities(const SceneProject* sceneProject) const {
    if (!sceneProject || !sceneProject->scene ||
        (sceneProject->playState != ScenePlayState::PLAYING && sceneProject->playState != ScenePlayState::PAUSED)) {
        return {};
    }

    const std::set<Entity> keep = getPlayKeptEntities(sceneProject);

    std::vector<Entity> created;
    for (Entity entity : sceneProject->scene->getEntityList()) {
        if (entity > EntityManager::lastSystemEntity() && keep.count(entity) == 0) {
            created.push_back(entity);
        }
    }

    return created;
}

// Entities a script spawns while playing are not in the editor list, so without this they
// stay alive after Stop: drawn in the viewport but no longer reachable from the Hierarchy.
void editor::Project::destroyPlayCreatedEntities(SceneProject* sceneProject) {
    Scene* scene = sceneProject->scene;

    const std::set<Entity> keep = getPlayKeptEntities(sceneProject);

    for (Entity entity : scene->getEntityList()) {
        // an owner's removal can destroy other entities in this list
        if (keep.count(entity) == 0 && scene->isEntityCreated(entity)) {
            scene->destroyEntity(entity);
        }
    }
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
        std::vector<Entity> restored = Stream::decodeEntity(entityNode, sceneProject->scene, nullptr, nullptr, sceneProject, NULL_ENTITY, false, true);

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
        SceneProject* sceneProject = entry.runtime;
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
        sceneProject->scene->getSystem<UISystem>()->setAnchorReferenceSize(canvasWidth, canvasHeight);
        sceneProject->scene->getSystem<UIDocumentSystem>()->setReferenceSize(canvasWidth, canvasHeight);

        // GameObject.destroy calls still pending; what the game made goes with the snapshot
        ObjectManager::forgetScene(sceneProject->scene);

        // Stop scene audio before restoring snapshot to prevent stale SoLoud handles on the next play
        sceneProject->scene->getSystem<AudioSystem>()->stopSceneSounds();

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

        // Restore snapshot if present
        const bool restoredFromSnapshot = restorePlaySnapshot(sceneProject);

        sceneProject->playState = ScenePlayState::STOPPED;
        if (!entry.ownedRuntime) {
            // the edit-time scene itself played (runtime copies have no history)
            CommandHandle::get(sceneProject->id)->endPlaySession(restoredFromSnapshot);
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

        editor::getEditorHost().prepareForProjectSwitch();

        resetConfigs();

        // Clear the last project path in settings when creating a new temp project (a
        // project elsewhere becomes it when saved below)
        AppSettings::setLastProjectPath(std::filesystem::path());

        projectPath = path;
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

            modelLoaded = true;
            assetDatabase.open(projectPath);
            saveProject();
            createNewScene("New Scene", SceneType::SCENE_3D);
            copyEngineApiToProject();
        } else {
            Out::info("Project directory already exists: \"%s\"", projectPath.string().c_str());
            loadProject(projectPath);
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

bool editor::Project::saveProjectFile() {
    if (projectPath.empty()) {
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

        // Trailing newline: git reports a file without one as changed as soon as
        // anything is appended to it
        if (!FileUtils::writeFileReplacing(projectFile, YAML::Dump(Stream::encodeProject(this)) + "\n", FileUtils::LineEndings::KeepExisting)) {
            Out::error("Failed to write project file: %s", projectFile.string().c_str());
            return false;
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
    "Thumbs.db\n";

// Scenes and project.yaml only diff and merge while every checkout agrees on the
// line ending, so it is not left to core.autocrlf
const char* versionControlAttributes =
    "# Generated by JasperEngine Editor.\n"
    "\n"
    "* text=auto eol=lf\n"
    "\n"
    "*.scene   text eol=lf\n"
    "*.prefab  text eol=lf\n"
    "*.yaml    text eol=lf\n"
    "*.lua     text eol=lf\n"
    "*.cmake   text eol=lf\n"
    "*.h       text eol=lf\n"
    "*.cpp     text eol=lf\n"
    "\n"
    "*.png     binary\n"
    "*.jpg     binary\n"
    "*.jpeg    binary\n"
    "*.tga     binary\n"
    "*.ktx     binary\n"
    "*.dds     binary\n"
    "*.glb     binary\n"
    "*.gltf    binary\n"
    "*.fbx     binary\n"
    "*.obj     binary\n"
    "*.ogg     binary\n"
    "*.wav     binary\n"
    "*.mp3     binary\n"
    "*.ttf     binary\n"
    "*.otf     binary\n";

} // namespace

bool editor::Project::writeVersionControlMetadata() {
    if (projectPath.empty()) {
        return false;
    }

    const std::pair<const char*, const char*> files[] = {
        {".gitignore", versionControlIgnore},
        {".gitattributes", versionControlAttributes},
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
        try {
            std::filesystem::remove_all(trashPath);
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
        assetDatabase.open(projectPath);
    }

    if (!saveProjectFile()) {
        return false;
    }

    if (!isTempProject()){
        AppSettings::setLastProjectPath(path);
    }
    editor::getEditorHost().updateResourcesPath();

    return true;
}

bool editor::Project::loadProject(const std::filesystem::path requestedPath, bool updateLastOpened) {
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
        YAML::Node projectNode = YAML::LoadFile(projectFile.string());

        // The workspace is written from the live model, which resetConfigs() is about
        // to drop, so the outgoing project is flushed first
        if (modelLoaded && !projectPath.empty()) {
            saveWorkspaceFile();
        }

        editor::getEditorHost().prepareForProjectSwitch();

        resetConfigs();
        projectPath = path;

        // shader manifests of forks are read from this project's files
        // (the project root, else the assets directory: findShaderFile, read when asked, so
        // the assets directory loaded with the project and any later change apply)
        ShaderManifests::setForkReader([this](const std::string& relativePath, std::string& content) {
            return readShaderFile(relativePath, content);
        });

        // Before any scene or prefab loads: every prefab file gets its GUID (the one-shot
        // upgrade of files written before them) and references resolve through it
        editor::getEditorHost().reportLoadingProgress("Scanning assets...");
        assetDatabase.open(projectPath);

        // Per-user state comes from .jasperengine/user/, falling back to the copy an older
        // editor left in project.yaml
        Workspace workspace;
        workspace.load(projectPath);
        workspace.adoptLegacyProjectState(projectNode);
        workspaceMigrationPending = workspace.hasAdoptedLegacyState();

        Stream::decodeProject(this, projectNode, workspace);
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

        for (auto& sceneProject : scenes) {
            if (sceneProject.opened) {
                removeMissingChildSceneReferences(sceneProject);
            }
        }

        if (editor::getEditorHost().shouldSyncEngineApi()) {
            copyEngineApiToProject();
        }

        editor::getEditorHost().updateResourcesPath();

        // Save this as the last opened project
        if (updateLastOpened && !isTempProject()) {
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

        if (!FileUtils::writeFileReplacing(fullPath, content + "\n", FileUtils::LineEndings::KeepExisting)) {
            Out::error("Failed to write scene file: %s", fullPath.string().c_str());
            return false;
        }
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

    if (!sceneProject->filepath.empty()) {
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
    auto saveNext = std::make_shared<std::function<void(bool)>>();
    *saveNext = [this, ids, index, callback, saveNext](bool previousSuccess) {
        if (!previousSuccess) {
            if (callback) callback(false);
            return;
        }

        while (*index < ids->size()) {
            uint32_t id = (*ids)[(*index)++];
            if (hasSceneUnsavedChanges(id)) {
                saveScene(id, *saveNext);
                return;
            }
        }

        if (callback) callback(true);
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

bool editor::Project::saveSceneFile(SceneProject* sceneProject, const std::filesystem::path& path, bool stopTransientPreviews) {
    if (!sceneProject || path.empty()) {
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

    // Its prefabs with changes, and the ones read from a file of an older version (upgraded)
    for (const auto& [filepath, prefab] : prefabAssets) {
        if (prefab.instances.find(sceneProject->id) != prefab.instances.end() && (prefab.isModified || prefab.needsUpgrade)) {
            savePrefabToDisk(filepath);
        }
    }

    SceneMaxValues maxValues;
    maxValues.maxBones = sceneProject->maxValues.maxBones;
    calculateSceneMaxValues(sceneProject, maxValues);
    sceneProject->maxValues = maxValues;

    updateSceneCppScripts(sceneProject);
    updateScenePrefabs(sceneProject);

    // What the content needs, the variants the scene drew since its last save (a script's
    // runtime meshes in Play), and the keys saved before
    std::set<ShaderKey> shaderKeys;
    collectRequiredShaderKeys(sceneProject, shaderKeys);
    collectSceneShaderKeys(sceneProject, shaderKeys);
    shaderKeys.insert(sceneProject->shaderKeys.begin(), sceneProject->shaderKeys.end());
    sceneProject->shaderKeys = std::move(shaderKeys);

    try {
        YAML::Node root = Stream::encodeSceneProject(this, sceneProject);
        if (!FileUtils::writeFileReplacing(fullPath, YAML::Dump(root) + "\n", FileUtils::LineEndings::KeepExisting)) {
            Out::error("Failed to write scene file: %s", fullPath.string().c_str());
            return false;
        }

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
        sceneProject->isModified = false;
    } catch (const std::exception& e) {
        Out::error("Failed to save scene file: %s", e.what());
        return false;
    }

    return true;
}

bool editor::Project::saveSceneToPath(uint32_t sceneId, const std::filesystem::path& path, bool stopTransientPreviews) {
    if (!writeSceneToPath(sceneId, path, stopTransientPreviews)) {
        return false;
    }

    return saveProject();
}

bool editor::Project::writeSceneToPath(uint32_t sceneId, const std::filesystem::path& path, bool stopTransientPreviews) {
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject) {
        return false;
    }

    fs::path fullPath = path;
    if (fullPath.is_relative()) {
        fullPath = getProjectPath() / fullPath;
    }

    std::string oldFilepath = sceneProject->filepath.string();
    if (!saveSceneFile(sceneProject, path, stopTransientPreviews)) {
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
    generator.writeSceneSource(sceneProject->scene, sceneProject->name, sceneProject->entities, getSceneCamera(sceneProject), getProjectPath(), getProjectInternalPath(), prefabInstances, collectResourcesInRegistry(sceneProject->scene));
}

bool editor::Project::hasMissingSceneSources() const {
    return std::any_of(scenes.begin(), scenes.end(), [&](const SceneProject& sceneProject){
        return !generator.hasSceneSource(sceneProject.name, getProjectInternalPath());
    });
}

// Scenes the editor never saved or played, like one created outside it
void editor::Project::writeMissingSceneSources() {
    for (const SceneProject& sceneProject : scenes) {
        if (generator.hasSceneSource(sceneProject.name, getProjectInternalPath())) {
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
    generator.configure(scenesToConfig, libName, mergedCppScripts, prefabBuildInfos, getProjectPath(), getProjectInternalPath(), getAssetsPath(), getLuaPath(), getScriptDirs(), getCxxStandard(), scalingMode, textureStrategy, canvasWidth, canvasHeight, vsyncEnabled, getWindowSettings(), loadingSettings, TagsAndLayers{tags, layerNames, layerCollisionMatrix3D, layerCollisionMatrix2D}, htmlUiMsaaSamples);
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

void editor::Project::saveSceneToPathAsync(uint32_t sceneId, const std::filesystem::path& path, std::function<void(bool)> callback) {
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

    sceneProject->playState = ScenePlayState::SAVING;
    fs::path savePath = path;

    std::thread([this, sceneId, savePath, callback = std::move(callback)]() mutable {
        bool success = false;
        try {
            success = writeSceneToPath(sceneId, savePath, false);
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
    }).detach();
}

void editor::Project::saveAllScenes(std::function<void(bool)> callback) {
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
                const LineRendererComponent& lines = scene->getComponent<LineRendererComponent>(entity);
                for (const LineData& line : lines.lines) {
                    mergeWorldPoint(transform.modelMatrix * line.pointA);
                    mergeWorldPoint(transform.modelMatrix * line.pointB);
                }
            }
        }
    }

    return aabb;
}

AABB editor::Project::getEntityLocalAABB(Scene* scene, Entity entity) const{
    AABB aabb;
    Signature signature = scene->getSignature(entity);

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
        if (signature.test(scene->getComponentId<LineRendererComponent>())){
            const LineRendererComponent& lines = scene->getComponent<LineRendererComponent>(entity);
            for (const LineData& line : lines.lines) {
                aabb.merge(line.pointA);
                aabb.merge(line.pointB);
            }
        }
    }

    return aabb;
}

// What the view draws of an entity: hidden, inactive (itself or through a parent) and
// disabled renderers draw nothing, so nothing of them can be clicked or boxed
static bool isEntityDrawn(Scene* scene, Entity entity){
    const Transform* transform = scene->findComponent<Transform>(entity);
    if (!transform || !transform->visible){
        return false;
    }
    if (scene->hasInactiveEntities() && !scene->isEntityActiveInHierarchy(entity)){
        return false;
    }
    if (const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)){
        return mesh->enabled;
    }
    if (const UIComponent* ui = scene->findComponent<UIComponent>(entity)){
        return ui->enabled;
    }
    if (const TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity)){
        return terrain->enabled;
    }
    return true;
}

// Bounds that come only from the icon the view's own scene draws for it
static bool hasOnlyIconBounds(Scene* scene, Entity entity){
    Signature signature = scene->getSignature(entity);
    if (signature.test(scene->getComponentId<MeshComponent>()) || signature.test(scene->getComponentId<UIComponent>())){
        return false;
    }
    return signature.test(scene->getComponentId<LightComponent>()) ||
           signature.test(scene->getComponentId<Light2DComponent>()) ||
           signature.test(scene->getComponentId<CameraComponent>()) ||
           signature.test(scene->getComponentId<AudioSourceComponent>());
}

Entity editor::Project::findBestEntityByRay(const std::vector<Entity>& entities, Scene* scene, const Ray& ray, Scene* sizeScene, SceneType sceneType, float& distance, size_t& index, bool iconsDrawn) const{
    Entity selEntity = NULL_ENTITY;
    Vector3 rayDirection = ray.getDirection();
    float rayLengthSq = rayDirection.dotProduct(rayDirection);
    if (rayLengthSq <= std::numeric_limits<float>::epsilon()) return NULL_ENTITY;

    for (auto& entity : entities) {
        if (!scene->isEntityCreated(entity)) continue;
        Signature signature = scene->getSignature(entity);
        if (!signature.test(scene->getComponentId<Transform>())) continue;
        if (!isEntityDrawn(scene, entity)) continue;
        if (!iconsDrawn && hasOnlyIconBounds(scene, entity)) continue;

        if (signature.test(scene->getComponentId<TerrainComponent>())) {
            TerrainComponent& terrain = scene->getComponent<TerrainComponent>(entity);
            Transform& transform = scene->getComponent<Transform>(entity);
            Vector3 worldPoint;
            if (!scene->getSystem<MeshSystem>()->raycastTerrainSurface(ray, terrain, transform, worldPoint)) continue;

            float terrainDistance = (worldPoint - ray.getOrigin()).dotProduct(rayDirection) / rayLengthSq;
            if (terrainDistance < 0.0f || terrainDistance > 1.0f) continue;

            size_t nIndex = scene->getComponentArray<Transform>()->getIndex(entity);
            if (terrainDistance < distance || (nIndex >= index && sceneType != SceneType::SCENE_3D)){
                distance = terrainDistance;
                index = nIndex;
                selEntity = entity;
            }
            continue;
        }

        AABB aabb = getEntityWorldAABB(scene, entity, sizeScene);

        if (!aabb.isNull() && !aabb.isInfinite()){
            RayReturn rreturn = ray.intersects(aabb);
            if (rreturn.hit){
                size_t nIndex = scene->getComponentArray<Transform>()->getIndex(entity);
                if (rreturn.distance < distance || (nIndex >= index && sceneType != SceneType::SCENE_3D)){
                    distance = rreturn.distance;
                    index = nIndex;
                    selEntity = entity;
                }
            }
        }
    }
    return selEntity;
}

bool editor::Project::selectEntitiesInRect(uint32_t sceneId, const std::vector<Entity>& entities, Scene* scene, const Matrix4& vpMatrix, Vector2 start, Vector2 end){
    Vector2 minRect = Vector2(std::min(start.x, end.x), std::min(start.y, end.y));
    Vector2 maxRect = Vector2(std::max(start.x, end.x), std::max(start.y, end.y));

    bool found = false;
    for (auto& entity : entities) {
        if (!scene->isEntityCreated(entity)) continue;
        if (!scene->getSignature(entity).test(scene->getComponentId<Transform>())) continue;
        if (!isEntityDrawn(scene, entity)) continue;

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

        if (selectEntitiesInRect(layer.sceneId, getPickableEntities(layer.sceneProject), layer.sceneProject->scene, vpMatrix, start, end)){
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
    sceneProject->sceneRender->update(sceneProject->selectedEntities, sceneProject->entities, sceneProject->mainCamera, sceneProject->displaySettings);
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
        if (entry.ownedRuntime && entry.runtime && entry.runtime->id != entry.sourceSceneId) {
            runtimes.push_back(entry.runtime);
        }
    }
    return runtimes;
}

uint32_t editor::Project::getPlayRuntimeSceneId(uint32_t sourceSceneId) const {
    std::scoped_lock lock(playSessionMutex);
    if (!activePlaySession || activePlaySession->cancelled.load(std::memory_order_acquire) ||
        !activePlaySession->startupSucceeded.load(std::memory_order_acquire)) {
        return NULL_PROJECT_SCENE;
    }
    for (const PlayRuntimeScene& entry : activePlaySession->runtimeScenes) {
        if (entry.ownedRuntime && entry.sourceSceneId == sourceSceneId && entry.runtime && entry.runtime->id != sourceSceneId) {
            return entry.runtime->id;
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
            if (entry.ownedRuntime && entry.runtime && entry.runtime->id == sceneId) {
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
            if (entry.ownedRuntime && entry.runtime && entry.runtime->scene && entry.runtime->id != entry.sourceSceneId) {
                runtimes.push_back({entry.runtime, Engine::isSceneRunning(entry.runtime->scene)});
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
    std::error_code ec;
    auto relPath = std::filesystem::relative(projectPath, std::filesystem::temp_directory_path(), ec);

    // relative() returns empty across different roots, like a project on D: while temp is on C:
    if (ec || relPath.empty()) {
        return false;
    }

    return *relPath.begin() != "..";
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

fs::path editor::Project::getUserDataPath() const{
    // One player's progress, not project content. At the assets root, as a standalone
    // build has it, every playtest would leave a modified file in the working tree.
    const fs::path path = getProjectInternalPath() / "user" / "rundata";
    std::error_code ec;
    fs::create_directories(path, ec);
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

    std::string hashInput = relativePath.empty() ? resolvedPath.generic_string() : relativePath.generic_string();

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

void editor::Project::replaceSelectedEntities(uint32_t sceneId, std::vector<Entity> selectedEntities){
    // Ctrl-click deselect reaches the selection only through here.
    markSelectionChanged(getScene(sceneId));
    getScene(sceneId)->selectedEntities = selectedEntities;
}

void editor::Project::setSelectedEntity(uint32_t sceneId, Entity selectedEntity){
    // The selection outline/gizmo is rendered, so a selection change needs a redraw.
    markSelectionChanged(getScene(sceneId));
    std::vector<Entity>& entities = getScene(sceneId)->selectedEntities;

    entities.clear();
    if (selectedEntity != NULL_ENTITY){
        entities.push_back(selectedEntity);
    }
}

void editor::Project::addSelectedEntity(uint32_t sceneId, Entity selectedEntity){
    markSelectionChanged(getScene(sceneId));
    std::vector<Entity>& entities = getScene(sceneId)->selectedEntities;
    Scene* scene = getScene(sceneId)->scene;
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
}

bool editor::Project::isSelectedEntity(uint32_t sceneId, Entity selectedEntity){
    std::vector<Entity>& entities = getScene(sceneId)->selectedEntities;

    if (std::find(entities.begin(), entities.end(), selectedEntity) != entities.end()) {
        return true;
    }

    return false;
}

void editor::Project::deselectDestroyedEntities(SceneProject* sceneProject){
    std::vector<Entity>& selected = sceneProject->selectedEntities;
    auto destroyed = std::remove_if(selected.begin(), selected.end(), [sceneProject](Entity entity) {
        return !sceneProject->scene->isEntityCreated(entity);
    });
    if (destroyed != selected.end()){
        selected.erase(destroyed, selected.end());
        markSelectionChanged(sceneProject);
    }
}

void editor::Project::clearSelectedEntities(uint32_t sceneId){
    SceneProject* sceneProject = getScene(sceneId);
    if (!sceneProject->selectedEntities.empty()){
        markSelectionChanged(sceneProject);
    }
    sceneProject->selectedEntities.clear();
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

std::vector<Entity> editor::Project::getSelectedEntities(uint32_t sceneId) const{
    return getScene(sceneId)->selectedEntities;
}

bool editor::Project::hasSelectedEntities(uint32_t sceneId) const{
    return (getScene(sceneId)->selectedEntities.size() > 0);
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

    for (fs::recursive_directory_iterator it(projectPath, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
        const fs::directory_entry& entry = *it;

        // '.jasperengine' holds the engine copy and the generated resources, 'build' the artifacts
        const std::string filename = entry.path().filename().string();
        if (filename.rfind('.', 0) == 0 || filename == "build") {
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
        SceneManager::clearAll();
        PrefabManager::clearAll();
        ObjectManager::clearAll();
        clearPlayPrefabTemplates();
        jasperengine::FunctionSubscribeGlobal::getCrashHandler() = nullptr;

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

void editor::Project::runPlayStartup(const std::shared_ptr<PlaySession>& session, uint32_t sceneId, const LocalBuildSettings& buildSettings) {
    auto isCancelled = [session]() {
        return session->cancelled.load(std::memory_order_acquire);
    };

    auto markStartupDone = [session]() {
        session->startupThreadDone.store(true, std::memory_order_release);
    };

    try {
        SceneProject* mainSceneProject = getScene(sceneId);
        if (!mainSceneProject) {
            failPlayStartup(session, sceneId, "Failed to find scene to start");
            return;
        }

        std::vector<uint32_t> involvedMainSceneIds;
        collectInvolvedScenes(sceneId, involvedMainSceneIds);

        std::vector<editor::SceneBuildInfo> scenesToGenerate;
        for (SceneProject& currentSceneProject : scenes) {
            if (isCancelled()) {
                markStartupDone();
                return;
            }

            bool isInvolvedScene = std::find(involvedMainSceneIds.begin(), involvedMainSceneIds.end(), currentSceneProject.id) != involvedMainSceneIds.end();

            if (currentSceneProject.opened || (isInvolvedScene && currentSceneProject.scene)) {
                updateAllScriptsProperties(currentSceneProject.id);

                if (currentSceneProject.isModified && !currentSceneProject.filepath.empty()) {
                    if (!saveSceneForPlayStartup(&currentSceneProject)) {
                        failPlayStartup(session, sceneId, "Failed to save modified scene before play startup");
                        return;
                    }
                    // involved scenes are written below
                    if (!isInvolvedScene) {
                        writeSceneSource(&currentSceneProject);
                    }
                } else {
                    updateSceneCppScripts(&currentSceneProject);
                    updateScenePrefabs(&currentSceneProject);
                }
            } else if (!currentSceneProject.scene && currentSceneProject.cppScripts.empty() && !currentSceneProject.filepath.empty()) {
                // SceneManager can load a closed scene while playing, so its scripts must be built too;
                // a scene never saved by the editor has no script list yet, read it from the entities
                std::unique_ptr<SceneProject> probe;
                try {
                    probe.reset(createRuntimeCloneFromSource(&currentSceneProject));
                    updateSceneCppScripts(probe.get());
                    currentSceneProject.cppScripts = probe->cppScripts;
                } catch (const std::exception& e) {
                    Out::warning("Scripts of scene '%s' not collected: %s", currentSceneProject.name.c_str(), e.what());
                }
                if (probe) {
                    deleteSceneProject(probe.get());
                }
            }

            if (isInvolvedScene) {
                PlayRuntimeScene entry;
                entry.sourceSceneId = currentSceneProject.id;

                if (currentSceneProject.id == sceneId) {
                    entry.runtime = &currentSceneProject;
                    entry.ownedRuntime = false;
                } else {
                    entry.runtime = createRuntimeCloneFromSource(&currentSceneProject);
                    entry.ownedRuntime = true;
                    assignPlayRuntimeId(entry.runtime);
                }

                if (!entry.runtime) {
                    failPlayStartup(session, sceneId, "Failed to create runtime scene for play startup");
                    return;
                }

                if (isCancelled()) {
                    if (entry.ownedRuntime) {
                        // never shown: nothing in the editor holds its id yet
                        releasePlayRuntimeId(entry.runtime, entry.sourceSceneId);
                        deleteSceneProject(entry.runtime);
                        delete entry.runtime;
                    }
                    markStartupDone();
                    return;
                }

                // prefabs only scripts reference or other prefabs nest get their sources too
                loadBuildPrefabs();
                const auto baked = bakePrefabsForExport();
                std::vector<PrefabInstanceInfo> prefabInstances = generator.writePrefabSources(prefabAssets, currentSceneProject.id, getProjectPath(), getProjectInternalPath(),
                    [this](const fs::path& path) { return getPrefabGuid(path); }, baked);
                generator.writeSceneSource(entry.runtime->scene, entry.runtime->name, entry.runtime->entities, getSceneCamera(entry.runtime), getProjectPath(), getProjectInternalPath(), prefabInstances, collectResourcesInRegistry(entry.runtime->scene));

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

        if (isCancelled()) {
            markStartupDone();
            return;
        }

        registerSceneManager();
        registerPrefabManager();

        YAML::Node playSnapshot = Stream::encodeSceneProject(nullptr, mainSceneProject);
        // with what describes its prefab instances, which Stop puts back with it
        PlayPrefabState playPrefabState = capturePlayPrefabState(mainSceneProject);

        // Play reuses the edit-time Scene, so anything alive now is authored or engine
        // owned. Whatever appears later belongs to the play session.
        const std::vector<Entity> aliveBeforePlay = mainSceneProject->scene->getEntityList();
        std::set<Entity> playEntities(aliveBeforePlay.begin(), aliveBeforePlay.end());

        if (isCancelled()) {
            markStartupDone();
            return;
        }

        std::vector<SceneScriptSource> mergedCppScripts = collectAllSceneCppScripts();
        std::vector<PrefabSceneInfo> prefabBuildInfos = collectAllPrefabs();
        generator.configure(scenesToGenerate, libName, mergedCppScripts, prefabBuildInfos, getProjectPath(), getProjectInternalPath(), getAssetsPath(), getLuaPath(), getScriptDirs(), getCxxStandard(), scalingMode, textureStrategy, canvasWidth, canvasHeight, vsyncEnabled, getWindowSettings(), loadingSettings, TagsAndLayers{tags, layerNames, layerCollisionMatrix3D, layerCollisionMatrix2D}, htmlUiMsaaSamples);
        if (hasMissingSceneSources()) {
            generatedSourcesDirty.store(true);
        }

        // play regenerates the standalone project, so its shaders are ensured here too
        buildStandaloneShaderCache();

        if (isCancelled()) { markStartupDone(); return; }

        const bool hasCppScripts = !mergedCppScripts.empty();

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
            generator.build(getProjectPath(), buildPath, playKit.cCompiler, playKit.cxxCompiler, playKit.generator, buildSettings.buildJobs);
            generator.waitForBuildToComplete();

            if (isCancelled()) { markStartupDone(); return; }

            if (!generator.didLastBuildSucceed()) {
                failPlayStartup(session, sceneId, "C++ script build failed");
                return;
            }

            if (!conector.connect(buildPath, libName)) {
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
                    if (entry.runtime && entry.runtime->scene) {
                        SceneManager::setScenePtr(entry.sourceSceneId, entry.runtime->scene);
                    }
                }
                // C++ projects run Lua scripts too, through the plugin's initScripts
                LuaBinding::clearLoadedProjectModules();

                // Apply project display settings before scripts read the canvas size.
                Engine::setCanvasSize(canvasWidth, canvasHeight);
                Engine::setScalingMode(scalingMode);
                Engine::setTextureStrategy(textureStrategy);
                Engine::systemViewChanged();

                // asyncLoading is only for exports, the editor always loads asynchronously
                if (getScene(loadingSettings.sceneId)) {
                    SceneManager::setLoadingScene(loadingSettings.sceneId);
                }
                SceneManager::setLoadingDelay(loadingSettings.delay);
                SceneManager::setLoadingTimeout(loadingSettings.timeout);
                // so Engine::onSceneLoaded fires, as for the exported start scene
                SceneManager::setCurrentScene(sceneId);

                // By index, and guarded: a script here can call addChildScene(), which
                // initializes another entry and appends to runtimeScenes. Initializing from
                // the snapshot above would then run a scene's scripts a second time.
                for (size_t i = 0; i < session->runtimeScenes.size(); i++) {
                    PlayRuntimeScene& entry = session->runtimeScenes[i];
                    if (entry.initialized || entry.initializing) continue;
                    if (!entry.runtime || !entry.runtime->scene) continue;

                    entry.initializing = true;

                    Scene* scene = entry.runtime->scene;
                    if (hasCppScripts) {
                        conector.init(scene);
                    } else {
                        LuaBinding::initializeLuaScripts(scene);
                    }

                    session->runtimeScenes[i].initializing = false;
                }

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
                    newEntry.runtime = createRuntimeCloneFromSource(sourceScene);
                    newEntry.ownedRuntime = true;

                    if (newEntry.runtime) {
                        assignPlayRuntimeId(newEntry.runtime);
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
        if (entry.runtime && entry.runtime->scene) {
            SceneManager::setScenePtr(entry.sourceSceneId, entry.runtime->scene);
        }
    }

    for (size_t entryIndex : stackIndices) {
        PlayRuntimeScene& entry = session->runtimeScenes[entryIndex];
        if (entry.initialized || entry.initializing || !entry.runtime || !entry.runtime->scene) {
            continue;
        }

        // Scripts can add another stack, which re-enters this function and pushes into
        // runtimeScenes. The flag stops it initializing this scene a second time, and the
        // entry is read again by index because the push invalidates references into it.
        entry.initializing = true;

        Scene* scene = entry.runtime->scene;
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

void editor::Project::registerSceneManager() {
    SceneManager::clearAll();
    for (SceneProject& sceneProject : scenes) {
        std::vector<uint32_t> stackSceneIds;
        collectStartActiveScenes(sceneProject.id, stackSceneIds);

        SceneManager::registerScene(sceneProject.id, sceneProject.name, [this, sceneId = sceneProject.id]() {
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
                if (!entry.runtime || !entry.runtime->scene) {
                    continue;
                }

                if (entry.sourceSceneId == sceneId) {
                    Engine::setScene(entry.runtime->scene);
                } else {
                    Engine::addSceneLayer(entry.runtime->scene);
                }
            }

            // Scenes no longer in the current stack
            std::vector<uint32_t> preservedSceneIds;
            std::vector<size_t> staleIndices;
            {
                std::scoped_lock lock(playSessionMutex);
                for (const PlayRuntimeScene& entry : session->runtimeScenes) {
                    if (entry.runtime && entry.runtime->scene && Engine::isSceneRunning(entry.runtime->scene)) {
                        collectInvolvedScenes(entry.sourceSceneId, preservedSceneIds);
                    }
                }

                for (size_t i = 0; i < session->runtimeScenes.size(); i++) {
                    PlayRuntimeScene& entry = session->runtimeScenes[i];
                    if (!entry.initialized || !entry.runtime || !entry.runtime->scene) continue;
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
                Scene* scene = session->runtimeScenes[entryIndex].runtime->scene;

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
                if (entry.sourceSceneId == sceneId && entry.runtime && entry.runtime->scene) {
                    runtimeScene = entry.runtime->scene;
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
            if (entry.runtime && entry.runtime->scene == scene) {
                return entry.runtime;
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

    {
        std::scoped_lock lock(playSessionMutex);
        if (activePlaySession) {
            Out::warning("Cannot start: a play session is already active");
            return;
        }
    }

    // An MCP agent's export or C++ script build would race Play's own script build
    const std::string busyReason = editor::getEditorHost().backgroundWorkConflict("play");
    if (!busyReason.empty()) {
        Out::warning("Cannot start Play: %s", busyReason.c_str());
        editor::getEditorHost().registerAlert("Cannot Play", busyReason);
        return;
    }

    jasperengine::FunctionSubscribeGlobal::getCrashHandler() = 
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
        };

    auto session = std::make_shared<PlaySession>();
    session->mainSceneId = sceneId;

    {
        std::scoped_lock lock(playSessionMutex);
        activePlaySession = session;
    }

    editor::getEditorHost().stopTransientPreviews();

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
    CommandHandle::get(sceneId)->beginPlaySession();
    editor::getEditorHost().saveAllCodeEditors();
    editor::getEditorHost().requestScenePlayFocus(sceneId);

    // Read on the UI thread: the startup thread must not race the settings dialog
    const LocalBuildSettings buildSettings = AppSettings::getBuildSettings(getProjectPath() / "project.yaml");

    std::thread startupThread([this, session, sceneId, buildSettings]() {
        runPlayStartup(session, sceneId, buildSettings);
    });
    startupThread.detach();
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
                if (!entry.runtime) continue;
                pauseEngineScene(entry.runtime->scene, true);
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
                if (!entry.runtime) continue;
                pauseEngineScene(entry.runtime->scene, false);
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
        Engine::onShutdown.call();
        Engine::pauseGameEvents(true);
        Engine::clearAllSubscriptions(true);
    }

    // Clear crash handler when stopping
    jasperengine::FunctionSubscribeGlobal::getCrashHandler() = nullptr;

    // Request cancellation asynchronously (returns a future we can wait on later if needed)
    auto cancelFuture = generator.cancelBuild();

    std::thread finalizeStopThread([this, session, sceneId, startupSucceededAtStop, cancelFuture = std::move(cancelFuture)]() mutable {
        if (cancelFuture.valid()) {
            cancelFuture.wait();
        }
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
                Engine::onShutdown.call();
                Engine::pauseGameEvents(true);
                Engine::clearAllSubscriptions(true);
            }

            std::vector<PlayRuntimeScene> runtimeScenes;
            {
                std::scoped_lock lock(playSessionMutex);
                runtimeScenes = session->runtimeScenes;
            }

            // anime: every animation of the play, and its script callbacks, before the scripts go
            AnimEngine::instance().reset();

            const bool libraryConnected = conector.isLibraryConnected();
            if (libraryConnected) {
                for (const auto& entry : runtimeScenes) {
                    if (entry.runtime) conector.cleanup(entry.runtime->scene);
                }
            } else if (startupSucceeded) {
                for (const auto& entry : runtimeScenes) {
                    if (entry.runtime) LuaBinding::cleanupLuaScripts(entry.runtime->scene);
                }
            }

            // What the scripts registered and did not remove is dropped here, while the code
            // behind those callbacks is still loaded
            for (const auto& entry : runtimeScenes) {
                if (entry.runtime && entry.runtime->scene) Engine::clearComponentSubscriptions(entry.runtime->scene);
            }

            if (libraryConnected) {
                conector.disconnect();
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

            {
                std::scoped_lock lock(playSessionMutex);
                if (activePlaySession == session) activePlaySession.reset();
            }
        });
    });
    finalizeStopThread.detach();
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
        if (entry.runtime && entry.runtime->scene) pauseEngineScene(entry.runtime->scene, false);
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
        if (entry.runtime && entry.runtime->scene) pauseEngineScene(entry.runtime->scene, true);
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
        SceneProject* runtimeProject = entry.runtime;
        if (!runtimeProject || !runtimeProject->scene || entry.sourceSceneId == sceneId) {
            continue;
        }
        if (Engine::isSceneRunning(runtimeProject->scene)) {
            runningLayers.push_back(runtimeProject);
        }
    }

    return runningLayers;
}
