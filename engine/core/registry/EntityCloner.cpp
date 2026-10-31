// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "EntityCloner.h"
#include "EntityRegistry.h"
#include "Scene.h"
#include "subsystem/MeshSystem.h"
#include "physics/PhysicsMaterial.h"
#include "render/BufferRender.h"
#include "render/ObjectRender.h"
#include "render/CameraRender.h"
#include "render/TextureRender.h"
#include "texture/Framebuffer.h"
#include "fluid/FluidSimulator.h"
#include "subsystem/PhysicsSystem.h"
#include "ecs/EntityManager.h"
#include "Log.h"

#include <algorithm>
#include <unordered_set>

using namespace jasperengine;

namespace {

struct CloneContext {
    const std::unordered_map<Entity, Entity>& map;
    bool sameRegistry;
    Entity sourceRoot;
    // EntityCloner::move: the copies take over what the originals run (script instances,
    // sound voices, running actions, subscriptions, body velocities), see each prepare()
    bool move = false;
    // ...and every GameObject kept its id (state bound to entity ids stays valid)
    bool sameIds = false;
    EntityRegistry* source = nullptr;
    // a copied camera's framebuffer (a copied fluid's output) -> the copy's, for textures
    // that show it, with the entities they belong to (the link ids name them)
    struct FramebufferCopy {
        Framebuffer* framebuffer = nullptr;
        Entity source = NULL_ENTITY;
        Entity copy = NULL_ENTITY;
    };
    std::unordered_map<const Framebuffer*, FramebufferCopy> framebuffers;
    // a copied fluid with a simulator -> the copy's own (made first: its output is mapped)
    std::unordered_map<Entity, std::shared_ptr<FluidSimulator>> fluidSimulators;
    // meshes whose external buffers point into the file data of a model copied with them
    std::unordered_set<Entity> modelMeshes;

