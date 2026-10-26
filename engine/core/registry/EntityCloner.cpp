// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "EntityCloner.h"
#include "EntityRegistry.h"
#include "Scene.h"
#include "subsystem/MeshSystem.h"
#include "render/BufferRender.h"
#include "render/ObjectRender.h"
#include "render/CameraRender.h"
#include "render/TextureRender.h"
#include "texture/Framebuffer.h"

#include <unordered_set>

using namespace jasperengine;

namespace {

struct CloneContext {
    const std::unordered_map<Entity, Entity>& map;
    bool sameRegistry;
    Entity sourceRoot;
    // a copied camera's framebuffer -> the copy's, for textures that show it
    std::unordered_map<const Framebuffer*, Framebuffer*> framebuffers;
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

void remapFramebufferTexture(Texture& texture, const CloneContext& ctx) {
    Framebuffer* framebuffer = texture.getFramebuffer();
    if (!framebuffer) {
        return;
    }
    auto it = ctx.framebuffers.find(framebuffer);
    if (it != ctx.framebuffers.end()) {
        texture.setFramebuffer(it->second);
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
        submesh.litDefineBits = 0;
        resetBuffer(submesh.lodIndices);
        submesh.needUpdateTexture = false;
        submesh.needUpdateDepthTexture = false;
        submesh.needUpdateGBufferTexture = false;
        for (Texture* texture : {&submesh.material.baseColorTexture, &submesh.material.emissiveTexture,
                &submesh.material.metallicRoughnessTexture, &submesh.material.occlusionTexture, &submesh.material.normalTexture}) {
            remapFramebufferTexture(*texture, ctx);
        }
        if (MaterialPbrExtensions* pbr = submesh.material.pbr.editIfSet()) {
            for (int m = 0; m < (int)PbrMap::COUNT; m++) {
                remapFramebufferTexture(MaterialPbr::getMap(*pbr, (PbrMap)m), ctx);
            }
        }
    }
    mesh.bonesBuffer = BufferRender();
    mesh.bonesTexture = TextureRender();
    mesh.needUpdateBones = true;
    mesh.needUpdateBuffer = false;
    mesh.needUpdateShaderUniforms = false;
    mesh.needUpdateAABB = true;
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
    remapFramebufferTexture(ui.texture, ctx);
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
void prepare<PointsComponent>(PointsComponent& points, Entity, CloneContext&) {
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
}

template<>
void prepare<LineRendererComponent>(LineRendererComponent& lines, Entity, CloneContext&) {
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
}

template<>
void prepare<SkyboxComponent>(SkyboxComponent& sky, Entity, CloneContext&) {
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
    instmesh.renderInstances.clear();
    instmesh.renderBounds.clear();
    instmesh.numVisible = 0;
    for (InstanceViewRange& view : instmesh.views) {
        view = InstanceViewRange();
    }
    instmesh.lastFadeCullEnd = -1;
    instmesh.needUpdateBuffer = false;
    instmesh.needUpdateInstances = true;
}

template<>
void prepare<TerrainComponent>(TerrainComponent& terrain, Entity, CloneContext&) {
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
}

template<>
void prepare<CameraComponent>(CameraComponent& camera, Entity, CloneContext& ctx) {
    // its own render target (clone() made it, so the textures showing the original's can
    // be pointed at it whatever the order); the RenderSystem deletes it with the entity
    auto it = camera.framebuffer ? ctx.framebuffers.find(camera.framebuffer) : ctx.framebuffers.end();
    camera.framebuffer = it != ctx.framebuffers.end() ? it->second : new Framebuffer();
    camera.render = CameraRender();
    camera.needUpdate = true;
}

template<>
void prepare<LightComponent>(LightComponent& light, Entity, CloneContext&) {
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
void prepare<AudioSourceComponent>(AudioSourceComponent& audio, Entity, CloneContext&) {
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
    // the copy simulates on its own (its runtime is made at its first update)
    particles.state.reset();
    particles.needReload = true;
    for (Entity& plane : particles.collisionPlanes) {
        plane = ctx.remap(plane);
    }
    particles.onCollision.clear();
    particles.onParticleDeath.clear();
    particles.onSubEmit.clear();
}

template<>
void prepare<ButtonComponent>(ButtonComponent& button, Entity, CloneContext& ctx) {
    button.label = ctx.remap(button.label);
    button.pressed = false;
    button.hovered = false;
    button.needUpdateButton = true;
    button.onPress.clear();
    button.onRelease.clear();
}

template<>
void prepare<InputFieldComponent>(InputFieldComponent& input, Entity, CloneContext& ctx) {
    input.text = ctx.remap(input.text);
    input.selection = ctx.remap(input.selection);
    input.cursor = ctx.remap(input.cursor);
    input.needUpdateInputField = true;
    input.onChange.clear();
    input.onSubmit.clear();
}

template<>
void prepare<PanelComponent>(PanelComponent& panel, Entity, CloneContext& ctx) {
    panel.headerimage = ctx.remap(panel.headerimage);
    panel.headercontainer = ctx.remap(panel.headercontainer);
    panel.headertext = ctx.remap(panel.headertext);
    panel.headerPointerDown = false;
    panel.edgePointerDown = PanelEdge::NONE;
    panel.needUpdatePanel = true;
    panel.onMove.clear();
    panel.onResize.clear();
}

template<>
void prepare<ScrollbarComponent>(ScrollbarComponent& scrollbar, Entity, CloneContext& ctx) {
    scrollbar.bar = ctx.remap(scrollbar.bar);
    scrollbar.barPointerDown = false;
    scrollbar.needUpdateScrollbar = true;
    scrollbar.onChange.clear();
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
void prepare<RectTransformComponent>(RectTransformComponent& layout, Entity, CloneContext& ctx) {
    layout.panel = ctx.remap(layout.panel);
    layout.needUpdateSizes = true;
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
void prepare<Rigidbody2DComponent>(Rigidbody2DComponent& body, Entity, CloneContext&) {
    body.body = b2_nullBodyId;
    for (size_t i = 0; i < body.numColliders && i < body.colliders.size(); i++) {
        body.colliders[i].shape = b2_nullShapeId;
        body.colliders[i].chain = b2_nullChainId;
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
}
#endif

#ifdef JASPERENGINE_PHYSICS_3D
template<>
void prepare<RigidbodyComponent>(RigidbodyComponent& body, Entity, CloneContext& ctx) {
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
    for (ScriptEntry& entry : scripts.scripts) {
        entry.instance = nullptr;
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

template<typename T>
void copyComponent(EntityRegistry* source, Entity sourceEntity, const Signature& signature, Scene* target, Entity targetEntity, CloneContext& ctx) {
    if (!signature.test(source->getComponentId<T>())) {
        return;
    }
    T component = source->getComponent<T>(sourceEntity);
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
    copyComponent<LocalVolumetricFogComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<ForceFieldComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<VolumetricCloudsComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<MirrorComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
    copyComponent<LayoutGroupComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
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
    copyComponent<ScriptComponent>(source, sourceEntity, signature, target, targetEntity, ctx);
}

bool hasComponentSignature(EntityRegistry* registry, Entity entity, ComponentId id) {
    return registry->getSignature(entity).test(id);
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

    // Transform descendants: the branch that follows the root in the Transform array
    const ComponentId transformId = registry->getComponentId<Transform>();
    if (hasComponentSignature(registry, root, transformId)) {
        auto transforms = registry->getComponentArray<Transform>();
        std::unordered_set<Entity> skipped;
        for (size_t i = transforms->getIndex(root) + 1; i < transforms->size(); i++) {
            const Entity parent = transforms->getComponentFromIndex(i).parent;
            const Entity entity = transforms->getEntity(i);
            if (skipped.count(parent)) {
                skipped.insert(entity);
                continue;
            }
            if (!inSet.count(parent)) {
                break;
            }
            if (isFoliage(entity)) {
                skipped.insert(entity);
                continue;
            }
            result.push_back(entity);
            inSet.insert(entity);
        }
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
        for (size_t i = 0; i < actions->size(); i++) {
            const Entity entity = actions->getEntity(i);
            if (inSet.count(actions->getComponentFromIndex(i).target) && addOwned(entity)) {
                added = true;
            }
        }
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

    CloneContext ctx{map, source == static_cast<EntityRegistry*>(target), sources[0], {}, {}};
    for (Entity entity : sources) {
        if (ModelComponent* model = source->findComponent<ModelComponent>(entity)) {
            ctx.modelMeshes.insert(entity);
            for (const auto& [node, meshEntity] : model->meshNodesMapping) {
                ctx.modelMeshes.insert(meshEntity);
            }
        }
    }
    // cameras first: textures showing what one renders follow it to the copy
    for (Entity entity : sources) {
        if (CameraComponent* camera = source->findComponent<CameraComponent>(entity)) {
            if (camera->framebuffer && !ctx.framebuffers.count(camera->framebuffer)) {
                ctx.framebuffers[camera->framebuffer] = new Framebuffer();
            }
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
