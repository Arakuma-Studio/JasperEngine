// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShaderVariantCollector.h"

#include "Engine.h"
#include "System.h"
#include "pool/ShaderPool.h"
#include "io/FileData.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"
#include "component/FogComponent.h"
#include "component/InstancedMeshComponent.h"
#include "component/Light2DComponent.h"
#include "component/LightComponent.h"
#include "component/LineRendererComponent.h"
#include "component/MeshComponent.h"
#include "component/MeshPolygonComponent.h"
#include "component/MirrorComponent.h"
#include "component/ModelComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/PointsComponent.h"
#include "component/ReflectionProbeComponent.h"
#include "component/ShadowCaster2DComponent.h"
#include "component/SkyboxComponent.h"
#include "component/SpriteComponent.h"
#include "component/TerrainComponent.h"
#include "component/TilemapComponent.h"
#include "component/Transform.h"
#include "component/UIComponent.h"

#include <filesystem>
#include <map>
#include <memory>
#include <unordered_set>

using namespace jasperengine;

namespace {

    // What in a scene's content (or a prefab's, once spawned in it) decides the lighting
    // part of mesh shader variants, see RenderSystem::getMeshShaderEnvironment
    enum LightingFact : uint32_t {
        FACT_LIGHTS          = 1u << 0, // Light components
        FACT_SHADOW_LIGHTS   = 1u << 1, // a Light casting shadows
        FACT_FOG             = 1u << 2,
        FACT_SKY_IBL         = 1u << 3, // a skybox with a texture: its environment maps light meshes once ready
        FACT_PROBES          = 1u << 4, // Reflection Probes
        FACT_LIGHTS_2D       = 1u << 5,
        FACT_SHADOW_LIGHTS_2D= 1u << 6, // a 2D light casting shadows
        FACT_OCCLUDERS_2D    = 1u << 7, // Shadow Caster 2D components
    };

    uint32_t collectFacts(const editor::ShaderVariantContent& content){
        EntityRegistry* registry = content.registry;
        uint32_t facts = 0;
        for (Entity entity : content.entities){
            if (!registry->isEntityCreated(entity)){
                continue;
            }
            // Counted as RenderSystem counts them: enabled or not (a script can switch them on)
            if (const LightComponent* light = registry->findComponent<LightComponent>(entity)){
                facts |= FACT_LIGHTS;
                if (light->shadows){
                    facts |= FACT_SHADOW_LIGHTS;
                }
            }
            if (registry->findComponent<FogComponent>(entity)){
                facts |= FACT_FOG;
            }
            if (const SkyboxComponent* sky = registry->findComponent<SkyboxComponent>(entity)){
                if (!sky->texture.empty()){
                    facts |= FACT_SKY_IBL;
                }
            }
            if (registry->findComponent<ReflectionProbeComponent>(entity)){
                facts |= FACT_PROBES;
            }
            if (const Light2DComponent* light2d = registry->findComponent<Light2DComponent>(entity)){
                facts |= FACT_LIGHTS_2D;
                if (light2d->shadows){
                    facts |= FACT_SHADOW_LIGHTS_2D;
                }
            }
            if (registry->findComponent<ShadowCaster2DComponent>(entity)){
                facts |= FACT_OCCLUDERS_2D;
            }
        }
        return facts;
    }

    MeshShaderEnvironment toEnvironment(uint32_t facts, Scene* scene){
        MeshShaderEnvironment environment;
        // RenderSystem::checkLightsAndShadow
        const LightState lightState = scene->getLightState();
        environment.lights = lightState == LightState::ON || (lightState == LightState::AUTO && (facts & FACT_LIGHTS));
        environment.lightCapacity = RenderSystem::getLightCapacity(scene->getMaxLightsPerObject());
        environment.shadows = (facts & FACT_SHADOW_LIGHTS) != 0;
        environment.ibl = (facts & FACT_SKY_IBL) != 0;
        environment.reflectionProbes = (facts & FACT_PROBES) != 0;
        environment.fog = (facts & FACT_FOG) != 0;
        environment.lights2D = (facts & FACT_LIGHTS_2D) != 0;
        // RenderSystem::loadLights2D: a shadow-casting 2D light with an occluder to cast
        environment.shadows2D = (facts & FACT_SHADOW_LIGHTS_2D) && (facts & FACT_OCCLUDERS_2D);
        environment.ssao = scene->isSSAOEnabled();
        environment.ssr = scene->isSSREnabled();
        environment.depthPrepass = scene->isDepthPrepassEnabled();
        return environment;
    }

