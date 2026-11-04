// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PoseViewport.h"
#include "component/AlphaTweenComponent.h"
#include "component/ButtonComponent.h"
#include "component/ColorTweenComponent.h"
#include "component/InputFieldComponent.h"
#include "component/Joint2DComponent.h"
#include "component/Joint3DComponent.h"
#include "component/KeyframeTracksComponent.h"
#include "component/LayoutGroupComponent.h"
#include "component/MeshPolygonComponent.h"
#include "component/MirrorComponent.h"
#include "component/MorphTracksComponent.h"
#include "component/OnScreenControlComponents.h"
#include "component/PanelComponent.h"
#include "component/PositionTweenComponent.h"
#include "component/ProgressbarComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/RotateTracksComponent.h"
#include "component/RotationTweenComponent.h"
#include "component/ScaleTracksComponent.h"
#include "component/ScaleTweenComponent.h"
#include "component/ScrollbarComponent.h"
#include "component/ShadowCaster2DComponent.h"
#include "component/SpriteAnimationComponent.h"
#include "component/TextComponent.h"
#include "component/TranslateTracksComponent.h"
#include "component/TweenComponent.h"
#include "component/UIDocumentComponent.h"

#include "Engine.h"
#include "Project.h"
#include "Scene.h"
#include "component/ActionComponent.h"
#include "component/AnimationComponent.h"
#include "component/AnimationPlayerComponent.h"
#include "component/VertexAnimatorComponent.h"
#include "component/AudioSourceComponent.h"
#include "component/BoneComponent.h"
#include "component/CameraComponent.h"
#include "component/LightComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/PoseAnimatorComponent.h"
#include "component/PrefabComponent.h"
#include "component/ScriptComponent.h"
#include "component/Transform.h"
#include "object/Camera.h"
#include "object/LineRenderer.h"
#include "object/Primitive.h"
#include "pose/PoseAnimationDocument.h"
#include "pose/PoseEditSession.h"
#include "pose/PoseMetrics.h"
#include "pose/PoseRigProvider.h"
#include "registry/EntityCloner.h"
#include "render/RenderUtil.h"
#include "subsystem/RenderSystem.h"
#include "util/Color.h"
#include "util/ScopedDefaultEntityPool.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    using pose::HumanBone;
    using poseview::HandleKind;

    std::vector<PoseViewport*>& instances(){
        static std::vector<PoseViewport*> list;
        return list;
    }

    const HumanBone END_BONES[4] = {HumanBone::LeftHand, HumanBone::RightHand, HumanBone::LeftFoot, HumanBone::RightFoot};
    const uint32_t RING_COLORS[4] = {poseview::RING_L_COLOR, poseview::RING_U_COLOR, poseview::RING_F_COLOR, poseview::RING_FREE_COLOR};

    // the ghost look: the library Hologram shader alpha blended, without scanlines or flicker,
    // so it is a tint whose opacity and brightness rise towards the silhouette (rim). The tint's
    // alpha is twice the onion opacity and the fill an eighth: surfaces facing the view show at a
    // quarter of the opacity, the rim (which carries the shape) up to twice it
    constexpr float GHOST_FILL = 0.125f;
    constexpr float GHOST_RIM_POWER = 2.0f;
    constexpr float GHOST_RIM_INTENSITY = 1.6f;
    // depth bias pushing the ghost back (polygon offset: slope factor, constant units), so where
    // the ghost's pose coincides with the model's the model wins and the ghost shows only where
    // the poses differ
    constexpr float GHOST_DEPTH_FACTOR = 1.0f;
    constexpr float GHOST_DEPTH_UNITS = 4.0f;



    std::string titleOf(HumanBone bone){
        std::string name = pose::humanBoneName(bone);
        bool upper = true;
        for (char& c : name){
            if (c == '_'){
                c = ' ';
                upper = true;
            }else if (upper){
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                upper = false;
            }
        }
        return name;
    }

    bool renderOnly(Scene* scene, Entity entity){
        Signature allowed;
        allowed.set(scene->getComponentId<Transform>());
        allowed.set(scene->getComponentId<MeshComponent>());
        allowed.set(scene->getComponentId<ModelComponent>());
        allowed.set(scene->getComponentId<BoneComponent>());
        return (scene->getSignature(entity) & ~allowed).none();
    }

    template<typename T>
    void strip(Scene* scene, Entity entity){
        if (scene->findComponent<T>(entity)) scene->removeComponent<T>(entity);
    }

    template<typename... T>
    void stripEach(Scene* scene, Entity entity){
        (strip<T>(scene, entity), ...);
    }

    // A ghost copy keeps what draws a posed model (Transform, Mesh, Model, Bone): every other
    // component type of the registry goes (EntityCloner's list)
    void stripToRender(Scene* scene, Entity entity){
        stripEach<RectTransformComponent, SkyboxComponent, FogComponent, MirrorComponent, LayoutGroupComponent, SpriteComponent,
                  SpriteAnimationComponent, CameraComponent, LightComponent, Light2DComponent, ShadowCaster2DComponent,
                  ActionComponent, TweenComponent, PositionTweenComponent, RotationTweenComponent, ScaleTweenComponent,
                  ColorTweenComponent, AlphaTweenComponent, ParticleSystemComponent, PointsComponent, LineRendererComponent,
                  TextComponent, UIComponent, ImageComponent, ButtonComponent, PanelComponent, ScrollbarComponent,
                  ProgressbarComponent, InputFieldComponent, MeshPolygonComponent, PolygonComponent, AnimationComponent,
                  KeyframeTracksComponent, MorphTracksComponent, RotateTracksComponent, TranslateTracksComponent,
                  ScaleTracksComponent, TerrainComponent, AudioSourceComponent, TilemapComponent, InstancedMeshComponent,
                  PrefabComponent, ReflectionProbeComponent, UIDocumentComponent, PoseAnimatorComponent, AnimationPlayerComponent,
                  VertexAnimatorComponent, OnScreenStickComponent, OnScreenButtonComponent, TouchAreaComponent,
                  ScriptComponent>(scene, entity);
#ifdef JASPERENGINE_PHYSICS_2D
        stripEach<Rigidbody2DComponent, Joint2DComponent>(scene, entity);
#endif
#ifdef JASPERENGINE_PHYSICS_3D
        stripEach<RigidbodyComponent, Joint3DComponent>(scene, entity);
#endif
    }

    void setGhostMaterial(MeshComponent& mesh, const Vector4& tint){
        for (unsigned int i = 0; i < mesh.numSubmeshes && i < mesh.submeshes.size(); i++){
            Material& material = mesh.submeshes[i].material;
            material.shader = "library:hologram";
            material.shaderFeatures = {"BLEND_MODE_ALPHA"};
            material.shaderParams.clear();
            material.shaderTextures.clear();
            ShaderUniforms::set(material.shaderParams, "tint", tint);
            ShaderUniforms::set(material.shaderParams, "fill", Vector4(GHOST_FILL, 0, 0, 0));
            ShaderUniforms::set(material.shaderParams, "scanlineStrength", Vector4(0, 0, 0, 0));
            ShaderUniforms::set(material.shaderParams, "flickerAmount", Vector4(0, 0, 0, 0));
            ShaderUniforms::set(material.shaderParams, "rimPower", Vector4(GHOST_RIM_POWER, 0, 0, 0));
            ShaderUniforms::set(material.shaderParams, "rimIntensity", Vector4(GHOST_RIM_INTENSITY, 0, 0, 0));
            material.blendMode = MaterialBlendMode::ALPHA;
            material.cullMode = MaterialCullMode::BACK;
            material.depthWrite = MaterialDepthWrite::DISABLED;
            // equal to the pre-pass's own depth (the front surface), so behind the scene's
            // (the model's, anything in front of the character) it fails like the pre-pass
            material.depthTest = MaterialDepthTest::LESS_EQUAL;
            material.depthOffsetFactor = GHOST_DEPTH_FACTOR;
            material.depthOffsetUnits = GHOST_DEPTH_UNITS;
            material.lighting = MaterialLighting::AUTO;
            material.sortPriority = 10;
            // the depth pre-pass: only the ghost's front surface takes the blend
            MaterialPass& prepass = material.extraPass.edit();
            prepass = MaterialPass();
            prepass.shader = "library:stencil_mask";
            prepass.order = MaterialPassOrder::BEFORE;
            prepass.colorWrite = MATERIAL_COLOR_WRITE_NONE;
            prepass.depthWrite = MaterialDepthWrite::ENABLED;
            prepass.depthTest = MaterialDepthTest::LESS;      // strictly in front of the scene, model included
            prepass.depthOffsetFactor = GHOST_DEPTH_FACTOR;
            prepass.depthOffsetUnits = GHOST_DEPTH_UNITS;
            prepass.cullMode = MaterialCullMode::BACK;
            prepass.blendMode = MaterialBlendMode::NONE;
            prepass.stencil.compare = MaterialStencilCompare::DISABLED;
        }
        mesh.castShadows = false;
        mesh.receiveShadows = false;
        mesh.needUpdateShaderUniforms = true;
    }

    // the transform of a skeleton bone's entity for a local TRS of the skeleton (the folded
    // non-bone ancestors taken off again, as PoseEditSession applies poses)
    void boneTransform(const Matrix4& bridge, Vector3& position, Quaternion& rotation, Vector3& scale){
        if (bridge == Matrix4()) return;
        const Matrix4 local = bridge.inverse() * Matrix4::translateMatrix(position) * rotation.getRotationMatrix() * Matrix4::scaleMatrix(scale);
        local.decompose(position, scale, rotation);
    }

    bool samePose(const pose::HumanPose& a, const pose::HumanPose& b){
        if (!(a.hipsPosition == b.hipsPosition)) return false;
        for (int i = 0; i < pose::HUMAN_BONE_COUNT; i++){
            if (!(a.rotations[i] == b.rotations[i])) return false;
        }
        return true;
    }

}

