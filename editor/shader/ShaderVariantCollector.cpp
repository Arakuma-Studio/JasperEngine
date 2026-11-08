// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShaderVariantCollector.h"
#include "component/VolumetricCloudsComponent.h"

#include "Engine.h"
#include "System.h"
#include "pool/ShaderPool.h"
#include "io/FileData.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"
#include "component/FluidSimulationComponent.h"
#include "component/TextEffectComponent.h"
#include "component/TextComponent.h"
#include "component/FogComponent.h"
#include "component/InstancedMeshComponent.h"
#include "component/Light2DComponent.h"
#include "component/LightComponent.h"
#include "component/LineRendererComponent.h"
#include "component/MeshComponent.h"
#include "component/MeshPolygonComponent.h"
#include "component/MirrorComponent.h"
#include "component/ModelComponent.h"
#include "util/FbxImporter.h"
#include "component/ParticleSystemComponent.h"
#include "particles/ParticleCompiler.h"
#include "subsystem/ParticleSubsystem.h"
#include "component/PointsComponent.h"
#include "component/ReflectionProbeComponent.h"
#include "component/ShadowCaster2DComponent.h"
#include "component/CapsuleShadowCasterComponent.h"
#include "component/CameraComponent.h"
#include "render/LightingTiers.h"
#include "device/QualityResolver.h"
#include "component/SkyboxComponent.h"
#include "component/SpriteComponent.h"
#include "component/TerrainComponent.h"
#include "component/VoxelVolumeComponent.h"
#include "voxel/TransvoxelMesher.h"
#include "component/TilemapComponent.h"
#include "component/Transform.h"
#include "component/UIComponent.h"
#include "component/UIDocumentComponent.h"
#include "component/CanvasGroupComponent.h"
#include "component/VideoPlayerComponent.h"
#include "htmlui/compat/CompatApi.h"
#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/effects/EffectRegistry.h"
#include "lattice/LatticeRender.h"
#include "animation/VertexAnimation.h"
#include "component/VertexAnimatorComponent.h"
#include "pool/VertexAnimationPool.h"
#include "util/ProjectChecks.h"
#include "util/Util.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <unordered_set>

