// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VatPreviewRender.h"

#include "Engine.h"
#include "Project.h"
#include "animation/ClipConverters.h"
#include "Scene.h"
#include "component/AnimationPlayerComponent.h"
#include "component/VertexAnimatorComponent.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Model.h"
#include "render/RenderUtil.h"
#include "subsystem/ActionSystem.h"
#include "subsystem/LatticeSystem.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/VertexAnimatorSystem.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <vector>

using namespace jasperengine;
namespace fs = std::filesystem;

namespace {

    std::vector<editor::VatPreviewView*>& liveViews(){
        static std::vector<editor::VatPreviewView*> views;
        return views;
    }

    std::string lowerExtension(const std::string& path){
        std::string extension = editor::FileUtils::pathToUtf8(editor::FileUtils::pathFromUtf8(path).extension());
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return extension;
    }

    // the three-quarter view: from the front-right, a little above
    const Vector3 VIEW_DIRECTION = Vector3(-0.55f, -0.3f, -0.78f).normalized();

}

editor::VatPreviewView::VatPreviewView(){
    createScene();
    liveViews().push_back(this);
}

editor::VatPreviewView::~VatPreviewView(){
    auto& views = liveViews();
    views.erase(std::remove(views.begin(), views.end(), this), views.end());
    destroyScene();
    target.destroy();
}

void editor::VatPreviewView::createScene(){
    scene = new Scene(EntityPool::System);
    camera = new Camera(scene);
    keyLight = new Light(scene);
    fillLight = new Light(scene);

    camera->setType(CameraType::CAMERA_ORTHO);
    camera->setAutoResize(false);

    keyLight->setType(LightType::DIRECTIONAL);
    keyLight->setDirection(Vector3(-0.45f, -0.6f, -0.65f));
    keyLight->setIntensity(2.0f);
    keyLight->setShadows(false);
    fillLight->setType(LightType::DIRECTIONAL);
    fillLight->setDirection(Vector3(0.6f, -0.25f, -0.75f));
    fillLight->setIntensity(0.7f);
    fillLight->setShadows(false);

    scene->setGlobalIllumination(0.3f, Vector3(1.0f, 1.0f, 1.0f));
    scene->setBackgroundColor(Vector4(0.16f, 0.16f, 0.17f, 1.0f));
    sceneLoaded = false;
    settle = SETTLE_FRAMES;
}

void editor::VatPreviewView::destroyScene(){
    if (scene && scene->getSystem<ActionSystem>() && !session.written.empty()){
        scene->getSystem<ActionSystem>()->restorePlayerPreview(session);
    }
    session.reset();
    delete model;
    delete fillLight;
    delete keyLight;
    delete camera;
    delete scene;
    model = nullptr;
    fillLight = nullptr;
    keyLight = nullptr;
    camera = nullptr;
    scene = nullptr;
    root = NULL_ENTITY;
    vat = false;
    unlit = false;
    file.clear();
    vatFile.clear();
    posedClip = nullptr;
    heldClip.clear();
    heldTime = -1.0f;
    sceneLoaded = false;
}

void editor::VatPreviewView::clear(){
    destroyScene();
    createScene();
}

bool editor::VatPreviewView::loadSource(Project* project, const std::string& path, std::string& error){
    clear();
    if (!project){
        error = "no project is open";
        return false;
    }
    const fs::path given = editor::FileUtils::pathFromUtf8(path);
    const fs::path absolute = (given.is_absolute() ? given : project->getProjectPath() / given).lexically_normal();
    const std::string extension = lowerExtension(path);
    if (extension == ".prefab"){
        root = project->instantiatePrefabPreview(scene, absolute);
        if (root == NULL_ENTITY){
            error = "the prefab '" + path + "' could not be instantiated";
            return false;
        }
    }else{
        const std::string asset = ClipConverters::assetRelative(project, absolute);
        project->prepareModelFile(asset);
        model = new Model(scene);
        root = model->getEntity();
        // the node hierarchy the clips address (as the baker's scratch scene loads it)
        scene->addComponent<AnimationPlayerComponent>(root, {});
        if (!model->loadModel(editor::FileUtils::pathToUtf8(absolute))){
            delete model;
            model = nullptr;
            root = NULL_ENTITY;
            error = "the model '" + path + "' could not be loaded";
            return false;
        }
    }
    file = path;
    settle = SETTLE_FRAMES;
    return true;
}

