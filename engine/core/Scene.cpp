// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "manager/ObjectManager.h"
#include "component/ButtonComponent.h"
#include "component/InputFieldComponent.h"
#include "component/OnScreenControlComponents.h"
#include "component/PanelComponent.h"
#include "component/ScrollbarComponent.h"
#include "script/ScriptRuntime.h"
#include "Scene.h"
#include "shader/ShaderGlobals.h"
#include "script/AsyncOwner.h"

#include "object/Camera.h"
#include "Engine.h"
#include "Screen.h"

#include "subsystem/RenderSystem.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/LatticeSystem.h"
#include "subsystem/VertexAnimatorSystem.h"
#include "subsystem/TrailSystem.h"
#include "subsystem/CameraControlSystem.h"
#include "subsystem/UISystem.h"
#include "subsystem/UIDocumentSystem.h"
#include "subsystem/FluidSystem.h"
#include "subsystem/VideoPlayerSystem.h"
#include "subsystem/ActionSystem.h"
#include "subsystem/PoseAnimatorSystem.h"
#include "subsystem/AnimatorSystem.h"
#include "subsystem/AudioSystem.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/NavigationSystem.h"
#include "picking/SceneBvh.h"
#include "subsystem/ParticleSubsystem.h"
#include "subsystem/SplineSystem.h"
#include "util/Color.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

Scene::Scene() : EntityRegistry() {
    init();
}

Scene::Scene(EntityPool defaultPool) : EntityRegistry(defaultPool) {
    init();
}

void Scene::init(){
    lifetime = std::make_shared<char>(0);

    registerSystem<ActionSystem>();
    // the Animators: after the Animation Players' blend is flushed (they own the bones they
    // bind), before the Pose Animators (whose weight blends over them) and the skinning
    registerSystem<AnimatorSystem>();
    // after the clips' blend is flushed, before the world matrices and the skinning
    registerSystem<PoseAnimatorSystem>();
    // after the scripts, before the meshes and transforms: generated chunks and followers are
    // drawn in the frame they change (docs/specs/S18-splines.md D3)
    registerSystem<SplineSystem>();
    registerSystem<MeshSystem>();
    registerSystem<LatticeSystem>();
    // after the lattices, before Render reads the GPU state it writes (vertex animation spec 3.4)
    registerSystem<VertexAnimatorSystem>();
    // Camera Follow, then the Camera Shakers: after this frame's animation, physics interpolation
    // and onUpdate, before the layout and Render build the matrices (camera shake spec 5.2)
    registerSystem<CameraControlSystem>();
    registerSystem<UISystem>();
    registerSystem<UIDocumentSystem>();
    registerSystem<FluidSystem>();
    // after the scripts' frame, before RenderSystem converts the frames it hands over
    registerSystem<VideoPlayerSystem>();
    registerSystem<RenderSystem>();
    // after Render, whose update makes this frame's world transforms (the trails' heads)
    registerSystem<TrailSystem>();
    // before physics: agents set their bodies' velocities before the world steps (S19 D2)
    registerSystem<NavigationSystem>();
    registerSystem<PhysicsSystem>();
    // after physics (world collisions query it, never while it steps) and the transforms
    registerSystem<ParticleSubsystem>();
    registerSystem<AudioSystem>();

    camera = NULL_ENTITY;
    defaultCamera = NULL_ENTITY;

    // All persistent scene configuration (background, SSAO/SSR, gravity, default shaders, ...)
    // defaults through the SceneSettings member; see SceneSettings.h.
    uiEventState = UIEventState::NOT_SET;
}

Scene::~Scene(){
    tearingDown = true;
    // async work owned by the scene or its GameObjects ends without its callbacks
    AsyncOwners::sceneDestroyed(this);
    // first: handles that own entities of this scene and go during (or after) its teardown
    // leave them to it
    lifetime.reset();
    // GameObject::destroy calls still pending for it
    ObjectManager::forgetScene(this);
    // and the starts of its scripts still queued
    ScriptRuntime::forgetScene(this);
    // sounds of a deleted scene would play on; not in destroy(), which also runs on view recreation
    getSystem<AudioSystem>()->stopSceneSounds();
    destroy();
}

template<typename Component, typename Callback>
static void removeComponentSubscriptionsByTag(Scene* scene, const std::string& substring, Callback callback) {
    auto components = scene->getComponentArray<Component>();

    for (size_t i = 0; i < components->size(); i++) {
        callback(components->getComponentFromIndex(i), substring);
    }
}

bool Scene::isActivationObserved() const{
    return ScriptRuntime::hasStarted(const_cast<Scene*>(this));
}

void Scene::onActiveInHierarchyChanged(const std::vector<Entity>& changed, bool nowActive){
    ScriptRuntime::onActiveInHierarchyChanged(this, changed, nowActive);
}

std::weak_ptr<char> Scene::getLifetime() const{
    return lifetime;
}

