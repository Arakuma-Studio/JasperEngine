// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PoseRender.h"

#include "Engine.h"
#include "component/BoneComponent.h"
#include "component/Transform.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "render/ObjectRender.h"
#include "render/RenderUtil.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"
#include "util/GraphicUtils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>

using namespace jasperengine;

namespace {

    // every live PoseRender, for processFrame and to notice one destroyed by a callback
    std::vector<editor::PoseRender*>& liveRenders(){
        static std::vector<editor::PoseRender*> renders;
        return renders;
    }

    bool isLive(const editor::PoseRender* render){
        const auto& renders = liveRenders();
        return std::find(renders.begin(), renders.end(), render) != renders.end();
    }

    std::function<void()>& frameRequester(){
        static std::function<void()> requester;
        return requester;
    }

    bool& inEditorFrame(){
        static bool inFrame = false;
        return inFrame;
    }

    double nowSeconds(){
        using Clock = std::chrono::steady_clock;
        return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
    }

    // a unit vector perpendicular to v
    Vector3 anyPerpendicular(const Vector3& v){
        const Vector3 axis = std::fabs(v.y) < 0.9f ? Vector3(0.0f, 1.0f, 0.0f) : Vector3(1.0f, 0.0f, 0.0f);
        return v.crossProduct(axis).normalized();
    }

    // 1, 2 or 5 times a power of ten, the nearest to value
    float niceStep(float value){
        if (!(value > 0.0f)){
            return 1.0f;
        }
        const float power = std::pow(10.0f, std::floor(std::log10(value)));
        const float mantissa = value / power;
        const float nice = mantissa < 1.5f ? 1.0f : (mantissa < 3.5f ? 2.0f : (mantissa < 7.5f ? 5.0f : 10.0f));
        return nice * power;
    }

    const Vector4 FLOOR_COLOR(0.11f, 0.11f, 0.12f, 1.0f);
    const Vector4 GRID_COLOR(0.22f, 0.22f, 0.24f, 1.0f);
    // grid lines per side of the floor at most
    const int MAX_GRID_LINES = 64;

}

editor::PoseRender::PoseRender(){
    createScene();
    liveRenders().push_back(this);
}

editor::PoseRender::~PoseRender(){
    auto& renders = liveRenders();
    renders.erase(std::remove(renders.begin(), renders.end(), this), renders.end());

    // the callbacks of queued renders still hear about them (ok false)
    cancelRenders();

    destroyScene();
    target.destroy();
}

// ---- scene ----

void editor::PoseRender::createScene(){
    scene = new Scene(EntityPool::System);
    // the scene's own camera stays the default one: the pose camera draws as a secondary view
    // (Engine::drawView), with its own lighting and culling, never as the scene's frame
    camera = new Camera(scene);
    keyLight = new Light(scene);
    fillLight = new Light(scene);
    floorMesh = new Primitive(scene);
    floorGrid = new LineRenderer(scene);

    camera->setType(CameraType::CAMERA_ORTHO);
    camera->setAutoResize(false);

    keyLight->setType(LightType::DIRECTIONAL);
    keyLight->setDirection(keyDirection);
    keyLight->setIntensity(2.0f);
    keyLight->setShadows(false);

    fillLight->setType(LightType::DIRECTIONAL);
    fillLight->setDirection(fillDirection);
    fillLight->setIntensity(0.7f);
    fillLight->setShadows(false);

    scene->setGlobalIllumination(0.3f, Vector3(1.0f, 1.0f, 1.0f));

    floorMesh->createPlane(1.0f, 1.0f);
    floorMesh->setColor(FLOOR_COLOR);
    floorMesh->setReceiveLights(false);
    floorMesh->setCastShadows(false);
    floorMesh->setReceiveShadows(false);

    sceneLoaded = false;
    viewWarmed = false;
    poseDirty = true;
    floorBuilt = false;
    appliedShaded = true;
    appliedModelVisible = true;
}