using namespace jasperengine;
namespace fs = std::filesystem;
namespace fx = ::jasperengine::htmlui;

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
        FACT_SCENE_DEPTH     = 1u << 8, // a shader requiring the scene depth (every mesh gets a depth program)
        FACT_HEIGHT_FOG      = 1u << 9, // a Fog of type Height (the sky composite)
        FACT_VOLUMETRIC_FOG  = 1u << 10, // its volumetric fog (every mesh gets a depth program)
        FACT_CLOUDS          = 1u << 11, // Volumetric Clouds (every mesh gets a depth program)
        // S57 (RenderSystem::updateLightingFeatures)
        FACT_LOCAL_LIGHTS    = 1u << 12, // a point or spot Light (Light Mode Tiled lists them per tile)
        FACT_CONTACT_LIGHT   = 1u << 13, // a shadowed directional or spot light with contact shadows
        FACT_CAPSULE_CASTERS = 1u << 14, // Capsule Shadow Casters
        FACT_OIT_CAMERA      = 1u << 15, // a camera drawing Order Independent
    };

    // whether a shader reference reads the scene depth with these features (manifest @require
    // scenedepth, shader library phase 3)
    bool readsSceneDepth(const std::string& shader, const std::vector<std::string>& features){
        if (shader.empty())
            return false;
        std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(shader);
        if (!manifest || !manifest->present)
            return false;
        return (manifest->resolveRequire(ShaderReference::getEnabledFeatures(shader, features)) & ShaderRequire_SceneDepth) != 0;
    }

    // RenderSystem::loadPostProcess: a pass of the chain (enabled or not: a script may turn it
    // on) whose fork declares u_depthTexture
    bool postProcessReadsDepth(Scene* scene){
        for (const PostProcessPass& pass : scene->getPostProcessPasses()){
            if (pass.shader.empty())
                continue;
            std::string source;
            if (ShaderManifests::readForkFile(editor::Util::resolveCustomShaderPaths(pass.shader).frag, source) &&
                    source.find("u_depthTexture") != std::string::npos)
                return true;
        }
        return false;
    }

    uint32_t collectFacts(const editor::ShaderVariantContent& content, Scene* scene){
        EntityRegistry* registry = content.registry;
        uint32_t facts = 0;
        // what draws without a shader of its own draws the scene's default (a prefab spawned
        // in this scene too), with its own features: RenderSystem resolves them so
        const std::string& sceneMeshShader = scene->getDefaultCustomShader(ShaderType::MESH);
        const std::string& scenePointsShader = scene->getDefaultCustomShader(ShaderType::POINTS);
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
                if (light->type != LightType::DIRECTIONAL){
                    facts |= FACT_LOCAL_LIGHTS;
                }
                if (light->shadows && light->contactShadows && light->type != LightType::POINT){
                    facts |= FACT_CONTACT_LIGHT;
                }
            }
            if (registry->findComponent<CapsuleShadowCasterComponent>(entity)){
                facts |= FACT_CAPSULE_CASTERS;
            }
            if (const CameraComponent* camera = registry->findComponent<CameraComponent>(entity)){
                if (camera->transparencyMode == TransparencyMode::ORDER_INDEPENDENT)
                    facts |= FACT_OIT_CAMERA;
            }
            if (const FogComponent* fog = registry->findComponent<FogComponent>(entity)){
                facts |= FACT_FOG;
                if (fog->type == FogType::HEIGHT){
                    facts |= FACT_HEIGHT_FOG;
                    if (fog->volumetric)
                        facts |= FACT_VOLUMETRIC_FOG;
                }
            }
            if (registry->findComponent<VolumetricCloudsComponent>(entity)){
                facts |= FACT_CLOUDS;
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
            // RenderSystem::update: a loaded mesh or points component reading the scene depth
            if (const MeshComponent* mesh = registry->findComponent<MeshComponent>(entity)){
                for (unsigned int s = 0; s < mesh->numSubmeshes && !(facts & FACT_SCENE_DEPTH); s++){
                    const Material& material = mesh->submeshes[s].material;
                    const std::string& shader = RenderSystem::getSubmeshShader(*mesh, mesh->submeshes[s], sceneMeshShader);
                    if (readsSceneDepth(shader, material.shaderFeatures) ||
                            readsSceneDepth(material.extraPass->shader, material.extraPass->shaderFeatures)){
                        facts |= FACT_SCENE_DEPTH;
                    }
                }
            }
            if (const PointsComponent* points = registry->findComponent<PointsComponent>(entity)){
                if (readsSceneDepth(points->customShader.empty() ? scenePointsShader : points->customShader.path, points->shaderFeatures)){
                    facts |= FACT_SCENE_DEPTH;
                }
            }
            // a particle system's soft particles (library:particles SOFT)
            if (const ParticleSystemComponent* particles = registry->findComponent<ParticleSystemComponent>(entity)){
                if (const particles::ParticleEffectDocument* effect = ParticleSubsystem::getDocument(*particles)){
                    if (readsSceneDepth(effect->renderer.shader, effect->renderer.shaderFeatures)){
                        facts |= FACT_SCENE_DEPTH;
                    }
                }
            }
        }
        return facts;
    }

    MeshShaderEnvironment toEnvironment(uint32_t facts, Scene* scene){
        MeshShaderEnvironment environment;
        // RenderSystem::checkLightsAndShadow
        const LightState lightState = scene->getLightState();
        environment.lights = lightState == LightState::ON || (lightState == LightState::AUTO && (facts & FACT_LIGHTS));
        // the device tier in effect while collecting (QualityResolver, as RenderSystem reads them)
        environment.lightCapacity = RenderSystem::getLightCapacity(QualityResolver::maxPixelLights(scene->getMaxLightsPerObject()));
        environment.shadows = (facts & FACT_SHADOW_LIGHTS) != 0;
        environment.ibl = (facts & FACT_SKY_IBL) != 0;
        environment.reflectionProbes = (facts & FACT_PROBES) != 0;
        environment.fog = (facts & FACT_FOG) != 0;
        // RenderSystem::update: the height fog variants, the atmosphere's depth programs
        environment.atmosphereDepth = (facts & (FACT_VOLUMETRIC_FOG | FACT_CLOUDS)) != 0 &&
                                      QualityResolver::volumetrics(AtmosphereQuality::HIGH) != AtmosphereQuality::OFF;
        environment.lights2D = (facts & FACT_LIGHTS_2D) != 0;
        // RenderSystem::loadLights2D: a shadow-casting 2D light with an occluder to cast
        environment.shadows2D = (facts & FACT_SHADOW_LIGHTS_2D) && (facts & FACT_OCCLUDERS_2D);
        environment.ssao = QualityResolver::ssao(scene->isSSAOEnabled());
        environment.ssr = scene->isSSREnabled();
        environment.depthPrepass = scene->isDepthPrepassEnabled();
        // a scene default shader reading the scene depth with its default features is drawn by
        // what a script makes without a shader of its own (the content's own objects resolve it
        // with their features: collectFacts); a post pass sampling the depth needs the depth
        // pre-pass too
        environment.sceneDepth = (facts & FACT_SCENE_DEPTH) ||
            readsSceneDepth(scene->getDefaultCustomShader(ShaderType::MESH), {}) ||
            readsSceneDepth(scene->getDefaultCustomShader(ShaderType::POINTS), {}) ||
            postProcessReadsDepth(scene);
        environment.litQualityHigh = QualityResolver::litQuality(scene->getLitQuality()) == LitQuality::HIGH;
        // S57: capsule shadows of a scene enabling them; Tiled and contact shadows follow the
        // device's tier as well (lightingStates adds their other state); OIT a camera asking
        environment.capsuleShadows = environment.lights && environment.shadows && scene->isCapsuleShadowsEnabled() &&
                                     (facts & FACT_CAPSULE_CASTERS);
        environment.contactShadows = environment.lights && environment.shadows && (facts & FACT_CONTACT_LIGHT);
        environment.tiledLights = environment.lights && (facts & FACT_LOCAL_LIGHTS) && LightingTiers::getLightMode() != LightMode::PER_OBJECT;
        environment.oit = (facts & FACT_OIT_CAMERA) != 0;
        return environment;
    }

    uint32_t environmentBits(const MeshShaderEnvironment& e){
        // T3 R34: the tier a build may force
        return (e.terrainForced ? (1u << (static_cast<int>(e.terrainForcedTier) + 26)) : 0u) | (e.lights ? 1u : 0u) | (e.shadows ? 2u : 0u) | (e.ibl ? 4u : 0u) | (e.reflectionProbes ? 8u : 0u) |
            (e.fog ? 16u : 0u) | (e.lights2D ? 32u : 0u) | (e.shadows2D ? 64u : 0u) | (e.ssao ? 128u : 0u) |
            (e.ssr ? 256u : 0u) | (e.depthPrepass ? 512u : 0u) | ((uint32_t)e.lightCapacity << 10) | (e.sceneDepth ? (1u << 20) : 0u) |
            (e.postColor ? (1u << 21) : 0u) | (e.shadowsHQ ? (1u << 22) : 0u) | (e.atmosphereDepth ? (1u << 23) : 0u) |
            (e.litQualityHigh ? (1u << 24) : 0u) | (e.pbrTier == PbrTier::REDUCED ? (1u << 25) : 0u) |
            (e.tiledLights ? (1u << 26) : 0u) | (e.capsuleShadows ? (1u << 27) : 0u) | (e.contactShadows ? (1u << 28) : 0u) |
            (e.oit ? (1u << 29) : 0u);
    }

    // the states of the High shadow filters' variant bit ('Shq', RenderSystem::updateShadowTier)
    // the scene's variants can be in: a tier the scene sets, both under Auto (the platform's
    // tier, which an export filters per backend: ShadowTiers::cooksHighFilters) and when a
    // script sets the quality
    std::vector<bool> shadowHQStates(Scene* scene, bool scriptsSetShadowQuality){
        // a device tier capping the shadows below High has no High filters
        if (!ShadowTiers::usesHighFilters(QualityResolver::shadowTier(ShadowTier::ULTRA)))
            return {false};
        if (scriptsSetShadowQuality)
            return {false, true};
        switch (scene->getShadowQuality()){
            case ShadowQuality::NONE:
            case ShadowQuality::LOW:
            case ShadowQuality::MEDIUM:
                return {false};
            case ShadowQuality::HIGH:
            case ShadowQuality::ULTRA:
                return {true};
            case ShadowQuality::AUTO:
                break;
        }
        return {false, true};
    }

    // the states of the post-processing colour bit (RenderSystem::getPostProcessingTiers) the
    // scene's variants can be in: its settings', and both when a script changes them
    std::vector<bool> postColorStates(Scene* scene, bool scriptsSetPostProcessing){
        const bool own = RenderSystem::getPostProcessingTiers(scene->getPostProcessing()).tailColor;
        if (scriptsSetPostProcessing)
            return {own, !own};
        return {own};
    }

    // The lighting setups a scene can be in: its own content, with any of the build's prefabs
    // spawned in it. A skybox's environment maps light meshes only once they are built, so
    // the setups without them are reachable too (an asynchronous load draws before that).
    std::vector<MeshShaderEnvironment> reachableEnvironments(Scene* scene, uint32_t sceneFacts, const std::vector<uint32_t>& prefabFacts,
                                                             bool allLightCapacities, const std::vector<bool>& postColor,
                                                             const std::vector<bool>& shadowHQ, bool allLitQualities,
                                                             PbrTier mobilePbrTier, uint32_t terrainTiers){
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
            for (bool postColorOn : postColor){
                for (bool highFilters : shadowHQ){
                    MeshShaderEnvironment environment = toEnvironment(facts, scene);
                    environment.postColor = postColorOn;
                    // the bit only exists on variants receiving shadows
                    environment.shadowsHQ = highFilters && environment.shadows;
                    if (distinct.insert(environmentBits(environment)).second){
                        environments.push_back(environment);
                    }
                    // a script setting maxLightsPerObject reaches the other capacity tiers
                    if (allLightCapacities && environment.lights){
                        for (int capacity : {4, 8, 16}){
                            environment.lightCapacity = capacity;
                            if (distinct.insert(environmentBits(environment)).second){
                                environments.push_back(environment);
                            }
                        }
                    }
                }
            }
        }
        // the other Lit Quality, when a script sets it (scene.litQuality), and the lobes the
        // mobile and web exports draw (MaterialPbr::resolveTier: the same rule as their runtime)
        const size_t own = environments.size();
        for (size_t e = 0; e < own; e++){
            if (allLitQualities){
                MeshShaderEnvironment other = environments[e];
                other.litQualityHigh = !other.litQualityHigh;
                if (distinct.insert(environmentBits(other)).second){
                    environments.push_back(other);
                }
            }
        }
        // S57: the device tier decides Light Mode Automatic (Per Object on Low) and contact shadows
        // (off on Low) at run time, so both states of each ship
        {
            const size_t tiered = environments.size();
            for (size_t e = 0; e < tiered; e++){
                MeshShaderEnvironment other = environments[e];
                bool changed = false;
                if (other.tiledLights && LightingTiers::getLightMode() == LightMode::AUTOMATIC){
                    other.tiledLights = false;
                    changed = true;
                }
                if (other.contactShadows){
                    other.contactShadows = false;
                    changed = true;
                }
                if (changed && distinct.insert(environmentBits(other)).second){
                    environments.push_back(other);
                }
            }
        }
        if (mobilePbrTier != PbrTier::FULL){
            const size_t full = environments.size();
            for (size_t e = 0; e < full; e++){
                MeshShaderEnvironment mobile = environments[e];
                mobile.pbrTier = MaterialPbr::resolveTier(true, mobilePbrTier);
                if (distinct.insert(environmentBits(mobile)).second){
                    environments.push_back(mobile);
                }
            }
        }
        // T3 R34: every tier the build ships. Only a terrain's keys differ by it, and the set of keys
        // takes each other key once.
        std::vector<MeshShaderEnvironment> tiered;
        for (const MeshShaderEnvironment& environment : environments){
            for (int tier = 0; tier < terrain::TERRAIN_QUALITY_COUNT; tier++){
                if (terrainTiers & (1u << tier)){
                    MeshShaderEnvironment withTier = environment;
                    withTier.terrainForced = true;
                    withTier.terrainForcedTier = static_cast<terrain::TerrainQuality>(tier);
                    tiered.push_back(withTier);
                }
            }
        }
        return tiered;
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
        bool vatMesh = false;   // a Vertex Animation mesh (.vat.glb): MeshComponent::vatMesh
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
        // The file as the engine opens it, for its modification time: an FBX's import, which
        // also changes when only its textures do
        const std::filesystem::path path(editor::FbxImporter::isFbxFile(filename) ? MeshSystem::getImportedModelPath(filename)
            : FileData::getSystemPath(filename));
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
            probe.vatMesh = scene.getComponent<MeshComponent>(entity).vatMesh;
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

    // A mesh a Vertex Animator plays: the Vertex Animation variants it draws with besides the
    // ones at rest (Vertices, Bones, or both when its asset cannot be read here), and whether it
    // is a .vat.glb (no skin: at rest it draws without one)
    struct VatTarget {
        bool vertices = false;
        bool bones = false;
        bool vatMesh = false;
    };

    VatTarget vatTargetOf(EntityRegistry* registry, Entity entity, bool vatMesh){
        VatTarget target;
        target.vatMesh = vatMesh;
        if (!registry || !registry->isComponentRegistered<VertexAnimatorComponent>()){
            return target;
        }
        const VertexAnimatorComponent* animator = registry->findComponent<VertexAnimatorComponent>(entity);
        if (!animator){
            return target;
        }
        std::shared_ptr<const vat::VertexAnimation> asset = animator->asset;
        if (!asset && !animator->animation.empty()){
            asset = VertexAnimationPool::getOrLoad(animator->animation);
        }
        if (asset){
            target.bones = asset->getData().mode == vat::VatMode::Bones;
            target.vertices = !target.bones;
        }else{
            target.vertices = true;
            target.bones = true;
        }
        return target;
    }

    class Collector {
    public:
        Collector(Scene* scene, const std::vector<MeshShaderEnvironment>& environments, const std::vector<bool>& postColor, std::set<ShaderKey>& keys):
            scene(scene), environments(environments), postColor(postColor), keys(keys) {}

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
                        // a FluidSimulation shown on it (MATERIAL) lends submesh 0 a base colour
                        // texture at draw time (FluidSystem::getMaterialTexture)
                        const FluidSimulationComponent* fluid = registry->isComponentRegistered<FluidSimulationComponent>()
                                                                ? registry->findComponent<FluidSimulationComponent>(entity) : nullptr;
                        lentBaseColor = fluid && fluid->output == FluidOutput::MATERIAL;
                        collectMesh(registry, entity, *mesh);
                        lentBaseColor = false;
                    }
                }
                if (const TerrainComponent* terrain = registry->findComponent<TerrainComponent>(entity)){
                    collectFoliage(*terrain);
                }
                if (const VoxelVolumeComponent* voxels = registry->findComponent<VoxelVolumeComponent>(entity)){
                    collectVoxelVolume(*voxels);
                }
                if (const UIComponent* ui = registry->findComponent<UIComponent>(entity)){
                    collectUI(*ui, registry->findComponent<TextComponent>(entity),
                              registry->isComponentRegistered<TextEffectComponent>() && registry->findComponent<TextEffectComponent>(entity));
                }
                // an HTML UI document draws geometry, gradients and its layer/filter passes
                // (RmlRenderer) (asked first: an editor built without the document component's
                // registration must not throw while it saves)
                if (registry->isComponentRegistered<UIDocumentComponent>() && registry->findComponent<UIDocumentComponent>(entity)){
                    for (const auto& variant : ShaderPool::getHtmlUIVariants())
                        insertKeys(variant.first, variant.second, 0);
                }
                // a Video Player's frame conversion and camera-plane blit (VideoTextureConverter)
                if (registry->isComponentRegistered<VideoPlayerComponent>() && registry->findComponent<VideoPlayerComponent>(entity)){
                    for (const auto& variant : ShaderPool::getVideoPlayerVariants())
                        insertKeys(variant.first, variant.second, 0);
                }
                if (const PointsComponent* points = registry->findComponent<PointsComponent>(entity)){
                    collectPoints(points);
                }
                if (const ParticleSystemComponent* particles = registry->findComponent<ParticleSystemComponent>(entity)){
                    collectParticles(*particles);
                }
                // the hairline, or the ribbon variant a width loads (pixel widths, a base texture)
                if (const LineRendererComponent* lines = registry->findComponent<LineRendererComponent>(entity)){
                    insertKeys(ShaderType::LINES, RenderSystem::getLinesVariantProperties(scene, *lines), shaderId(lines->customShader, ShaderType::LINES, lines->shaderFeatures, lines->blendMode));
                }
                if (const TrailRendererComponent* trail = registry->findComponent<TrailRendererComponent>(entity)){
                    insertKeys(ShaderType::LINES, RenderSystem::getTrailVariantProperties(scene, *trail), shaderId(trail->customShader, ShaderType::LINES, trail->shaderFeatures, trail->blendMode));
                }
                if (const SkyboxComponent* sky = registry->findComponent<SkyboxComponent>(entity)){
                    insertKeys(ShaderType::SKYBOX, 0, shaderId(sky->customShader, ShaderType::SKYBOX, sky->shaderFeatures, MaterialBlendMode::AUTO));
                }
            }
        }

    private:
        Scene* scene;
        bool lentBaseColor = false;     // the mesh being collected shows a fluid's output
        // the mesh being collected is a 3D mesh that may get detail levels (not a terrain,
        // sprite, tilemap or polygon: RenderSystem::updateMeshLods)
        bool meshHasLevels = false;
        const std::vector<MeshShaderEnvironment>& environments;
        // the post colour bit's states, for the points, lines and sky (meshes: environments)
        const std::vector<bool>& postColor;
        std::set<ShaderKey>& keys;

        // The effective custom shader: the component's, the scene default, or built-in, with
        // the features the component enables and the define of its blend mode
        // (RenderSystem::getComponentShaderReference)
        uint16_t shaderId(const std::string& customShader, ShaderType type, const std::vector<std::string>& features, MaterialBlendMode blendMode){
            return ShaderPool::registerCustomShader(RenderSystem::getComponentShaderReference(
                customShader.empty() ? scene->getDefaultCustomShader(type) : customShader, features, blendMode));
        }

        // A custom shader that fails to build falls back to the built-in variant at runtime
        void insertKeys(ShaderType type, uint64_t properties, uint16_t customId){
            // a mesh's post colour bit comes with its environment; the others take it here
            const uint64_t postColorBit = type == ShaderType::MESH ? 0 : ShaderPool::getPostColorProperty(type);
            for (bool postColorOn : postColor){
                const uint64_t variant = properties | (postColorOn ? postColorBit : uint64_t(0));
                keys.insert(ShaderPool::getShaderKey(type, variant, customId));
                if (customId != 0){
                    keys.insert(ShaderPool::getShaderKey(type, variant, 0));
                }
                if (!postColorBit)
                    break;
            }
        }

        // lattice: the mesh is bound to a lattice (LatticeRender::isLatticeTarget), so it also
        // draws with the lattice variants (the bindings can be switched off at runtime: both).
        // vat: a Vertex Animator plays it, so it also draws with those variants (and at rest
        // until its animation is on the GPU, or when the animator is off: both)
        void collectSubmesh(const MeshComponent& mesh, Submesh submesh, const std::vector<const Buffer*>* buffers,
                            const TerrainComponent* terrain, const InstancedMeshComponent* instmesh, bool mirror,
                            bool lattice = false, const VatTarget& vat = VatTarget()){
            // MeshSystem::update turns the texture rect path on before the mesh loads
            if (MeshSystem::usesTextureRect(submesh)){
                submesh.hasTextureRect = true;
            }
            if (buffers){
                RenderSystem::scanSubmeshAttributes(submesh, *buffers);
            }
            // the submesh's shader as RenderSystem::loadMesh picks it: its material's, the
            // mesh's or the scene's, with its features; the depth pass's library program
            const std::string& sceneMeshShader = scene->getDefaultCustomShader(ShaderType::MESH);
            SubmeshShaderInputs inputs = RenderSystem::getSubmeshShaderInputs(mesh, submesh, terrain, instmesh, mirror, sceneMeshShader);
            // T3 R34, T6 R40: a terrain with an attached Automatic volume ships its holed variant, so a
            // first in-game dig finds it in the build
            if (terrain && !inputs.terrainHoles){
                auto volumes = scene->getComponentArray<VoxelVolumeComponent>();
                for (size_t i = 0; i < volumes->size() && !inputs.terrainHoles; i++){
                    const VoxelVolumeComponent& volume = volumes->getComponentFromIndex(i);
                    inputs.terrainHoles = volume.terrainCut == VoxelTerrainCut::Automatic && volume.terrain != NULL_ENTITY
                        && scene->findComponent<TerrainComponent>(volume.terrain) == terrain;
                }
            }

            const std::string depthShaderSrc = RenderSystem::getSubmeshDepthShaderReference(mesh, submesh, sceneMeshShader);
            const uint16_t depthShaderId = ShaderPool::registerCustomShader(depthShaderSrc);
            const bool hasPass = RenderSystem::hasExtraPass(submesh, terrain != nullptr);
            const uint16_t passShaderId = hasPass ? ShaderPool::registerCustomShader(RenderSystem::getSubmeshExtraPassReference(submesh)) : 0;
            // (a terrain is never deformed)
            const bool latticeVariants = lattice && !terrain;
            // (a terrain never plays one)
            std::vector<VatVariant> vatVariants{VatVariant::NONE};
            if (vat.vertices && !terrain){
                vatVariants.push_back(VatVariant::VERTICES);
            }
            if (vat.bones && !terrain){
                vatVariants.push_back(VatVariant::BONES);
            }
            // the geometry as RenderSystem::getSubmeshShaderInputs gives it, whatever the
            // variant the mesh draws with now
            const bool restSkinning = submesh.hasSkinning && !mesh.vatMesh && !vat.vatMesh;
            const bool restMorph = !mesh.vatMesh && !vat.vatMesh;
            for (VatVariant vatVariant : vatVariants)
            for (int latticeVariant = 0; latticeVariant <= (latticeVariants ? 1 : 0); latticeVariant++){
                inputs.lattice = latticeVariant != 0;
                inputs.vat = vatVariant;
                const bool rest = vatVariant == VatVariant::NONE;
                inputs.skinning = rest && restSkinning;
                inputs.morphTarget = rest && restMorph && submesh.hasMorphTarget;
                inputs.morphNormal = rest && restMorph && submesh.hasMorphNormal;
                inputs.morphTangent = rest && restMorph && submesh.hasMorphTangent;
                for (const MeshShaderEnvironment& environment : environments){
                    const SubmeshShaderVariant variant = RenderSystem::getSubmeshShaderVariant(environment, inputs);
                    // the reference takes the variant's built-in Lit defines (lobes, Lit Quality)
                    const std::string meshShader = RenderSystem::getSubmeshShaderReference(mesh, submesh, sceneMeshShader, variant.litDefines);
                    const uint16_t meshShaderId = ShaderPool::registerCustomShader(meshShader);
                    insertKeys(ShaderType::MESH, variant.properties, meshShaderId);
                    // its LOD cross-fade variant (spec S54 R10): only for a mesh that can fade,
                    // so with Cross Fade or Mesh LOD off a build ships none
                    if (meshMayFade(mesh, submesh, meshShader, depthShaderId))
                        insertKeys(ShaderType::MESH, variant.properties | ShaderPool::getLodFadeProperty(), meshShaderId);
                    // the OIT render of an alpha-blended submesh (RenderSystem::loadSubmeshOit): its
                    // material's blend decides at load, so any that may blend
                    if (environment.oit && !hasPass && !terrain &&
                            (submesh.material.blendMode == MaterialBlendMode::ALPHA || submesh.material.blendMode == MaterialBlendMode::AUTO)){
                        insertKeys(ShaderType::MESH, variant.properties | ShaderPool::getLightingProperty(ShaderPool::LightingFeature::OIT), meshShaderId);
                    }
                    if (variant.depth){
                        insertKeys(ShaderType::DEPTH, variant.depthProperties, depthShaderId);
                    }
                    // its GPU Instancing variants (with the project's setting, RenderSystem::loadMesh)
                    const SubmeshBatchVariant batch = RenderSystem::getSubmeshBatchVariant(mesh, submesh, inputs, variant, meshShader, depthShaderSrc);
                    if (batch.properties){
                        insertKeys(ShaderType::MESH, batch.properties, meshShaderId);
                    }
                    if (batch.depthProperties){
                        insertKeys(ShaderType::DEPTH, batch.depthProperties, depthShaderId);
                    }
                    if (variant.gbuffer){
                        keys.insert(ShaderPool::getShaderKey(ShaderType::GBUFFER, variant.gbufferProperties));
                    }
                }

                // its material's extra pass, as RenderSystem::loadSubmeshExtraPass picks it (a pass
                // that fails is not drawn: no built-in fallback)
                if (hasPass){
                    const SubmeshShaderInputs passInputs = RenderSystem::getExtraPassShaderInputs(inputs, submesh.material.extraPass);
                    for (const MeshShaderEnvironment& environment : environments){
                        keys.insert(ShaderPool::getShaderKey(ShaderType::MESH, RenderSystem::getSubmeshShaderVariant(environment, passInputs).properties, passShaderId));
                    }
                }
            }
        }

        // T6 R49: a volume's chunks are never in a saved scene, so a build without a runtime compiler
        // reaches their variants only from here. A chunk as VoxelSystem::writeChunk makes it, in each
        // tier the volume can take: its Quality's, or for Auto whichever the target and an attached
        // terrain choose (R51), which are not known here.
        void collectVoxelVolume(const VoxelVolumeComponent& volume){
            auto chunk = std::make_unique<MeshComponent>();
            chunk->numSubmeshes = 1;
            chunk->castShadows = volume.castShadows;
            chunk->submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
            chunk->buffer.Buffer::addAttribute(AttributeType::POSITION, AttributeDataType::FLOAT, 3, offsetof(voxel::MeshVertex, position));
            chunk->buffer.Buffer::addAttribute(AttributeType::NORMAL, AttributeDataType::FLOAT, 3, offsetof(voxel::MeshVertex, normal));
            chunk->buffer.Buffer::addAttribute(AttributeType::VOXELSLOTS, AttributeDataType::UNSIGNED_SHORT, 2, offsetof(voxel::MeshVertex, slots));
            chunk->buffer.Buffer::addAttribute(AttributeType::VOXELWEIGHTS, AttributeDataType::UNSIGNED_BYTE, 4, offsetof(voxel::MeshVertex, weights));
            const std::vector<const Buffer*> buffers{&chunk->buffer};
            const VoxelQuality all[] = {VoxelQuality::High, VoxelQuality::Medium, VoxelQuality::Low};
            for (VoxelQuality quality : all){
                if (volume.quality != VoxelQuality::Auto && volume.quality != quality){
                    continue;
                }
                // (MeshComponent::voxelTier: High 1, Medium 2, Low 3, the order of the quality enum)
                chunk->voxelTier = static_cast<uint8_t>(quality);
                collectSubmesh(*chunk, chunk->submeshes[0], &buffers, nullptr, nullptr, false);
            }
        }

        void collectMesh(EntityRegistry* registry, Entity entity, const MeshComponent& mesh){
            const InstancedMeshComponent* instmesh = registry->findComponent<InstancedMeshComponent>(entity);
            const bool mirror = registry->findComponent<MirrorComponent>(entity) != nullptr;
            const bool lattice = LatticeRender::isLatticeTarget(registry, entity);
            const VatTarget vat = vatTargetOf(registry, entity, mesh.vatMesh);

            // Generated meshes: the layout their generator gives them, whatever is saved
            const TerrainComponent* terrain = registry->findComponent<TerrainComponent>(entity);
            const bool sprite = registry->findComponent<SpriteComponent>(entity) != nullptr;
            const bool tilemap = registry->findComponent<TilemapComponent>(entity) != nullptr;
            const bool polygon = registry->findComponent<MeshPolygonComponent>(entity) != nullptr;
            meshHasLevels = !(terrain || sprite || tilemap || polygon);
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
                    collectSubmesh(*generated, generated->submeshes[s], &buffers, terrain, instmesh, mirror, lattice, vat);
                    if (s == 0 && lentBaseColor)
                        collectSubmesh(*generated, lent(generated->submeshes[s]), &buffers, terrain, instmesh, mirror, lattice, vat);
                }
                return;
            }

            // A mesh with no geometry is not drawn (RenderSystem::loadMesh)
            const std::vector<const Buffer*> buffers = meshBuffers(mesh);
            if (buffers.empty()){
                return;
            }
            for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
                collectSubmesh(mesh, mesh.submeshes[s], &buffers, nullptr, instmesh, mirror, lattice, vat);
                if (s == 0 && lentBaseColor)
                    collectSubmesh(mesh, lent(mesh.submeshes[s]), &buffers, nullptr, instmesh, mirror, lattice, vat);
            }
        }

        // RenderSystem's rules (updateMeshLods, loadMesh's lodFadeCapable): levels and a fade
        // for a triangle mesh of the built-in shaders, with the scene's and its own settings on
        bool meshMayFade(const MeshComponent& mesh, const Submesh& submesh, const std::string& meshShader, uint16_t depthShaderId) const{
            return meshHasLevels && scene->isMeshLodEnabled() && scene->isMeshLodCrossFade() && mesh.lodEnabled &&
                   mesh.lodFadeMode == LodFadeMode::CROSS_FADE && submesh.primitiveType == PrimitiveType::TRIANGLES &&
                   (meshShader.empty() || ShaderReference::isBuiltin(meshShader)) && depthShaderId == 0;
        }

        // the submesh as it draws with a texture lent to its base colour
        static Submesh lent(Submesh submesh){
            static Framebuffer lentOutput;   // never made: it only makes the texture non-empty
            if (submesh.material.baseColorTexture.empty())
                submesh.material.baseColorTexture.setFramebuffer(&lentOutput);
            return submesh;
        }

        // A model mesh: the file's submeshes, with the fields saved over them (overrideFields)
        // and the renderer settings of the mesh they load into
        void collectModelSubmeshes(EntityRegistry* registry, Entity entity, const MeshComponent& mesh, const std::vector<Submesh>& probed,
                                   bool vatMesh = false){
            meshHasLevels = true;
            const InstancedMeshComponent* instmesh = registry ? registry->findComponent<InstancedMeshComponent>(entity) : nullptr;
            const bool mirror = registry && registry->findComponent<MirrorComponent>(entity) != nullptr;
            const bool lattice = registry && LatticeRender::isLatticeTarget(registry, entity);
            const VatTarget vat = vatTargetOf(registry, entity, vatMesh);
            for (size_t s = 0; s < probed.size(); s++){
                Submesh submesh = probed[s];
                if (s < mesh.numSubmeshes && mesh.submeshes[s].overrideFields != 0){
                    MeshSystem::applySubmeshOverride(mesh.submeshes[s], submesh);
                }
                collectSubmesh(mesh, submesh, nullptr, nullptr, instmesh, mirror, lattice, vat);
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
                collectModelSubmeshes(registry, entity, *rootMesh, probe.root, probe.vatMesh);
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
                    collectModelSubmeshes(registry, nodeEntity, *nodeMesh, probed->second, probe.vatMesh);
                }else{
                    // The file loads this node into the GameObject's own mesh here
                    collectModelSubmeshes(registry, nodeEntity, *nodeMesh, probe.root, probe.vatMesh);
                }
            }
            for (const auto& [nodeIndex, submeshes] : probe.nodes){
                if (!mappedNodes.count(nodeIndex)){
                    collectModelSubmeshes(nullptr, NULL_ENTITY, *defaultMesh, submeshes, probe.vatMesh);
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
        // A Text with a Text Effect also draws its atlas with a vertex colour, one with an outline or a
        // shadow from a distance field (ui-scaling-text; scripts turning them on later rely on the
        // runtime shader compiler)
        void collectUI(const UIComponent& ui, const TextComponent* text = nullptr, bool textEffect = false){
            const uint16_t id = shaderId(ui.customShader, ShaderType::UI, ui.shaderFeatures, ui.blendMode);
            insertKeys(ShaderType::UI, ShaderPool::getUIProperties(false, false, false, true), id);
            insertKeys(ShaderType::UI, ShaderPool::getUIProperties(true, false, false, true), id);
            insertKeys(ShaderType::UI, ShaderPool::getUIProperties(false, true, false, false), id);
            if (text){
                const bool sdf = (std::isfinite(text->outlineWidth) && text->outlineWidth > 0.0f) || text->shadowEnabled;
                if (textEffect)
                    insertKeys(ShaderType::UI, ShaderPool::getUIProperties(false, true, false, true), id);
                if (sdf)
                    insertKeys(ShaderType::UI, ShaderPool::getUIProperties(false, true, false, textEffect, true), id);
            }
        }

        // what RenderSystem::loadParticles draws with: the effect's quads, meshes and trails,
        // with and without the scene fog
        void collectParticles(const ParticleSystemComponent& particles){
            const particles::ParticleEffectDocument* document = ParticleSubsystem::getDocument(particles);
            if (!document)
                return;
            const std::shared_ptr<particles::CompiledEffect> effect = particles::ParticleCompiler::compile(*document);
            if (!effect->valid)
                return;
            for (bool fog : {false, true}){
                const RenderSystem::ParticleVariantSet set = RenderSystem::getParticleVariants(*effect, fog);
                const uint16_t id = set.reference.empty() ? 0 : ShaderPool::registerCustomShader(set.reference);
                if (set.quads) insertKeys(ShaderType::PARTICLES, set.quadProperties, id);
                if (set.meshes) insertKeys(ShaderType::PARTICLES, set.meshProperties, id);
                if (set.trails) insertKeys(ShaderType::PARTICLES, set.trailProperties, id);
            }
        }

        // Likewise for points, with or without a texture and texture rects (RenderSystem::loadPoints)
        void collectPoints(const PointsComponent* points){
            const uint16_t id = shaderId(points ? points->customShader.path : std::string(), ShaderType::POINTS,
                                         points ? points->shaderFeatures : std::vector<std::string>(),
                                         points ? points->blendMode : MaterialBlendMode::AUTO);
            insertKeys(ShaderType::POINTS, ShaderPool::getPointsProperties(false, false, true, false), id);
            insertKeys(ShaderType::POINTS, ShaderPool::getPointsProperties(true, false, true, false), id);
            insertKeys(ShaderType::POINTS, ShaderPool::getPointsProperties(true, false, true, true), id);
        }
    };

}