    uint32_t environmentBits(const MeshShaderEnvironment& e){
        return (e.lights ? 1u : 0u) | (e.shadows ? 2u : 0u) | (e.ibl ? 4u : 0u) | (e.reflectionProbes ? 8u : 0u) |
            (e.fog ? 16u : 0u) | (e.lights2D ? 32u : 0u) | (e.shadows2D ? 64u : 0u) | (e.ssao ? 128u : 0u) |
            (e.ssr ? 256u : 0u) | (e.depthPrepass ? 512u : 0u) | ((uint32_t)e.lightCapacity << 10);
    }

    // The lighting setups a scene can be in: its own content, with any of the build's prefabs
    // spawned in it. A skybox's environment maps light meshes only once they are built, so
    // the setups without them are reachable too (an asynchronous load draws before that).
    std::vector<MeshShaderEnvironment> reachableEnvironments(Scene* scene, uint32_t sceneFacts, const std::vector<uint32_t>& prefabFacts){
        std::set<uint32_t> reached{sceneFacts};
        std::vector<uint32_t> pending{sceneFacts};
        while (!pending.empty()){
            const uint32_t facts = pending.back();
            pending.pop_back();
            for (uint32_t added : prefabFacts){
                if (reached.insert(facts | added).second){
                    pending.push_back(facts | added);
                }
            }
        }

        std::set<uint32_t> withLoading = reached;
        for (uint32_t facts : reached){
            if (facts & FACT_SKY_IBL){
                withLoading.insert(facts & ~FACT_SKY_IBL);
            }
        }

        std::vector<MeshShaderEnvironment> environments;
        std::set<uint32_t> distinct;
        for (uint32_t facts : withLoading){
            MeshShaderEnvironment environment = toEnvironment(facts, scene);
            if (distinct.insert(environmentBits(environment)).second){
                environments.push_back(environment);
            }
        }
        return environments;
    }

    // A texture standing for one a model file gives a material: only whether there is one
    // decides a variant, and this keeps no image data alive in the cache
    const Texture& presentTexture(){
        static const Texture texture("shader-variant-texture");
        return texture;
    }

    void standInTextures(Material& material){
        for (Texture* texture : {&material.baseColorTexture, &material.metallicRoughnessTexture,
                &material.normalTexture, &material.occlusionTexture, &material.emissiveTexture}){
            *texture = texture->empty() ? Texture() : presentTexture();
        }
    }

    // The buffers RenderSystem::loadMesh draws a mesh from
    std::vector<const Buffer*> meshBuffers(const MeshComponent& mesh){
        std::vector<const Buffer*> buffers;
        if (mesh.buffer.getSize() > 0){
            buffers.push_back(&mesh.buffer);
        }
        if (mesh.indices.getSize() > 0){
            buffers.push_back(&mesh.indices);
        }
        for (unsigned int i = 0; i < mesh.numExternalBuffers; i++){
            buffers.push_back(&mesh.eBuffers[i]);
        }
        return buffers;
    }

    // What a model file loads into its meshes: the submeshes of the GameObject's own mesh and
    // of each node's, attribute flags scanned (RenderSystem::scanSubmeshAttributes)
    struct ModelProbe {
        bool loaded = false;
        std::vector<Submesh> root;
        std::map<int, std::vector<Submesh>> nodes; // glTF node index
        std::filesystem::file_time_type writeTime;
        uintmax_t size = 0;
    };