void editor::PoseRender::destroyScene(){
    delete model;
    delete floorGrid;
    delete floorMesh;
    delete fillLight;
    delete keyLight;
    delete camera;
    delete scene;

    model = nullptr;
    floorGrid = nullptr;
    floorMesh = nullptr;
    fillLight = nullptr;
    keyLight = nullptr;
    camera = nullptr;
    scene = nullptr;

    bones.clear();
    modelMeshes.clear();
    sceneLoaded = false;
    viewWarmed = false;
    floorBuilt = false;
}

void editor::PoseRender::releaseGpuResources(){
    if (scene && sceneLoaded){
        scene->destroy();
    }
    target.destroy();
    sceneLoaded = false;
    viewWarmed = false;
    poseDirty = true;
}

bool editor::PoseRender::ensureLoaded(){
    if (!Engine::isViewLoaded() || !scene){
        return false;
    }
    if (!sceneLoaded){
        scene->load();
        sceneLoaded = true;
    }
    return true;
}

void editor::PoseRender::updateScene(){
    if (!ensureLoaded()){
        return;
    }
    // twice: a skinned mesh can come before its bones in the update, so its posed bounds (used
    // for culling and computeBounds) settle on the second pass
    for (int pass = 0; pass < 2; pass++){
        scene->getSystem<MeshSystem>()->update(0);
        scene->getSystem<RenderSystem>()->update(0);
    }
    poseDirty = false;
}

void editor::PoseRender::updatePose(){
    if (poseDirty){
        updateScene();
    }
}

// ---- model ----

bool editor::PoseRender::setModel(const std::string& path, std::string* error){
    cancelRenders();

    // a fresh scene: nothing of a previous model stays behind
    destroyScene();
    createScene();
    modelPath.clear();

    if (path.empty()){
        if (error) *error = "No model file given.";
        return false;
    }

    model = new Model(scene);
    if (!model->loadModel(path)){
        delete model;
        model = nullptr;
        if (error) *error = "Could not load the model '" + path + "'.";
        return false;
    }
    modelPath = path;

    collectModel();

    // the default framing: the rest pose from the front, standing on the floor
    AABB bounds;
    if (computeBounds(bounds)){
        style.floorHeight = bounds.getMinimum().y;
        setView(Vector3(0.0f, 0.0f, -1.0f), Vector3(0.0f, 1.0f, 0.0f), bounds, DEFAULT_MARGIN);
    }
    return true;
}

void editor::PoseRender::clearModel(){
    cancelRenders();
    destroyScene();
    createScene();
    modelPath.clear();
}

Entity editor::PoseRender::getModelEntity() const{
    return model ? model->getEntity() : NULL_ENTITY;
}

void editor::PoseRender::collectModel(){
    bones.clear();
    modelMeshes.clear();
    if (!model){
        return;
    }

    const Entity modelEntity = model->getEntity();
    ModelComponent& modelComponent = scene->getComponent<ModelComponent>(modelEntity);

    // bonesIdMapping is keyed by glTF node index: the bones in file order
    std::vector<Entity> boneEntities;
    for (const auto& bone : modelComponent.bonesIdMapping){
        boneEntities.push_back(bone.second);
    }
    for (Entity entity : boneEntities){
        BoneInfo info;
        info.entity = entity;
        info.name = scene->getEntityName(entity);
        const Transform& transform = scene->getComponent<Transform>(entity);
        info.restPosition = transform.position;
        info.restRotation = transform.rotation;
        info.restScale = transform.scale;

        // the nearest ancestor that is a bone
        Entity parent = transform.parent;
        while (parent != NULL_ENTITY && parent != modelEntity){
            auto it = std::find(boneEntities.begin(), boneEntities.end(), parent);
            if (it != boneEntities.end()){
                info.parent = (int)(it - boneEntities.begin());
                break;
            }
            const Transform* parentTransform = scene->findComponent<Transform>(parent);
            parent = parentTransform ? parentTransform->parent : NULL_ENTITY;
        }
        bones.push_back(info);
    }

    // the model's meshes: its own and its mesh node children
    const size_t lastIndex = scene->findBranchLastIndex(modelEntity);
    auto transforms = scene->getComponentArray<Transform>();
    const size_t startIndex = transforms->getIndex(modelEntity);
    for (size_t i = startIndex; i <= lastIndex && i < transforms->size(); i++){
        const Entity entity = transforms->getEntity(i);
        if (scene->findComponent<MeshComponent>(entity)){
            modelMeshes.push_back(entity);
        }
    }

    // what the file set up: shaded unless every mesh is unlit, visible as loaded
    appliedShaded = modelMeshes.empty();
    for (Entity entity : modelMeshes){
        if (scene->getComponent<MeshComponent>(entity).receiveLights){
            appliedShaded = true;
        }
    }
    appliedModelVisible = scene->getComponent<Transform>(modelEntity).visible;
}