// ---- UI effects (phase-5 spec 3.8)

namespace {

    bool collectorIsIdentChar(char c){
        return std::isalnum((unsigned char)c) || c == '-' || c == '_';
    }

    std::string collectorLowerAscii(std::string text){
        for (char& c : text)
            c = (char)std::tolower((unsigned char)c);
        return text;
    }

    // CSS and HTML comments blanked out (kept the same length), so what they mention is no use
    std::string withoutComments(const std::string& text){
        std::string out = text;
        auto blank = [&](const char* open, const char* close){
            size_t at = 0;
            while ((at = out.find(open, at)) != std::string::npos){
                size_t end = out.find(close, at + std::strlen(open));
                end = (end == std::string::npos) ? out.size() : end + std::strlen(close);
                for (size_t i = at; i < end; i++){
                    if (out[i] != '\n')
                        out[i] = ' ';
                }
                at = end;
            }
        };
        blank("/*", "*/");
        blank("<!--", "-->");
        return out;
    }

    // The property of the declaration a value at `at` belongs to: the name before the ':' that
    // follows the last ';', '{', '}' or quote ("" when there is none)
    std::string propertyAt(const std::string& text, size_t at){
        size_t start = at;
        while (start > 0){
            const char c = text[start - 1];
            if (c == ';' || c == '{' || c == '}' || c == '"' || c == '\'')
                break;
            start--;
        }
        const size_t colon = text.find(':', start);
        if (colon == std::string::npos || colon >= at)
            return std::string();
        size_t b = start, e = colon;
        while (b < e && std::isspace((unsigned char)text[b]))
            b++;
        while (e > b && std::isspace((unsigned char)text[e - 1]))
            e--;
        return collectorLowerAscii(text.substr(b, e - b));
    }