// ---- lifetime ----

editor::PoseViewport::PoseViewport(Scene* editedScene, Scene* toolsLayerScene): scene(editedScene), toolsScene(toolsLayerScene){
    // nothing is made until the view first poses or previews (createObjects)
    instances().push_back(this);
}

editor::PoseViewport::~PoseViewport(){
    instances().erase(std::remove(instances().begin(), instances().end(), this), instances().end());
    for (Ghost& ghost : ghosts){
        destroyGhost(ghost);
    }
    ghosts.clear();
    for (Primitive*& end : ends){
        delete end;
        end = nullptr;
    }
    delete hipsCube;
    for (Primitive* dot : dots) delete dot;
    dots.clear();
    delete ringLines;
    delete floorFill;
    delete floorLines;
}

void editor::PoseViewport::createObjects(){
    if (objectsCreated) return;
    objectsCreated = true;
    {
        ScopedDefaultEntityPool sys(*toolsScene, EntityPool::System);
        for (int i = 0; i < 4; i++){
            ends[i] = new Primitive(toolsScene);
            ends[i]->createSphere(1.0f, 16, 12);
            ends[i]->setColor(poseview::color(poseview::sideColor(END_BONES[i])));
            ends[i]->setVisible(false);
        }
        hipsCube = new Primitive(toolsScene);
        hipsCube->createBox(2.0f, 2.0f, 2.0f);
        hipsCube->setColor(poseview::color(poseview::HIPS_COLOR));
        hipsCube->setVisible(false);
        for (HumanBone bone : poseview::jointDotBones()){
            Primitive* dot = new Primitive(toolsScene);
            dot->createSphere(1.0f, 12, 8);
            dot->setColor(poseview::color(poseview::sideColor(bone)));
            dot->setVisible(false);
            dots.push_back(dot);
        }
        ringLines = new LineRenderer(toolsScene);
        ringLines->addLine(Vector3::ZERO, Vector3::ZERO, Vector4::ZERO);
        ringLines->setVisible(false);
    }
    {
        ScopedDefaultEntityPool sys(*scene, EntityPool::System);
        std::shared_ptr<RenderSystem> render = scene->getSystem<RenderSystem>();
        floorFill = new Primitive(scene);
        floorFill->createCylinder(1.0f, 1.0f, 1.0f, 48, 1);
        floorFill->setColor(Vector4(1.0f, 1.0f, 1.0f, poseview::FLOOR_FILL_ALPHA));
        floorFill->setVisible(false);
        MeshComponent& fill = floorFill->getComponent<MeshComponent>();
        fill.castShadows = false;
        fill.receiveShadows = false;
        render->setEditorOverlay(floorFill->getEntity(), true);
        floorLines = new LineRenderer(scene);
        floorLines->addLine(Vector3::ZERO, Vector3::ZERO, Vector4::ZERO);
        floorLines->setVisible(false);
        render->setEditorOverlay(floorLines->getEntity(), true);
    }
}

editor::PoseViewport* editor::PoseViewport::forScene(Scene* scene){
    for (PoseViewport* viewport : instances()){
        if (viewport->scene == scene) return viewport;
    }
    return nullptr;
}

// ---- state ----

editor::PoseEditSession* editor::PoseViewport::posedSession() const{
    PoseEditSession* session = PoseEditSession::active();
    if (!session || !session->bound() || session->scene() != scene || !session->rig() || !session->rig()->valid()){
        return nullptr;
    }
    if (!scene->isEntityCreated(session->model())) return nullptr;
    return session;
}

bool editor::PoseViewport::isPosing() const{
    PoseEditSession* session = posedSession();
    return posing && session && session->mode() == PoseEditSession::Mode::Posing;
}

bool editor::PoseViewport::isDragging() const{
    return drag.kind != HandleKind::None;
}

bool editor::PoseViewport::covers(Entity entity) const{
    PoseEditSession* session = posedSession();
    if (!session || session->mode() != PoseEditSession::Mode::Posing) return false;
    const Entity model = session->model();
    for (int guard = 0; entity != NULL_ENTITY && guard < 4096; guard++){
        if (entity == model) return true;
        const Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform) return false;
        entity = transform->parent;
    }
    return false;
}