    // A copied GameObject becomes its copy; another one stays in the same registry, and
    // means nothing in another
    Entity remap(Entity entity) const {
        if (entity == NULL_ENTITY) {
            return NULL_ENTITY;
        }
        auto it = map.find(entity);
        if (it != map.end()) {
            return it->second;
        }
        return sameRegistry ? entity : NULL_ENTITY;
    }
};

void resetBuffer(Buffer& buffer) {
    *buffer.getRender() = BufferRender();
}

// A buffer showing bytes someone else owns (a component's vector, refreshed by its
// system) must not keep showing the original's
void detachExternalBuffer(ExternalBuffer& buffer) {
    resetBuffer(buffer);
    if (!buffer.ownsData()) {
        buffer.setData(nullptr, 0);
    }
}

// A texture showing a copied camera's or fluid's framebuffer shows the copy's. A link id
// naming the original ("camera|<e>", "fluid|<e>": the editor's, kept in scene files) names
// the copy (setFramebuffer clears the id)
void remapFramebufferTexture(Texture& texture, const CloneContext& ctx) {
    Framebuffer* framebuffer = texture.getFramebuffer();
    if (!framebuffer) {
        return;
    }
    auto it = ctx.framebuffers.find(framebuffer);
    if (it == ctx.framebuffers.end()) {
        return;
    }
    const std::string id = texture.getId();
    texture.setFramebuffer(it->second.framebuffer);
    const size_t bar = id.rfind('|');
    if (bar != std::string::npos && it->second.source != NULL_ENTITY && id.compare(bar + 1, std::string::npos, std::to_string(it->second.source)) == 0) {
        texture.setId(id.substr(0, bar + 1) + std::to_string(it->second.copy));
    }
}

void remapFramebufferTextures(std::vector<std::pair<std::string, Texture>>& textures, const CloneContext& ctx) {
    for (auto& entry : textures) {
        remapFramebufferTexture(entry.second, ctx);
    }
}

// every texture slot of a material: its PBR maps, its shader's textures and its extra pass's
void remapMaterialTextures(Material& material, const CloneContext& ctx) {
    for (Texture* texture : {&material.baseColorTexture, &material.emissiveTexture,
            &material.metallicRoughnessTexture, &material.occlusionTexture, &material.normalTexture}) {
        remapFramebufferTexture(*texture, ctx);
    }
    remapFramebufferTextures(material.shaderTextures, ctx);
    if (MaterialPass* pass = material.extraPass.editIfSet()) {
        remapFramebufferTextures(pass->shaderTextures, ctx);
    }
}

template<typename T>
void prepare(T& component, Entity sourceEntity, CloneContext& ctx) {
    (void)component;
    (void)sourceEntity;
    (void)ctx;
}

template<>
void prepare<Transform>(Transform& transform, Entity sourceEntity, CloneContext& ctx) {
    // the copy of the root starts at the top of the hierarchy
    transform.parent = sourceEntity == ctx.sourceRoot ? NULL_ENTITY : ctx.remap(transform.parent);
    transform.needUpdate = true;
    transform.needUpdateChildVisibility = true;
}

template<>
void prepare<MeshComponent>(MeshComponent& mesh, Entity sourceEntity, CloneContext& ctx) {
    mesh.loaded = false;
    mesh.loadCalled = false;
    mesh.loadFailed = false;
    mesh.needReload = false;
    resetBuffer(mesh.buffer);
    resetBuffer(mesh.indices);
    const bool keepsModelData = ctx.modelMeshes.count(sourceEntity) != 0;
    for (unsigned int i = 0; i < mesh.numExternalBuffers && i < mesh.eBuffers.size(); i++) {
        resetBuffer(mesh.eBuffers[i]);
        // bytes in the file data of a model not copied along may go with the original
        if (!keepsModelData && !mesh.eBuffers[i].ownsData() && mesh.eBuffers[i].getData()) {
            mesh.eBuffers[i].copyToOwnData();
        }
    }
    for (unsigned int i = 0; i < mesh.numSubmeshes && i < mesh.submeshes.size(); i++) {
        Submesh& submesh = mesh.submeshes[i];
        submesh.render = ObjectRender();
        submesh.depthRender = ObjectRender();
        submesh.gbufferRender = ObjectRender();
        // the clone's load makes its own extra pass (sharing it would let the clone write the
        // original's parameter blocks, and leave its pipelines to whichever is freed last)
        submesh.extraPass.reset();
        submesh.shader.reset();
        submesh.depthShader.reset();
        submesh.gbufferShader.reset();
        submesh.shaderProperties = 0;
        submesh.depthShaderProperties = 0;
        submesh.gbufferShaderProperties = 0;
        submesh.customShaderId = 0;
        submesh.customDepthShaderId = 0;
        resetBuffer(submesh.lodIndices);
        submesh.needUpdateTexture = false;
        submesh.needUpdateDepthTexture = false;
        submesh.needUpdateGBufferTexture = false;
        remapMaterialTextures(submesh.material, ctx);
    }
    mesh.bonesBuffer = BufferRender();
    mesh.bonesTexture = TextureRender();
    mesh.needUpdateBones = true;
    mesh.needUpdateBuffer = false;
    mesh.needUpdateShaderUniforms = false;
    mesh.needUpdateAABB = true;
    // the lattice records and their texture are the original's: the copy packs and uploads
    // its own (LatticeRender::prepare). The sprite grid request stays, the copied vertices
    // are built for it (reset first, the copy would rebuild them as a quad, draw that quad
    // for a frame, then rebuild and reload the grid)
    const bool wantsTessellation = mesh.lattice.wantsTessellation;
    const int tessellation = mesh.lattice.tessellation;
    mesh.lattice = LatticeMeshState();
    mesh.lattice.wantsTessellation = wantsTessellation;
    mesh.lattice.tessellation = tessellation;
    // the Vertex Animation draw state is the original's load's: the copy's own is made with
    // its load (RenderSystem, from the copy's Vertex Animator)
    mesh.vat = VatMeshState();
}

template<>
void prepare<UIComponent>(UIComponent& ui, Entity, CloneContext& ctx) {
    ui.loaded = false;
    ui.loadCalled = false;
    ui.needReload = false;
    resetBuffer(ui.buffer);
    resetBuffer(ui.indices);
    ui.render = ObjectRender();
    ui.shader.reset();
    ui.shaderProperties = 0;
    ui.customShaderId = 0;
    ui.needUpdateShaderUniforms = false;
    ui.needUpdateTexture = false;
    ui.needUpdateBuffer = false;
    ui.needUpdateAABB = true;
    ui.focused = false;
    ui.pointerMoved = false;
    // the target scene's groups set these (a scene without any group never runs the sync)
    ui.groupAlpha = 1.0f;
    ui.groupLayerAlpha = 1.0f;
    ui.groupInteractable = true;
    ui.groupBlocksRaycasts = true;
    ui.groupEntity = NULL_ENTITY;
    remapFramebufferTexture(ui.texture, ctx);
    remapFramebufferTextures(ui.shaderTextures, ctx);
    if (ctx.move) {
        // a moved GameObject keeps who listens to it
        return;
    }
    ui.onGetFocus.clear();
    ui.onLostFocus.clear();
    ui.onPointerEnter.clear();
    ui.onPointerLeave.clear();
    ui.onPointerMove.clear();
    ui.onPointerDown.clear();
    ui.onPointerUp.clear();
    ui.onClick.clear();
    ui.onDoubleClick.clear();
    ui.onDragStart.clear();
    ui.onDrag.clear();
    ui.onDragEnd.clear();
}

template<>
void prepare<PointsComponent>(PointsComponent& points, Entity, CloneContext& ctx) {
    remapFramebufferTexture(points.texture, ctx);
    remapFramebufferTextures(points.shaderTextures, ctx);
    points.loaded = false;
    points.loadCalled = false;
    points.needReload = false;
    detachExternalBuffer(points.buffer);
    points.renderPoints.clear();
    points.numVisible = 0;
    points.render = ObjectRender();
    points.shader.reset();
    points.shaderProperties = 0;
    points.customShaderId = 0;
    points.needUpdateShaderUniforms = false;
    points.needUpdate = true;
    points.needUpdateBuffer = false;
    points.needUpdateTexture = false;
    points.uiGroupAlpha = 1.0f;
}

template<>
void prepare<LineRendererComponent>(LineRendererComponent& lines, Entity, CloneContext& ctx) {
    remapFramebufferTextures(lines.shaderTextures, ctx);
    lines.loaded = false;
    lines.loadCalled = false;
    lines.needReload = false;
    detachExternalBuffer(lines.buffer);
    lines.render = ObjectRender();
    lines.shader.reset();
    lines.shaderProperties = 0;
    lines.customShaderId = 0;
    lines.needUpdateShaderUniforms = false;
    lines.needUpdateBuffer = false;
    // the copy builds its own ribbon and GPU buffers
    lines.loadedRibbon = false;
    lines.loadedRibbonKey = 0;
    lines.needUpdateRibbon = true;
    lines.ribbon = LineRibbonData();
    lines.polylineSegments.clear();
    lines.warnedInvalidPoints = false;
}

template<>
void prepare<TrailRendererComponent>(TrailRendererComponent& trail, Entity, CloneContext& ctx) {
    remapFramebufferTextures(trail.shaderTextures, ctx);
    // a copy starts without points (an instantiated prefab's trail starts where it is)
    trail.points.reset();
    trail.hasLastHeadPosition = false;
    trail.idleTime = 0.0f;
    trail.warnedCapacity = false;
    trail.destroyRequested = false;
    trail.needUpdateRibbon = true;
    trail.loaded = false;
    trail.loadCalled = false;
    trail.needReload = false;
    trail.needUpdateBuffer = false;
    trail.loadedRibbonKey = 0;
    detachExternalBuffer(trail.buffer);
    trail.ribbon = LineRibbonData();
    trail.render = ObjectRender();
    trail.shader.reset();
    trail.shaderProperties = 0;
    trail.customShaderId = 0;
    trail.needUpdateShaderUniforms = false;
    trail.worldAABB.setNull();
}

template<>
void prepare<SkyboxComponent>(SkyboxComponent& sky, Entity, CloneContext& ctx) {
    remapFramebufferTexture(sky.texture, ctx);
    remapFramebufferTextures(sky.shaderTextures, ctx);
    sky.loaded = false;
    sky.loadCalled = false;
    sky.needReload = false;
    resetBuffer(sky.buffer);
    sky.render = ObjectRender();
    sky.shader.reset();
    sky.customShaderId = 0;
    sky.needUpdateShaderUniforms = false;
    sky.irradianceMap.reset();
    sky.prefilteredMap.reset();
    sky.envMapsLoaded = false;
    sky.needUpdateEnvironment = true;
    sky.needUpdateTexture = false;
    sky.needUpdateSky = true;
}

template<>
void prepare<InstancedMeshComponent>(InstancedMeshComponent& instmesh, Entity, CloneContext&) {
    detachExternalBuffer(instmesh.buffer);
    detachExternalBuffer(instmesh.vatBuffer);
    instmesh.renderInstances.clear();
    instmesh.renderBounds.clear();
    instmesh.renderVat.clear();
    instmesh.numVisible = 0;
    for (InstanceViewRange& view : instmesh.views) {
        view = InstanceViewRange();
    }
    instmesh.lastFadeCullEnd = -1;
    instmesh.needUpdateBuffer = false;
    instmesh.needUpdateInstances = true;
}

template<>
void prepare<TerrainComponent>(TerrainComponent& terrain, Entity, CloneContext& ctx) {
    remapFramebufferTexture(terrain.heightMap, ctx);
    for (Texture& blendMap : terrain.blendMaps) {
        remapFramebufferTexture(blendMap, ctx);
    }
    for (TerrainSurfaceLayer& layer : terrain.surfaceLayers) {
        for (Texture* texture : {&layer.colorTexture, &layer.normalTexture, &layer.roughnessTexture, &layer.metallicTexture}) {
            remapFramebufferTexture(*texture, ctx);
        }
    }
    for (TerrainFoliageLayer& layer : terrain.foliageLayers) {
        remapFramebufferTexture(layer.densityMap, ctx);
    }
    for (TerrainView& view : terrain.views) {
        resetBuffer(view.nodesbuffer[0]);
        resetBuffer(view.nodesbuffer[1]);
        view.needUpdateNodesBuffer = false;
    }
    terrain.heightMapLoaded = false;
    terrain.needUpdateTerrain = true;
    terrain.needUpdateTexture = false;
    terrain.needUpdateFoliage = true;
}

template<>
void prepare<TextComponent>(TextComponent& text, Entity, CloneContext&) {
    text.loaded = false;
    text.stbtext.reset();
    text.atlasVersion = 0;
    text.needReloadAtlas = false;
    text.needUpdateText = true;
    text.glyphInfo.clear();
    text.rasterScale = 1.0f;
    text.sdfPadding = 0;
}

template<>
void prepare<TextEffectComponent>(TextEffectComponent& effect, Entity, CloneContext&) {
    // the authored fields are copied; the copy starts as a loaded scene starts it (autoPlay),
    // with no callbacks
    effect.onComplete.clear();
    effect.onCharacterRevealed.clear();
    effect.playing = false;
    effect.finished = false;
    effect.started = false;
    effect.time = 0.0;
    effect.cycleTime = 0.0;
    effect.revealed = 0;
    effect.previewInEditor = false;
    effect.needRestart = false;
    effect.needApply = true;
    effect.applied = false;
    effect.wasActive = false;
    effect.appliedBuild = 0;
    effect.basePositions.clear();
    effect.appliedText.clear();
    effect.appliedFontSize = 0;
}

template<>
void prepare<CameraComponent>(CameraComponent& camera, Entity, CloneContext& ctx) {
    // its own render target (clone() made it, so the textures showing the original's can
    // be pointed at it whatever the order); the RenderSystem deletes it with the entity
    auto it = camera.framebuffer ? ctx.framebuffers.find(camera.framebuffer) : ctx.framebuffers.end();
    camera.framebuffer = it != ctx.framebuffers.end() ? it->second.framebuffer : new Framebuffer();
    camera.render = CameraRender();
    camera.needUpdate = true;
}

template<>
void prepare<LightComponent>(LightComponent& light, Entity, CloneContext& ctx) {
    remapFramebufferTexture(light.spotMask, ctx);
    for (LightCamera& lightCamera : light.cameras) {
        lightCamera.render = CameraRender();
    }
    light.shadowMapIndex = -1;
    light.spotMaskReady = false;
    light.needUpdateShadowCamera = true;
    light.needUpdateShadowMap = true;
}

template<>
void prepare<Light2DComponent>(Light2DComponent& light, Entity, CloneContext&) {
    light.shadowMapIndex = -1;
}

template<>
void prepare<ReflectionProbeComponent>(ReflectionProbeComponent& probe, Entity, CloneContext&) {
    probe.texture = Texture(); // the cubemap it captures is its own
    probe.captureRevision = 0;
    probe.needUpdate = true;
}

template<>
void prepare<UIDocumentComponent>(UIDocumentComponent& document, Entity, CloneContext&) {
    // the copy loads its own document
    document.needReload = true;
    document.loaded = false;
    document.loadFailed = false;
    document.appliedSortOrder = 0;
    document.appliedVisible = false;
}

template<>
void prepare<PoseAnimatorComponent>(PoseAnimatorComponent& animator, Entity, CloneContext& ctx) {
    if (ctx.move && ctx.sameIds) {
        // its rig is on the same bones (the same ids): it plays on
        return;
    }
    // the copy builds its own rig on its own bones and starts as a loaded scene starts it
    // (a moved one whose ids changed too, keeping its listeners)
    animator.playing = false;
    animator.time = 0;
    animator.started = false;
    animator.needsLoad = true;
    animator.runtime.reset();
    if (!ctx.move) {
        animator.onFinish.clear();
    }
}

template<>
void prepare<AnimationPlayerComponent>(AnimationPlayerComponent& player, Entity, CloneContext& ctx) {
    if (ctx.move && ctx.sameIds) {
        // its runtime is bound to the same ids: it plays on where it was
        return;
    }
    // the authored fields are copied; the copy binds its own hierarchy and starts as a loaded
    // scene starts it (playAutomatically on its first update), with no callbacks (a moved one
    // whose ids changed starts over, keeping its callbacks)
    player.needsLoad = true;
    player.started = false;
    player.runtime.reset();
    if (!ctx.move) {
        player.onEvent.clear();
        player.onFinish.clear();
    }
}

template<>
void prepare<VertexAnimatorComponent>(VertexAnimatorComponent& animator, Entity, CloneContext& ctx) {
    // the authored fields are copied (a copied attachment follows the copy); the copy loads,
    // starts and plays on its own as a loaded scene starts it, with no callbacks
    for (VertexAnimatorAttachment& attachment : animator.attachments) {
        attachment.entity = ctx.remap(attachment.entity);
    }
    animator.preview = false;
    animator.needsLoad = true;
    animator.started = false;
    animator.loadedGeneration = 0;
    animator.loadError.clear();
    animator.asset.reset();
    animator.state = vat::VatTrackPair();
    animator.instanceStates.clear();
    animator.queued.clear();
    animator.requests.clear();
    animator.lastNow = 0.0f;
    animator.runtime.reset();
    animator.gpuMode = vat::VatMode::Vertices;
    animator.gpuState0 = VertexAnimatorComponent().gpuState0;
    animator.gpuState1 = VertexAnimatorComponent().gpuState1;
    animator.gpuNow = 0.0f;
    animator.gpuActive = false;
    // a moved one starts over too (its draw state is its mesh's, which reloads), keeping its
    // callbacks
    if (!ctx.move) {
        animator.onEvent.clear();
        animator.onFinish.clear();
        animator.onInstanceEvent.clear();
        animator.onInstanceFinish.clear();
    }
}

template<>
void prepare<FluidSimulationComponent>(FluidSimulationComponent& fluid, Entity sourceEntity, CloneContext& ctx) {
    // the copy runs its own simulation (its own targets and output), restarted from its
    // settings: the simulator clone() made for it when the original had one (the copied
    // textures showing the original's output show its output), else one made when it runs
    auto it = ctx.fluidSimulators.find(sourceEntity);
    fluid.simulator = it != ctx.fluidSimulators.end() ? it->second : nullptr;
    fluid.pendingSplats.clear();
    fluid.pendingBursts.clear();
    fluid.pendingSteps = 0;
    fluid.needReset = true;
    fluid.revision = 0;
}

template<>
void prepare<ModelComponent>(ModelComponent& model, Entity, CloneContext& ctx) {
    // the parsed file data is shared (read only, kept alive by the copy too)
    model.skeleton = ctx.remap(model.skeleton);
    for (auto& [name, entity] : model.bonesNameMapping) entity = ctx.remap(entity);
    for (auto& [id, entity] : model.bonesIdMapping) entity = ctx.remap(entity);
    for (auto& [id, entity] : model.meshNodesMapping) entity = ctx.remap(entity);
    for (auto& [id, entity] : model.nodesIdMapping) entity = ctx.remap(entity);
    for (Entity& entity : model.animations) entity = ctx.remap(entity);
    for (ModelSkinBinding& binding : model.skinBindings) {
        binding.mesh = ctx.remap(binding.mesh);
        for (Entity& joint : binding.joints) joint = ctx.remap(joint);
    }
}

template<>
void prepare<BoneComponent>(BoneComponent& bone, Entity, CloneContext& ctx) {
    bone.model = ctx.remap(bone.model);
}

template<>
void prepare<AudioSourceComponent>(AudioSourceComponent& audio, Entity, CloneContext& ctx) {
    if (ctx.move) {
        // the voice plays on (SoLoud handles are engine-wide), with its sample and listeners
        return;
    }
    // its own voice and sample: a Playing state starts it (play on awake)
    audio.sample.reset();
    audio.handle = 0;
    audio.loaded = false;
    audio.loadFailed = false;
    audio.playingTime = 0;
    if (audio.state == AudioSourceState::Paused) {
        audio.state = AudioSourceState::Stopped;
    }
    audio.startTrigger = false;
    audio.stopTrigger = false;
    audio.pauseTrigger = false;
    audio.needUpdate = true;
    audio.onStart.clear();
    audio.onPause.clear();
    audio.onStop.clear();
}

template<>
void prepare<ActionComponent>(ActionComponent& action, Entity, CloneContext& ctx) {
    action.target = ctx.remap(action.target);
    if (ctx.move) {
        // it runs on from where it is (state, time), with its listeners
        return;
    }
    // started again from the beginning, the way a loaded scene starts them
    const ActionState state = action.state;
    action.state = ActionState::Stopped;
    action.timecount = 0;
    action.stopTrigger = false;
    action.startTrigger = action.startTrigger || state != ActionState::Stopped;
    action.pauseTrigger = action.pauseTrigger || state == ActionState::Paused;
    action.onStart.clear();
    action.onPause.clear();
    action.onStop.clear();
    action.onStep.clear();
}

template<>
void prepare<AnimationComponent>(AnimationComponent& animation, Entity, CloneContext& ctx) {
    for (ActionFrame& frame : animation.actions) {
        frame.action = ctx.remap(frame.action);
    }
}

template<>
void prepare<ParticleSystemComponent>(ParticleSystemComponent& particles, Entity, CloneContext& ctx) {
    if (ctx.move) {
        // its live particles go on (they are drawn by its target, which reloads)
        return;
    }
    particles.particles.clear();
    particles.newParticlesCount = 0;
    particles.lastUsedParticle = 0;
    particles.currentBurst = 0;
}

template<>
void prepare<ButtonComponent>(ButtonComponent& button, Entity, CloneContext& ctx) {
    for (Texture* texture : {&button.textureNormal, &button.textureHovered, &button.texturePressed, &button.textureDisabled}) {
        remapFramebufferTexture(*texture, ctx);
    }
    button.label = ctx.remap(button.label);
    button.pressed = false;
    button.hovered = false;
    button.needUpdateButton = true;
    if (!ctx.move) {
        button.onPress.clear();
        button.onRelease.clear();
    }
}

template<>
void prepare<InputFieldComponent>(InputFieldComponent& input, Entity, CloneContext& ctx) {
    input.text = ctx.remap(input.text);
    input.selection = ctx.remap(input.selection);
    input.cursor = ctx.remap(input.cursor);
    input.needUpdateInputField = true;
    if (!ctx.move) {
        input.onChange.clear();
        input.onSubmit.clear();
    }
}

template<>
void prepare<PanelComponent>(PanelComponent& panel, Entity, CloneContext& ctx) {
    panel.headerimage = ctx.remap(panel.headerimage);
    panel.headercontainer = ctx.remap(panel.headercontainer);
    panel.headertext = ctx.remap(panel.headertext);
    panel.headerPointerDown = false;
    panel.edgePointerDown = PanelEdge::NONE;
    panel.needUpdatePanel = true;
    if (!ctx.move) {
        panel.onMove.clear();
        panel.onResize.clear();
    }
}

template<>
void prepare<ScrollbarComponent>(ScrollbarComponent& scrollbar, Entity, CloneContext& ctx) {
    scrollbar.bar = ctx.remap(scrollbar.bar);
    scrollbar.barPointerDown = false;
    scrollbar.needUpdateScrollbar = true;
    if (!ctx.move) {
        scrollbar.onChange.clear();
    }
}

template<>
void prepare<ProgressbarComponent>(ProgressbarComponent& progressbar, Entity, CloneContext& ctx) {
    progressbar.fill = ctx.remap(progressbar.fill);
    progressbar.needUpdateProgressbar = true;
}

template<>
void prepare<LayoutGroupComponent>(LayoutGroupComponent& layout, Entity, CloneContext& ctx) {
    for (unsigned int i = 0; i < MAX_CONTAINER_BOXES; i++) {
        layout.boxes[i].layout = ctx.remap(layout.boxes[i].layout);
    }
}

template<>
void prepare<CanvasGroupComponent>(CanvasGroupComponent& group, Entity, CloneContext&) {
    // the runtime block is the target scene's (UISystem::syncCanvasGroups fills it)
    const CanvasGroupComponent defaults;
    group.seen = defaults.seen;
    group.layerAlpha = defaults.layerAlpha;
    group.subtreeDraws = defaults.subtreeDraws;
    group.layered = defaults.layered;
    group.warnedInvalidAlpha = false;
}

template<>
void prepare<RectTransformComponent>(RectTransformComponent& layout, Entity, CloneContext& ctx) {
    layout.panel = ctx.remap(layout.panel);
    layout.needUpdateSizes = true;
}

template<>
void prepare<LatticeComponent>(LatticeComponent& lattice, Entity, CloneContext& ctx) {
    if (ctx.move) {
        // its jiggle goes on
        return;
    }
    // the copy starts still: its jiggle warms up again where it is placed; a running tween
    // goes on from where the original is
    lattice.dynamicOffsets.clear();
    lattice.jiggleVelocities.clear();
    lattice.jiggleLastPosition = Vector3(0, 0, 0);
    lattice.jiggleLastVelocity = Vector3(0, 0, 0);
    lattice.jiggleWarmup = 0;
    lattice.jiggleSleeping = true;
}

template<>
void prepare<LatticeModifierComponent>(LatticeModifierComponent& modifier, Entity, CloneContext& ctx) {
    // a copied lattice deforms the copies; one not copied stays bound in the same registry,
    // and is none in another (the binding stays, skipped like one to a deleted lattice)
    for (LatticeBinding& binding : modifier.bindings) {
        binding.lattice = ctx.remap(binding.lattice);
    }
    modifier.warnedOverflow = false;
}

template<>
void prepare<LatticeFollowerComponent>(LatticeFollowerComponent& follower, Entity, CloneContext& ctx) {
    follower.lattice = ctx.remap(follower.lattice);
    follower.rest = ctx.remap(follower.rest);
}

template<>
void prepare<SpriteComponent>(SpriteComponent& sprite, Entity, CloneContext&) {
    sprite.needUpdateSprite = true;
}

template<>
void prepare<ImageComponent>(ImageComponent& image, Entity, CloneContext&) {
    image.needUpdatePatches = true;
}

template<>
void prepare<PolygonComponent>(PolygonComponent& polygon, Entity, CloneContext&) {
    polygon.needUpdatePolygon = true;
}

template<>
void prepare<MeshPolygonComponent>(MeshPolygonComponent& polygon, Entity, CloneContext&) {
    polygon.needUpdatePolygon = true;
}

template<>
void prepare<TilemapComponent>(TilemapComponent& tilemap, Entity, CloneContext&) {
    tilemap.needUpdateTilemap = true;
}

#ifdef JASPERENGINE_PHYSICS_2D
template<>
void prepare<Rigidbody2DComponent>(Rigidbody2DComponent& body, Entity, CloneContext& ctx) {
    // the target's physics makes the body (in its own world) on its next step; a moved one
    // starts with the velocities it had
    if (ctx.move && b2Body_IsValid(body.body)) {
        const b2Vec2 velocity = b2Body_GetLinearVelocity(body.body);
        body.pendingLinearVelocity = Vector2(velocity.x, velocity.y);
        body.pendingAngularVelocity = b2Body_GetAngularVelocity(body.body);
        body.pendingAwake = b2Body_IsAwake(body.body);
        body.hasPendingVelocity = true;
    }
    body.body = b2_nullBodyId;
    for (Collider2D& collider : body.colliders) {
        collider.shapes.clear();
        collider.chain = b2_nullChainId;
        collider.dirty = true;
        collider.receiver = ctx.remap(collider.receiver);
        // a collider's own material copy (setColliderFriction on a material): the clone gets
        // its own copy of it, with the same values; a script's shared PhysicsMaterial stays shared
        if (collider.ownsMaterialInstance && collider.materialInstance) {
            collider.materialInstance = std::make_shared<PhysicsMaterialRuntime>(collider.materialInstance->toData(), std::string());
        }
    }
    body.needReloadBody = true;
    body.needUpdateColliders = true;
    body.newBody = true;
}

template<>
void prepare<Joint2DComponent>(Joint2DComponent& joint, Entity, CloneContext& ctx) {
    joint.joint = b2_nullJointId;
    joint.bodyA = ctx.remap(joint.bodyA);
    joint.bodyB = ctx.remap(joint.bodyB);
    joint.needUpdateJoint = true;
    joint.needApplyParameters = false;
    joint.broken = false;
    joint.breakArmed = true;
    joint.loadFailed = false;
}
#endif

#ifdef JASPERENGINE_PHYSICS_3D
template<>
void prepare<RigidbodyComponent>(RigidbodyComponent& body, Entity, CloneContext& ctx) {
    Scene* sourceScene = ctx.move ? dynamic_cast<Scene*>(ctx.source) : nullptr;
    if (sourceScene && !body.body.IsInvalid()) {
        JPH::BodyInterface& bodies = sourceScene->getSystem<PhysicsSystem>()->getBodyInterface3D();
        const JPH::Vec3 linear = bodies.GetLinearVelocity(body.body);
        const JPH::Vec3 angular = bodies.GetAngularVelocity(body.body);
        body.pendingLinearVelocity = Vector3(linear.GetX(), linear.GetY(), linear.GetZ());
        body.pendingAngularVelocity = Vector3(angular.GetX(), angular.GetY(), angular.GetZ());
        body.pendingAwake = bodies.IsActive(body.body);
        body.hasPendingVelocity = true;
    }
    body.body = JPH::BodyID();
    for (size_t i = 0; i < body.numColliders && i < body.colliders.size(); i++) {
        body.colliders[i].shape = nullptr;
        body.colliders[i].sourceEntity = ctx.remap(body.colliders[i].sourceEntity);
    }
    body.needReloadBody = true;
    body.needUpdateColliders = true;
    body.newBody = true;
}

template<>
void prepare<Joint3DComponent>(Joint3DComponent& joint, Entity, CloneContext& ctx) {
    joint.joint = nullptr;
    joint.bodyA = ctx.remap(joint.bodyA);
    joint.bodyB = ctx.remap(joint.bodyB);
    joint.hingeA = ctx.remap(joint.hingeA);
    joint.hingeB = ctx.remap(joint.hingeB);
    joint.hinge = ctx.remap(joint.hinge);
    joint.slider = ctx.remap(joint.slider);
    joint.needUpdateJoint = true;
}
#endif

template<>
void prepare<ScriptComponent>(ScriptComponent& scripts, Entity, CloneContext& ctx) {
    if (ctx.move) {
        // the same instances (Lua tables, C++ objects) and their bound properties: they follow
        // the GameObject (ObjectManager::moveToScene); a reference to a moved GameObject goes
        // to its new id, one to another GameObject of this scene means nothing in the new one
        for (ScriptEntry& entry : scripts.scripts) {
            for (ScriptProperty& property : entry.properties) {
                if (property.type == ScriptPropertyType::EntityReference) {
                    if (EntityReference* reference = std::get_if<EntityReference>(&property.value)) {
                        if (reference->sceneId == 0) {
                            reference->entity = ctx.remap(reference->entity);
                        }
                    }
                }
            }
        }
        return;
    }
    for (ScriptEntry& entry : scripts.scripts) {
        entry.instance = nullptr;
        entry.cppScript = nullptr;
        entry.cppMessageMask = 0;
        for (ScriptProperty& property : entry.properties) {
            property.memberPtr = nullptr;
            property.ownedInstance = nullptr;
            property.luaRef = 0;
            if (property.type == ScriptPropertyType::EntityReference) {
                if (EntityReference* reference = std::get_if<EntityReference>(&property.value)) {
                    // a reference into another scene stays as it is
                    if (reference->sceneId == 0) {
                        reference->entity = ctx.remap(reference->entity);
                    }
                }
            }
        }
    }
}

// A moved GameObject's component that cannot come along
template<typename T>
bool keepInMove(const T&, Entity, const CloneContext&) {
    return true;
}

// a joint to a body outside the moved GameObjects (the bodies are in different worlds then)
bool jointBodiesMove(const std::initializer_list<Entity>& bodies, Entity sourceEntity, const CloneContext& ctx) {
    for (Entity body : bodies) {
        if (body != NULL_ENTITY && !ctx.map.count(body)) {
            Log::warn("joint '%s' to an object outside the moved hierarchy was removed", ctx.source->getEntityName(sourceEntity).c_str());
            return false;
        }
    }
    return true;
}

#ifdef JASPERENGINE_PHYSICS_2D
template<>
bool keepInMove<Joint2DComponent>(const Joint2DComponent& joint, Entity sourceEntity, const CloneContext& ctx) {
    return jointBodiesMove({joint.bodyA, joint.bodyB}, sourceEntity, ctx);
}
#endif

#ifdef JASPERENGINE_PHYSICS_3D
template<>
bool keepInMove<Joint3DComponent>(const Joint3DComponent& joint, Entity sourceEntity, const CloneContext& ctx) {
    return jointBodiesMove({joint.bodyA, joint.bodyB}, sourceEntity, ctx);
}
#endif

template<typename T>
void copyComponent(EntityRegistry* source, Entity sourceEntity, const Signature& signature, Scene* target, Entity targetEntity, CloneContext& ctx) {
    if (!signature.test(source->getComponentId<T>())) {
        return;
    }
    T component = source->getComponent<T>(sourceEntity);
    if (ctx.move && !keepInMove<T>(component, sourceEntity, ctx)) {
        return;
    }
    prepare<T>(component, sourceEntity, ctx);
    if (target->findComponent<T>(targetEntity)) {
        target->getComponent<T>(targetEntity) = std::move(component);
    } else {
        target->addComponent<T>(targetEntity, std::move(component));
    }
}

// Every component type of EntityRegistry. Transform first: it places the entity in the
// hierarchy before the components kept in Transform order are added.
void copyComponents(EntityRegistry* source, Entity sourceEntity, Scene* target, Entity targetEntity, CloneContext& ctx) {
    const Signature signature = source->getSignature(sourceEntity);
    copyComponent<Transform>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<RectTransformComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<MeshComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<ModelComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<BoneComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<SkyboxComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<FogComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<MirrorComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<LayoutGroupComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<CanvasGroupComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<SpriteComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<SpriteAnimationComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<CameraComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<LightComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<Light2DComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<ShadowCaster2DComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<ActionComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<TweenComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<PositionTweenComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<RotationTweenComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<ScaleTweenComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<ColorTweenComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<AlphaTweenComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<ParticleSystemComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<PointsComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<LineRendererComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<TrailRendererComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<TextComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<UIComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<ImageComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<ButtonComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<PanelComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<ScrollbarComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<ProgressbarComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<InputFieldComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<MeshPolygonComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<PolygonComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<AnimationComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<KeyframeTracksComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<MorphTracksComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<RotateTracksComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<TranslateTracksComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<ScaleTracksComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<TerrainComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<AudioSourceComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<TilemapComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
#ifdef JASPERENGINE_PHYSICS_2D
    copyComponent<Rigidbody2DComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<Joint2DComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
#endif
#ifdef JASPERENGINE_PHYSICS_3D
    copyComponent<RigidbodyComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<Joint3DComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
#endif
    copyComponent<InstancedMeshComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<PrefabComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<ReflectionProbeComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<UIDocumentComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<FluidSimulationComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<LatticeComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<LatticeModifierComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<LatticeFollowerComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<LatticeTracksComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<PoseAnimatorComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<AnimationPlayerComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<VertexAnimatorComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<TextEffectComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<ScriptComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
}

bool hasComponentSignature(EntityRegistry* registry, Entity entity, ComponentId id) {
    return registry->getSignature(entity).test(id);
}

template<typename T>
T* componentOf(EntityRegistry* registry, Entity entity, const Signature& signature) {
    return signature.test(registry->getComponentId<T>()) ? &registry->getComponent<T>(entity) : nullptr;
}

// EntityCloner::move: what the copy took over is no longer the original's, so destroying it
// stops no voice, releases no script instance, fires no callback and destroys nothing else
void handOver(EntityRegistry* source, Entity entity, const CloneContext& ctx) {
    const Signature signature = source->getSignature(entity);
    if (ScriptComponent* scripts = componentOf<ScriptComponent>(source, entity, signature)) {
        for (ScriptEntry& entry : scripts->scripts) {
            entry.instance = nullptr;
            entry.cppScript = nullptr;
            entry.cppMessageMask = 0;
            for (ScriptProperty& property : entry.properties) {
                property.memberPtr = nullptr;
                property.ownedInstance = nullptr;
                property.luaRef = 0;
            }
        }
    }
    if (AudioSourceComponent* audio = componentOf<AudioSourceComponent>(source, entity, signature)) {
        audio->handle = 0;
        audio->sample.reset();
        audio->loaded = false;
        audio->state = AudioSourceState::Stopped;
        audio->startTrigger = false;
        audio->stopTrigger = false;
        audio->pauseTrigger = false;
        audio->onStart.clear();
        audio->onPause.clear();
        audio->onStop.clear();
    }
    if (ActionComponent* action = componentOf<ActionComponent>(source, entity, signature)) {
        // stopped: its removal stops nothing (no onStop), and a target outside it stays
        action->state = ActionState::Stopped;
        action->startTrigger = false;
        action->stopTrigger = false;
        action->pauseTrigger = false;
        action->ownedTarget = false;
        action->onStart.clear();
        action->onPause.clear();
        action->onStop.clear();
        action->onStep.clear();
    }
    if (UIComponent* ui = componentOf<UIComponent>(source, entity, signature)) {
        ui->onGetFocus.clear();
        ui->onLostFocus.clear();
        ui->onPointerEnter.clear();
        ui->onPointerLeave.clear();
        ui->onPointerMove.clear();
        ui->onPointerDown.clear();
        ui->onPointerUp.clear();
        ui->onClick.clear();
        ui->onDoubleClick.clear();
        ui->onDragStart.clear();
        ui->onDrag.clear();
        ui->onDragEnd.clear();
    }
    if (ButtonComponent* button = componentOf<ButtonComponent>(source, entity, signature)) {
        button->onPress.clear();
        button->onRelease.clear();
    }
    if (PanelComponent* panel = componentOf<PanelComponent>(source, entity, signature)) {
        panel->onMove.clear();
        panel->onResize.clear();
    }
    if (ScrollbarComponent* scrollbar = componentOf<ScrollbarComponent>(source, entity, signature)) {
        scrollbar->onChange.clear();
    }
    if (InputFieldComponent* input = componentOf<InputFieldComponent>(source, entity, signature)) {
        input->onChange.clear();
        input->onSubmit.clear();
    }
    if (AnimationPlayerComponent* player = componentOf<AnimationPlayerComponent>(source, entity, signature)) {
        player->onEvent.clear();
        player->onFinish.clear();
        if (ctx.sameIds) {
            player->runtime.reset();
        }
    }
    if (PoseAnimatorComponent* animator = componentOf<PoseAnimatorComponent>(source, entity, signature)) {
        animator->onFinish.clear();
        if (ctx.sameIds) {
            // the rig is the copy's now: the original's removal must not put its bones back
            animator->runtime.reset();
        }
    }
    if (VertexAnimatorComponent* animator = componentOf<VertexAnimatorComponent>(source, entity, signature)) {
        animator->onEvent.clear();
        animator->onFinish.clear();
        animator->onInstanceEvent.clear();
        animator->onInstanceFinish.clear();
    }
}

}

