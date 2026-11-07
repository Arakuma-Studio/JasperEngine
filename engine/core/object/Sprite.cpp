// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Sprite.h"

#include "component/ShadowCaster2DComponent.h"
#include "util/Color.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"
#include "component/CameraComponent.h"
#include "sprite/SpriteFrameGeometry.h"
#include "sprite/SpriteSheet.h"
#include "sprite/SpriteSheetPool.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

Sprite::Sprite(Scene* scene): Mesh(scene){
    addComponent<SpriteComponent>();
    // its one submesh exists from the start (MeshSystem::prepareSpriteMesh builds it on the
    // first update), so a script can set its material right after creating it
    // (animation: a shared_ptr, made on first use by ensureAnimation)
    getComponent<MeshComponent>().numSubmeshes = 1;
}

Sprite::Sprite(Scene* scene, Entity entity): Mesh(scene, entity){
}

Sprite::~Sprite(){
}

Sprite::Sprite(const Sprite& rhs) = default;
Sprite& Sprite::operator=(const Sprite& rhs) = default;
Sprite::Sprite(Sprite&& rhs) noexcept = default;
Sprite& Sprite::operator=(Sprite&& rhs) noexcept = default;

SpriteAnimation& Sprite::ensureAnimation(){
    if (!animation){
        animation = std::make_shared<SpriteAnimation>(scene);
        animation->setTarget(entity);
    }
    return *animation;
}

bool Sprite::createSprite(){
    SpriteComponent& spritecomp = getComponent<SpriteComponent>();
    MeshComponent& mesh = getComponent<MeshComponent>();

    return scene->getSystem<MeshSystem>()->createOrUpdateSprite(spritecomp, mesh);
}

ShadowCaster2D Sprite::getShadowCaster2D(){
    if (!scene->findComponent<ShadowCaster2DComponent>(entity)){
        addComponent<ShadowCaster2DComponent>();
    }

    return ShadowCaster2D(scene, entity);
}

void Sprite::removeShadowCaster2D(){
    if (scene->findComponent<ShadowCaster2DComponent>(entity)){
        removeComponent<ShadowCaster2DComponent>();
    }
}

void Sprite::setSize(unsigned int width, unsigned int height){
    SpriteComponent& spritecomp = getComponent<SpriteComponent>();

    if ((spritecomp.width != width) || (spritecomp.height != height)){
        spritecomp.width = width;
        spritecomp.height = height;

        spritecomp.needUpdateSprite = true;
        markChanged<SpriteComponent>();
    }
}

void Sprite::setWidth(unsigned int width){
    SpriteComponent& spritecomp = getComponent<SpriteComponent>();

    if (spritecomp.width != width){
        spritecomp.width = width;

        spritecomp.needUpdateSprite = true;
        markChanged<SpriteComponent>();
    }
}

void Sprite::setHeight(unsigned int height){
    SpriteComponent& spritecomp = getComponent<SpriteComponent>();

    if (spritecomp.height != height){
        spritecomp.height = height;

        spritecomp.needUpdateSprite = true;
        markChanged<SpriteComponent>();
    }
}

unsigned int Sprite::getWidth() const{
    SpriteComponent& spritecomp = getComponent<SpriteComponent>();

    return spritecomp.width;
}

unsigned int Sprite::getHeight() const{
    SpriteComponent& spritecomp = getComponent<SpriteComponent>();

    return spritecomp.height;
}

void Sprite::setFlipY(bool flipY){
    SpriteComponent& spritecomp = getComponent<SpriteComponent>();

    spritecomp.automaticFlipY = false;
    if (spritecomp.flipY != flipY){
        spritecomp.flipY = flipY;

        spritecomp.needUpdateSprite = true;
        markChanged<SpriteComponent>();
    }
}

bool Sprite::isFlipY() const{
    SpriteComponent& spritecomp = getComponent<SpriteComponent>();

    return spritecomp.flipY;
}

void Sprite::setTextureScaleFactor(float textureScaleFactor){
    SpriteComponent& spritecomp = getComponent<SpriteComponent>();
    
    if (spritecomp.textureScaleFactor != textureScaleFactor){
        spritecomp.textureScaleFactor = textureScaleFactor;
        spritecomp.needUpdateSprite = true;
        markChanged<SpriteComponent>();
    }
}