void Scene::removeSubscriptionsByTag(const std::string& substring) {
    removeComponentSubscriptionsByTag<ActionComponent>(this, substring, [](ActionComponent& action, const std::string& tag) {
        action.onStart.removeByTagSubstring(tag);
        action.onPause.removeByTagSubstring(tag);
        action.onStop.removeByTagSubstring(tag);
        action.onStep.removeByTagSubstring(tag);
    });

    removeComponentSubscriptionsByTag<SplineAnimateComponent>(this, substring, [](SplineAnimateComponent& animate, const std::string& tag) {
        animate.onComplete.removeByTagSubstring(tag);
    });

    removeComponentSubscriptionsByTag<AudioSourceComponent>(this, substring, [](AudioSourceComponent& sound, const std::string& tag) {
        sound.onStart.removeByTagSubstring(tag);
        sound.onPause.removeByTagSubstring(tag);
        sound.onStop.removeByTagSubstring(tag);
    });

    removeComponentSubscriptionsByTag<PoseAnimatorComponent>(this, substring, [](PoseAnimatorComponent& animator, const std::string& tag) {
        animator.onFinish.removeByTagSubstring(tag);
    });

    removeComponentSubscriptionsByTag<AnimationPlayerComponent>(this, substring, [](AnimationPlayerComponent& player, const std::string& tag) {
        player.onEvent.removeByTagSubstring(tag);
        player.onFinish.removeByTagSubstring(tag);
        player.onStateEnter.removeByTagSubstring(tag);
        player.onStateExit.removeByTagSubstring(tag);
    });

    removeComponentSubscriptionsByTag<VertexAnimatorComponent>(this, substring, [](VertexAnimatorComponent& animator, const std::string& tag) {
        animator.onEvent.removeByTagSubstring(tag);
        animator.onFinish.removeByTagSubstring(tag);
        animator.onInstanceEvent.removeByTagSubstring(tag);
        animator.onInstanceFinish.removeByTagSubstring(tag);
    });

    removeComponentSubscriptionsByTag<AnimatorComponent>(this, substring, [](AnimatorComponent& animator, const std::string& tag) {
        animator.onEvent.removeByTagSubstring(tag);
        animator.onStateEnter.removeByTagSubstring(tag);
        animator.onStateExit.removeByTagSubstring(tag);
        animator.onMontageEnded.removeByTagSubstring(tag);
        animator.onAnimatorMove.removeByTagSubstring(tag);
    });

    removeComponentSubscriptionsByTag<UIComponent>(this, substring, [](UIComponent& ui, const std::string& tag) {
        ui.onGetFocus.removeByTagSubstring(tag);
        ui.onLostFocus.removeByTagSubstring(tag);
        ui.onPointerEnter.removeByTagSubstring(tag);
        ui.onPointerLeave.removeByTagSubstring(tag);
        ui.onPointerMove.removeByTagSubstring(tag);
        ui.onPointerDown.removeByTagSubstring(tag);
        ui.onPointerUp.removeByTagSubstring(tag);
        ui.onClick.removeByTagSubstring(tag);
        ui.onDoubleClick.removeByTagSubstring(tag);
        ui.onDragStart.removeByTagSubstring(tag);
        ui.onDrag.removeByTagSubstring(tag);
        ui.onDragEnd.removeByTagSubstring(tag);
    });

    removeComponentSubscriptionsByTag<ButtonComponent>(this, substring, [](ButtonComponent& button, const std::string& tag) {
        button.onPress.removeByTagSubstring(tag);
        button.onRelease.removeByTagSubstring(tag);
    });

    removeComponentSubscriptionsByTag<PanelComponent>(this, substring, [](PanelComponent& panel, const std::string& tag) {
        panel.onMove.removeByTagSubstring(tag);
        panel.onResize.removeByTagSubstring(tag);
    });

    removeComponentSubscriptionsByTag<ScrollbarComponent>(this, substring, [](ScrollbarComponent& scrollbar, const std::string& tag) {
        scrollbar.onChange.removeByTagSubstring(tag);
    });

    removeComponentSubscriptionsByTag<InputFieldComponent>(this, substring, [](InputFieldComponent& inputField, const std::string& tag) {
        inputField.onChange.removeByTagSubstring(tag);
        inputField.onSubmit.removeByTagSubstring(tag);
    });

    removeComponentSubscriptionsByTag<OnScreenStickComponent>(this, substring, [](OnScreenStickComponent& stick, const std::string& tag) {
        stick.onValueChanged.removeByTagSubstring(tag);
        stick.onPress.removeByTagSubstring(tag);
        stick.onRelease.removeByTagSubstring(tag);
    });

    removeComponentSubscriptionsByTag<OnScreenButtonComponent>(this, substring, [](OnScreenButtonComponent& button, const std::string& tag) {
        button.onPress.removeByTagSubstring(tag);
        button.onRelease.removeByTagSubstring(tag);
    });

    removeComponentSubscriptionsByTag<TouchAreaComponent>(this, substring, [](TouchAreaComponent& area, const std::string& tag) {
        area.onDrag.removeByTagSubstring(tag);
    });

    getSystem<PhysicsSystem>()->removeSubscriptionsByTag(substring);
}