std::vector<Entity> EntityCloner::collectHierarchy(EntityRegistry* registry, Entity root) {
    std::vector<Entity> result;
    if (!registry || root == NULL_ENTITY || !registry->isEntityCreated(root)) {
        return result;
    }

    std::unordered_set<Entity> inSet;
    result.push_back(root);
    inSet.insert(root);

    // foliage a terrain streams in is the terrain's, not part of the hierarchy to copy
    MeshSystem* meshSystem = nullptr;
    if (Scene* scene = dynamic_cast<Scene*>(registry)) {
        meshSystem = scene->getSystem<MeshSystem>().get();
    }
    auto isFoliage = [&](Entity entity) {
        return meshSystem && meshSystem->getFoliageOwner(entity) != NULL_ENTITY;
    };

    // Transform descendants: the branch that follows the root in the Transform array (walked by
    // entity: with the holes of GameObjects destroyed before, an index walk is O(log n) a step)
    const ComponentId transformId = registry->getComponentId<Transform>();
    if (hasComponentSignature(registry, root, transformId)) {
        auto transforms = registry->getComponentArray<Transform>();
        std::unordered_set<Entity> skipped;
        transforms->forEachAfter(root, [&](Entity entity, const Transform& transform) {
            const Entity parent = transform.parent;
            if (skipped.count(parent)) {
                skipped.insert(entity);
                return true;
            }
            if (!inSet.count(parent)) {
                return false;
            }
            if (isFoliage(entity)) {
                skipped.insert(entity);
                return true;
            }
            result.push_back(entity);
            inSet.insert(entity);
            return true;
        });
    }

    // GameObjects without a Transform that belong to these: actions acting on them (their
    // virtual children), a model's animations, an animation's tracks; to any depth
    auto addOwned = [&](Entity entity) {
        if (entity != NULL_ENTITY && registry->isEntityCreated(entity) && !inSet.count(entity) &&
                !hasComponentSignature(registry, entity, transformId)) {
            result.push_back(entity);
            inSet.insert(entity);
            return true;
        }
        return false;
    };
    auto actions = registry->getComponentArray<ActionComponent>();
    bool added = true;
    size_t scanned = 0;
    while (added) {
        added = false;
        actions->forEach([&](Entity entity, const ActionComponent& action) {
            if (inSet.count(action.target) && addOwned(entity)) {
                added = true;
            }
            return true;
        });
        for (; scanned < result.size(); scanned++) {
            const Entity entity = result[scanned];
            if (ModelComponent* model = registry->findComponent<ModelComponent>(entity)) {
                for (Entity animation : model->animations) {
                    if (addOwned(animation)) added = true;
                }
            }
            if (AnimationComponent* animation = registry->findComponent<AnimationComponent>(entity)) {
                for (const ActionFrame& frame : animation->actions) {
                    if (addOwned(frame.action)) added = true;
                }
            }
        }
    }

    return result;
}