    // the effect form a property draws with: 'p' paint, 'f' filter, 0 either (custom properties, unknown)
    char formOfProperty(std::string property){
        for (const char* prefix : {"-webkit-", "-moz-"}){
            if (property.rfind(prefix, 0) == 0)
                property = property.substr(std::strlen(prefix));
        }
        if (property == "filter" || property == "backdrop-filter" || property == "jfx-text-fill")
            return 'f';
        if (property == "background" || property == "background-image" || property == "mask" || property == "mask-image" ||
            property == "decorator")
            return 'p';
        return 0;
    }

}

void editor::ShaderVariantCollector::scanUiEffects(const std::string& textIn, bool lua, UiEffectUses& uses){
    const std::string text = lua ? textIn : withoutComments(textIn);
    auto add = [&](const std::string& name, size_t at){
        if (name.empty())
            return;
        if (lua){
            uses.any.insert(name);
            return;
        }
        switch (formOfProperty(propertyAt(text, at))){
            case 'p': uses.paint.insert(name); break;
            case 'f': uses.filter.insert(name); break;
            default: uses.any.insert(name); break;
        }
    };
    // effect(<name> ...) as an author writes it (CSS, Lua, the runtime form), and the typed
    // RCSS function jfx-<name>(...) the compatibility layer writes for it
    for (const char* prefix : {"effect(", "jfx-"}){
        const bool typed = prefix[0] == 'j';
        const size_t length = std::strlen(prefix);
        size_t at = 0;
        while ((at = text.find(prefix, at)) != std::string::npos){
            const size_t found = at;
            at += length;
            if (found > 0 && collectorIsIdentChar(text[found - 1]))
                continue;
            size_t p = at;
            if (!typed){
                while (p < text.size() && std::isspace((unsigned char)text[p]))
                    p++;
            }
            const size_t nameStart = p;
            if (nameStart >= text.size() || !std::isalpha((unsigned char)text[nameStart]))
                continue;
            while (p < text.size() && (std::isalnum((unsigned char)text[p]) || text[p] == '-'))
                p++;
            std::string name = collectorLowerAscii(text.substr(nameStart, p - nameStart));
            if (typed && (p >= text.size() || text[p] != '('))
                continue;
            add(name, found);
        }
    }
}

