// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleCanvasView.h"

#include "render/RenderUtil.h"
#include "titleart/TitleArtRenderHost.h"

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
#include "object/Sprite.h"
#include "render/FrameReadback.h"
#include "render/SystemRender.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"
#include "titleart/render/TitleGpu.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titledesigner;
namespace ta = jasperengine::titleart;

namespace {

    std::vector<TitleCanvasView*>& liveViews() {
        static std::vector<TitleCanvasView*> views;
        return views;
    }

    double msSince(std::chrono::steady_clock::time_point start) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    bool sameOverrides(const std::vector<ta::FieldOverride>& a, const std::vector<ta::FieldOverride>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++) {
            if (a[i].layerId != b[i].layerId || a[i].path != b[i].path || a[i].value != b[i].value) return false;
        }
        return true;
    }

    // a 2 x 2 checker; the sprite repeats it so a square is `checkerPixels` on screen
    const uint8_t CHECKER[16] = {204, 204, 204, 255, 255, 255, 255, 255, 255, 255, 255, 255, 204, 204, 204, 255};
    // solid backdrops: white tinted by the sprite's colour
    const uint8_t WHITE[4] = {255, 255, 255, 255};

    // `order`: the transparent queue sorts by the distance from the camera to each sprite's
    // origin (its bottom-left corner here), which a panned view or a title image a few pixels
    // larger than the stage can turn around; the queue order puts the title over the backdrop
    void setBlended(Scene* scene, Entity entity, int order) {
        MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
        mesh.autoTransparency = false;
        mesh.transparent = true;
        mesh.submeshes[0].material.alphaMode = MaterialAlphaMode::BLEND;
        mesh.submeshes[0].material.sortPriority = order;
    }

}

TitleCanvasView::TitleCanvasView(Project* project) : project(project) {
    createScene();
    liveViews().push_back(this);
}

TitleCanvasView::~TitleCanvasView() {
    auto& views = liveViews();
    views.erase(std::remove(views.begin(), views.end(), this), views.end());
    running.reset();
    shown.reset();
    destroyScene();
    target.destroy();
}

size_t TitleCanvasView::liveCount() {
    return liveViews().size();
}

void TitleCanvasView::createScene() {
    scene = new Scene(EntityPool::System);
    camera = new Camera(scene);
    camera->setType(CameraType::CAMERA_ORTHO);
    camera->setAutoResize(false);
    // the scene's camera: sprites build their quads for its type (a UI camera flips them)
    scene->setCamera(camera->getEntity());
    scene->setLightState(LightState::OFF);
    scene->setBackgroundColor(Vector4(0.13f, 0.13f, 0.14f, 1.0f));

    backdropSprite = new Sprite(scene);
    backdropSprite->setReceiveLights(false);
    backdropSprite->setCastShadows(false);
    titleSprite = new Sprite(scene);
    titleSprite->setReceiveLights(false);
    titleSprite->setCastShadows(false);
    titleSprite->setVisible(false);
    setBlended(scene, titleSprite->getEntity(), 1);
    setBlended(scene, backdropSprite->getEntity(), 0);
    backdropTextureKey.clear();
    sceneLoaded = false;
    sceneDirty = true;
    settle = SETTLE_FRAMES;
}

void TitleCanvasView::destroyScene() {
    delete titleSprite;
    delete backdropSprite;
    delete camera;
    delete scene;
    titleSprite = nullptr;
    backdropSprite = nullptr;
    camera = nullptr;
    scene = nullptr;
    shownFramebuffer = nullptr;
    sceneLoaded = false;
}

void TitleCanvasView::clear() {
    running.reset();
    shown.reset();
    hasWanted = false;
    status = {};
    if (titleSprite) titleSprite->setVisible(false);
    shownFramebuffer = nullptr;
    sceneDirty = true;
    settle = SETTLE_FRAMES;
}

bool TitleCanvasView::sameRender(const CanvasRenderRequest& a, const CanvasRenderRequest& b) {
    return a.doc == b.doc && a.docChange == b.docChange && a.pxScale == b.pxScale && a.region == b.region && a.draft == b.draft &&
           a.includeGuides == b.includeGuides && a.assets == b.assets && a.testFlags == b.testFlags && sameOverrides(a.overrides, b.overrides);
}

void TitleCanvasView::requestRender(const CanvasRenderRequest& request) {
    if (!request.doc) return;
    wanted = request;
    hasWanted = true;
}

void TitleCanvasView::requestDraw(const CanvasViewSetup& next, const CanvasBackdrop& nextBackdrop) {
    if (!(next == setup)) {
        if (next.width != setup.width || next.height != setup.height) settle = SETTLE_FRAMES;
        setup = next;
        sceneDirty = true;
    }
    if (!(nextBackdrop == backdrop)) {
        backdrop = nextBackdrop;
        sceneDirty = true;
    }
    drawRequested = true;
}