void editor::PoseViewport::removeCovered(std::vector<Entity>& entities) const{
    if (!posedSession()) return;
    entities.erase(std::remove_if(entities.begin(), entities.end(), [this](Entity e){ return covers(e); }), entities.end());
}

Matrix4 editor::PoseViewport::modelMatrix() const{
    PoseEditSession* session = posedSession();
    if (!session) return Matrix4();
    const Transform* transform = scene->findComponent<Transform>(session->model());
    return transform ? transform->modelMatrix : Matrix4();
}

Quaternion editor::PoseViewport::modelRotation() const{
    Vector3 position, scale;
    Quaternion rotation;
    modelMatrix().decompose(position, scale, rotation);
    return rotation;
}

float editor::PoseViewport::modelScale() const{
    Vector3 position, scale;
    Quaternion rotation;
    modelMatrix().decompose(position, scale, rotation);
    return std::max((std::fabs(scale.x) + std::fabs(scale.y) + std::fabs(scale.z)) / 3.0f, 1e-6f);
}

Vector3 editor::PoseViewport::toWorld(const Vector3& model) const{
    const Vector4 p = modelMatrix() * Vector4(model.x, model.y, model.z, 1.0f);
    return Vector3(p.x, p.y, p.z);
}

float editor::PoseViewport::pixelsToWorld(const Vector3& world, float pixels) const{
    return poseview::worldPerPixel(viewProjection, world, viewDirection, static_cast<float>(width), static_cast<float>(height)) *
           pixels * pixelScale;
}

bool editor::PoseViewport::jointLimits() const{
    PoseEditSession* session = posedSession();
    return session && session->document() && session->document()->data().settings.humanize.jointLimits;
}

bool editor::PoseViewport::grounded() const{
    PoseEditSession* session = posedSession();
    if (!session || !session->document()) return true;
    const pose::PoseAnimationData& data = session->document()->data();
    const int primary = session->primary();
    return primary < 0 || primary >= static_cast<int>(data.poses.size()) || data.poses[primary].grounded;
}



// ---- per frame ----

void editor::PoseViewport::frame(Project* project){
    for (PoseViewport* viewport : instances()){
        PoseEditSession* session = viewport->posedSession();
        const bool shows = session && session->mode() != PoseEditSession::Mode::Idle;
        if (!shows){
            // nothing to show (the view may not draw again soon): hidden, ghosts gone
            if (viewport->posing || viewport->previewing || viewport->floorShown || !viewport->ghosts.empty() ||
                    viewport->isDragging()){
                viewport->clear();
            }
            viewport->watching = false;
            viewport->posing = false;
            viewport->previewing = false;
            continue;
        }
        if (project){
            // followed while previewing too, so a selection made just after Stop counts
            viewport->watchSelection(project, session->mode() == PoseEditSession::Mode::Posing);
        }
    }
}

void editor::PoseViewport::watchSelection(Project* project, bool act){
    PoseEditSession* session = posedSession();
    if (!session) return;
    std::vector<Entity> selection = project->getSelectedEntities(session->sceneId());
    if (!watching){
        // posing began: the selection it began with is not a reason to stop
        lastSelection = selection;
        watching = true;
        return;
    }
    if (selection == lastSelection) return;
    lastSelection = selection;
    if (!act) return;
    bool keeps = !selection.empty();
    for (Entity entity : selection){
        if (!covers(entity)){
            keeps = false;
            break;
        }
    }
    if (!keeps){
        // clicking empty space or another object leaves posing (the bones go back)
        if (isDragging()) endDrag(false);
        watching = false;
        session->goIdle();
    }
}

void editor::PoseViewport::update(Camera* camera, int viewWidth, int viewHeight, float overlayScale, bool disabled){
    PoseEditSession* session = disabled ? nullptr : posedSession();
    const PoseEditSession::Mode mode = session ? session->mode() : PoseEditSession::Mode::Idle;
    posing = mode == PoseEditSession::Mode::Posing;
    previewing = mode == PoseEditSession::Mode::Previewing;
    if (!session || mode == PoseEditSession::Mode::Idle){
        if (isDragging()) endDrag(false);
        hoverKind = HandleKind::None;
        hoverRing = -1;
        publishEngaged();
        hideHandles();
        hideFloor();
        dropGhosts();
        handles.clear();
        rings.clear();
        updated = false;
        return;
    }
    const pose::PoseRig& rig = *session->rig();
    // (a weak pointer: a new rig at the address of a freed one is still a new rig)
    if (activeRig.lock() != session->rig()){
        activeRig = session->rig();
        activeJoint = HumanBone::Count;
        metricsValid = false;
    }
    if (activeJoint != HumanBone::Count && !rig.has(activeJoint)) activeJoint = HumanBone::Count;

    // the camera as the view draws it
    CameraComponent& cameraComponent = camera->getComponent<CameraComponent>();
    viewProjection = cameraComponent.viewProjectionMatrix;
    cameraPosition = camera->getWorldPosition();
    orthographic = cameraComponent.type != CameraType::CAMERA_PERSPECTIVE;
    nearClip = cameraComponent.nearClip;
    farClip = cameraComponent.farClip;
    width = viewWidth;
    height = viewHeight;
    pixelScale = overlayScale > 0.0f ? overlayScale : 1.0f;
    {
        const Vector3 toCamera = cameraComponent.worldDirection;   // the view's +Z: towards the camera
        viewDirection = toCamera.length() > 1e-6f ? (-toCamera).normalized() : Vector3(0, 0, -1);
    }
    updated = width > 0 && height > 0;
    createObjects();

    const pose::HumanPose& shown = session->displayed();
    if (posing && updated){
        if (hasHoverRay && !isDragging()) hover(hoverRay);
        buildHandles(rig, shown);
        buildRings(rig, shown);
        drawHandles();
        drawRings(rig, shown);
    }else{
        hideHandles();
        handles.clear();
        rings.clear();
        hoverKind = HandleKind::None;
        hoverRing = -1;
    }
    publishEngaged();
    drawFloor(rig, shown, session->view().showBalance);

    // ghosts
    std::vector<GhostWish> wishes;
    const auto& document = session->document();
    const int count = document ? static_cast<int>(document->data().poses.size()) : 0;
    const bool loop = document && document->data().settings.loop;
    const float opacity = std::clamp(session->onion().opacity, 0.0f, 1.0f);
    auto tint = [opacity](uint32_t rgb){
        const Vector4 c = poseview::color(rgb, std::min(1.0f, 2.0f * opacity));
        return Color::sRGBToLinear(c.x, c.y, c.z, c.w);
    };
    if (posing){
        if (session->globalEditActive()){
            wishes.push_back({"global_edit", session->globalEditBaseline(), tint(poseview::GLOBAL_EDIT_COLOR)});
        }else{
            const int primary = session->primary();
            if (session->onion().previous){
                const int index = poseview::onionPrevious(primary, count, loop);
                if (index >= 0) wishes.push_back({"previous", session->keyPose(index), tint(poseview::ONION_PREVIOUS_COLOR)});
            }
            if (session->onion().next){
                const int index = poseview::onionNext(primary, count, loop);
                if (index >= 0) wishes.push_back({"next", session->keyPose(index), tint(poseview::ONION_NEXT_COLOR)});
            }
        }
    }else if (previewing && session->view().ghostDuringPreview){
        std::shared_ptr<const PoseRealisation> realised = session->realisation();
        if (realised){
            const int index = poseview::keyLeaving(realised->timeline, session->previewTime());
            if (index >= 0) wishes.push_back({"preview", session->keyPose(index), tint(poseview::ONION_PREVIOUS_COLOR)});
        }
    }
    updateGhosts(*session, wishes);
}