// ---- skeleton ----

int editor::PoseRender::findBone(const std::string& name) const{
    for (size_t i = 0; i < bones.size(); i++){
        if (bones[i].name == name){
            return (int)i;
        }
    }
    return -1;
}

bool editor::PoseRender::setBoneLocal(int index, const Vector3& position, const Quaternion& rotation, const Vector3& scale){
    if (index < 0 || index >= (int)bones.size()){
        return false;
    }
    Transform& transform = scene->getComponent<Transform>(bones[index].entity);
    if (transform.position != position || transform.rotation != rotation || transform.scale != scale){
        transform.position = position;
        transform.rotation = rotation;
        transform.scale = scale;
        transform.needUpdate = true;
        poseDirty = true;
    }
    return true;
}

bool editor::PoseRender::setBoneRotation(int index, const Quaternion& rotation){
    if (index < 0 || index >= (int)bones.size()){
        return false;
    }
    const Transform& transform = scene->getComponent<Transform>(bones[index].entity);
    return setBoneLocal(index, transform.position, rotation, transform.scale);
}

bool editor::PoseRender::getBoneLocal(int index, Vector3& position, Quaternion& rotation, Vector3& scale) const{
    if (index < 0 || index >= (int)bones.size()){
        return false;
    }
    const Transform& transform = scene->getComponent<Transform>(bones[index].entity);
    position = transform.position;
    rotation = transform.rotation;
    scale = transform.scale;
    return true;
}

void editor::PoseRender::resetToRest(){
    for (size_t i = 0; i < bones.size(); i++){
        setBoneLocal((int)i, bones[i].restPosition, bones[i].restRotation, bones[i].restScale);
    }
}

Vector3 editor::PoseRender::boneWorldPosition(int index){
    if (index < 0 || index >= (int)bones.size() || !ensureLoaded()){
        return Vector3(0.0f, 0.0f, 0.0f);
    }
    updatePose();
    return scene->getComponent<Transform>(bones[index].entity).worldPosition;
}

bool editor::PoseRender::boneWorldPositions(std::vector<Vector3>& out){
    out.clear();
    if (!ensureLoaded()){
        return false;
    }
    updatePose();
    out.reserve(bones.size());
    for (const BoneInfo& bone : bones){
        out.push_back(scene->getComponent<Transform>(bone.entity).worldPosition);
    }
    return true;
}

bool editor::PoseRender::computeBounds(AABB& out){
    out.setNull();
    if (!model || !ensureLoaded()){
        return false;
    }
    updatePose();

    for (Entity entity : modelMeshes){
        const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        if (mesh && !mesh->worldAABB.isNull() && !mesh->worldAABB.isInfinite()){
            out.merge(mesh->worldAABB);
        }
    }
    if (out.isNull()){
        for (const BoneInfo& bone : bones){
            out.merge(scene->getComponent<Transform>(bone.entity).worldPosition);
        }
    }
    return !out.isNull();
}

// ---- camera ----