TextureRender* TitleCanvasView::texture() {
    if (draws == 0 || !target.isCreated()) return nullptr;
    return &target.getRender().getColorTexture();
}

Framebuffer* TitleCanvasView::framebuffer() {
    if (draws == 0 || !target.isCreated()) return nullptr;
    return &target;
}

void TitleCanvasView::invalidateForTesting() {
    forceInvalid = true;
}

void TitleCanvasView::startJob(const CanvasRenderRequest& request) {
    ta::RenderRequest render;
    render.doc = request.doc;
    render.pxScale = request.pxScale;
    render.region = request.region;
    render.includeGuides = request.includeGuides;
    render.overrides = request.overrides;
    render.testFlags = request.testFlags;
    render.output = ta::TitleOutputFormat::Srgb8;
    render.assets = request.assets;
    std::string error;
    running = TitleArtRenderHost::editor().begin(project, render, error);
    runningRequest = request;
    runningStart = std::chrono::steady_clock::now();
    if (!running) {
        status.error = error;
        hasWanted = false;
    } else {
        // a pass larger than the slice is cut into tiles, so a frame never waits on one
        running->splitOversizedSteps((uint32_t)std::max(1000.0, request.sliceBudgetMs * 1000.0));
    }
}

void TitleCanvasView::rebuildResources() {
    // the shown image's target is gone (or about to be): the quad must not sample it
    if (titleSprite) titleSprite->setVisible(false);
    if (titleSprite) titleSprite->setTexture((Framebuffer*)nullptr);
    shownFramebuffer = nullptr;
    shown.reset();
    running.reset();
    status.hasImage = false;
    status.rebuilding = true;
    // the request that was shown (or wanted) renders again
    if (!hasWanted && shownRequest.doc) {
        wanted = shownRequest;
        hasWanted = true;
    }
    sceneDirty = true;
    settle = SETTLE_FRAMES;
}

void TitleCanvasView::presentFinished() {
    shownMs = msSince(runningStart);
    shownRequest = runningRequest;
    shown = std::move(running);
    status.rebuilding = false;
    status.error.clear();
    status.hasImage = true;
    status.draft = shownRequest.draft;
    status.lastRenderMs = shownMs;
    status.lastPasses = shown->passesTotal();
    status.shownPxScale = shownRequest.pxScale;
    status.shownFrame = shown->image().frame;
    status.imageWidth = shown->image().width;
    status.imageHeight = shown->image().height;
    status.problems = shown->problems();
    status.nodeCosts = shown->nodeCosts();
    status.totalLayers = status.nodeCosts.size();
    status.cachedLayers = 0;
    for (const ta::NodeCost& cost : status.nodeCosts) {
        if (cost.passes == 0) status.cachedLayers++;
    }
    shownFramebuffer = nullptr;
    if (ta::TitleTarget* t = shown->image().target.get()) {
        if (auto* sokol = dynamic_cast<ta::SokolTitleTarget*>(t)) shownFramebuffer = &sokol->framebufferObject();
    }
    sceneDirty = true;
    settle = SETTLE_FRAMES;
}

void TitleCanvasView::step() {
    const uint64_t context = SystemRender::getContextId();
    if (forceInvalid || (contextId != 0 && context != contextId)) {
        forceInvalid = false;
        contextId = context;
        rebuildResources();
    }
    contextId = context;
    if (shown && !shown->image().target.get()) rebuildResources();

    if (hasWanted && !(shown && sameRender(wanted, shownRequest)) && !(running && sameRender(wanted, runningRequest))) {
        startJob(wanted);
    }
    if (hasWanted && shown && sameRender(wanted, shownRequest)) {
        running.reset();
        hasWanted = false;
    }
    status.busy = running != nullptr;
    if (!running) return;
    ta::StepBudget budget;
    budget.budgetUs = (uint32_t)std::max(500.0, runningRequest.sliceBudgetMs * 1000.0);
    const bool complete = running->step(budget);
    status.progress = running->progress();
    status.passesDone = running->passesDone();
    status.passesTotal = running->passesTotal();
    status.runningMs = msSince(runningStart);
    if (running->failed()) {
        const std::string error = running->error();
        status.error = error;
        if (error.rfind("device_lost", 0) == 0 || error.find("out_of_target_memory") != std::string::npos) {
            rebuildResources();
        } else {
            running.reset();
            hasWanted = false;
        }
        status.busy = false;
        return;
    }
    if (complete) {
        presentFinished();
        if (sameRender(wanted, shownRequest)) hasWanted = false;
        status.busy = false;
    }
}