float Sprite::getTextureScaleFactor() const{
    SpriteComponent& spritecomp = getComponent<SpriteComponent>();
    
    return spritecomp.textureScaleFactor;
}

void Sprite::setTextureRect(float x, float y, float width, float height){
    setTextureRect(Rect(x, y, width, height));
}

void Sprite::setTextureRect(Rect textureRect){
    MeshComponent& mesh = getComponent<MeshComponent>();
    SpriteComponent& spritecomp = getComponent<SpriteComponent>();

    if (MeshSystem::changesSpriteInset(spritecomp, mesh.submeshes[0].textureRect, textureRect)){
        spritecomp.needUpdateSprite = true;
        markChanged<SpriteComponent>();
    }

    // An explicit UV rectangle supersedes a frame waiting for its texture size.
    spritecomp.needUpdateFrameRect = false;
    mesh.submeshes[0].textureRect = textureRect;
}

Rect Sprite::getTextureRect() const{
    MeshComponent& mesh = getComponent<MeshComponent>();

    return mesh.submeshes[0].textureRect;
}

void Sprite::setPivotPreset(PivotPreset pivotPreset){
    SpriteComponent& spritecomp = getComponent<SpriteComponent>();

    spritecomp.pivotPreset = pivotPreset;
    spritecomp.needUpdateSprite = true;
    markChanged<SpriteComponent>();
}

PivotPreset Sprite::getPivotPreset() const{
    SpriteComponent& spritecomp = getComponent<SpriteComponent>();

    return spritecomp.pivotPreset;
}

void Sprite::setDrawMode(SpriteDrawMode drawMode){
    SpriteComponent& spritecomp = getComponent<SpriteComponent>();
    if (spritecomp.drawMode != drawMode){
        spritecomp.drawMode = drawMode;
        spritecomp.needUpdateSprite = true;
    }
}

SpriteDrawMode Sprite::getDrawMode() const{
    return getComponent<SpriteComponent>().drawMode;
}

void Sprite::addFrame(int id, const std::string& name, Rect rect){
    SpriteComponent& spritecomp = getComponent<SpriteComponent>();
    if (spritecomp.framesRect.validIndex(id)){
        spritecomp.framesRect[id] = {name, rect};
        if ((unsigned int)(id + 1) > spritecomp.numFramesRect){
            spritecomp.numFramesRect = id + 1;
        }
    }else{
        Log::error("Cannot set frame %s at id %i: ids go from 0 to %zu", name.c_str(), id, ARRAY_INDEX_LIMIT - 1);
    }
}

void Sprite::addFrame(const std::string& name, float x, float y, float width, float height){
    SpriteComponent& spritecomp = getComponent<SpriteComponent>();

    int id = (int)spritecomp.numFramesRect;

    if (spritecomp.framesRect.validIndex(id)){
        addFrame(id, name, Rect(x, y, width, height));
    }else{
        Log::error("Cannot add frame %s: at most %zu frames", name.c_str(), ARRAY_INDEX_LIMIT);
    }
}

void Sprite::addFrame(float x, float y, float width, float height){
    addFrame("", x, y, width, height);
}

void Sprite::addFrame(Rect rect){
    addFrame(rect.getX(), rect.getY(), rect.getWidth(), rect.getHeight());
}

void Sprite::removeFrame(int id){
    SpriteComponent& spritecomp = getComponent<SpriteComponent>();
    if (id >= 0 && (unsigned int)id < spritecomp.numFramesRect){
        for (unsigned int i = id; i + 1 < spritecomp.numFramesRect; i++){
            spritecomp.framesRect[i] = spritecomp.framesRect[i + 1];
        }
        spritecomp.numFramesRect--;
        spritecomp.framesRect[spritecomp.numFramesRect] = SpriteFrameData{};
    }
}

void Sprite::removeFrame(const std::string& name){
    SpriteComponent& spritecomp = getComponent<SpriteComponent>();

    for (int id = (int)spritecomp.numFramesRect - 1; id >= 0; id--){
        if (spritecomp.framesRect[id].name == name){
            removeFrame(id);
        }
    }
}

void Sprite::applyFrameRect(Rect frameRect){
    MeshSystem::setSpriteFrameRect(getComponent<MeshComponent>(), getComponent<SpriteComponent>(), frameRect);
}