void editor::PoseRender::setView(const Vector3& viewDirection, const Vector3& screenUp, const AABB& frame, float margin){
    ViewState state;

    Vector3 direction = viewDirection;
    if (direction.squaredLength() < 1e-12f){
        direction = Vector3(0.0f, 0.0f, -1.0f);
    }
    direction.normalize();
    Vector3 right = direction.crossProduct(screenUp);
    if (right.squaredLength() < 1e-12f){
        // up along the view direction: any perpendicular up
        right = direction.crossProduct(anyPerpendicular(direction));
    }
    right.normalize();
    state.direction = direction;
    state.right = right;
    state.up = right.crossProduct(direction).normalized();

    AABB box = frame;
    if (box.isNull() || box.isInfinite()){
        box = AABB(-1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f);
    }
    state.center = box.getCenter();

    float halfRight = 0.0f;
    float halfUp = 0.0f;
    float radius = 0.0f;
    const Vector3* corners = box.getCorners();
    for (int i = 0; i < 8; i++){
        const Vector3 offset = corners[i] - state.center;
        halfRight = std::max(halfRight, std::fabs(offset.dotProduct(state.right)));
        halfUp = std::max(halfUp, std::fabs(offset.dotProduct(state.up)));
        radius = std::max(radius, offset.length());
    }
    // a flat or point box still gets a visible frame
    const float minimum = std::max(radius * 0.05f, 1e-3f);
    const float grow = 1.0f + std::max(margin, 0.0f);
    state.halfRight = std::max(halfRight, minimum) * grow;
    state.halfUp = std::max(halfUp, minimum) * grow;
    state.radius = std::max(radius, 1e-3f);

    view = state;
}

void editor::PoseRender::setView(const Vector3& viewDirection, const Vector3& screenUp, const std::vector<Vector3>& points, float margin){
    if (points.empty()){
        setView(viewDirection, screenUp, AABB(), margin);
        return;
    }
    ViewState state;

    Vector3 direction = viewDirection;
    if (direction.squaredLength() < 1e-12f){
        direction = Vector3(0.0f, 0.0f, -1.0f);
    }
    direction.normalize();
    Vector3 right = direction.crossProduct(screenUp);
    if (right.squaredLength() < 1e-12f){
        right = direction.crossProduct(anyPerpendicular(direction));
    }
    right.normalize();
    state.direction = direction;
    state.right = right;
    state.up = right.crossProduct(direction).normalized();

    // extents along the view axes; the centre is the middle of each
    float low[3] = {0.0f, 0.0f, 0.0f};
    float high[3] = {0.0f, 0.0f, 0.0f};
    const Vector3 axes[3] = {state.right, state.up, state.direction};
    for (size_t i = 0; i < points.size(); i++){
        for (int a = 0; a < 3; a++){
            const float d = points[i].dotProduct(axes[a]);
            if (i == 0 || d < low[a]) low[a] = d;
            if (i == 0 || d > high[a]) high[a] = d;
        }
    }
    state.center = axes[0] * ((low[0] + high[0]) * 0.5f) + axes[1] * ((low[1] + high[1]) * 0.5f) +
                   axes[2] * ((low[2] + high[2]) * 0.5f);

    float radius = 0.0f;
    for (const Vector3& point : points){
        radius = std::max(radius, (point - state.center).length());
    }
    const float minimum = std::max(radius * 0.05f, 1e-3f);
    const float grow = 1.0f + std::max(margin, 0.0f);
    state.halfRight = std::max((high[0] - low[0]) * 0.5f, minimum) * grow;
    state.halfUp = std::max((high[1] - low[1]) * 0.5f, minimum) * grow;
    state.radius = std::max(radius, 1e-3f);

    view = state;
}

void editor::PoseRender::setViewSize(int width, int height){
    viewWidth = std::max(width, 1);
    viewHeight = std::max(height, 1);
}