void TitleCanvasView::updateScene() {
    if (!scene) return;
    const double halfW = setup.width / std::max(1e-6, (double)setup.zoom) * 0.5;
    const double halfH = setup.height / std::max(1e-6, (double)setup.zoom) * 0.5;
    camera->setPosition(Vector3((float)setup.centerX, (float)setup.centerY, 50.0f));
    camera->setTarget(Vector3((float)setup.centerX, (float)setup.centerY, 0.0f));
    camera->setUp(Vector3(0.0f, 1.0f, 0.0f));
    camera->setOrtho((float)-halfW, (float)halfW, (float)-halfH, (float)halfH, 0.1f, 100.0f);

    // the backdrop covers the stage
    const ta::TitleRect& stage = setup.stage;
    backdropSprite->setVisible(!stage.empty());
    backdropSprite->setPosition((float)stage.x, (float)stage.y, 0.0f);
    backdropSprite->setSize((unsigned int)std::max(1.0, std::round(stage.width)), (unsigned int)std::max(1.0, std::round(stage.height)));
    std::string key;
    switch (backdrop.kind) {
        case CanvasBackdrop::Kind::Checker: key = "checker"; break;
        case CanvasBackdrop::Kind::Image: key = "image:" + backdrop.imageKey; break;
        default: key = "solid"; break;
    }
    if (key != backdropTextureKey) {
        backdropTextureKey = key;
        if (backdrop.kind == CanvasBackdrop::Kind::Checker) {
            backdropSprite->setTexture("editor:titledesigner-checker", TextureData(2, 2, sizeof(CHECKER), ColorFormat::RGBA, 4, (void*)CHECKER));
        } else if (backdrop.kind == CanvasBackdrop::Kind::Image && !backdrop.imageRgba.empty()) {
            backdropSprite->setTexture("editor:titledesigner-backdrop:" + backdrop.imageKey,
                                       TextureData(backdrop.imageWidth, backdrop.imageHeight, (unsigned int)backdrop.imageRgba.size(),
                                                   ColorFormat::RGBA, 4, backdrop.imageRgba.data()));
        } else {
            backdropSprite->setTexture("editor:titledesigner-white", TextureData(1, 1, sizeof(WHITE), ColorFormat::RGBA, 4, (void*)WHITE));
        }
        MeshComponent& mesh = scene->getComponent<MeshComponent>(backdropSprite->getEntity());
        Texture& texture = mesh.submeshes[0].material.baseColorTexture;
        const bool checker = backdrop.kind == CanvasBackdrop::Kind::Checker;
        texture.setWrapU(checker ? TextureWrap::REPEAT : TextureWrap::CLAMP_TO_EDGE);
        texture.setWrapV(checker ? TextureWrap::REPEAT : TextureWrap::CLAMP_TO_EDGE);
        texture.setMinFilter(checker ? TextureFilter::NEAREST : TextureFilter::LINEAR);
        texture.setMagFilter(checker ? TextureFilter::NEAREST : TextureFilter::LINEAR);
    }
    switch (backdrop.kind) {
        case CanvasBackdrop::Kind::Checker: {
            backdropSprite->setColor(Vector4(1.0f, 1.0f, 1.0f, 1.0f));
            // squares of `checkerPixels` screen pixels: the 2 x 2 texture repeats across the stage
            const float squares = std::max(1.0f, setup.checkerPixels) / std::max(1e-6f, setup.zoom);
            backdropSprite->setTextureRect(0.0f, 0.0f, (float)(stage.width / (2.0 * squares)), (float)(stage.height / (2.0 * squares)));
            break;
        }
        case CanvasBackdrop::Kind::Black: backdropSprite->setColor(Vector4(0.0f, 0.0f, 0.0f, 1.0f)); break;
        case CanvasBackdrop::Kind::White: backdropSprite->setColor(Vector4(1.0f, 1.0f, 1.0f, 1.0f)); break;
        case CanvasBackdrop::Kind::Gray: backdropSprite->setColor(Vector4(0.5f, 0.5f, 0.5f, 1.0f)); break;
        case CanvasBackdrop::Kind::Color: backdropSprite->setColor(Vector4(backdrop.color.x, backdrop.color.y, backdrop.color.z, 1.0f)); break;
        case CanvasBackdrop::Kind::Image:
            backdropSprite->setColor(Vector4(1.0f, 1.0f, 1.0f, 1.0f));
            backdropSprite->setTextureRect(0.0f, 0.0f, 1.0f, 1.0f);
            break;
    }

    // the title quad: the shown image at its frame (canvas px, y down) in document space
    if (shown && shownFramebuffer && status.hasImage) {
        const ta::TitleImage& image = shown->image();
        const ta::TitleArtDoc& doc = *shownRequest.doc;
        const double left = image.frame.x - doc.canvasWidth() * 0.5;
        const double bottom = doc.canvasHeight() * 0.5 - (image.frame.y + image.frame.height);
        titleSprite->setTexture(shownFramebuffer);
        titleSprite->setSize((unsigned int)std::max(1, image.frame.width), (unsigned int)std::max(1, image.frame.height));
        titleSprite->setPosition((float)(setup.stageOffsetX + left * setup.stageScale), (float)(setup.stageOffsetY + bottom * setup.stageScale), 1.0f);
        titleSprite->setScale(Vector3((float)setup.stageScale, (float)setup.stageScale, 1.0f));
        titleSprite->setColor(Vector4(1.0f, 1.0f, 1.0f, setup.dim));
        titleSprite->setVisible(true);
        status.titleDrawn = true;
    } else {
        titleSprite->setVisible(false);
        status.titleDrawn = false;
    }
}

