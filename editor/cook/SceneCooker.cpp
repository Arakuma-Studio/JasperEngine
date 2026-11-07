// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SceneCooker.h"

#include "Catalog.h"
#include "Configs.h"
#include "Factory.h"
#include "Project.h"
#include "Stream.h"
#include "reflect/Reflect.h"
#include "serialization/CookedScene.h"
#include "serialization/RuntimeFields.h"
#include "serialization/SceneSettingsCodec.h"
#include "util/Fnv.h"
#include "component/MeshComponent.h"
#include "util/ProjectUtils.h"
#include "component/SkyboxComponent.h"
#include "component/SpriteComponent.h"
#include "resources/sky/Daylight_Box_Back_png.h"
#include "resources/sky/Daylight_Box_Bottom_png.h"
#include "resources/sky/Daylight_Box_Front_png.h"
#include "resources/sky/Daylight_Box_Left_png.h"
#include "resources/sky/Daylight_Box_Right_png.h"
#include "resources/sky/Daylight_Box_Top_png.h"

#include <algorithm>
#include <unordered_map>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace rt = jasperengine::cook;

namespace {

    // where a prebuilt build ships the default sky's faces (under the assets directory)
    const char* const kDefaultSkyDir = "je-default-sky";

    std::string defaultSkyFace(const char* name) {
        return std::string(kDefaultSkyDir) + "/Daylight_Box_" + name + ".png";
    }

    // the new 3D scene's sky: six images the editor holds in memory (ProjectUtils::setDefaultSkyTexture)
    bool isDefaultSky(const Texture& texture) {
        return texture.getId() == DEFAULT_SKY_ID && texture.getPath(0).empty();
    }

    bool usesDefaultSky(EntityRegistry* registry) {
        if (!registry) return false;
        auto skies = registry->getComponentArray<SkyboxComponent>();
        for (size_t i = 0; i < skies->size(); i++)
            if (isDefaultSky(skies->getComponentFromIndex(i).texture)) return true;
        return false;
    }

    const rt::ComponentTable* tableOf(ComponentType type) {
        const reflect::ComponentDesc* desc = reflect::find(type);
        return desc ? rt::findComponentTable(fnv1a32(desc->key)) : nullptr;
    }

    void writeState(rt::Writer& w, const EntityRegistry* registry, Entity entity) {
        w.string(registry->getEntityName(entity));
        w.boolean(registry->isEntityActive(entity));
        w.string(registry->getEntityTag(entity));
        w.svarint(registry->getEntityLayer(entity));
    }

    // the components of a GameObject in the exported code's order (Catalog::findComponents);
    // `skip` leaves one out, a Transform is written without its parent when `noParent`
    void writeComponents(rt::Writer& w, EntityRegistry* registry, Entity entity, bool skipPrefabLink, bool noParent) {
        std::vector<std::pair<const rt::ComponentTable*, const void*>> written;
        for (ComponentType type : Catalog::findComponents(registry, entity)) {
            if (skipPrefabLink && type == ComponentType::PrefabComponent) continue;
            const rt::ComponentTable* table = tableOf(type);
            if (!table) continue;
            const void* component = table->find(registry, entity);
            if (component) written.emplace_back(table, component);
        }
        w.varint(written.size());
        rt::WriteContext ctx;
        editor::cook::SceneCooker::describe(registry, entity, ctx);
        for (const auto& [table, component] : written) {
            if (noParent && std::string_view(table->cppName) == "Transform") {
                Transform copy = *static_cast<const Transform*>(component);
                copy.parent = NULL_ENTITY;
                rt::writeComponent(w, *table, &copy, ctx);
            } else if (std::string_view(table->cppName) == "MeshComponent" && registry->findComponent<SpriteComponent>(entity) &&
                       !registry->findComponent<SpriteComponent>(entity)->sheet.empty()) {
                // a sheet sprite's page is its sheet's (S44 R48): the game resolves it, never the editor's cache path
                MeshComponent copy = *static_cast<const MeshComponent*>(component);
                Texture pageless;
                pageless.setMinFilter(copy.submeshes[0].material.baseColorTexture.getMinFilter());
                pageless.setMagFilter(copy.submeshes[0].material.baseColorTexture.getMagFilter());
                pageless.setWrapU(copy.submeshes[0].material.baseColorTexture.getWrapU());
                pageless.setWrapV(copy.submeshes[0].material.baseColorTexture.getWrapV());
                copy.submeshes[0].material.baseColorTexture = pageless;
                rt::writeComponent(w, *table, &copy, ctx);
            } else if (std::string_view(table->cppName) == "SkyboxComponent" &&
                       isDefaultSky(static_cast<const SkyboxComponent*>(component)->texture)) {
                // a player reads files only: the faces cookProject ships (defaultSkyFiles), as the
                // Compile path's generated code embeds them
                SkyboxComponent copy = *static_cast<const SkyboxComponent*>(component);
                Texture shipped;
                shipped.setCubePaths(defaultSkyFace("Front"), defaultSkyFace("Back"), defaultSkyFace("Left"),
                                     defaultSkyFace("Right"), defaultSkyFace("Top"), defaultSkyFace("Bottom"));
                shipped.setMinFilter(copy.texture.getMinFilter());
                shipped.setMagFilter(copy.texture.getMagFilter());
                shipped.setWrapU(copy.texture.getWrapU());
                shipped.setWrapV(copy.texture.getWrapV());
                copy.texture = shipped;
                rt::writeComponent(w, *table, &copy, ctx);
            } else {
                rt::writeComponent(w, *table, component, ctx);
            }
        }
    }

}