void editor::ShaderVariantCollector::addUiEffectKeys(const UiEffectUses& uses, std::set<ShaderKey>& keys){
    fx::EffectRegistry& registry = fx::EffectRegistry::get();
    auto add = [&](const std::string& name, bool paintForm, bool filterForm){
        const fx::EffectManifest* m = registry.find(name);
        if (!m)
            return;
        // a filter effect has the filter form only
        if (m->kind == fx::EffectKind::Filter){
            paintForm = false;
            filterForm = true;
        }
        // phase 6: a stateful effect's step form (and scatter form) of each draw form it is used in
        auto addForm = [&](bool filter){
            const ShaderKey key = registry.shaderKey(*m, filter);
            keys.insert(ShaderPool::normalizeKey(key));
            if (!m->stateful())
                return;
            const ShaderType type = ShaderPool::getShaderTypeFromKey(key);
            const uint64_t properties = ShaderPool::getPropertiesFromKey(key);
            const uint16_t customId = ShaderPool::getCustomIdFromKey(key);
            keys.insert(ShaderPool::normalizeKey(ShaderPool::getShaderKey(type, properties | 2u, customId)));
            if (m->hasScatter())
                keys.insert(ShaderPool::normalizeKey(ShaderPool::getShaderKey(type, properties | 4u, customId)));
        };
        if (paintForm)
            addForm(false);
        if (filterForm)
            addForm(true);
        if (m->hasPrepass && !m->prepassThreshold.empty())
            keys.insert(ShaderPool::normalizeKey(registry.brightPassKey()));
    };
    for (const std::string& name : uses.paint)
        add(name, true, false);
    for (const std::string& name : uses.filter)
        add(name, false, true);
    for (const std::string& name : uses.any)
        add(name, true, true);
}