void TitleCanvasView::draw() {
    if (!Engine::isViewLoaded() || !scene || setup.width <= 0 || setup.height <= 0) return;
    if (!sceneLoaded) {
        scene->load();
        sceneLoaded = true;
    }
    if (sceneDirty) {
        updateScene();
        sceneDirty = false;
    }
    scene->getSystem<MeshSystem>()->update(0);
    scene->getSystem<RenderSystem>()->update(0);
    if (!target.isCreated() || target.getWidth() != (unsigned int)setup.width || target.getHeight() != (unsigned int)setup.height) {
        target.destroy();
        target.setWidth((unsigned int)setup.width);
        target.setHeight((unsigned int)setup.height);
        target.create();
    }
    Engine::ViewLayer layer;
    layer.scene = scene;
    layer.camera = camera->getEntity();
    layer.viewRect = Rect(0.0f, 0.0f, (float)setup.width, (float)setup.height);
    layer.postProcessing = false;
    Engine::drawView(TITLE_DESIGNER_VIEW_ID, &target, {layer});
    draws++;
    if (!scene->getSystem<RenderSystem>()->isAllLoaded()) settle = std::max(settle, 1);
}

void TitleCanvasView::requestSceneSnapshot(Scene* scene, Entity camera, int width, int height,
                                           std::function<void(std::vector<uint8_t>, int, int, std::string)> done) {
    snapshotScene = scene;
    snapshotCamera = camera;
    snapshotWidth = std::clamp(width, 16, 8192);
    snapshotHeight = std::clamp(height, 16, 8192);
    snapshotDone = std::move(done);
}

void TitleCanvasView::drawSnapshot() {
    Scene* scene = snapshotScene;
    snapshotScene = nullptr;
    auto done = std::move(snapshotDone);
    snapshotDone = nullptr;
    if (!Engine::isViewLoaded() || !scene || snapshotCamera == NULL_ENTITY) {
        if (done) done({}, 0, 0, "the scene has no camera to render through");
        return;
    }
    Framebuffer framebuffer;
    framebuffer.setWidth((unsigned int)snapshotWidth);
    framebuffer.setHeight((unsigned int)snapshotHeight);
    framebuffer.create();
    Engine::ViewLayer layer;
    layer.scene = scene;
    layer.camera = snapshotCamera;
    layer.viewRect = Rect(0.0f, 0.0f, (float)snapshotWidth, (float)snapshotHeight);
    layer.fixedResolution = true;
    layer.postProcessing = true;
    Engine::drawView(GAME_VIEW_ID, &framebuffer, {layer});
    std::vector<uint8_t> rgba;
    std::string error;
    const bool ok = FrameReadback::readFramebuffer(framebuffer, rgba, &error);
    framebuffer.destroy();
    if (done) done(ok ? std::move(rgba) : std::vector<uint8_t>{}, ok ? snapshotWidth : 0, ok ? snapshotHeight : 0, ok ? std::string() : error);
}

void TitleCanvasView::processFrame() {
    const std::vector<TitleCanvasView*> views = liveViews();
    for (TitleCanvasView* view : views) {
        if (view->snapshotScene) view->drawSnapshot();
        view->step();
        if (!view->drawRequested) continue;
        view->drawRequested = false;
        if (!view->sceneDirty && view->settle <= 0 && view->draws > 0) continue;
        if (view->sceneDirty) view->settle = std::max(view->settle, 1);
        view->settle = std::max(0, view->settle - 1);
        view->draw();
    }
}

void TitleCanvasView::releaseGpuResources() {
    running.reset();
    shown.reset();
    shownFramebuffer = nullptr;
    if (titleSprite) titleSprite->setVisible(false);
    if (scene && sceneLoaded) scene->destroy();
    target.destroy();
    sceneLoaded = false;
    backdropTextureKey.clear();
    draws = 0;
    status.hasImage = false;
    sceneDirty = true;
    settle = SETTLE_FRAMES;
}

void TitleCanvasView::releaseGpu() {
    for (TitleCanvasView* view : liveViews()) view->releaseGpuResources();
}