void editor::PoseRender::halfExtents(const ViewState& state, int width, int height, float& halfWidth, float& halfHeight) const{
    const float aspect = (float)std::max(width, 1) / (float)std::max(height, 1);
    if (state.halfRight / state.halfUp > aspect){
        halfWidth = state.halfRight;
        halfHeight = state.halfRight / aspect;
    }else{
        halfHeight = state.halfUp;
        halfWidth = state.halfUp * aspect;
    }
}

Vector2 editor::PoseRender::project(const Vector3& world) const{
    float halfWidth = 1.0f;
    float halfHeight = 1.0f;
    halfExtents(view, viewWidth, viewHeight, halfWidth, halfHeight);
    const Vector3 offset = world - view.center;
    const float x = offset.dotProduct(view.right);
    const float y = offset.dotProduct(view.up);
    return Vector2((x + halfWidth) / (2.0f * halfWidth) * viewWidth,
                   (halfHeight - y) / (2.0f * halfHeight) * viewHeight);
}

float editor::PoseRender::getPixelsPerUnit() const{
    float halfWidth = 1.0f;
    float halfHeight = 1.0f;
    halfExtents(view, viewWidth, viewHeight, halfWidth, halfHeight);
    return viewWidth / (2.0f * halfWidth);
}

void editor::PoseRender::applyCamera(const ViewState& state, int width, int height){
    float halfWidth = 1.0f;
    float halfHeight = 1.0f;
    halfExtents(state, width, height, halfWidth, halfHeight);

    // far enough back that the floor (8 frame radii wide) and the model stay inside the depth range
    const float reach = state.radius * 8.0f + 1.0f;
    const Vector3 position = state.center - state.direction * reach;
    camera->setPosition(position);
    camera->setTarget(state.center);
    camera->setUp(state.up);
    camera->setOrtho(-halfWidth, halfWidth, -halfHeight, halfHeight, 0.01f, reach * 2.0f);
}

// ---- look ----

void editor::PoseRender::setStyle(const Style& newStyle){
    style = newStyle;
}

void editor::PoseRender::setLighting(const Vector3& key, const Vector3& fill){
    keyDirection = key;
    fillDirection = fill;
}

void editor::PoseRender::applyToScene(const RenderState& state){
    scene->setBackgroundColor(state.style.background);
    keyLight->setDirection(state.keyDirection);
    fillLight->setDirection(state.fillDirection);
    floorMesh->setVisible(state.style.floorVisible);
    floorGrid->setVisible(state.style.floorVisible);
    applyFloor();

    if (model){
        // only on a change: receiving lights is a shader variant, so the meshes reload
        if (appliedShaded != state.style.shaded){
            for (Entity entity : modelMeshes){
                Mesh mesh(scene, entity);
                mesh.setReceiveLights(state.style.shaded);
            }
            appliedShaded = state.style.shaded;
        }
        if (appliedModelVisible != state.style.modelVisible){
            model->setVisible(state.style.modelVisible);
            appliedModelVisible = state.style.modelVisible;
        }
    }

    applyCamera(state.view, state.width, state.height);
}

void editor::PoseRender::applyFloor(){
    if (floorBuilt && appliedFloorCenter == view.center && appliedFloorRadius == view.radius &&
            appliedFloorHeight == style.floorHeight){
        return;
    }
    floorBuilt = true;
    appliedFloorCenter = view.center;
    appliedFloorRadius = view.radius;
    appliedFloorHeight = style.floorHeight;

    // centred under the framed box, wide enough to reach past the image from any view
    const float size = view.radius * 8.0f;
    floorMesh->setPosition(view.center.x, style.floorHeight, view.center.z);
    floorMesh->setScale(Vector3(size, 1.0f, size));

    // grid lines a little above the quad, about 8 to a frame
    floorGrid->clearLines();
    float step = niceStep(view.radius * 2.0f / 8.0f);
    while (size / step > MAX_GRID_LINES){
        step *= 2.0f;
    }
    const float lift = std::max(view.radius * 1e-3f, 1e-4f);
    const float y = style.floorHeight + lift;
    const float half = size * 0.5f;
    const float startX = std::ceil((view.center.x - half) / step) * step;
    const float startZ = std::ceil((view.center.z - half) / step) * step;
    for (float x = startX; x <= view.center.x + half; x += step){
        floorGrid->addLine(Vector3(x, y, view.center.z - half), Vector3(x, y, view.center.z + half), GRID_COLOR);
    }
    for (float z = startZ; z <= view.center.z + half; z += step){
        floorGrid->addLine(Vector3(view.center.x - half, y, z), Vector3(view.center.x + half, y, z), GRID_COLOR);
    }
}