void editor::cook::SceneCooker::describe(const EntityRegistry* registry, Entity entity, rt::WriteContext& ctx) {
    ctx.registry = registry;
    ctx.entity = entity;
    const Signature signature = registry->getSignature(entity);
    ctx.modelBackedMesh = Stream::isModelBackedMesh(entity, registry, signature);
    ctx.generatedMesh = ctx.modelBackedMesh || Stream::isGeneratedMesh(registry, signature);
    const TerrainComponent* terrain = const_cast<EntityRegistry*>(registry)->findComponent<TerrainComponent>(entity);
    ctx.terrainHeightmap = terrain && !terrain->heightMap.empty();
}

std::vector<editor::cook::CookedFileBytes> editor::cook::SceneCooker::defaultSkyFiles() {
    auto file = [](const char* name, const unsigned char* data, unsigned int length) {
        return CookedFileBytes{"assets/" + defaultSkyFace(name), std::vector<uint8_t>(data, data + length)};
    };
    return {file("Front", Daylight_Box_Front_png, Daylight_Box_Front_png_len),
            file("Back", Daylight_Box_Back_png, Daylight_Box_Back_png_len),
            file("Left", Daylight_Box_Left_png, Daylight_Box_Left_png_len),
            file("Right", Daylight_Box_Right_png, Daylight_Box_Right_png_len),
            file("Top", Daylight_Box_Top_png, Daylight_Box_Top_png_len),
            file("Bottom", Daylight_Box_Bottom_png, Daylight_Box_Bottom_png_len)};
}

std::vector<uint8_t> editor::cook::SceneCooker::cookScene(Scene* scene, uint32_t sceneId, const std::vector<Entity>& entities, Entity camera,
                                                  const SceneResources& resources) {
    rt::StringTable strings;
    // a scene's GameObjects keep their ids
    rt::Writer w(strings, [](Entity entity) { return static_cast<uint32_t>(entity); });

    {
        const size_t at = w.beginLength();
        rt::writeSceneSettings(w, *scene);
        w.endLength(at);
    }

    std::vector<Entity> ordered;
    for (Entity entity : entities)
        if (scene->isEntityCreated(entity)) ordered.push_back(entity);
    // parents before their children, whatever order the list came in (the runtime adds them so)
    ProjectUtils::sortEntitiesByTransformOrder(scene, ordered);
    // a default camera that is none of the scene's GameObjects: a system entity of its own
    const bool systemCamera = camera != NULL_ENTITY && std::find(ordered.begin(), ordered.end(), camera) == ordered.end() &&
                              scene->isEntityCreated(camera);
    {
        const size_t at = w.beginLength();
        w.varint(ordered.size() + (systemCamera ? 1 : 0));
        auto writeEntity = [&](Entity entity, uint8_t flags) {
            const size_t record = w.beginLength();
            w.u32(static_cast<uint32_t>(entity));
            w.u8(flags);
            writeState(w, scene, entity);
            writeComponents(w, scene, entity, false, false);
            w.endLength(record);
        };
        for (Entity entity : ordered) writeEntity(entity, 0);
        if (systemCamera) writeEntity(camera, rt::ENTITY_SYSTEM);
        w.endLength(at);
    }

    {
        const size_t at = w.beginLength();
        w.entity(camera);
        w.endLength(at);
    }

    {
        const size_t at = w.beginLength();
        rt::write(w, resources.textures);
        rt::write(w, resources.sounds);
        rt::write(w, resources.models);
        rt::write(w, resources.clips);
        w.endLength(at);
    }

    return rt::assembleFile(rt::SCENE_MAGIC, sceneId, static_cast<uint32_t>(ordered.size() + (systemCamera ? 1 : 0)), w.data(),
                            strings.strings());
}