    std::vector<Submesh> probeSubmeshes(const MeshComponent& mesh){
        std::vector<Submesh> submeshes;
        const std::vector<const Buffer*> buffers = meshBuffers(mesh);
        for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
            Submesh submesh = mesh.submeshes[s];
            RenderSystem::scanSubmeshAttributes(submesh, buffers);
            standInTextures(submesh.material);
            submeshes.push_back(std::move(submesh));
        }
        return submeshes;
    }

    std::map<std::string, ModelProbe>& modelProbes(){
        static std::map<std::string, ModelProbe> probes;
        return probes;
    }

    const ModelProbe& probeModel(const std::string& filename, bool mergeStaticMeshes){
        // The file as the engine opens it, for its modification time
        const std::filesystem::path path(FileData::getSystemPath(filename));
        std::error_code timeError;
        std::error_code sizeError;
        const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(path, timeError);
        const uintmax_t size = std::filesystem::file_size(path, sizeError);

        const std::string key = MeshSystem::getModelFilenameKey(filename) + (mergeStaticMeshes ? "|merged" : "");
        auto cached = modelProbes().find(key);
        if (cached != modelProbes().end() && !timeError && !sizeError &&
                cached->second.writeTime == writeTime && cached->second.size == size){
            return cached->second;
        }

        ModelProbe probe;
        probe.writeTime = writeTime;
        probe.size = size;

        // Loaded as a scene loads it, into a scene of its own (the node meshes the file makes
        // are its children), then released as closing that scene would
        Scene scene(EntityPool::System);
        Entity entity = scene.createEntity();
        scene.addComponent<Transform>(entity, {});
        scene.addComponent<MeshComponent>(entity, {});
        ModelComponent modelComponent;
        modelComponent.mergeStaticMeshes = mergeStaticMeshes;
        modelComponent.needUpdateModel = false;
        scene.addComponent<ModelComponent>(entity, modelComponent);

        std::shared_ptr<MeshSystem> meshSystem = scene.getSystem<MeshSystem>();
        const bool obj = FileData::getFilePathExtension(filename) == "obj";
        probe.loaded = obj ? meshSystem->loadOBJ(entity, filename, false) : meshSystem->loadGLTF(entity, filename, false, false, false);

        if (probe.loaded){
            probe.root = probeSubmeshes(scene.getComponent<MeshComponent>(entity));
            const ModelComponent& model = scene.getComponent<ModelComponent>(entity);
            for (const auto& [nodeIndex, nodeEntity] : model.meshNodesMapping){
                if (const MeshComponent* nodeMesh = scene.findComponent<MeshComponent>(nodeEntity)){
                    probe.nodes[nodeIndex] = probeSubmeshes(*nodeMesh);
                }
            }
        }

        // The textures the load pooled go with the probe, unless a scene shares them
        auto meshes = scene.getComponentArray<MeshComponent>();
        for (size_t i = 0; i < meshes->size(); i++){
            MeshComponent& mesh = meshes->getComponentFromIndex(i);
            for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
                Material& material = mesh.submeshes[s].material;
                for (Texture* texture : {&material.baseColorTexture, &material.metallicRoughnessTexture,
                        &material.normalTexture, &material.occlusionTexture, &material.emissiveTexture}){
                    texture->destroy();
                }
            }
        }
        meshSystem->destroyModel(scene.getComponent<ModelComponent>(entity));

        if (timeError || sizeError){
            // No stamp to tell a changed file by: read again next time
            static ModelProbe uncached;
            uncached = std::move(probe);
            modelProbes().erase(key);
            return uncached;
        }
        ModelProbe& stored = modelProbes()[key];
        stored = std::move(probe);
        return stored;
    }

    class Collector {
    public:
        Collector(Scene* scene, const std::vector<MeshShaderEnvironment>& environments, std::set<ShaderKey>& keys):
            scene(scene), environments(environments), keys(keys) {}

        void collect(const editor::ShaderVariantContent& content){
            EntityRegistry* registry = content.registry;
            std::unordered_set<Entity> modelMeshes;

            // Model meshes first: their geometry and materials come from the file
            for (Entity entity : content.entities){
                if (!registry->isEntityCreated(entity)){
                    continue;
                }
                const ModelComponent* model = registry->findComponent<ModelComponent>(entity);
                if (model && !model->filename.empty()){
                    collectModel(registry, entity, *model, modelMeshes);
                }
            }

            for (Entity entity : content.entities){
                if (!registry->isEntityCreated(entity)){
                    continue;
                }
                if (!modelMeshes.count(entity)){
                    if (const MeshComponent* mesh = registry->findComponent<MeshComponent>(entity)){
                        collectMesh(registry, entity, *mesh);
                    }
                }
                if (const TerrainComponent* terrain = registry->findComponent<TerrainComponent>(entity)){
                    collectFoliage(*terrain);
                }
                if (const UIComponent* ui = registry->findComponent<UIComponent>(entity)){
                    collectUI(*ui);
                }
                const PointsComponent* points = registry->findComponent<PointsComponent>(entity);
                if (points || registry->findComponent<ParticleSystemComponent>(entity)){
                    collectPoints(points);
                }
                if (const LineRendererComponent* lines = registry->findComponent<LineRendererComponent>(entity)){
                    insertKeys(ShaderType::LINES, ShaderPool::getLinesProperties(false, true), shaderId(lines->customShader, ShaderType::LINES));
                }
                if (const SkyboxComponent* sky = registry->findComponent<SkyboxComponent>(entity)){
                    insertKeys(ShaderType::SKYBOX, 0, shaderId(sky->customShader, ShaderType::SKYBOX));
                }
            }
        }

    private:
        Scene* scene;
        const std::vector<MeshShaderEnvironment>& environments;
        std::set<ShaderKey>& keys;

        // The effective custom shader: the component's, the scene default, or built-in
        uint16_t shaderId(const std::string& customShader, ShaderType type){
            return ShaderPool::registerCustomShader(customShader.empty() ? scene->getDefaultCustomShader(type) : customShader);
        }

        // A custom shader that fails to build falls back to the built-in variant at runtime
        void insertKeys(ShaderType type, uint32_t properties, uint16_t customId){
            keys.insert(ShaderPool::getShaderKey(type, properties, customId));
            if (customId != 0){
                keys.insert(ShaderPool::getShaderKey(type, properties, 0));
            }
        }

        void collectSubmesh(const MeshComponent& mesh, Submesh submesh, const std::vector<const Buffer*>* buffers,
                            const TerrainComponent* terrain, const InstancedMeshComponent* instmesh, bool mirror){
            // MeshSystem::update turns the texture rect path on before the mesh loads
            if (MeshSystem::usesTextureRect(submesh)){
                submesh.hasTextureRect = true;
            }
            if (buffers){
                RenderSystem::scanSubmeshAttributes(submesh, *buffers);
            }
            const SubmeshShaderInputs inputs = RenderSystem::getSubmeshShaderInputs(mesh, submesh, terrain, instmesh, mirror);

            const uint16_t meshShaderId = shaderId(mesh.customShader, ShaderType::MESH);
            const uint16_t depthShaderId = ShaderPool::registerCustomShader(mesh.customDepthShader);
            for (const MeshShaderEnvironment& environment : environments){
                const SubmeshShaderVariant variant = RenderSystem::getSubmeshShaderVariant(environment, inputs);
                insertKeys(ShaderType::MESH, variant.properties, meshShaderId);
                if (variant.depth){
                    insertKeys(ShaderType::DEPTH, variant.depthProperties, depthShaderId);
                }
                if (variant.gbuffer){
                    keys.insert(ShaderPool::getShaderKey(ShaderType::GBUFFER, variant.gbufferProperties));
                }
            }
        }

        void collectMesh(EntityRegistry* registry, Entity entity, const MeshComponent& mesh){
            const InstancedMeshComponent* instmesh = registry->findComponent<InstancedMeshComponent>(entity);
            const bool mirror = registry->findComponent<MirrorComponent>(entity) != nullptr;

            // Generated meshes: the layout their generator gives them, whatever is saved
            const TerrainComponent* terrain = registry->findComponent<TerrainComponent>(entity);
            const bool sprite = registry->findComponent<SpriteComponent>(entity) != nullptr;
            const bool tilemap = registry->findComponent<TilemapComponent>(entity) != nullptr;
            const bool polygon = registry->findComponent<MeshPolygonComponent>(entity) != nullptr;
            if (terrain || sprite || tilemap || polygon){
                // A terrain without a height map is never built, see MeshSystem::createOrUpdateTerrain
                if (terrain && terrain->heightMap.empty()){
                    return;
                }
                std::unique_ptr<MeshComponent> generated = std::make_unique<MeshComponent>(mesh);
                if (terrain){
                    MeshSystem::prepareTerrainMesh(*generated);
                }else if (sprite){
                    MeshSystem::prepareSpriteMesh(*generated);
                }else if (tilemap){
                    MeshSystem::prepareTilemapMesh(*generated);
                }else{
                    MeshSystem::prepareMeshPolygonMesh(*generated);
                }
                // The generator fills the buffer, so it counts before it has data
                const std::vector<const Buffer*> buffers{&generated->buffer};
                // The terrain's two submeshes (full and half resolution) share submesh 0's
                // material (RenderSystem::loadMesh), so they draw with the same variant
                const unsigned int submeshes = terrain ? 1 : generated->numSubmeshes;
                for (unsigned int s = 0; s < submeshes; s++){
                    collectSubmesh(*generated, generated->submeshes[s], &buffers, terrain, instmesh, mirror);
                }
                return;
            }

            // A mesh with no geometry is not drawn (RenderSystem::loadMesh)
            const std::vector<const Buffer*> buffers = meshBuffers(mesh);
            if (buffers.empty()){
                return;
            }
            for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
                collectSubmesh(mesh, mesh.submeshes[s], &buffers, nullptr, instmesh, mirror);
            }
        }

        // A model mesh: the file's submeshes, with the fields saved over them (overrideFields)
        // and the renderer settings of the mesh they load into
        void collectModelSubmeshes(EntityRegistry* registry, Entity entity, const MeshComponent& mesh, const std::vector<Submesh>& probed){
            const InstancedMeshComponent* instmesh = registry ? registry->findComponent<InstancedMeshComponent>(entity) : nullptr;
            const bool mirror = registry && registry->findComponent<MirrorComponent>(entity) != nullptr;
            for (size_t s = 0; s < probed.size(); s++){
                Submesh submesh = probed[s];
                if (s < mesh.numSubmeshes && mesh.submeshes[s].overrideFields != 0){
                    MeshSystem::applySubmeshOverride(mesh.submeshes[s], submesh);
                }
                collectSubmesh(mesh, submesh, nullptr, nullptr, instmesh, mirror);
            }
        }

        void collectModel(EntityRegistry* registry, Entity entity, const ModelComponent& model, std::unordered_set<Entity>& modelMeshes){
            const ModelProbe& probe = probeModel(model.filename, model.mergeStaticMeshes);
            const MeshComponent* rootMesh = registry->findComponent<MeshComponent>(entity);
            if (rootMesh){
                modelMeshes.insert(entity);
            }
            if (!probe.loaded){
                return; // the file cannot be read: nothing of it is drawn
            }

            // A mesh the load puts its own geometry in keeps that mesh's settings; a node mesh
            // the scene does not have yet is made with the default ones
            // (MeshComponent is large in a build with fixed capacities, kept off the stack)
            const std::unique_ptr<MeshComponent> defaultMesh = std::make_unique<MeshComponent>();
            if (rootMesh){
                collectModelSubmeshes(registry, entity, *rootMesh, probe.root);
            }
            std::set<int> mappedNodes;
            for (const auto& [nodeIndex, nodeEntity] : model.meshNodesMapping){
                const MeshComponent* nodeMesh = registry->isEntityCreated(nodeEntity) ? registry->findComponent<MeshComponent>(nodeEntity) : nullptr;
                if (!nodeMesh){
                    continue;
                }
                modelMeshes.insert(nodeEntity);
                auto probed = probe.nodes.find(nodeIndex);
                if (probed != probe.nodes.end()){
                    mappedNodes.insert(nodeIndex);
                    collectModelSubmeshes(registry, nodeEntity, *nodeMesh, probed->second);
                }else{
                    // The file loads this node into the GameObject's own mesh here
                    collectModelSubmeshes(registry, nodeEntity, *nodeMesh, probe.root);
                }
            }
            for (const auto& [nodeIndex, submeshes] : probe.nodes){
                if (!mappedNodes.count(nodeIndex)){
                    collectModelSubmeshes(nullptr, NULL_ENTITY, *defaultMesh, submeshes);
                }
            }
        }

        // Terrain foliage: instanced meshes MeshSystem makes from each layer's model, merged
        // into one mesh, fading out with distance (MeshSystem::updateFoliageLayer)
        void collectFoliage(const TerrainComponent& terrain){
            if (terrain.heightMap.empty()){
                return;
            }
            for (const TerrainFoliageLayer& layer : terrain.foliageLayers){
                if (layer.meshPath.empty()){
                    continue;
                }
                const ModelProbe& probe = probeModel(layer.meshPath, true);
                if (!probe.loaded){
                    continue;
                }
                const std::unique_ptr<MeshComponent> mesh = std::make_unique<MeshComponent>();
                mesh->castShadows = layer.castShadows;
                const std::unique_ptr<InstancedMeshComponent> instmesh = std::make_unique<InstancedMeshComponent>();
                instmesh->distanceFade = true;
                for (const Submesh& submesh : probe.root){
                    collectSubmesh(*mesh, submesh, nullptr, nullptr, instmesh.get(), false);
                }
            }
        }

        // The three UI variants (plain, texture, font atlas) are all a UI component can take:
        // its texture comes and goes with what the UI shows (a button's state, a script, a
        // font atlas), see RenderSystem::loadUI
        void collectUI(const UIComponent& ui){
            const uint16_t id = shaderId(ui.customShader, ShaderType::UI);
            insertKeys(ShaderType::UI, ShaderPool::getUIProperties(false, false, false, true), id);
            insertKeys(ShaderType::UI, ShaderPool::getUIProperties(true, false, false, true), id);
            insertKeys(ShaderType::UI, ShaderPool::getUIProperties(false, true, false, false), id);
        }

        // Likewise for points, with or without a texture and texture rects (RenderSystem::loadPoints)
        void collectPoints(const PointsComponent* points){
            const uint16_t id = shaderId(points ? points->customShader : std::string(), ShaderType::POINTS);
            insertKeys(ShaderType::POINTS, ShaderPool::getPointsProperties(false, false, true, false), id);
            insertKeys(ShaderType::POINTS, ShaderPool::getPointsProperties(true, false, true, false), id);
            insertKeys(ShaderType::POINTS, ShaderPool::getPointsProperties(true, false, true, true), id);
        }
    };

}