std::vector<Entity> EntityCloner::clone(EntityRegistry* source, const std::vector<Entity>& sources, Scene* target,
                                        Entity targetRoot, std::unordered_map<Entity, Entity>* outMap) {
    std::vector<Entity> copies;
    if (!source || !target || sources.empty()) {
        return copies;
    }

    std::unordered_map<Entity, Entity> map;
    map.reserve(sources.size());
    copies.reserve(sources.size());
    for (size_t i = 0; i < sources.size(); i++) {
        Entity copy = (i == 0 && targetRoot != NULL_ENTITY) ? targetRoot : target->createEntity();
        map[sources[i]] = copy;
        copies.push_back(copy);
    }

    CloneContext ctx{map, source == static_cast<EntityRegistry*>(target), sources[0]};
    ctx.source = source;
    for (Entity entity : sources) {
        if (ModelComponent* model = source->findComponent<ModelComponent>(entity)) {
            ctx.modelMeshes.insert(entity);
            for (const auto& [node, meshEntity] : model->meshNodesMapping) {
                ctx.modelMeshes.insert(meshEntity);
            }
        }
    }
    // cameras and fluids first: textures showing what one renders follow it to the copy
    const bool fluidsRegistered = source->isComponentRegistered<FluidSimulationComponent>();
    for (Entity entity : sources) {
        if (CameraComponent* camera = source->findComponent<CameraComponent>(entity)) {
            if (camera->framebuffer && !ctx.framebuffers.count(camera->framebuffer)) {
                ctx.framebuffers[camera->framebuffer] = {new Framebuffer(), entity, map[entity]};
            }
        }
        FluidSimulationComponent* fluid = fluidsRegistered ? source->findComponent<FluidSimulationComponent>(entity) : nullptr;
        if (fluid && fluid->simulator) {
            std::shared_ptr<FluidSimulator> copy = std::make_shared<FluidSimulator>();
            ctx.framebuffers[fluid->simulator->getOutputFramebuffer()] = {copy->getOutputFramebuffer(), entity, map[entity]};
            ctx.fluidSimulators[entity] = copy;
        }
    }

    // A root that already has a Transform somewhere in the array would have its copied
    // children appended away from it
    if (targetRoot != NULL_ENTITY && target->findComponent<Transform>(targetRoot)) {
        target->removeComponent<Transform>(targetRoot);
    }

    for (size_t i = 0; i < sources.size(); i++) {
        const Entity sourceEntity = sources[i];
        const Entity copy = copies[i];

        target->setEntityName(copy, source->getEntityName(sourceEntity));
        target->setEntityActive(copy, source->isEntityActive(sourceEntity));
        target->setEntityTag(copy, source->getEntityTag(sourceEntity));
        target->setEntityLayer(copy, source->getEntityLayer(sourceEntity));

        copyComponents(source, sourceEntity, target, copy, ctx);
    }

    if (outMap) {
        *outMap = std::move(map);
    }
    return copies;
}