// ---- handles ----

void editor::PoseViewport::hideHandles(){
    for (Primitive* end : ends) if (end) end->setVisible(false);
    if (hipsCube) hipsCube->setVisible(false);
    for (Primitive* dot : dots) dot->setVisible(false);
    if (ringLines) ringLines->setVisible(false);
}

void editor::PoseViewport::buildHandles(const pose::PoseRig& rig, const pose::HumanPose& pose){
    handles.clear();
    std::vector<Quaternion> rotations;
    std::vector<Vector3> positions;
    rig.computeWorld(pose, rotations, positions);
    auto add = [&](HandleKind kind, HumanBone bone, float pickPixels, bool box){
        poseview::Handle handle;
        if (rig.has(bone)){
            handle.kind = kind;
            handle.bone = bone;
            handle.center = toWorld(positions[static_cast<size_t>(rig.boneIndex(bone))]);
            handle.radius = pixelsToWorld(handle.center, pickPixels);
            handle.box = box;
        }
        handles.push_back(handle);
    };
    // order: the four ends, the hips, the dots (drawHandles relies on it)
    for (HumanBone bone : END_BONES){
        const bool foot = bone == HumanBone::LeftFoot || bone == HumanBone::RightFoot;
        add(foot ? HandleKind::Foot : HandleKind::Hand, bone, poseview::HANDLE_RADIUS_PX, false);
    }
    add(HandleKind::Hips, HumanBone::Hips, poseview::HIPS_HALF_SIZE_PX, true);
    // the hips cube faces where the character faces
    handles.back().orientation = modelRotation() * rig.characterRotation();
    for (HumanBone bone : poseview::jointDotBones()){
        add(HandleKind::Joint, bone, poseview::JOINT_PICK_RADIUS_PX, false);
    }
}

void editor::PoseViewport::drawHandles(){
    auto look = [this](HandleKind kind, HumanBone bone, uint32_t base){
        Vector4 c = poseview::color(base);
        const bool active = (drag.kind == kind && drag.bone == bone) || (kind != HandleKind::Hips && activeJoint == bone);
        if (active) return poseview::color(poseview::ACTIVE_COLOR);
        if (hoverKind == kind && hoverBone == bone) c = poseview::lighten(c, 0.45f);
        return c;
    };
    for (int i = 0; i < 4; i++){
        const poseview::Handle& handle = handles[static_cast<size_t>(i)];
        Primitive* sphere = ends[static_cast<size_t>(i)];
        if (handle.kind == HandleKind::None){
            sphere->setVisible(false);
            continue;
        }
        sphere->setPosition(handle.center);
        sphere->setScale(pixelsToWorld(handle.center, poseview::HANDLE_RADIUS_PX));
        sphere->setColor(look(handle.kind, handle.bone, poseview::sideColor(handle.bone)));
        sphere->setVisible(true);
    }
    const poseview::Handle& hips = handles[4];
    if (hips.kind == HandleKind::None){
        hipsCube->setVisible(false);
    }else{
        hipsCube->setPosition(hips.center);
        hipsCube->setRotation(hips.orientation);
        hipsCube->setScale(pixelsToWorld(hips.center, poseview::HIPS_HALF_SIZE_PX));
        Vector4 c = poseview::color(poseview::HIPS_COLOR);
        if (drag.kind == HandleKind::Hips) c = poseview::color(poseview::ACTIVE_COLOR);
        else if (hoverKind == HandleKind::Hips) c = poseview::lighten(c, 0.45f);
        hipsCube->setColor(c);
        hipsCube->setVisible(true);
    }
    for (size_t i = 0; i < dots.size(); i++){
        const poseview::Handle& handle = handles[5 + i];
        if (handle.kind == HandleKind::None){
            dots[i]->setVisible(false);
            continue;
        }
        dots[i]->setPosition(handle.center);
        dots[i]->setScale(pixelsToWorld(handle.center, poseview::JOINT_RADIUS_PX));
        dots[i]->setColor(look(HandleKind::Joint, handle.bone, poseview::sideColor(handle.bone)));
        dots[i]->setVisible(true);
    }
}

// ---- rings ----

void editor::PoseViewport::buildRings(const pose::PoseRig& rig, const pose::HumanPose& pose){
    rings.clear();
    if (activeJoint == HumanBone::Count || !rig.has(activeJoint)) return;
    std::vector<Quaternion> rotations;
    std::vector<Vector3> positions;
    rig.computeWorld(pose, rotations, positions);
    const Vector3 center = toWorld(positions[static_cast<size_t>(rig.boneIndex(activeJoint))]);
    const Quaternion rotation = modelRotation();
    for (int a = 0; a < 3; a++){
        poseview::Ring ring;
        ring.center = center;
        ring.axis = (rotation * poseview::jointAxisModel(rig, pose, activeJoint, static_cast<pose::CanonicalAxis>(a))).normalized();
        ring.radius = pixelsToWorld(center, poseview::RING_RADIUS_PX);
        ring.frontHalfOnly = true;
        if (drag.kind == HandleKind::Ring && drag.ring == a){
            ring.axis = drag.axisWorld;   // the axis the drag turns about
        }
        rings.push_back(ring);
    }
    poseview::Ring free;
    free.center = center;
    free.axis = orthographic ? -viewDirection : (cameraPosition - center).normalized();
    if (drag.kind == HandleKind::Ring && drag.ring == 3) free.axis = drag.axisWorld;
    free.radius = pixelsToWorld(center, poseview::FREE_RING_RADIUS_PX);
    free.frontHalfOnly = false;
    rings.push_back(free);
}