void editor::ShaderVariantCollector::collectUiEffects(const std::vector<std::string>& documents, const fs::path& projectRoot,
                                                      std::set<ShaderKey>& keys){
    if (documents.empty() || projectRoot.empty())
        return;
    const fx::compat::FileAccess files = ProjectChecks::projectFiles(projectRoot);
    UiEffectUses uses;
    std::set<std::string> scanned;   // files read as written
    auto scanFile = [&](const std::string& assetPath){
        if (!scanned.insert(assetPath).second)
            return;
        const std::string ext = collectorLowerAscii(fs::path(assetPath).extension().string());
        if (ext != ".html" && ext != ".htm" && ext != ".css" && ext != ".rml" && ext != ".rcss")
            return;
        std::string content;
        if (files.read && files.read(assetPath, content))
            scanUiEffects(content, false, uses);
    };
    for (const std::string& document : documents){
        if (fx::compat::isHtmlDocumentPath(document)){
            // as it loads: what the conversion writes (it also declares the @effect project
            // effects, so their names are known below)
            fx::compat::CompatOptions options;
            const std::shared_ptr<const fx::compat::CompatDocument> compiled =
                fx::compat::DocumentLoader::compile(document, options, files);
            if (compiled && compiled->compiled){
                for (const auto& sheet : compiled->stylesheets)
                    scanUiEffects(sheet.rcss, false, uses);
                scanUiEffects(compiled->hints.rcss, false, uses);
                scanUiEffects(compiled->important.rcss, false, uses);
                for (const auto& attribute : compiled->styleAttributes)
                    scanUiEffects(attribute.second, false, uses);
                for (const auto& script : compiled->scripts)
                    scanUiEffects(script.code, true, uses);
                for (const std::string& dependency : compiled->dependencies)
                    scanFile(dependency);
            }
        }
        scanFile(document);
    }
    // the project's Lua scripts (style.backgroundImage = "effect(...)")
    for (const fs::path& script : ProjectChecks::findProjectFiles(projectRoot, {".lua"})){
        std::string code;
        if (ProjectChecks::readFile(script, code) && code.find("effect") != std::string::npos)
            scanUiEffects(code, true, uses);
    }
    addUiEffectKeys(uses, keys);
}