void Scene::setCamera(Camera* camera){
    setCamera(camera->getEntity());
}

void Scene::setCamera(Entity camera){
    if (CameraComponent* cameracomp = findComponent<CameraComponent>(camera)){
        if (camera != this->camera){
            this->camera = camera;
            if (defaultCamera != NULL_ENTITY){
                destroyEntity(defaultCamera);
                defaultCamera = NULL_ENTITY;
            }
            cameracomp->needUpdate = true;
            updateCameraSize();
        }
    }else{
        Log::error("Invalid camera entity: need CameraComponent");
    }
}

Entity Scene::getCamera() const{
    return camera;
}

void Scene::useDefaultCamera(){
    if (defaultCamera == NULL_ENTITY || !isEntityCreated(defaultCamera)){
        defaultCamera = createDefaultCamera();
    }
    camera = defaultCamera;
    if (CameraComponent* cameracomp = findComponent<CameraComponent>(camera)){
        cameracomp->needUpdate = true;
    }
    updateCameraSize();
}

Entity Scene::createDefaultCamera(){
    defaultCamera = createSystemEntity();
    addComponent<CameraComponent>(defaultCamera);
    addComponent<Transform>(defaultCamera);

    CameraComponent& camera = getComponent<CameraComponent>(defaultCamera);
    camera.type = CameraType::CAMERA_UI;
    camera.transparentSort = false;
    camera.depthTest = false;

    Transform& cameratransform = getComponent<Transform>(defaultCamera);
    cameratransform.position = Vector3(0.0, 0.0, 1.0);

    return defaultCamera;
}

void Scene::setBackgroundColor(Vector4 color){
    settings.backgroundColor = color;
}

void Scene::setBackgroundColor(float red, float green, float blue){
    setBackgroundColor(Vector4(red, green, blue, 1.0));
}

void Scene::setBackgroundColor(float red, float green, float blue, float alpha){
    setBackgroundColor(Vector4(red, green, blue, alpha));
}

Vector4 Scene::getBackgroundColor() const{
    return settings.backgroundColor;
}

void Scene::setShadowQuality(ShadowQuality quality){
    // uniform-driven (no shader variant), so no mesh reload is needed
    settings.shadowQuality = quality;
}

ShadowQuality Scene::getShadowQuality() const{
    return settings.shadowQuality;
}

void Scene::setShadowDistance(float distance){
    settings.shadowDistance = (std::isfinite(distance) && distance > 0.0f) ? distance : 0.0f;
}

float Scene::getShadowDistance() const{
    return settings.shadowDistance;
}

void Scene::setShadowFadeFraction(float fraction){
    settings.shadowFadeFraction = std::isfinite(fraction) ? std::clamp(fraction, 0.0f, 1.0f) : SceneSettings().shadowFadeFraction;
}

float Scene::getShadowFadeFraction() const{
    return settings.shadowFadeFraction;
}

void Scene::setShadowCascadeBlend(bool blend){
    settings.shadowCascadeBlend = blend;
}

bool Scene::isShadowCascadeBlend() const{
    return settings.shadowCascadeBlend;
}

ShadowTier Scene::getEffectiveShadowTier() const{
    return const_cast<Scene*>(this)->getSystem<RenderSystem>()->getEffectiveShadowTier();
}

void Scene::setShadowDebugView(ShadowDebugView view){
    shadowDebugView = view;
}

ShadowDebugView Scene::getShadowDebugView() const{
    return shadowDebugView;
}

void Scene::setMeshLodEnabled(bool enabled){
    settings.meshLodEnabled = enabled;
}

bool Scene::isMeshLodEnabled() const{
    return settings.meshLodEnabled;
}

void Scene::setMeshLodThreshold(float pixels){
    settings.meshLodThreshold = std::max(pixels, 0.0f);
}

float Scene::getMeshLodThreshold() const{
    return settings.meshLodThreshold;
}

void Scene::setMeshLodCrossFade(bool crossFade){
    settings.meshLodCrossFade = crossFade;
}

bool Scene::isMeshLodCrossFade() const{
    return settings.meshLodCrossFade;
}

void Scene::setMeshLodCrossFadeDuration(float seconds){
    settings.meshLodCrossFadeDuration = std::isfinite(seconds) ? std::clamp(seconds, 0.05f, 2.0f) : settings.meshLodCrossFadeDuration;
}

float Scene::getMeshLodCrossFadeDuration() const{
    return settings.meshLodCrossFadeDuration;
}

void Scene::setDepthPrepassEnabled(bool enabled){
    if (settings.depthPrepassEnabled != enabled){
        settings.depthPrepassEnabled = enabled;
        // the prepass pipelines are built with the mesh
        getSystem<RenderSystem>()->needReloadMeshes();
    }
}

bool Scene::isDepthPrepassEnabled() const{
    return settings.depthPrepassEnabled;
}