void editor::PoseViewport::drawRings(const pose::PoseRig& rig, const pose::HumanPose& pose){
    if (rings.empty()){
        ringLines->setVisible(false);
        return;
    }
    ringLines->clearLines();
    std::vector<std::pair<Vector3, Vector3>> segments;
    const bool limits = jointLimits();
    const Quaternion rotation = modelRotation();
    const Quaternion inverse = rotation.inverse();
    for (size_t r = 0; r < rings.size(); r++){
        const poseview::Ring& ring = rings[r];
        const Vector3 toCamera = orthographic ? -viewDirection : (cameraPosition - ring.center).normalized();
        Vector4 c = poseview::color(RING_COLORS[r]);
        const bool dragged = drag.kind == HandleKind::Ring && drag.ring == static_cast<int>(r);
        if (dragged) c = poseview::color(poseview::ACTIVE_COLOR);
        else if (hoverRing == static_cast<int>(r)) c = poseview::lighten(c, 0.5f);
        segments.clear();
        poseview::ringSegments(ring, toCamera, poseview::RING_SEGMENTS, segments);
        // copies a pixel out along the axis and the radius read as one thicker line, whichever
        // way the ring faces
        const float pixel = pixelsToWorld(ring.center, 1.0f);
        const Vector3 nudge = ring.axis * pixel;
        const float grow = 1.0f + pixel / std::max(ring.radius, 1e-6f);
        for (const auto& s : segments){
            ringLines->addLine(s.first, s.second, c);
            ringLines->addLine(s.first + nudge, s.second + nudge, c);
            ringLines->addLine(ring.center + (s.first - ring.center) * grow, ring.center + (s.second - ring.center) * grow, c);
        }
        if (!limits || r == 3) continue;
        // the limits as red ticks across the ring, where the joint stops
        poseview::AngleRange range;
        Vector3 reference;
        if (dragged){
            range = drag.range;
            reference = drag.reference;
        }else{
            const Vector3 axisModel = (inverse * ring.axis).normalized();
            RingLimitCache& cache = ringLimits[r];
            // keyed by the rig too: after a remap the same pose and axis measure on another skeleton
            const std::shared_ptr<const pose::PoseRig> currentRig = activeRig.lock();
            if (!cache.valid || cache.rig.lock() != currentRig || !currentRig || cache.bone != activeJoint ||
                !(cache.axisModel == axisModel) || !samePose(cache.pose, pose)){
                cache.range = poseview::ringLimitRange(rig, pose, activeJoint, axisModel);
                cache.reference = poseview::ringReference(rig, pose, activeJoint, axisModel);
                cache.rig = currentRig;
                cache.bone = activeJoint;
                cache.axisModel = axisModel;
                cache.pose = pose;
                cache.valid = true;
            }
            range = cache.range;
            reference = (rotation * cache.reference).normalized();
        }
        const Vector4 tick = poseview::color(poseview::LIMIT_TICK_COLOR);
        for (int side = 0; side < 2; side++){
            const bool limited = side == 0 ? range.lowLimited : range.highLimited;
            if (!limited) continue;
            const float degrees = side == 0 ? range.low : range.high;
            const Quaternion turn = pose::axisAngleRad(ring.axis, static_cast<float>(pose::degToRad(degrees)));
            const Vector3 direction = (turn * reference).normalized();
            const Vector3 point = ring.center + direction * ring.radius;
            if (!poseview::ringPointVisible(ring, point, toCamera)) continue;
            const float length = ring.radius * 0.14f;
            ringLines->addLine(point - direction * length, point + direction * length, tick);
            ringLines->addLine(point - direction * length + nudge, point + direction * length + nudge, tick);
        }
    }
    ringLines->setVisible(true);
}

// ---- floor ----

void editor::PoseViewport::hideFloor(){
    if (floorFill) floorFill->setVisible(false);
    if (floorLines) floorLines->setVisible(false);
    floorShown = false;
    balanceShown = false;
    contactSides.clear();
}

void editor::PoseViewport::updateMetrics(const pose::PoseRig& rig, const pose::HumanPose& pose){
    if (metricsValid && samePose(metricsPose, pose)) return;
    const pose::PoseMetrics metrics = pose::computeMetrics(rig, pose, rig.toBodyPose(pose));
    leftHeight = metrics.left.height;
    rightHeight = metrics.right.height;
    leftFoot = metrics.left.position;
    rightFoot = metrics.right.position;
    centerOfMass = metrics.centerOfMass;
    metricsBalanced = metrics.balanced;
    metricsPose = pose;
    metricsValid = true;
}

void editor::PoseViewport::drawFloor(const pose::PoseRig& rig, const pose::HumanPose& pose, bool showBalance){
    updateMetrics(rig, pose);
    const float hs = rig.humanScale();
    const float scale = modelScale();
    const Quaternion rotation = modelRotation();
    const Vector3 up = (rotation * rig.characterUp()).normalized();
    const Vector3 left = (rotation * rig.characterLeft()).normalized();
    const Vector3 forward = (rotation * rig.characterForward()).normalized();
    const float lift = 0.002f * hs * scale;   // above a ground plane at the floor's height
    // under the character: the hips on the floor
    const Vector3 hipsModel = rig.hipsWorldPosition(pose);
    const Vector3 upModel = rig.characterUp();
    const Vector3 centerModel = hipsModel - upModel * (hipsModel.dotProduct(upModel) - rig.floorHeight());
    const Vector3 center = toWorld(centerModel) + up * lift;
    const Vector3 origin = toWorld(upModel * rig.floorHeight()) + up * lift;
    const float radius = poseview::FLOOR_RADIUS * hs * scale;

    floorFill->setPosition(center);
    floorFill->setRotation(rotation);
    floorFill->setScale(Vector3(radius, std::max(0.0005f * hs * scale, 1e-5f), radius));
    floorFill->setVisible(true);

    floorLines->clearLines();
    std::vector<std::pair<Vector3, Vector3>> segments;
    poseview::discGrid(center, radius, left, forward, origin, poseview::FLOOR_GRID_SPACING, segments);
    const Vector4 gridColor(1.0f, 1.0f, 1.0f, 0.14f);
    for (const auto& s : segments) floorLines->addLine(s.first, s.second, gridColor);
    segments.clear();
    poseview::circleSegments(center, up, radius, 72, segments);
    const Vector4 rimColor(1.0f, 1.0f, 1.0f, 0.35f);
    for (const auto& s : segments) floorLines->addLine(s.first, s.second, rimColor);

    contactSides.clear();
    auto contact = [&](float heightAbove, const Vector3& canonical, HumanBone foot, const char* side){
        if (heightAbove > poseview::CONTACT_RING_HEIGHT) return;
        const Vector3 at = toWorld(poseview::canonicalToModel(rig, Vector3(canonical.x, 0.0f, canonical.z))) + up * (2.0f * lift);
        segments.clear();
        poseview::circleSegments(at, up, 0.07f * hs * scale, 32, segments);
        const Vector4 c = poseview::color(poseview::sideColor(foot));
        for (const auto& s : segments) floorLines->addLine(s.first, s.second, c);
        contactSides.push_back(side);
    };
    if (rig.has(HumanBone::LeftFoot)) contact(leftHeight, leftFoot, HumanBone::LeftFoot, "left");
    if (rig.has(HumanBone::RightFoot)) contact(rightHeight, rightFoot, HumanBone::RightFoot, "right");

    balanceShown = showBalance;
    balanced = metricsBalanced;
    if (showBalance){
        // a diamond on the floor under the centre of mass
        const Vector3 at = toWorld(poseview::canonicalToModel(rig, Vector3(centerOfMass.x, 0.0f, centerOfMass.z))) + up * (3.0f * lift);
        const float r = 0.045f * hs * scale;
        const Vector4 c = poseview::color(metricsBalanced ? poseview::BALANCED_COLOR : poseview::UNBALANCED_COLOR);
        const Vector3 corners[4] = {at + left * r, at + forward * r, at - left * r, at - forward * r};
        for (int i = 0; i < 4; i++){
            floorLines->addLine(corners[i], corners[(i + 1) % 4], c);
            floorLines->addLine(corners[i] + up * lift, corners[(i + 1) % 4] + up * lift, c);
        }
        floorLines->addLine(corners[0], corners[2], c);
        floorLines->addLine(corners[1], corners[3], c);
    }
    floorLines->setVisible(true);
    floorShown = true;
}