const std::vector<std::string>& editor::ShaderVariantCollector::getScriptableEffectFields(){
    static const std::vector<std::string> fields = {
        "fxaaEnabled", "chromaticAberrationEnabled", "pixelateEnabled", "crtEnabled", "edgeDetectEnabled",
        "filmGrainEnabled", "sharpenEnabled", "bloomEnabled", "blurEnabled",
        "colorGradingEnabled", "monochromeEnabled", "vignetteEnabled", "ditherEnabled", "renderScale",
        "vhsEnabled", "halationEnabled", "filmDamageEnabled", "filmGrainType", "dynamicResolutionEnabled"
    };
    return fields;
}

std::set<uint32_t> editor::ShaderVariantCollector::plausibleUberFeatures(const PostProcessingSettings& settings, bool scriptsSetPostProcessing,
                                                                         const std::vector<std::string>& namedFields){
    std::set<uint32_t> features;
    std::function<void(const PostProcessingSettings&)> add = [&features](const PostProcessingSettings& candidate){
        const PostProcessingTiers tiers = RenderSystem::getPostProcessingTiers(candidate);
        if (tiers.uber && tiers.uberFeatures != 0)
            features.insert(tiers.uberFeatures);
    };
    add(settings);
    if (!scriptsSetPostProcessing)
        return features;

    // the switches: the ones the scripts name, else all of them
    std::vector<const PostProcessingSettings::Field*> switches;
    const std::vector<std::string>& candidates = namedFields.empty() ? getScriptableEffectFields() : namedFields;
    for (const std::string& name : candidates){
        const PostProcessingSettings::Field* field = PostProcessingSettings::findField(name);
        // the bool switches, and the render scale (below 1 runs the uber pass: its upscale); the
        // grain type is not a switch, every type is added below
        if (field && (field->type == PostProcessingSettings::FieldType::Bool || name == "renderScale"))
            switches.push_back(field);
    }
    const bool grainTypes = std::find(candidates.begin(), candidates.end(), "filmGrainType") != candidates.end();
    auto flipped = [&](uint32_t mask){
        PostProcessingSettings candidate = settings;
        for (size_t i = 0; i < switches.size(); i++){
            if (mask & (1u << i)){
                if (switches[i]->type == PostProcessingSettings::FieldType::Bool){
                    bool* value = static_cast<bool*>(switches[i]->ref(candidate));
                    *value = !*value;
                }else{
                    // the render scale: full against scaled
                    float* value = static_cast<float*>(switches[i]->ref(candidate));
                    *value = *value < 1.0f ? 1.0f : 0.5f;
                }
            }
        }
        // a switched-on effect at its neutral value would add nothing: its default instead
        static const PostProcessingSettings defaults;
        if (candidate.bloomEnabled && candidate.bloomIntensity <= 0.0f) candidate.bloomIntensity = defaults.bloomIntensity;
        if (candidate.chromaticAberrationEnabled && candidate.chromaticAberrationIntensity <= 0.0f)
            candidate.chromaticAberrationIntensity = defaults.chromaticAberrationIntensity;
        if (candidate.filmGrainEnabled && candidate.filmGrainIntensity <= 0.0f) candidate.filmGrainIntensity = defaults.filmGrainIntensity;
        if (candidate.vhsEnabled && candidate.vhsIntensity <= 0.0f) candidate.vhsIntensity = defaults.vhsIntensity;
        if (candidate.halationEnabled && candidate.halationIntensity <= 0.0f) candidate.halationIntensity = defaults.halationIntensity;
        return candidate;
    };
    // a script setting filmGrainType may pick any type: each candidate with each of them
    if (grainTypes){
        auto addAll = add;
        add = [addAll](const PostProcessingSettings& candidate){
            for (FilmGrainType type : {FilmGrainType::CLASSIC, FilmGrainType::LENS, FilmGrainType::FILMIC}){
                PostProcessingSettings typed = candidate;
                typed.filmGrainType = type;
                addAll(typed);
            }
        };
        add(settings);
    }
    if (!namedFields.empty() && switches.size() <= 4){
        // every combination of the few switches the scripts name
        for (uint32_t mask = 1; mask < (1u << switches.size()); mask++)
            add(flipped(mask));
    }else{
        for (size_t i = 0; i < switches.size(); i++)
            add(flipped(1u << i));
    }
    return features;
}

void editor::ShaderVariantCollector::addBuildRequiredKeys(std::set<ShaderKey>& keys){
    keys.insert(ShaderPool::getShaderKey(ShaderType::HTMLUI_POST, ShaderPool::getHtmlUIPostProperties(HtmlUIPostVariant::PASSTHROUGH)));
}

void editor::ShaderVariantCollector::addFluidKeys(std::set<ShaderKey>& keys){
    // ShaderType::FLUID bits: 'Lok' 1, 'Mfl' 2 (the solver and the look, each with and
    // without linear filtering: the device decides at run time)
    keys.insert(ShaderPool::getShaderKey(ShaderType::FLUID, 0));
    keys.insert(ShaderPool::getShaderKey(ShaderType::FLUID, 1u));
    keys.insert(ShaderPool::getShaderKey(ShaderType::FLUID, 2u));
    keys.insert(ShaderPool::getShaderKey(ShaderType::FLUID, 3u));
}

void editor::ShaderVariantCollector::collectScene(Scene* scene, const ShaderVariantContent& sceneContent,
                                                  const std::vector<ShaderVariantContent>& prefabContents,
                                                  std::set<ShaderKey>& keys, const fs::path& projectRoot,
                                                  bool allLightCapacities, bool scriptsSetPostProcessing,
                                                  const std::vector<std::string>& scriptPostProcessingFields,
                                                  bool scriptsSetShadowQuality, bool scriptsSetLitQuality,
                                                  PbrTier mobilePbrTier, uint32_t terrainTiers){
    if (!scene || !sceneContent.registry){
        return;
    }
    // Every Device Tier a player can apply, and the thermal steps that move between them (spec
    // S56): its render scale (the uber pass's upscale), post effects it turns off, Lit Quality,
    // shadow tier, light count and volumetrics change the variants the game draws with. The
    // project's settings decide which states exist; each distinct one is collected once.
    QualityResolver::pushState();
    std::set<std::string> resolved;
    auto collectUnderTier = [&](){
        const PostProcessingTiers tiers = RenderSystem::getPostProcessingTiers(scene->getPostProcessing());
        const std::string signature = std::to_string(tiers.uberFeatures) + (tiers.uber ? "u" : "") + (tiers.tailColor ? "t" : "") +
            (QualityResolver::ssao(true) ? "s" : "") + (QualityResolver::bloom(true) ? "b" : "") +
            std::to_string((int)QualityResolver::litQuality(LitQuality::HIGH)) +
            std::to_string((int)QualityResolver::shadowTier(ShadowTier::ULTRA)) +
            std::to_string(QualityResolver::maxPixelLights(scene->getMaxLightsPerObject())) +
            (QualityResolver::volumetrics(AtmosphereQuality::HIGH) == AtmosphereQuality::OFF ? "v" : "");
        if (!resolved.insert(signature).second)
            return;
        collectSceneUnderTier(scene, sceneContent, prefabContents, keys, projectRoot, scriptsSetPostProcessing, scriptPostProcessingFields,
                              allLightCapacities, scriptsSetShadowQuality, scriptsSetLitQuality, mobilePbrTier, terrainTiers);
    };
    QualityResolver::setTier(std::nullopt);
    QualityResolver::forceThermalResponse(ThermalState::Nominal);
    collectUnderTier();
    for (int g = 0; g < DEVICE_PLATFORM_GROUP_COUNT; g++){
        QualityResolver::setPlatformGroup((DevicePlatformGroup)g);
        const bool thermal = QualityResolver::getSettings().thermalResponse[g];
        for (int t = 0; t < DEVICE_TIER_COUNT; t++){
            QualityResolver::setTier((DeviceTier)t);
            for (ThermalState state : {ThermalState::Nominal, ThermalState::Serious, ThermalState::Critical}){
                if (state != ThermalState::Nominal && !thermal)
                    continue;
                QualityResolver::forceThermalResponse(state);
                collectUnderTier();
            }
        }
    }
    QualityResolver::popState();
}