// ---- rendering ----

editor::PoseRender::RenderState editor::PoseRender::captureState(int width, int height) const{
    RenderState state;
    state.pose.reserve(bones.size());
    for (const BoneInfo& bone : bones){
        const Transform& transform = scene->getComponent<Transform>(bone.entity);
        state.pose.push_back({transform.position, transform.rotation, transform.scale});
    }
    state.view = view;
    state.style = style;
    state.keyDirection = keyDirection;
    state.fillDirection = fillDirection;
    state.width = std::max(width, 1);
    state.height = std::max(height, 1);
    return state;
}

void editor::PoseRender::setState(const RenderState& state){
    const size_t count = std::min(state.pose.size(), bones.size());
    for (size_t i = 0; i < count; i++){
        setBoneLocal((int)i, state.pose[i].position, state.pose[i].rotation, state.pose[i].scale);
    }
    view = state.view;
    style = state.style;
    keyDirection = state.keyDirection;
    fillDirection = state.fillDirection;
    viewWidth = state.width;
    viewHeight = state.height;
}

bool editor::PoseRender::isSceneReady(){
    updateScene();
    return scene->getSystem<RenderSystem>()->isAllLoaded();
}

void editor::PoseRender::draw(int width, int height){
    if (!target.isCreated() || target.getWidth() != (unsigned int)width || target.getHeight() != (unsigned int)height){
        target.destroy();
        target.setWidth((unsigned int)width);
        target.setHeight((unsigned int)height);
        target.create();
    }

    Engine::ViewLayer layer;
    layer.scene = scene;
    layer.camera = camera->getEntity();
    layer.viewRect = Rect(0.0f, 0.0f, (float)width, (float)height);
    layer.postProcessing = false;
    Engine::drawView(POSE_VIEW_ID, &target, {layer});
}

bool editor::PoseRender::readBack(int width, int height, bool opaque, PoseImage& out){
    std::vector<uint8_t> rgba;
    if (!GraphicUtils::readFramebufferRGBA(&target, rgba) || !out.assign(width, height, std::move(rgba))){
        out.clear();
        return false;
    }
    if (opaque){
        uint8_t* pixels = out.data();
        const size_t size = (size_t)width * height * 4;
        for (size_t i = 3; i < size; i += 4){
            pixels[i] = 255;
        }
    }
    return true;
}

bool editor::PoseRender::tryRender(const RenderState& state, PoseImage& out){
    out.clear();
    if (!ensureLoaded()){
        return false;
    }
    setState(state);
    applyToScene(state);

    if (!viewWarmed){
        // the first draw through this camera adds its pipelines to the scene; the meshes are
        // rebuilt with them on the next update, and skip the draw until then (nothing read back)
        if (!isSceneReady()){
            return false;
        }
        draw(state.width, state.height);
        viewWarmed = true;
    }
    if (!isSceneReady()){
        return false;
    }
    // what is loaded may still wait for its pipelines (spec S32, a GPU's compile threads: seconds)
    // and draws nothing meanwhile: an image without the model is never read back
    const uint32_t waitingBefore = ObjectRender::getWaitingDrawCount();
    draw(state.width, state.height);
    drawsWaiting = ObjectRender::getWaitingDrawCount() != waitingBefore;
    if (drawsWaiting){
        return false;
    }
    return readBack(state.width, state.height, state.style.opaque, out);
}