void editor::ShaderVariantCollector::collectScene(Scene* scene, const ShaderVariantContent& sceneContent,
                                                  const std::vector<ShaderVariantContent>& prefabContents,
                                                  std::set<ShaderKey>& keys){
    if (!scene || !sceneContent.registry){
        return;
    }

    const uint32_t sceneFacts = collectFacts(sceneContent);
    std::vector<uint32_t> prefabFacts;
    uint32_t allFacts = sceneFacts;
    for (const ShaderVariantContent& prefab : prefabContents){
        if (!prefab.registry){
            continue;
        }
        const uint32_t facts = collectFacts(prefab);
        allFacts |= facts;
        if (facts != 0){
            prefabFacts.push_back(facts);
        }
    }

    const std::vector<MeshShaderEnvironment> environments = reachableEnvironments(scene, sceneFacts, prefabFacts);

    Collector collector(scene, environments, keys);
    collector.collect(sceneContent);
    for (const ShaderVariantContent& prefab : prefabContents){
        if (prefab.registry){
            collector.collect(prefab);
        }
    }

    // Scene passes
    if (scene->isSSAOEnabled()){
        keys.insert(ShaderPool::getShaderKey(ShaderType::SSAO, 0));
        keys.insert(ShaderPool::getShaderKey(ShaderType::SSAO_BLUR, 0));
    }
    if (scene->isSSREnabled()){
        keys.insert(ShaderPool::getShaderKey(ShaderType::SSR, 0));
        keys.insert(ShaderPool::getShaderKey(ShaderType::SSR_BLUR, 0));
        keys.insert(ShaderPool::getShaderKey(ShaderType::COMPOSITE, 0));
    }
    // disabled passes ship too: the generated scene carries the whole chain
    for (const PostProcessPass& pass : scene->getPostProcessPasses()){
        const uint16_t id = ShaderPool::registerCustomShader(pass.shader);
        keys.insert(ShaderPool::getShaderKey(ShaderType::POSTPROCESS, 0, id));
        if (id != 0){
            keys.insert(ShaderPool::getShaderKey(ShaderType::POSTPROCESS, 0, 0));
        }
    }
    // fixed resolution upscales with it and a scene stack is presented with it
    keys.insert(ShaderPool::getShaderKey(ShaderType::BLIT, 0));
    // 2D lights with shadows render occluder segments through the shadow2d pass, once an
    // occluder exists (a script can spawn one, so the light alone asks for it)
    if (allFacts & FACT_SHADOW_LIGHTS_2D){
        keys.insert(ShaderPool::getShaderKey(ShaderType::SHADOW2D, 0));
    }
}