void Scene::setSceneColorScale(SceneColorScale scale){
    // read by RenderSystem each frame, so no reload is needed
    settings.sceneColorScale = scale;
}

SceneColorScale Scene::getSceneColorScale() const{
    return settings.sceneColorScale;
}

void Scene::setSSAOEnabled(bool ssaoEnabled){
    if (settings.ssaoEnabled != ssaoEnabled){
        settings.ssaoEnabled = ssaoEnabled;
        // toggling SSAO changes the mesh shader variant (USE_SSAO), so recompile (the loaded
        // variants draw meanwhile: every pass binds its AO texture or white)
        getSystem<RenderSystem>()->needReloadMeshVariants();
    }
}

bool Scene::isSSAOEnabled() const{
    return settings.ssaoEnabled;
}

void Scene::setSSAORadius(float radius){
    settings.ssaoRadius = radius;
}

float Scene::getSSAORadius() const{
    return settings.ssaoRadius;
}

void Scene::setSSAOIntensity(float intensity){
    settings.ssaoIntensity = intensity;
}

float Scene::getSSAOIntensity() const{
    return settings.ssaoIntensity;
}

void Scene::setSSAOBias(float bias){
    settings.ssaoBias = bias;
}

float Scene::getSSAOBias() const{
    return settings.ssaoBias;
}

void Scene::setSSAODebug(bool debug){
    settings.ssaoDebug = debug;
}

bool Scene::isSSAODebug() const{
    return settings.ssaoDebug;
}

void Scene::setSSREnabled(bool ssrEnabled){
    if (settings.ssrEnabled != ssrEnabled){
        settings.ssrEnabled = ssrEnabled;
        // SSR requires the depth pre-pass, so meshes must (re)build their depth shader
        getSystem<RenderSystem>()->needReloadMeshes();
    }
}

bool Scene::isSSREnabled() const{
    return settings.ssrEnabled;
}

void Scene::setSSRMaxDistance(float maxDistance){
    settings.ssrMaxDistance = maxDistance;
}

float Scene::getSSRMaxDistance() const{
    return settings.ssrMaxDistance;
}

void Scene::setSSRThickness(float thickness){
    settings.ssrThickness = thickness;
}

float Scene::getSSRThickness() const{
    return settings.ssrThickness;
}

void Scene::setSSRMaxSteps(int maxSteps){
    settings.ssrMaxSteps = maxSteps;
}

int Scene::getSSRMaxSteps() const{
    return settings.ssrMaxSteps;
}

void Scene::setSSRIntensity(float intensity){
    settings.ssrIntensity = intensity;
}

float Scene::getSSRIntensity() const{
    return settings.ssrIntensity;
}

void Scene::setSSRBlur(float blur){
    settings.ssrBlur = blur;
}

float Scene::getSSRBlur() const{
    return settings.ssrBlur;
}

void Scene::setSSRDebugMode(int mode){
    settings.ssrDebugMode = mode;
}

int Scene::getSSRDebugMode() const{
    return settings.ssrDebugMode;
}

void Scene::setFixedResolutionEnabled(bool fixedResolutionEnabled){
    if (settings.fixedResolutionEnabled != fixedResolutionEnabled){
        settings.fixedResolutionEnabled = fixedResolutionEnabled;
        // the offscreen target renders with PIP_RTT; (re)bake it on all
        // renderables so a runtime toggle works in exported builds too
        getSystem<RenderSystem>()->needReloadMeshes();
        getSystem<RenderSystem>()->needReloadUIs();
        getSystem<RenderSystem>()->needReloadSky();
        getSystem<RenderSystem>()->needReloadPoints();
        getSystem<RenderSystem>()->needReloadLines();
    }
}

bool Scene::isFixedResolutionEnabled() const{
    return settings.fixedResolutionEnabled;
}

void Scene::setFixedResolutionWidth(unsigned int width){
    settings.fixedResolutionWidth = width;
}

unsigned int Scene::getFixedResolutionWidth() const{
    return settings.fixedResolutionWidth;
}

void Scene::setFixedResolutionHeight(unsigned int height){
    settings.fixedResolutionHeight = height;
}

unsigned int Scene::getFixedResolutionHeight() const{
    return settings.fixedResolutionHeight;
}

void Scene::setFixedResolutionSize(unsigned int width, unsigned int height){
    settings.fixedResolutionWidth = width;
    settings.fixedResolutionHeight = height;
}

void Scene::setFixedResolutionFilter(TextureFilter filter){
    settings.fixedResolutionFilter = filter;
}

TextureFilter Scene::getFixedResolutionFilter() const{
    return settings.fixedResolutionFilter;
}

void Scene::setLightState(LightState state){
    if (settings.lightState != state){
        settings.lightState = state;
        getSystem<RenderSystem>()->needReloadMeshes();
    }
}

LightState Scene::getLightState() const{
    return settings.lightState;
}