bool editor::PoseRender::renderNow(int width, int height, PoseImage& out){
    out.clear();
    setViewSize(width, height);
    if (!inEditorFrame()){
        return false;
    }
    const RenderState callerState = captureState(width, height);
    return tryRender(callerState, out);
}

void editor::PoseRender::requestRender(int width, int height, RenderCallback done){
    setViewSize(width, height);
    Request request;
    request.state = captureState(width, height);
    request.done = std::move(done);
    pending.push_back(std::move(request));
    if (frameRequester()){
        frameRequester()();
    }
}

void editor::PoseRender::cancelRenders(){
    std::deque<Request> cancelled;
    cancelled.swap(pending);
    for (Request& request : cancelled){
        if (request.done){
            request.done(false, PoseImage());
        }
    }
}

bool editor::PoseRender::serveQueue(double frameStart, bool& ranOne){
    // the caller's pose, view, style and lighting, put back as values after each render (the
    // scene's look is only changed by the next render); captured again only after a callback,
    // which may change them
    RenderState callerState = captureState(viewWidth, viewHeight);

    while (!pending.empty()){
        if (ranOne && nowSeconds() - frameStart > FRAME_BUDGET_SECONDS){
            break;
        }
        Request& front = pending.front();
        PoseImage image;
        drawsWaiting = false;
        const bool ok = tryRender(front.state, image);
        setState(callerState);
        if (front.firstTry == 0.0){
            front.firstTry = nowSeconds();
        }
        // (frames go fast past a GPU's pipelines: those are waited for by the clock)
        const bool waitOn = drawsWaiting ? nowSeconds() - front.firstTry < RENDER_TIMEOUT_SECONDS : ++front.frames < RENDER_TIMEOUT_FRAMES;
        if (!ok && waitOn && Engine::isViewLoaded()){
            break;  // still loading: next frame
        }
        ranOne = true;

        Request finished = std::move(front);
        pending.pop_front();
        if (finished.done){
            finished.done(ok, std::move(image));
            if (!isLive(this)){
                return false;
            }
            callerState = captureState(viewWidth, viewHeight);
        }
    }
    return true;
}

bool editor::PoseRender::processFrame(){
    if (!hasPendingRenders()){
        return false;
    }
    const double frameStart = nowSeconds();
    bool ranOne = false;
    // a copy: callbacks may create or destroy renderers
    const std::vector<PoseRender*> renders = liveRenders();
    for (PoseRender* render : renders){
        if (!isLive(render) || render->pending.empty()){
            continue;
        }
        render->serveQueue(frameStart, ranOne);
    }
    return hasPendingRenders();
}

bool editor::PoseRender::hasPendingRenders(){
    for (const PoseRender* render : liveRenders()){
        if (!render->pending.empty()){
            return true;
        }
    }
    return false;
}

void editor::PoseRender::setFrameRequester(std::function<void()> requester){
    frameRequester() = std::move(requester);
}

void editor::PoseRender::setInEditorFrame(bool inFrame){
    inEditorFrame() = inFrame;
}

void editor::PoseRender::releaseGpu(){
    // Shutdown only: App::engineViewDestroyed is called by the editor backends only once the
    // main loop has ended (BackendLinux/BackendWin after the loop, BackendMac after the loop and
    // on the Metal command queue failure at startup), right before Engine::systemShutdown; the
    // editor never recreates its view. scene->destroy() runs here while the view still exists,
    // so RenderSystem::destroy takes its reloadView=false path and discards the CPU assets: were
    // a view-recreation path ever added, a renderer used after it must reload its model too
    // (reloading the scene alone, as sceneLoaded=false does, would find no CPU assets).
    // a copy: cancelled callbacks may destroy renderers
    const std::vector<PoseRender*> renders = liveRenders();
    for (PoseRender* render : renders){
        if (isLive(render)){
            render->cancelRenders();
        }
    }
    for (PoseRender* render : liveRenders()){
        render->releaseGpuResources();
    }
}