void Sprite::setFrame(int id){
    SpriteComponent& spritecomp = getComponent<SpriteComponent>();
    if (!spritecomp.sheet.empty()){
        std::shared_ptr<const SpriteSheetData> sheet = getSheetData();
        if (sheet && (id < 0 || id >= sheet->frameCount())){
            Log::error("Cannot use invalid sprite frame: %i (%s has %i frames)", id, spritecomp.sheet.path.c_str(), sheet->frameCount());
            return;
        }
        spritecomp.frame = id;
        spritecomp.resolveFrameName = false;
        return;
    }
    if (id >= 0 && (unsigned int)id < spritecomp.numFramesRect){
        applyFrameRect(spritecomp.framesRect[id].rect);
    }else{
        Log::error("Cannot use invalid sprite frame: %i", id);
    }
}

void Sprite::setFrame(const std::string& name){
    SpriteComponent& spritecomp = getComponent<SpriteComponent>();
    if (!spritecomp.sheet.empty()){
        std::shared_ptr<const SpriteSheetData> sheet = getSheetData();
        const int index = sheet ? sheet->findFrame(name) : -1;
        if (index < 0){
            Log::error("Cannot use nonexistent sprite frame: %s (sheet %s)", name.c_str(), spritecomp.sheet.path.c_str());
            return;
        }
        spritecomp.frame = index;
        spritecomp.resolveFrameName = false;
        return;
    }
    for (unsigned int id = 0; id < spritecomp.numFramesRect; id++) {
        if (spritecomp.framesRect[id].name == name){
            applyFrameRect(spritecomp.framesRect[id].rect);
            return;
        }
    }
    Log::error("Cannot use nonexistent sprite frame: %s", name.c_str());
}

void Sprite::startAnimation(std::vector<int> frames, std::vector<int> framesTime, bool loop){
    if (animation && animation->isRunning()){
        stopAnimation();
    }
    ensureAnimation();

    if (frames.size() > 0 && framesTime.size() > 0){
        animation->setAnimation(frames, framesTime, loop);
        animation->start();
    }else{
        Log::error("Cannot start animation without frames or intervals");
    }
}

void Sprite::startAnimation(int startFrame, int endFrame, int interval, bool loop){
    if (animation && animation->isRunning()){
        stopAnimation();
    }
    ensureAnimation();

    animation->setAnimation(startFrame, endFrame, interval, loop);
    animation->start();
}

void Sprite::startAnimation(const std::string& name, int interval, bool loop){
    if (animation && animation->isRunning()){
        stopAnimation();
    }
    ensureAnimation();

    SpriteComponent& spritecomp = getComponent<SpriteComponent>();
    std::vector<int> frames;
    std::vector<int> framesTime;

    for (unsigned int id = 0; id < spritecomp.numFramesRect; id++){
        if (spritecomp.framesRect[id].name.find(name) != std::string::npos){
            frames.push_back(id);
            framesTime.push_back(interval);
        }
    }

    if (frames.size() > 0){
        animation->setAnimation(frames, framesTime, loop);
        animation->start();
    }else{
        Log::error("Cannot start animation '%s' without frames", name.c_str());
    }
}

void Sprite::pauseAnimation(){
    if (animation){
        animation->pause();
    }
}

void Sprite::resumeAnimation(){
    if (animation){
        animation->start();
    }
}

void Sprite::stopAnimation(){
    if (animation){
        animation->stop();
    }
}

void Sprite::setEnabled(bool enabled){
    getComponent<SpriteComponent>().enabled = enabled;
}

bool Sprite::isEnabled() const{
    return getComponent<SpriteComponent>().enabled;
}

void Sprite::setSheet(const std::string& path){
    SpriteComponent& spritecomp = getComponent<SpriteComponent>();
    if (spritecomp.sheet.path == path){
        return;
    }
    if (spritecomp.sheet.empty() && !path.empty()){
        // a sheet's pages are pixel art (R15): sampled Nearest
        Texture& page = getComponent<MeshComponent>().submeshes[0].material.baseColorTexture;
        page.setMinFilter(TextureFilter::NEAREST);
        page.setMagFilter(TextureFilter::NEAREST);
    }
    spritecomp.sheet = path;
    spritecomp.frame = 0;
    spritecomp.frameName.clear();
    spritecomp.resolveFrameName = false;
    spritecomp.playingTag.clear();
    spritecomp.tagPlaying = false;
    spritecomp.needUpdateSprite = true;
    if (path.empty()){
        // back to the inline frames and the mesh texture
        spritecomp.sheetData.reset();
        spritecomp.loadedSheet.clear();
        spritecomp.builtFrame = -1;
    }
}