void Scene::setGlobalIllumination(float intensity, Vector3 color){
    settings.globalIllumIntensity = intensity;
    settings.globalIllumColor = Color::sRGBToLinear(color);
}

void Scene::setGlobalIllumination(float intensity){
    settings.globalIllumIntensity = intensity;
}

void Scene::setGlobalIllumination(Vector3 color){
    settings.globalIllumColor = Color::sRGBToLinear(color);
}

float Scene::getGlobalIlluminationIntensity() const{
    return settings.globalIllumIntensity;
}

Vector3 Scene::getGlobalIlluminationColor() const{
    return Color::linearTosRGB(settings.globalIllumColor);
}

Vector3 Scene::getGlobalIlluminationColorLinear() const{
    return settings.globalIllumColor;
}

void Scene::setMaxLightsPerObject(int maxLightsPerObject){
    // a new capacity tier reloads the meshes (RenderSystem::update)
    settings.maxLightsPerObject = std::clamp(maxLightsPerObject, 1, MAX_LIGHTS_PER_OBJECT);
}

int Scene::getMaxLightsPerObject() const{
    return settings.maxLightsPerObject;
}

void Scene::setLitQuality(LitQuality litQuality){
    // a new quality reloads the lit meshes' variants (RenderSystem::update)
    settings.litQuality = litQuality;
}

LitQuality Scene::getLitQuality() const{
    return settings.litQuality;
}

void Scene::setPhysicalLightEV100(float ev100){
    // the lights in physical units resolve again (RenderSystem::resolveLights)
    if (std::isfinite(ev100))
        settings.physicalLightEV100 = std::clamp(ev100, -6.0f, 24.0f);
}

float Scene::getPhysicalLightEV100() const{
    return settings.physicalLightEV100;
}

void Scene::setAmbientLight2D(float intensity, Vector3 color){
    settings.ambientLight2DIntensity = intensity;
    settings.ambientLight2DColor = Color::sRGBToLinear(color);
}

void Scene::setAmbientLight2D(float intensity){
    settings.ambientLight2DIntensity = intensity;
}

void Scene::setAmbientLight2D(Vector3 color){
    settings.ambientLight2DColor = Color::sRGBToLinear(color);
}

float Scene::getAmbientLight2DIntensity() const{
    return settings.ambientLight2DIntensity;
}

Vector3 Scene::getAmbientLight2DColor() const{
    return Color::linearTosRGB(settings.ambientLight2DColor);
}

Vector3 Scene::getAmbientLight2DColorLinear() const{
    return settings.ambientLight2DColor;
}

void Scene::setShadow2DQuality(ShadowQuality quality){
    settings.shadow2DQuality = quality;
}

ShadowQuality Scene::getShadow2DQuality() const{
    return settings.shadow2DQuality;
}

Vector2 Scene::getGravity2D() const{
    // getSystem is non-const but this lookup does not mutate the scene
    return const_cast<Scene*>(this)->getSystem<PhysicsSystem>()->getGravity2D();
}

void Scene::setGravity2D(Vector2 gravity){
    getSystem<PhysicsSystem>()->setGravity2D(gravity);
}

void Scene::setGravity2D(float x, float y){
    getSystem<PhysicsSystem>()->setGravity2D(x, y);
}

Vector3 Scene::getGravity3D() const{
    return const_cast<Scene*>(this)->getSystem<PhysicsSystem>()->getGravity3D();
}

void Scene::setGravity3D(Vector3 gravity){
    getSystem<PhysicsSystem>()->setGravity3D(gravity);
}

void Scene::setGravity3D(float x, float y, float z){
    getSystem<PhysicsSystem>()->setGravity3D(x, y, z);
}

void Scene::setPhysicsInterpolation(bool interpolation){
    getSystem<PhysicsSystem>()->setInterpolation(interpolation);
}

bool Scene::isPhysicsInterpolation() const{
    return const_cast<Scene*>(this)->getSystem<PhysicsSystem>()->isInterpolation();
}

void Scene::setPhysics2DSubSteps(int subSteps){
#ifdef JASPERENGINE_PHYSICS_2D
    getSystem<PhysicsSystem>()->setSubSteps2D(subSteps);
#else
    (void)subSteps;
#endif
}

int Scene::getPhysics2DSubSteps() const{
#ifdef JASPERENGINE_PHYSICS_2D
    return const_cast<Scene*>(this)->getSystem<PhysicsSystem>()->getSubSteps2D();
#else
    return SceneSettings().physics2DSubSteps;
#endif
}

void Scene::setDefaultMeshShader(const std::string& path){
    if (settings.defaultMeshShader != path){
        settings.defaultMeshShader = path;
        getSystem<RenderSystem>()->needReloadMeshes();
    }
}

const std::string& Scene::getDefaultMeshShader() const{
    return settings.defaultMeshShader;
}

void Scene::setDefaultUIShader(const std::string& path){
    if (settings.defaultUIShader != path){
        settings.defaultUIShader = path;
        getSystem<RenderSystem>()->needReloadUIs();
    }
}