void editor::ShaderVariantCollector::collectSceneUnderTier(Scene* scene, const ShaderVariantContent& sceneContent,
                                                           const std::vector<ShaderVariantContent>& prefabContents,
                                                           std::set<ShaderKey>& keys, const fs::path& projectRoot,
                                                           bool scriptsSetPostProcessing,
                                                           const std::vector<std::string>& scriptPostProcessingFields,
                                                           bool allLightCapacities, bool scriptsSetShadowQuality,
                                                           bool scriptsSetLitQuality, PbrTier mobilePbrTier, uint32_t terrainTiers){
    const std::vector<bool> postColor = postColorStates(scene, scriptsSetPostProcessing);

    const uint32_t sceneFacts = collectFacts(sceneContent, scene);
    std::vector<uint32_t> prefabFacts;
    uint32_t allFacts = sceneFacts;
    for (const ShaderVariantContent& prefab : prefabContents){
        if (!prefab.registry){
            continue;
        }
        const uint32_t facts = collectFacts(prefab, scene);
        allFacts |= facts;
        if (facts != 0){
            prefabFacts.push_back(facts);
        }
    }

    const std::vector<MeshShaderEnvironment> environments = reachableEnvironments(scene, sceneFacts, prefabFacts, allLightCapacities, postColor,
                                                                                  shadowHQStates(scene, scriptsSetShadowQuality),
                                                                                  scriptsSetLitQuality, mobilePbrTier, terrainTiers);

    Collector collector(scene, environments, postColor, keys);
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
    // S57's passes: the contact shadows' mask, the OIT resolve
    if (allFacts & FACT_CONTACT_LIGHT){
        keys.insert(ShaderPool::getShaderKey(ShaderType::LIGHTING_PASS, 0));
    }
    if (allFacts & FACT_OIT_CAMERA){
        keys.insert(ShaderPool::getShaderKey(ShaderType::LIGHTING_PASS, 1));
    }
    if (scene->isSSREnabled()){
        keys.insert(ShaderPool::getShaderKey(ShaderType::SSR, 0));
        keys.insert(ShaderPool::getShaderKey(ShaderType::SSR_BLUR, 0));
        keys.insert(ShaderPool::getShaderKey(ShaderType::COMPOSITE, 0));
    }
    // a reflection probe's runtime capture is prefiltered into roughness mips
    // (RenderSystem::filterReflectionProbes)
    if (allFacts & FACT_PROBES){
        keys.insert(ShaderPool::getShaderKey(ShaderType::PROBE_FILTER, 0));
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
    // the uber pass of the scene's post-processing (phase 4: a BLIT variant of its features),
    // and the ones its scripts plausibly switch to; the bloom and blur chain's programs with them
    bool bloom = false;
    bool blur = false;
    bool halation = false;
    for (uint32_t features : plausibleUberFeatures(scene->getPostProcessing(), scriptsSetPostProcessing, scriptPostProcessingFields)){
        keys.insert(ShaderPool::getShaderKey(ShaderType::BLIT, features));
        bloom = bloom || (features & UBER_BLOOM);
        blur = blur || (features & UBER_BLUR);
        // halation without bloom runs its own prefilter and down pass (S13)
        halation = halation || ((features & UBER_HALATION) && !(features & UBER_BLOOM));
    }
    // ShaderType::BLOOM bits: 'Pre' 1, 'Thr' 2, 'Ups' 4, 'Mix' 8 (RenderSystem::loadBloomProgram)
    if (bloom || blur || halation){
        keys.insert(ShaderPool::getShaderKey(ShaderType::BLOOM, 0));
    }
    if (halation){
        keys.insert(ShaderPool::getShaderKey(ShaderType::BLOOM, 1u | 2u));
    }
    if (bloom){
        keys.insert(ShaderPool::getShaderKey(ShaderType::BLOOM, 1u | 2u));
        keys.insert(ShaderPool::getShaderKey(ShaderType::BLOOM, 4u | 8u));
    }
    if (blur){
        keys.insert(ShaderPool::getShaderKey(ShaderType::BLOOM, 1u));
        keys.insert(ShaderPool::getShaderKey(ShaderType::BLOOM, 4u));
    }
    // a light casting 3D shadows copies the static shadow cache back and clears slots with the
    // shadow cache's program (RenderSystem::renderShadowMaps)
    if (allFacts & FACT_SHADOW_LIGHTS){
        keys.insert(ShaderPool::getShaderKey(ShaderType::SHADOW_CACHE, 0));
    }
    // the atmosphere's passes (height fog's sky fill, volumetric fog, clouds)
    for (ShaderType type : AtmosphereRenderer::passShaderTypes((allFacts & FACT_HEIGHT_FOG) != 0, (allFacts & FACT_VOLUMETRIC_FOG) != 0,
                                                               (allFacts & FACT_CLOUDS) != 0)){
        keys.insert(ShaderPool::getShaderKey(type, 0));
    }
    // 2D lights with shadows render occluder segments through the shadow2d pass, once an
    // occluder exists (a script can spawn one, so the light alone asks for it)
    if (allFacts & FACT_SHADOW_LIGHTS_2D){
        keys.insert(ShaderPool::getShaderKey(ShaderType::SHADOW2D, 0));
    }
    // the fluid simulations' programs (FluidSimulator, ShaderType::FLUID bits 'Lok' 1, 'Mfl' 2):
    // the solver, the look, and the solver of devices that cannot filter half floats
    auto hasFluid = [](const ShaderVariantContent& content){
        if (!content.registry || !content.registry->isComponentRegistered<FluidSimulationComponent>())
            return false;
        for (Entity entity : content.entities){
            if (content.registry->isEntityCreated(entity) && content.registry->findComponent<FluidSimulationComponent>(entity))
                return true;
        }
        return false;
    };
    bool fluid = hasFluid(sceneContent);
    for (const ShaderVariantContent& prefab : prefabContents)
        fluid = fluid || hasFluid(prefab);
    if (fluid)
        addFluidKeys(keys);

    // the UI effects the scene's (and its prefabs') HTML UI documents use
    if (!projectRoot.empty()){
        std::vector<std::string> documents;
        auto addDocuments = [&](const ShaderVariantContent& content){
            if (!content.registry || !content.registry->isComponentRegistered<UIDocumentComponent>())
                return;
            for (Entity entity : content.entities){
                if (!content.registry->isEntityCreated(entity))
                    continue;
                const UIDocumentComponent* document = content.registry->findComponent<UIDocumentComponent>(entity);
                if (document && !document->filename.empty() &&
                    std::find(documents.begin(), documents.end(), document->filename) == documents.end())
                    documents.push_back(document->filename);
            }
        };
        addDocuments(sceneContent);
        for (const ShaderVariantContent& prefab : prefabContents)
            addDocuments(prefab);
        collectUiEffects(documents, projectRoot, keys);
    }
}