std::vector<uint8_t> editor::cook::SceneCooker::cookPrefab(const std::filesystem::path& prefabPath, const BakedPrefab& baked,
                                                   const std::string& guid, uint32_t id) {
    Scene* scene = baked.scene.get();
    const Entity root = (scene && scene->isEntityCreated(baked.root)) ? baked.root : NULL_ENTITY;

    // references: 1 the root, 2 + i slot i; a slot whose GameObject an earlier one holds stays empty
    std::unordered_map<Entity, uint32_t> refs;
    std::vector<Entity> made;
    std::vector<bool> present(baked.slots.size(), false);
    for (size_t i = 0; i < baked.slots.size(); i++) {
        const Entity entity = baked.slots[i];
        if (entity != NULL_ENTITY && scene && scene->isEntityCreated(entity) && !refs.count(entity)) {
            refs[entity] = static_cast<uint32_t>(i + 2);
            made.push_back(entity);
            present[i] = true;
        }
    }
    if (root != NULL_ENTITY) refs[root] = 1;

    rt::StringTable strings;
    rt::Writer w(strings, [&refs](Entity entity) {
        auto it = refs.find(entity);
        return it != refs.end() ? it->second : 0u;
    });

    const std::string stem = prefabPath.stem().string();
    std::string rootName = root != NULL_ENTITY ? scene->getEntityName(root) : stem;
    if (rootName.empty()) rootName = stem.empty() ? "Prefab" : stem;
    {
        const size_t at = w.beginLength();
        w.string(stem);
        w.string(prefabPath.generic_string());
        w.string(guid);
        w.string(rootName);
        w.endLength(at);
    }

    {
        const size_t at = w.beginLength();
        w.boolean(root != NULL_ENTITY);
        if (root != NULL_ENTITY) {
            writeState(w, scene, root);
            writeComponents(w, scene, root, true, true);
        }
        w.endLength(at);
    }

    {
        const size_t at = w.beginLength();
        w.varint(baked.slots.size());
        for (size_t i = 0; i < baked.slots.size(); i++) {
            if (!present[i]) {
                w.u8(0);
                continue;
            }
            const Entity entity = baked.slots[i];
            w.u8(rt::ENTITY_PRESENT);
            writeState(w, scene, entity);
            // the roots of the nested instances keep their prefab link
            const PrefabComponent* link = scene->findComponent<PrefabComponent>(entity);
            w.boolean(link != nullptr);
            if (link) {
                w.string(link->name);
                w.string(std::filesystem::path(link->path).generic_string());
                w.string(link->guid);
            }
        }
        w.endLength(at);
    }

    {
        const size_t at = w.beginLength();
        w.varint(made.size());
        for (Entity entity : made) {
            const size_t record = w.beginLength();
            w.entity(entity);
            writeComponents(w, scene, entity, true, true);
            w.endLength(record);
        }
        w.endLength(at);
    }

    // the hierarchy in Transform order, each child appended to its parent
    {
        std::vector<std::pair<size_t, Entity>> orderedTransforms;
        if (scene) {
            auto transforms = scene->getComponentArray<Transform>();
            for (Entity entity : made)
                if (scene->findComponent<Transform>(entity)) orderedTransforms.emplace_back(transforms->getIndex(entity), entity);
        }
        std::sort(orderedTransforms.begin(), orderedTransforms.end());
        const size_t at = w.beginLength();
        w.varint(orderedTransforms.size());
        for (const auto& [index, entity] : orderedTransforms) {
            const Entity parent = scene->getComponent<Transform>(entity).parent;
            w.entity(entity);
            w.entity(refs.count(parent) ? parent : NULL_ENTITY);
        }
        w.endLength(at);
    }

    return rt::assembleFile(rt::PREFAB_MAGIC, id, static_cast<uint32_t>(made.size() + (root != NULL_ENTITY ? 1 : 0)), w.data(),
                            strings.strings());
}

bool editor::cook::SceneCooker::cookProject(Project* project, std::vector<editor::cook::CookedFileBytes>& files, std::vector<editor::cook::CookedPrefabInfo>& prefabs,
                                    std::string& error) {
    bool defaultSky = false;
    for (const SceneProject& sceneProject : project->getScenes()) {
        if (!sceneProject.scene) {
            error = "scene " + sceneProject.name + " is not loaded for the cook";
            return false;
        }
        defaultSky = defaultSky || usesDefaultSky(sceneProject.scene);
        files.push_back({"scenes/" + std::to_string(sceneProject.id) + ".jscn",
                         cookScene(sceneProject.scene, sceneProject.id, sceneProject.entities, project->getSceneCamera(&sceneProject),
                                   Project::collectResourcesInRegistry(sceneProject.scene))});
    }

    // the build's prefabs, by PrefabManager id (their position in collectAllPrefabs + 1), from
    // their baked instances
    project->loadBuildPrefabs();
    const auto baked = project->bakePrefabsForExport();
    const std::vector<PrefabSceneInfo> all = project->collectAllPrefabs();
    for (size_t i = 0; i < all.size(); i++) {
        const uint32_t id = static_cast<uint32_t>(i + 1);
        std::filesystem::path noExtension = all[i].prefabPath;
        noExtension.replace_extension();
        CookedPrefabInfo info{id, noExtension.generic_string(), all[i].guid, "prefabs/" + std::to_string(id) + ".jpfb"};
        auto bakedIt = baked.find(all[i].prefabPath);
        static const BakedPrefab unbaked;
        files.push_back({info.file, cookPrefab(all[i].prefabPath, bakedIt != baked.end() && bakedIt->second ? *bakedIt->second : unbaked,
                                               all[i].guid, id)});
        prefabs.push_back(std::move(info));
        defaultSky = defaultSky || (bakedIt != baked.end() && bakedIt->second && usesDefaultSky(bakedIt->second->scene.get()));
    }
    if (defaultSky) {
        for (CookedFileBytes& file : defaultSkyFiles()) files.push_back(std::move(file));
    }
    return true;
}