std::string Sprite::getSheet() const{
    return getComponent<SpriteComponent>().sheet.path;
}

std::shared_ptr<const SpriteSheetData> Sprite::getSheetData() const{
    const SpriteComponent& spritecomp = getComponent<SpriteComponent>();
    if (spritecomp.sheet.empty()){
        return nullptr;
    }
    if (spritecomp.sheetData && spritecomp.loadedSheet == spritecomp.sheet.path){
        return spritecomp.sheetData;
    }
    return SpriteSheetPool::get(spritecomp.sheet.path);
}

int Sprite::getFrameIndex() const{
    return getComponent<SpriteComponent>().frame;
}

std::string Sprite::getFrameName() const{
    const SpriteComponent& spritecomp = getComponent<SpriteComponent>();
    if (std::shared_ptr<const SpriteSheetData> sheet = getSheetData()){
        if (spritecomp.frame >= 0 && spritecomp.frame < sheet->frameCount()){
            return sheet->frames[(size_t)spritecomp.frame].name;
        }
    }
    return spritecomp.frameName;
}

int Sprite::getFrameCount() const{
    const SpriteComponent& spritecomp = getComponent<SpriteComponent>();
    if (!spritecomp.sheet.empty()){
        std::shared_ptr<const SpriteSheetData> sheet = getSheetData();
        return sheet ? sheet->frameCount() : 0;
    }
    return (int)spritecomp.numFramesRect;
}

void Sprite::playTag(const std::string& name, float speed){
    SpriteComponent& spritecomp = getComponent<SpriteComponent>();
    if (!std::isfinite(speed) || speed <= 0.0f){
        Log::error("Sprite:playTag(\"%s\"): speed must be a number above 0 (got %g)", name.c_str(), (double)speed);
        return;
    }
    std::shared_ptr<const SpriteSheetData> sheet = getSheetData();
    if (!sheet){
        Log::error("Sprite:playTag(\"%s\"): the sprite has no sheet", name.c_str());
        return;
    }
    const int index = sheet->findTag(name);
    if (index < 0){
        std::string tags;
        for (const SpriteTag& tag : sheet->tags){
            tags += (tags.empty() ? "" : ", ") + tag.name;
        }
        Log::error("Sprite:playTag: %s has no tag \"%s\" (tags: %s)", spritecomp.sheet.path.c_str(), name.c_str(),
                   tags.empty() ? "none" : tags.c_str());
        return;
    }
    const SpriteTag& tag = sheet->tags[(size_t)index];
    spritecomp.tagLoop = tag.repeat == 0;
    spritecomp.tagFrames = spritecomp.tagLoop ? tag.sequence() : tag.playback();
    spritecomp.playingTag = tag.name;
    spritecomp.tagPlaying = !spritecomp.tagFrames.empty();
    spritecomp.tagStep = 0;
    spritecomp.tagElapsedMs = 0.0;
    spritecomp.tagSpeed = speed;
    if (spritecomp.tagPlaying){
        spritecomp.frame = spritecomp.tagFrames[0];
        spritecomp.resolveFrameName = false;
    }
}

void Sprite::stopTag(){
    SpriteComponent& spritecomp = getComponent<SpriteComponent>();
    spritecomp.tagPlaying = false;
    spritecomp.playingTag.clear();
    spritecomp.tagFrames.clear();
}

std::string Sprite::getTag() const{
    return getComponent<SpriteComponent>().playingTag;
}

bool Sprite::isTagPlaying() const{
    return getComponent<SpriteComponent>().tagPlaying;
}

bool Sprite::getSliceRect(const std::string& name, Rect& bounds, Rect* center, Vector2* pivot) const{
    std::shared_ptr<const SpriteSheetData> sheet = getSheetData();
    if (!sheet){
        return false;
    }
    bool uiCamera = false;
    if (const CameraComponent* camera = scene->findComponent<CameraComponent>(scene->getCamera())){
        uiCamera = camera->type == CameraType::CAMERA_UI;
    }
    return spriteSliceLocalRect(getComponent<SpriteComponent>(), *sheet, uiCamera, name, bounds, center, pivot);
}