// ---- ghosts ----

bool editor::PoseViewport::createGhost(PoseEditSession& session, Ghost& ghost){
    const std::shared_ptr<const PoseRigInfo>& info = session.rigInfo();
    const Entity model = session.model();
    if (!info || !scene->isEntityCreated(model)) return false;
    // the model and the GameObjects under it that only draw (meshes, bones, glTF nodes), and
    // the whole skeleton: every bone and skinned mesh with the GameObjects above it, whatever
    // else they carry (a script or a collider on a hand bone), so the ghost's skin and mappings
    // point at its own copies. The copies keep only what draws (stripped below)
    std::unordered_set<Entity> skeleton;
    auto needs = [this, model, &skeleton](Entity entity){
        for (int guard = 0; entity != NULL_ENTITY && entity != model && guard < 4096; guard++){
            if (!skeleton.insert(entity).second) break;   // its parents are in already
            const Transform* transform = scene->findComponent<Transform>(entity);
            entity = transform ? transform->parent : NULL_ENTITY;
        }
    };
    for (Entity bone : info->boneEntities) needs(bone);
    if (const ModelComponent* modelComponent = scene->findComponent<ModelComponent>(model)){
        for (const ModelSkinBinding& binding : modelComponent->skinBindings) needs(binding.mesh);
    }
    std::vector<Entity> all = EntityCloner::collectHierarchy(scene, model);
    std::vector<Entity> sources;
    std::unordered_set<Entity> kept;
    for (Entity entity : all){
        if (entity != model){
            const Transform* transform = scene->findComponent<Transform>(entity);
            if (!transform || !kept.count(transform->parent)) continue;
            if (!skeleton.count(entity) && !renderOnly(scene, entity)) continue;
        }
        sources.push_back(entity);
        kept.insert(entity);
    }
    // System entities are few (EntityManager: ids 1..1000, shared with the editor's own): a ghost
    // that does not fit is not made at all (no partial ghost), the session goes on without it
    size_t freeSlots = 0;
    for (Entity e = EntityManager::firstSystemEntity(); e <= EntityManager::lastSystemEntity() && freeSlots < sources.size(); e++){
        if (!scene->isEntityCreated(e)) freeSlots++;
    }
    if (sources.empty() || freeSlots < sources.size()) return false;
    std::unordered_map<Entity, Entity> map;
    std::vector<Entity> copies;
    {
        // in the edited scene (its depth occludes the ghost), as System entities: never saved,
        // never in the Hierarchy or the scene's GameObject list, drawn only by the Scene view
        ScopedDefaultEntityPool sys(*scene, EntityPool::System);
        copies = EntityCloner::clone(scene, sources, scene, NULL_ENTITY, &map);
    }
    if (copies.size() != sources.size() || std::find(copies.begin(), copies.end(), NULL_ENTITY) != copies.end()){
        // the clone came back short anyway: none of it stays
        for (Entity copy : copies){
            if (copy != NULL_ENTITY && scene->isEntityCreated(copy)) scene->destroyEntity(copy);
        }
        return false;
    }
    ghost.root = copies.front();
    ghost.entities = copies;
    for (Entity copy : copies) stripToRender(scene, copy);
    if (ModelComponent* copy = scene->findComponent<ModelComponent>(ghost.root)){
        copy->animations.clear();   // within one registry they would still name the model's
    }
    std::shared_ptr<RenderSystem> render = scene->getSystem<RenderSystem>();
    ghost.bones.assign(info->boneEntities.size(), NULL_ENTITY);
    for (size_t i = 0; i < info->boneEntities.size(); i++){
        auto it = map.find(info->boneEntities[i]);
        if (it != map.end()) ghost.bones[i] = it->second;
    }
    for (Entity copy : copies){
        render->setEditorOverlay(copy, true);
        if (scene->findComponent<MeshComponent>(copy)) ghost.meshes.push_back(copy);
    }
    ghost.colorSet = false;
    return true;
}

void editor::PoseViewport::destroyGhost(Ghost& ghost){
    for (auto it = ghost.entities.rbegin(); it != ghost.entities.rend(); ++it){
        if (scene->isEntityCreated(*it)) scene->destroyEntity(*it);
    }
    ghost = Ghost();
}

void editor::PoseViewport::dropGhosts(){
    for (Ghost& ghost : ghosts) destroyGhost(ghost);
    ghosts.clear();
    ghostKinds.clear();
    ghostSource = nullptr;
    ghostModel = NULL_ENTITY;
}

void editor::PoseViewport::colorGhost(Ghost& ghost, const Vector4& color){
    if (ghost.colorSet && ghost.color == color) return;
    for (Entity entity : ghost.meshes){
        if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)){
            if (!ghost.colorSet){
                setGhostMaterial(*mesh, color);
            }else{
                for (unsigned int i = 0; i < mesh->numSubmeshes && i < mesh->submeshes.size(); i++){
                    ShaderUniforms::set(mesh->submeshes[i].material.shaderParams, "tint", color);
                }
                mesh->needUpdateShaderUniforms = true;
            }
        }
    }
    ghost.color = color;
    ghost.colorSet = true;
}

void editor::PoseViewport::poseGhost(PoseEditSession& session, Ghost& ghost, const pose::HumanPose& pose){
    const std::shared_ptr<const PoseRigInfo>& info = session.rigInfo();
    const pose::PoseRig& rig = *session.rig();
    ghost.pose = pose;
    // the ghost stands where the model stands
    if (Transform* root = scene->findComponent<Transform>(ghost.root)){
        Vector3 position, scale;
        Quaternion rotation;
        modelMatrix().decompose(position, scale, rotation);
        root->position = position;
        root->rotation = rotation;
        root->scale = scale;
        root->needUpdate = true;
    }
    const pose::Skeleton& skeleton = rig.skeleton();
    for (size_t i = 0; i < skeleton.bones.size() && i < ghost.bones.size(); i++){
        Transform* transform = scene->findComponent<Transform>(ghost.bones[i]);
        if (!transform) continue;
        const pose::SkeletonBone& rest = skeleton.bones[i];
        const int slot = rig.slotOfBone(static_cast<int>(i));
        Vector3 position = slot == pose::boneIndex(HumanBone::Hips) ? pose.hipsPosition : rest.position;
        Quaternion rotation = slot >= 0 ? pose.rotations[static_cast<size_t>(slot)] : rest.rotation;
        Vector3 scale = rest.scale;
        if (i < info->bridges.size()) boneTransform(info->bridges[i], position, rotation, scale);
        transform->position = position;
        transform->rotation = rotation;
        transform->scale = scale;
        transform->needUpdate = true;
    }
}