bool editor::VatPreviewView::loadVat(const std::string& vatPath, const std::string& meshPath, std::string& error, bool unlitColors){
    clear();
    model = new Model(scene);
    root = model->getEntity();
    if (!model->loadModel(meshPath)){
        delete model;
        model = nullptr;
        root = NULL_ENTITY;
        error = "the baked mesh '" + meshPath + "' could not be loaded";
        return false;
    }
    VertexAnimatorComponent animator;
    animator.animation = vatPath;
    animator.playAutomatically = false;
    animator.randomStart = false;
    animator.speed = 0.0f;
    scene->addComponent<VertexAnimatorComponent>(root, animator);
    if (unlitColors) model->setReceiveLights(false);
    vat = true;
    unlit = unlitColors;
    file = meshPath;
    vatFile = vatPath;
    settle = SETTLE_FRAMES;
    return true;
}

void editor::VatPreviewView::poseSource(const std::shared_ptr<const AnimationClip>& clip, float time){
    if (!scene || root == NULL_ENTITY || vat) return;
    std::shared_ptr<ActionSystem> actions = scene->getSystem<ActionSystem>();
    // another clip starts from the rest (what the previous one moved and this one doesn't stays posed otherwise)
    if (!clip || (posedClip && posedClip != clip.get())){
        if (!session.written.empty()) actions->restorePlayerPreview(session);
        session.reset();
        posedClip = nullptr;
    }
    if (clip){
        ActionSystem::PlayerPreview preview;
        ActionSystem::PlayerPreviewState state;
        state.clip = clip;
        state.time = std::max(0.0f, time);
        state.wrap = ClipWrapMode::ClampForever;
        preview.states.push_back(state);
        if (session.root != root){
            session.reset();
            session.root = root;
        }
        actions->previewPlayer(root, preview, session);
        posedClip = clip.get();
    }
    settle = SETTLE_FRAMES;
}

void editor::VatPreviewView::poseVat(const std::string& clip, float time){
    if (!scene || root == NULL_ENTITY || !vat) return;
    VertexAnimatorComponent* animator = scene->findComponent<VertexAnimatorComponent>(root);
    if (!animator) return;
    if (clip == heldClip && std::fabs(time - heldTime) < 1e-6f) return;
    if (clip != heldClip){
        VertexAnimatorRequest play;
        play.kind = VertexAnimatorRequest::Kind::Play;
        play.clip = clip;
        animator->requests.push_back(play);
        VertexAnimatorRequest hold;
        hold.kind = VertexAnimatorRequest::Kind::SetSpeed;
        hold.value = 0.0f;
        animator->requests.push_back(hold);
    }
    VertexAnimatorRequest at;
    at.kind = VertexAnimatorRequest::Kind::SetTime;
    at.value = std::max(0.0f, time);
    animator->requests.push_back(at);
    heldClip = clip;
    heldTime = time;
    // applied now when the asset is loaded (else at the next update)
    scene->getSystem<VertexAnimatorSystem>()->update(0.0);
    settle = SETTLE_FRAMES;
}

bool editor::VatPreviewView::vatActive() const{
    if (!scene || root == NULL_ENTITY || !vat) return false;
    const VertexAnimatorComponent* animator = scene->findComponent<VertexAnimatorComponent>(root);
    return animator && animator->gpuActive;
}

std::string editor::VatPreviewView::vatError() const{
    if (!scene || root == NULL_ENTITY || !vat) return "";
    const VertexAnimatorComponent* animator = scene->findComponent<VertexAnimatorComponent>(root);
    return animator ? animator->loadError : std::string();
}