const std::string& Scene::getDefaultUIShader() const{
    return settings.defaultUIShader;
}

void Scene::setDefaultSkyShader(const std::string& path){
    if (settings.defaultSkyShader != path){
        settings.defaultSkyShader = path;
        getSystem<RenderSystem>()->needReloadSky();
    }
}

const std::string& Scene::getDefaultSkyShader() const{
    return settings.defaultSkyShader;
}

void Scene::setDefaultPointsShader(const std::string& path){
    if (settings.defaultPointsShader != path){
        settings.defaultPointsShader = path;
        getSystem<RenderSystem>()->needReloadPoints();
    }
}

const std::string& Scene::getDefaultPointsShader() const{
    return settings.defaultPointsShader;
}

void Scene::setDefaultLinesShader(const std::string& path){
    if (settings.defaultLinesShader != path){
        settings.defaultLinesShader = path;
        getSystem<RenderSystem>()->needReloadLines();
    }
}

const std::string& Scene::getDefaultLinesShader() const{
    return settings.defaultLinesShader;
}

void Scene::setDefaultCustomShader(ShaderType type, const std::string& path){
    switch (type){
        case ShaderType::MESH:   setDefaultMeshShader(path);   break;
        case ShaderType::UI:     setDefaultUIShader(path);     break;
        case ShaderType::SKYBOX: setDefaultSkyShader(path);    break;
        case ShaderType::POINTS: setDefaultPointsShader(path); break;
        case ShaderType::LINES:  setDefaultLinesShader(path);  break;
        default: break; // internal pass types cannot have scene defaults
    }
}

const std::string& Scene::getDefaultCustomShader(ShaderType type) const{
    static const std::string empty;
    switch (type){
        case ShaderType::MESH:   return settings.defaultMeshShader;
        case ShaderType::UI:     return settings.defaultUIShader;
        case ShaderType::SKYBOX: return settings.defaultSkyShader;
        case ShaderType::POINTS: return settings.defaultPointsShader;
        case ShaderType::LINES:  return settings.defaultLinesShader;
        default: return empty;
    }
}

const ShaderRef& Scene::getDefaultShaderRef(ShaderType type) const{
    static const ShaderRef empty;
    switch (type){
        case ShaderType::MESH:   return settings.defaultMeshShader;
        case ShaderType::UI:     return settings.defaultUIShader;
        case ShaderType::SKYBOX: return settings.defaultSkyShader;
        case ShaderType::POINTS: return settings.defaultPointsShader;
        case ShaderType::LINES:  return settings.defaultLinesShader;
        default: return empty;
    }
}

void Scene::setDefaultShaderRef(ShaderType type, const ShaderRef& ref){
    setDefaultCustomShader(type, ref.path);
    switch (type){
        case ShaderType::MESH:   settings.defaultMeshShader = ref;   break;
        case ShaderType::UI:     settings.defaultUIShader = ref;     break;
        case ShaderType::SKYBOX: settings.defaultSkyShader = ref;    break;
        case ShaderType::POINTS: settings.defaultPointsShader = ref; break;
        case ShaderType::LINES:  settings.defaultLinesShader = ref;  break;
        default: break;
    }
}

const std::vector<PostProcessPass>& Scene::getPostProcessPasses() const{
    return settings.postProcess;
}

void Scene::setPostProcessPasses(const std::vector<PostProcessPass>& passes){
    // only uniform values changed: keep the compiled chain
    bool sameChain = passes.size() == settings.postProcess.size();
    for (size_t i = 0; sameChain && i < passes.size(); i++){
        // (textures are bound when the chain loads)
        sameChain = passes[i].shader == settings.postProcess[i].shader &&
                    passes[i].enabled == settings.postProcess[i].enabled &&
                    passes[i].textures == settings.postProcess[i].textures;
    }

    settings.postProcess = passes;

    if (sameChain){
        getSystem<RenderSystem>()->needUpdatePostProcessUniforms();
    }else{
        getSystem<RenderSystem>()->needReloadPostProcess();
    }
}

void Scene::setPostProcessUniform(unsigned int index, const std::string& name, const Vector4& value){
    if (index >= settings.postProcess.size()){
        Log::error("Post-process pass %u does not exist, cannot set uniform '%s'", index, name.c_str());
        return;
    }

    if (ShaderUniforms::set(settings.postProcess[index].uniforms, name, value))
        getSystem<RenderSystem>()->needUpdatePostProcessUniforms();
}

void Scene::setPostProcessUniform(unsigned int index, const std::string& name, const Vector3& value){
    setPostProcessUniform(index, name, Vector4(value.x, value.y, value.z, 0.0f));
}

void Scene::setPostProcessUniform(unsigned int index, const std::string& name, const Vector2& value){
    setPostProcessUniform(index, name, Vector4(value.x, value.y, 0.0f, 0.0f));
}

void Scene::setPostProcessUniform(unsigned int index, const std::string& name, float value){
    setPostProcessUniform(index, name, Vector4(value, 0.0f, 0.0f, 0.0f));
}