void editor::PoseViewport::updateGhosts(PoseEditSession& session, const std::vector<GhostWish>& wishes){
    const void* source = session.rigInfo().get();
    if (source != ghostSource || session.model() != ghostModel){
        dropGhosts();
        ghostSource = source;
        ghostModel = session.model();
    }
    while (ghosts.size() > wishes.size()){
        destroyGhost(ghosts.back());
        ghosts.pop_back();
    }
    ghostKinds.clear();
    for (size_t i = 0; i < wishes.size(); i++){
        if (i >= ghosts.size()){
            Ghost ghost;
            if (!createGhost(session, ghost)) break;
            ghosts.push_back(ghost);
        }
        Ghost& ghost = ghosts[i];
        ghost.kind = wishes[i].kind;
        colorGhost(ghost, wishes[i].color);
        poseGhost(session, ghost, wishes[i].pose);
        ghostKinds.push_back(ghost.kind);
    }
}

// ---- mouse ----

void editor::PoseViewport::publishEngaged(){
    PoseEditSession* session = PoseEditSession::active();
    if (!session || session->scene() != scene) return;
    const bool engaged = isPosing() && (isDragging() || hoverKind != HandleKind::None);
    if (engaged != engagedPublished || session->viewportHandleEngaged() != engaged){
        session->setViewportHandleEngaged(engaged);
        engagedPublished = engaged;
    }
}

bool editor::PoseViewport::sceneEditable() const{
    // no pose edits while the scene saves (also asynchronously) or plays: the session writes
    // no bones then
    PoseEditSession* session = posedSession();
    if (!session || !session->editable() || !session->project()) return false;
    const SceneProject* sceneProject = session->project()->getScene(session->sceneId());
    return sceneProject && sceneProject->playState == ScenePlayState::STOPPED;
}

void editor::PoseViewport::hoverEnd(){
    hasHoverRay = false;
    if (hoverKind == HandleKind::None && hoverRing < 0){
        publishEngaged();
        return;
    }
    hoverKind = HandleKind::None;
    hoverBone = HumanBone::Count;
    hoverRing = -1;
    publishEngaged();
    if (PoseEditSession* session = posedSession()){
        if (SceneProject* sceneProject = session->project()->getScene(session->sceneId())) sceneProject->needUpdateRender = true;
    }
}

bool editor::PoseViewport::clicked(Entity picked, Entity rawHit){
    PoseEditSession* session = posedSession();
    if (!session || session->mode() != PoseEditSession::Mode::Posing) return false;
    // the character under a collapsed Hierarchy parent: selection resolves the hit to the
    // parent, the raw hit is still the model
    const bool onModel = covers(picked) || covers(rawHit);
    if (!poseview::clickLeavesPosing(false, false, picked != NULL_ENTITY || rawHit != NULL_ENTITY, onModel)) return false;
    // empty space or another object: posing ends (the bones go back), even when the
    // selection does not change
    if (isDragging()) endDrag(false);
    watching = false;
    session->goIdle();
    return true;
}

void editor::PoseViewport::hover(const Ray& ray){
    hoverRay = ray;
    hasHoverRay = true;
    if (!isPosing() || isDragging() || !updated) return;
    const Vector3 origin = ray.getOrigin();
    const Vector3 direction = ray.getDirection().normalized();
    const HandleKind oldKind = hoverKind;
    const HumanBone oldBone = hoverBone;
    const int oldRing = hoverRing;
    hoverKind = HandleKind::None;
    hoverBone = HumanBone::Count;
    hoverRing = -1;
    const int hit = poseview::pickHandle(origin, direction, handles);
    if (hit >= 0){
        hoverKind = handles[static_cast<size_t>(hit)].kind;
        hoverBone = handles[static_cast<size_t>(hit)].bone;
    }else if (!rings.empty()){
        const float tolerance = pixelsToWorld(rings.front().center, poseview::RING_PICK_PX);
        hoverRing = poseview::pickRing(origin, direction, rings, cameraPosition, orthographic, viewDirection, tolerance);
        if (hoverRing >= 0) hoverKind = HandleKind::Ring;
    }
    publishEngaged();
    if (hoverKind != oldKind || hoverBone != oldBone || hoverRing != oldRing){
        if (PoseEditSession* session = posedSession()){
            if (SceneProject* sceneProject = session->project()->getScene(session->sceneId())){
                sceneProject->needUpdateRender = true;
            }
        }
    }
}

bool editor::PoseViewport::mouseDown(const Ray& ray, bool shift, bool ctrl){
    (void)shift;
    (void)ctrl;
    PoseEditSession* session = posedSession();
    if (!session || session->mode() != PoseEditSession::Mode::Posing || !updated || !sceneEditable()) return false;
    const pose::PoseRig& rig = *session->rig();
    // what is shown now (the pose may have changed since the last draw)
    buildHandles(rig, session->working());
    buildRings(rig, session->working());
    const Vector3 origin = ray.getOrigin();
    const Vector3 direction = ray.getDirection().normalized();
    drag = Drag();
    const int hit = poseview::pickHandle(origin, direction, handles);
    if (hit >= 0){
        const poseview::Handle& handle = handles[static_cast<size_t>(hit)];
        activeJoint = handle.bone;
        if (handle.kind == HandleKind::Joint){
            // a click on a dot: its rings show
            if (SceneProject* sceneProject = session->project()->getScene(session->sceneId())) sceneProject->needUpdateRender = true;
            return true;
        }
        drag.kind = handle.kind;
        drag.bone = handle.bone;
        drag.start = session->working();
        drag.startWorld = handle.center;
        drag.offset = poseview::grabOffset(origin, direction, handle.center, viewDirection);
        drag.undoName = handle.kind == HandleKind::Hips ? "Move Hips" : "Move " + titleOf(handle.bone);
        drag.mergeKey = "pose-viewport-" + std::to_string(++dragSerial);
        publishEngaged();
        if (SceneProject* sceneProject = session->project()->getScene(session->sceneId())) sceneProject->needUpdateRender = true;
        return true;
    }
    if (!rings.empty()){
        const float tolerance = pixelsToWorld(rings.front().center, poseview::RING_PICK_PX);
        const int ring = poseview::pickRing(origin, direction, rings, cameraPosition, orthographic, viewDirection, tolerance);
        if (ring >= 0){
            Vector3 startDirection;
            const poseview::Ring& r = rings[static_cast<size_t>(ring)];
            if (!poseview::ringPlaneDirection(r, origin, direction, viewDirection, startDirection)) return true;
            const Quaternion rotation = modelRotation();
            drag.kind = HandleKind::Ring;
            drag.bone = activeJoint;
            drag.ring = ring;
            drag.start = session->working();
            drag.axisWorld = r.axis;
            drag.axisModel = (rotation.inverse() * r.axis).normalized();
            drag.startDirection = startDirection;
            drag.angle = 0.0f;
            drag.limited = jointLimits();
            if (drag.limited){
                drag.range = poseview::ringLimitRange(rig, drag.start, activeJoint, drag.axisModel);
            }
            drag.reference = (rotation * poseview::ringReference(rig, drag.start, activeJoint, drag.axisModel)).normalized();
            drag.undoName = "Rotate " + titleOf(activeJoint);
            drag.mergeKey = "pose-viewport-" + std::to_string(++dragSerial);
            publishEngaged();
            if (SceneProject* sceneProject = session->project()->getScene(session->sceneId())) sceneProject->needUpdateRender = true;
            return true;
        }
    }
    return false;
}