void editor::VatPreviewView::setFraming(const Vector3& min, const Vector3& max){
    if ((min - frameMin).length() < 1e-6f && (max - frameMax).length() < 1e-6f) return;
    frameMin = min;
    frameMax = max;
    settle = SETTLE_FRAMES;
}

void editor::VatPreviewView::applyCamera(int width, int height){
    const Vector3 center = (frameMin + frameMax) * 0.5f;
    const float radius = std::max(0.05f, (frameMax - frameMin).length() * 0.5f);
    const float aspect = height > 0 ? (float)width / (float)height : 1.0f;
    float halfHeight = radius * 1.08f;
    float halfWidth = halfHeight * aspect;
    if (aspect < 1.0f){
        halfWidth = radius * 1.08f;
        halfHeight = halfWidth / aspect;
    }
    const float reach = radius * 6.0f + 1.0f;
    camera->setPosition(center - VIEW_DIRECTION * reach);
    camera->setTarget(center);
    camera->setUp(Vector3(0.0f, 1.0f, 0.0f));
    camera->setOrtho(-halfWidth, halfWidth, -halfHeight, halfHeight, 0.01f, reach * 2.0f);
}

void editor::VatPreviewView::requestDraw(int width, int height){
    width = std::max(16, width);
    height = std::max(16, height);
    if (width != requestedWidth || height != requestedHeight) settle = SETTLE_FRAMES;
    requestedWidth = width;
    requestedHeight = height;
    requested = true;
}

TextureRender* editor::VatPreviewView::texture(){
    if (drawCount == 0 || !target.isCreated()) return nullptr;
    return &target.getRender().getColorTexture();
}

void editor::VatPreviewView::draw(){
    if (!Engine::isViewLoaded() || !scene) return;
    if (!sceneLoaded){
        scene->load();
        sceneLoaded = true;
    }
    // twice: a skinned mesh can come before its bones in the update
    for (int pass = 0; pass < 2; pass++){
        scene->getSystem<MeshSystem>()->update(0);
        scene->getSystem<LatticeSystem>()->update(0);
        scene->getSystem<VertexAnimatorSystem>()->update(0);
        scene->getSystem<RenderSystem>()->update(0);
    }
    const int width = requestedWidth;
    const int height = requestedHeight;
    if (!target.isCreated() || target.getWidth() != (unsigned int)width || target.getHeight() != (unsigned int)height){
        target.destroy();
        target.setWidth((unsigned int)width);
        target.setHeight((unsigned int)height);
        target.create();
    }
    applyCamera(width, height);
    Engine::ViewLayer layer;
    layer.scene = scene;
    layer.camera = camera->getEntity();
    layer.viewRect = Rect(0.0f, 0.0f, (float)width, (float)height);
    layer.postProcessing = false;
    Engine::drawView(VAT_PREVIEW_VIEW_ID, &target, {layer});
    drawCount++;
    // still loading (meshes, shaders, the VAT texture): keep drawing
    if (!scene->getSystem<RenderSystem>()->isAllLoaded()) settle = std::max(settle, 1);
}

void editor::VatPreviewView::processFrame(){
    const std::vector<VatPreviewView*> views = liveViews();
    for (VatPreviewView* view : views){
        if (!view->requested) continue;
        view->requested = false;
        if (view->settle <= 0 && view->drawCount > 0) continue;
        view->settle = std::max(0, view->settle - 1);
        view->draw();
    }
}

void editor::VatPreviewView::releaseGpuResources(){
    if (scene && sceneLoaded) scene->destroy();
    target.destroy();
    sceneLoaded = false;
    drawCount = 0;
    settle = SETTLE_FRAMES;
}

void editor::VatPreviewView::releaseGpu(){
    for (VatPreviewView* view : liveViews()) view->releaseGpuResources();
}