Vector4 Scene::getPostProcessUniform(unsigned int index, const std::string& name) const{
    if (index >= settings.postProcess.size())
        return Vector4::ZERO;

    return ShaderUniforms::get(settings.postProcess[index].uniforms, name);
}

bool Scene::removePostProcessUniform(unsigned int index, const std::string& name){
    if (index >= settings.postProcess.size())
        return false;

    if (!ShaderUniforms::remove(settings.postProcess[index].uniforms, name))
        return false;

    getSystem<RenderSystem>()->needUpdatePostProcessUniforms();
    return true;
}

bool Scene::setPostProcessGlobalBinding(unsigned int index, const std::string& member, const std::string& global){
    if (index >= settings.postProcess.size()){
        Log::error("Post-process pass %u does not exist, cannot bind '%s'", index, member.c_str());
        return false;
    }
    if (!ShaderGlobals::setBinding(settings.postProcess[index].shaderGlobalBindings, member, global))
        return false;

    getSystem<RenderSystem>()->needUpdatePostProcessUniforms();
    return true;
}

std::string Scene::getPostProcessGlobalBinding(unsigned int index, const std::string& member) const{
    if (index >= settings.postProcess.size())
        return "";

    return ShaderGlobals::getBinding(settings.postProcess[index].shaderGlobalBindings, member);
}

const PostProcessingSettings& Scene::getPostProcessing() const{
    return settings.postProcessing;
}

void Scene::setPostProcessing(const PostProcessingSettings& postProcessing){
    PostProcessingSettings clamped = postProcessing;
    clamped.clamp();
    if (clamped == settings.postProcessing)
        return;
    settings.postProcessing = clamped;
    getSystem<RenderSystem>()->needUpdatePostProcessing();
}

void Scene::setPostProcessPassEnabled(unsigned int index, bool enabled){
    if (index >= settings.postProcess.size()){
        Log::error("Post-process pass %u does not exist, cannot enable it", index);
        return;
    }
    if (settings.postProcess[index].enabled == enabled)
        return;

    settings.postProcess[index].enabled = enabled;
    getSystem<RenderSystem>()->needReloadPostProcess();
}

bool Scene::isPostProcessPassEnabled(unsigned int index) const{
    if (index >= settings.postProcess.size())
        return false;

    return settings.postProcess[index].enabled;
}

bool Scene::canReceiveUIEvents(){
    switch (this->uiEventState) {
        case UIEventState::ENABLED:
            return true;
        case UIEventState::DISABLED:
            return false;
        case UIEventState::NOT_SET:
        default:
            Signature cameraSignature = getSignature(getCamera());
            if (cameraSignature.test(getComponentId<CameraComponent>())){
                return getComponent<CameraComponent>(getCamera()).type == CameraType::CAMERA_UI;
            }

            return false;
    }
}

UIEventState Scene::getEnableUIEvents() const{
    return this->uiEventState;
}

void Scene::setEnableUIEvents(UIEventState enableUIEvents){
    this->uiEventState = enableUIEvents;
}

void Scene::enableUIEvents(){
    this->uiEventState = UIEventState::ENABLED;
}

bool Scene::isEnableUIEvents() const{
    return this->uiEventState != UIEventState::DISABLED;
}

void Scene::setEnableUIEvents(bool enableUIEvents){
    this->uiEventState = enableUIEvents ? UIEventState::ENABLED : UIEventState::DISABLED;
}

void Scene::load(){
    if (camera == NULL_ENTITY){
        camera = createDefaultCamera();
    }

    for (auto const &pair: systems) {
        if (Engine::isViewLoaded()){
            pair.second->load();
        }
    }
}

SceneBvh& Scene::getPickBvh(){
    if (!pickBvh){
        pickBvh = std::make_unique<SceneBvh>(this);
    }
    return *pickBvh;
}

void Scene::notifyPickStructure(Signature components){
    if (pickBvh && pickBvh->tracks(components)){
        pickBvh->markStructureChanged();
    }
}

void Scene::notifyPickDirty(Entity entity){
    if (pickBvh){
        pickBvh->markDirty(entity);
    }
}

void Scene::destroy(){
    for (auto const& pair : systems){
        pair.second->destroy();
    }
}

void Scene::scheduleSubSystem(const std::string& name, SubSystem* system, const SystemDescriptor& descriptor){
    SystemDescriptor entry = descriptor;
    entry.name = name;

    entry.phase = SystemPhase::FixedUpdate;
    scheduler.add(entry, [system](SystemContext& context){ system->fixedUpdate(context.dt); }, system);
    entry.phase = SystemPhase::Update;
    scheduler.add(entry, [system](SystemContext& context){ system->update(context.dt); }, system);
    entry.phase = SystemPhase::Draw;
    scheduler.add(entry, [system](SystemContext&){ system->draw(); }, system);
}

ScheduledSystem& Scene::addSystemFunction(SystemDescriptor descriptor, std::function<void(SystemContext&)> function){
    return scheduler.add(std::move(descriptor), std::move(function));
}