void editor::PoseViewport::mouseDrag(const Ray& ray, bool shift, bool ctrl){
    PoseEditSession* session = posedSession();
    if (!isDragging()) return;
    if (!session || session->mode() != PoseEditSession::Mode::Posing || !sceneEditable()){
        endDrag(false);
        return;
    }
    const pose::PoseRig& rig = *session->rig();
    const Vector3 origin = ray.getOrigin();
    const Vector3 direction = ray.getDirection().normalized();
    const bool limits = jointLimits();
    const bool onFloor = grounded();
    pose::HumanPose result = drag.start;
    if (drag.kind == HandleKind::Ring){
        Vector3 now;
        const poseview::Ring& r = drag.ring < static_cast<int>(rings.size()) ? rings[static_cast<size_t>(drag.ring)] : poseview::Ring();
        poseview::Ring ring = r;
        ring.axis = drag.axisWorld;
        if (!poseview::ringPlaneDirection(ring, origin, direction, viewDirection, now)) return;
        const float raw = poseview::signedAngle(drag.startDirection, now, drag.axisWorld);
        drag.angle = poseview::unwrapDegrees(drag.angle, raw);
        result = poseview::ringDrag(rig, drag.start, drag.bone, drag.axisModel, drag.angle, ctrl, drag.limited ? &drag.range : nullptr,
                                    &drag.applied);
        poseview::finishHandleEdit(rig, result, {}, false, onFloor);
    }else{
        const Vector3 up = (modelRotation() * rig.characterUp()).normalized();
        Vector3 target;
        if (!poseview::dragPoint(origin, direction, drag.startWorld, drag.offset, viewDirection, shift, up, target)) return;
        const Vector3 targetModel = [&]{
            const Vector4 p = modelMatrix().inverse() * Vector4(target.x, target.y, target.z, 1.0f);
            return Vector3(p.x, p.y, p.z);
        }();
        if (drag.kind == HandleKind::Hips){
            result = poseview::dragHips(rig, drag.start, targetModel, limits, onFloor);
        }else{
            result = poseview::dragLimb(rig, drag.start, drag.bone, targetModel, limits, onFloor);
        }
    }
    drag.changed = drag.changed || !samePose(result, drag.start);
    session->setWorking(result);
}

void editor::PoseViewport::mouseUp(){
    if (isDragging()) endDrag(true);
}

void editor::PoseViewport::endDrag(bool commit){
    const Drag finished = drag;
    drag = Drag();
    publishEngaged();
    PoseEditSession* session = posedSession();
    if (!session) return;
    if (commit && finished.changed && session->mode() == PoseEditSession::Mode::Posing && sceneEditable()){
        // the whole drag is one undo step
        session->commitWorking(finished.undoName, finished.mergeKey);
    }else if (!commit && finished.changed && session->mode() == PoseEditSession::Mode::Posing){
        session->setWorking(finished.start);
    }
    if (SceneProject* sceneProject = session->project()->getScene(session->sceneId())) sceneProject->needUpdateRender = true;
}

void editor::PoseViewport::clear(){
    if (isDragging()) endDrag(false);
    hoverKind = HandleKind::None;
    hoverRing = -1;
    if (engagedPublished){
        if (PoseEditSession* session = PoseEditSession::active()){
            if (session->scene() == scene || !session->bound()) session->setViewportHandleEngaged(false);
        }
        engagedPublished = false;
    }
    hideHandles();
    hideFloor();
    dropGhosts();
    handles.clear();
    rings.clear();
}

// ---- state ----

editor::PoseViewport::State editor::PoseViewport::getState() const{
    State state;
    PoseEditSession* session = posedSession();
    state.posing = session && posing && session->mode() == PoseEditSession::Mode::Posing;
    state.previewing = session && previewing && session->mode() == PoseEditSession::Mode::Previewing;
    state.viewWidth = width;
    state.viewHeight = height;
    if (state.posing && updated){
        for (const poseview::Handle& handle : handles){
            if (handle.kind == HandleKind::None) continue;
            Vector2 position;
            if (poseview::projectToView(viewProjection, handle.center, static_cast<float>(width), static_cast<float>(height), position))
                state.visibleHandles++;
        }
        const float w = static_cast<float>(width);
        const float h = static_cast<float>(height);
        const float tolerance = rings.empty() ? 0.0f : pixelsToWorld(rings.front().center, poseview::RING_PICK_PX);
        for (size_t r = 0; r < rings.size(); r++){
            // Count a ring when its drawn part contains a point that can be picked.
            const poseview::Ring& ring = rings[r];
            const Vector3 toCamera = orthographic ? -viewDirection : (cameraPosition - ring.center).normalized();
            std::vector<std::pair<Vector3, Vector3>> segments;
            poseview::ringSegments(ring, toCamera, poseview::RING_SEGMENTS, segments);
            for (const auto& s : segments){
                const Vector3 mid = (s.first + s.second) * 0.5f;
                Vector2 p;
                if (!poseview::projectToView(viewProjection, mid, w, h, p)) continue;
                const Ray ray = screenToRayFromMatrix(viewProjection, orthographic, nearClip, farClip, p.x, p.y, w, h);
                const Vector3 origin = ray.getOrigin();
                const Vector3 direction = ray.getDirection().normalized();
                if (poseview::pickHandle(origin, direction, handles) >= 0) continue;
                if (poseview::pickRing(origin, direction, rings, cameraPosition, orthographic, viewDirection, tolerance) !=
                        static_cast<int>(r)){
                    continue;
                }
                state.visibleHandles++;
                break;
            }
        }
    }
    state.activeJoint = activeJoint == HumanBone::Count ? "" : pose::humanBoneName(activeJoint);
    state.ghosts = ghostKinds;
    state.floor = floorShown;
    state.contacts = contactSides;
    state.balanceShown = balanceShown;
    state.balanced = balanced;
    return state;
}