std::vector<Entity> EntityCloner::move(Scene* source, const std::vector<Entity>& sources, Scene* target,
                                       std::unordered_map<Entity, Entity>& map) {
    std::vector<Entity> copies;
    map.clear();
    if (!source || !target || source == target || sources.empty()) {
        return copies;
    }

    // The same ids where the target has them free (user GameObjects only), else new ones
    copies.reserve(sources.size());
    map.reserve(sources.size());
    Entity highest = NULL_ENTITY;
    bool sameIds = true;
    for (Entity entity : sources) {
        Entity copy = NULL_ENTITY;
        if (entity >= EntityManager::firstUserEntity() && !target->isEntityCreated(entity) && target->recreateEntity(entity)) {
            copy = entity;
        } else {
            copy = target->createUserEntity();
            sameIds = false;
        }
        map[entity] = copy;
        copies.push_back(copy);
        highest = std::max(highest, copy);
    }
    // what is made in the target later counts from past the kept ids (an instantiated prefab
    // finds its GameObjects above the counter as it was)
    target->setLastEntity(highest);

    CloneContext ctx{map, false, sources[0]};
    ctx.move = true;
    ctx.sameIds = sameIds;
    ctx.source = source;
    for (Entity entity : sources) {
        if (ModelComponent* model = source->findComponent<ModelComponent>(entity)) {
            ctx.modelMeshes.insert(entity);
            for (const auto& [node, meshEntity] : model->meshNodesMapping) {
                ctx.modelMeshes.insert(meshEntity);
            }
        }
    }
    // render targets are the copies' own (the GPU state of everything drawn reloads), as for
    // a clone: textures showing a moved camera's or fluid's output show the copy's
    const bool fluidsRegistered = source->isComponentRegistered<FluidSimulationComponent>();
    for (Entity entity : sources) {
        if (CameraComponent* camera = source->findComponent<CameraComponent>(entity)) {
            if (camera->framebuffer && !ctx.framebuffers.count(camera->framebuffer)) {
                ctx.framebuffers[camera->framebuffer] = {new Framebuffer(), entity, map[entity]};
            }
        }
        FluidSimulationComponent* fluid = fluidsRegistered ? source->findComponent<FluidSimulationComponent>(entity) : nullptr;
        if (fluid && fluid->simulator) {
            std::shared_ptr<FluidSimulator> copy = std::make_shared<FluidSimulator>();
            ctx.framebuffers[fluid->simulator->getOutputFramebuffer()] = {copy->getOutputFramebuffer(), entity, map[entity]};
            ctx.fluidSimulators[entity] = copy;
        }
    }

    // in hierarchy order: parents before children, siblings in order
    for (size_t i = 0; i < sources.size(); i++) {
        const Entity sourceEntity = sources[i];
        const Entity copy = copies[i];

        target->setEntityName(copy, source->getEntityName(sourceEntity));
        target->setEntityActive(copy, source->isEntityActive(sourceEntity));
        target->setEntityTag(copy, source->getEntityTag(sourceEntity));
        target->setEntityLayer(copy, source->getEntityLayer(sourceEntity));

        copyComponents(source, sourceEntity, target, copy, ctx);
    }

    for (Entity entity : sources) {
        handOver(source, entity, ctx);
    }
    return copies;
}