SystemScheduler& Scene::getScheduler(){
    return scheduler;
}

void Scene::destroyHierarchyNow(Entity entity){
    ObjectManager::destroyImmediate(this, entity);
}

// Each phase starts with the holes of entities destroyed since closed (ComponentArray): the
// systems then find a settled array, and nothing destroyed before keeps a hole across frames
void Scene::draw(){
    compactComponents();
    scheduler.runPhase(*this, SystemPhase::Draw, 0.0);
    endFrame();
}

void Scene::update(double dt){
    compactComponents();
    scheduler.runPhase(*this, SystemPhase::Update, dt);
    scheduler.runPhase(*this, SystemPhase::PostUpdate, dt);
}

void Scene::fixedUpdate(double dt){
    compactComponents();
    scheduler.runPhase(*this, SystemPhase::FixedUpdate, dt);
}

void Scene::endFrame(){
    // what every enabled system's gate is past, else what is older than two frames
    Tick trimmed = frameEndTicks[0];
    Tick gate;
    if (scheduler.oldestGate(gate)){
        trimmed = gate;
    }
    if (trimmed > 0){
        trimRemovedComponents(trimmed);
    }
    frameEndTicks[0] = frameEndTicks[1];
    frameEndTicks[1] = getChangeClock().peek();
}

void Scene::updateCameraSize(){
    const Rect view = Engine::getViewRect();
    updateCanvasScale(view.getWidth(), view.getHeight(), (float)Engine::getCanvasWidth(), (float)Engine::getCanvasHeight(), Screen::getDpi());

    if (isUIScaled()){
        getSystem<RenderSystem>()->updateCameraSizeForCanvas(getCamera(), canvasScale.uiWidth, canvasScale.uiHeight);
    }else{
        getSystem<RenderSystem>()->updateCameraSize(getCamera());
    }
}

void Scene::setCanvasScaler(const CanvasScalerSettings& settings){
    this->settings.canvasScaler = settings;
    // a scene the Engine runs lays out in it from the next frame (the editor applies its own)
    if (Engine::isViewLoaded() && Engine::isSceneRunning(this))
        updateCameraSize();
}

const CanvasScalerSettings& Scene::getCanvasScaler() const{
    return settings.canvasScaler;
}

void Scene::setCanvasScalerUseProjectDefault(bool useProjectDefault){
    settings.canvasScalerUseProjectDefault = useProjectDefault;
    if (Engine::isViewLoaded() && Engine::isSceneRunning(this))
        updateCameraSize();
}

bool Scene::isCanvasScalerUseProjectDefault() const{
    return settings.canvasScalerUseProjectDefault;
}

const CanvasScalerSettings& Scene::getEffectiveCanvasScaler() const{
    return settings.canvasScalerUseProjectDefault ? Engine::getDefaultCanvasScaler() : settings.canvasScaler;
}

void Scene::updateCanvasScale(float viewWidth, float viewHeight, float canvasWidth, float canvasHeight, float screenDpi){
    canvasScale = CanvasScaler::compute(getEffectiveCanvasScaler(), viewWidth, viewHeight, canvasWidth, canvasHeight, screenDpi, canvasScale);
    // a FollowGameCanvas result before a valid view: nothing is scaled
    if (getEffectiveCanvasScaler().mode == UIScaleMode::FollowGameCanvas)
        canvasScale.scaled = false;
}

const CanvasScalerResult& Scene::getCanvasScale() const{
    return canvasScale;
}

bool Scene::isUIScaled() const{
    if (!canvasScale.scaled)
        return false;
    // (EntityRegistry has no const lookup)
    const CameraComponent* cameraComponent = const_cast<Scene*>(this)->findComponent<CameraComponent>(camera);
    return cameraComponent && cameraComponent->type == CameraType::CAMERA_UI;
}

bool Scene::isDocumentScaled() const{
    return canvasScale.scaled;
}

Vector2 Scene::canvasToUI(const Vector2& canvasPoint) const{
    const int canvasWidth = Engine::getCanvasWidth();
    const int canvasHeight = Engine::getCanvasHeight();
    if (!isUIScaled() || canvasWidth <= 0 || canvasHeight <= 0)
        return canvasPoint;
    return Vector2(canvasPoint.x * canvasScale.uiWidth / (float)canvasWidth, canvasPoint.y * canvasScale.uiHeight / (float)canvasHeight);
}

Vector2 Scene::uiToCanvas(const Vector2& uiPoint) const{
    const int canvasWidth = Engine::getCanvasWidth();
    const int canvasHeight = Engine::getCanvasHeight();
    if (!isUIScaled() || canvasScale.uiWidth <= 0.0f || canvasScale.uiHeight <= 0.0f)
        return uiPoint;
    return Vector2(uiPoint.x * (float)canvasWidth / canvasScale.uiWidth, uiPoint.y * (float)canvasHeight / canvasScale.uiHeight);
}
