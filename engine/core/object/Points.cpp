// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Points.h"
#include "Log.h"
#include "Scene.h"
#include "component/ParticleSystemComponent.h"
#include "object/ParticleSystem.h"
#include "shader/ShaderGlobals.h"
#include "shader/ShaderManifest.h"
#include "subsystem/RenderSystem.h"

#include <algorithm>

using namespace jasperengine;

namespace {

    // a sprite frame in UV units (0..1), with room for rounding at the edges
    bool isUvRect(const Rect& rect){
        const float values[4] = {rect.getX(), rect.getY(), rect.getWidth(), rect.getHeight()};
        return std::all_of(std::begin(values), std::end(values), [](float v){ return v >= -0.001f && v <= 1.001f; });
    }

    // the pixel rects a Sprite takes (Sprite:addFrame) given to a Points: they sample far outside
    // the texture, so it is said once per Points (S94 F1, the port's R54)
    void warnFrameUnits(bool& warned, const Rect& rect){
        if (warned || isUvRect(rect))
            return;
        warned = true;
        Log::warn("[Points] addSpriteFrame rect is in UV units (0..1), got (%g, %g, %g, %g): divide a pixel rect by the texture size (Sprite:addFrame takes pixels)",
                  rect.getX(), rect.getY(), rect.getWidth(), rect.getHeight());
    }

    particles::ParticleBlend particleBlendOf(MaterialBlendMode blendMode){
        switch (blendMode){
            case MaterialBlendMode::PREMULTIPLIED: return particles::ParticleBlend::Premultiplied;
            case MaterialBlendMode::ADDITIVE: return particles::ParticleBlend::Additive;
            case MaterialBlendMode::MULTIPLY: return particles::ParticleBlend::Multiply;
            default: return particles::ParticleBlend::Alpha;
        }
    }

    MaterialBlendMode materialBlendOf(particles::ParticleBlend blend){
        switch (blend){
            case particles::ParticleBlend::Premultiplied: return MaterialBlendMode::PREMULTIPLIED;
            case particles::ParticleBlend::Additive: return MaterialBlendMode::ADDITIVE;
            case particles::ParticleBlend::Multiply: return MaterialBlendMode::MULTIPLY;
            default: return MaterialBlendMode::ALPHA;
        }
    }

    // the frames an old Points held, as the effect's Texture Sheet Animation sprites (what the
    // S20 migration made of them): the module holds the rects; it animates with an old sprite
    // initializer's list once every frame the list picks exists (an effect whose list picks a
    // missing frame does not compile), as LegacyParticleConverter sets it
    void holdSprites(particles::TextureSheetAnimationModule& sheet){
        if (!sheet.present){
            sheet.present = true;
            sheet.enabled = false;
        }
        sheet.mode = particles::TextureSheetMode::Sprites;
        if (!sheet.startFrameList.empty()){
            sheet.enabled = std::all_of(sheet.startFrameList.begin(), sheet.startFrameList.end(),
                                        [&](int frame){ return frame >= 0 && (size_t)frame < sheet.sprites.size(); });
        }
        if (sheet.sprites.empty())
            sheet.enabled = false;
    }

}

ParticleSystemComponent* Points::legacyParticles(const char* call) const{
    if (scene->findComponent<PointsComponent>(entity))
        return nullptr;
    ParticleSystemComponent* particles = scene->findComponent<ParticleSystemComponent>(entity);
    if (particles && !particles->legacyPointsNoted){
        particles->legacyPointsNoted = true;
        Log::warn("[Particles] Points:%s on '%s', whose Particle System draws its particles now: forwarded to the effect (ps.textureSheetAnimation sprites for frames, ps.renderer for the blend and depth write); deprecated",
                  call, getName().c_str());
    }
    return particles;
}

Points::Points(Scene* scene): GameObject(scene){
    addComponent<PointsComponent>();
}

Points::Points(Scene* scene, Entity entity): GameObject(scene, entity){
}

Points::~Points(){
}

Points::Points(const Points& rhs) = default;
Points& Points::operator=(const Points& rhs) = default;
Points::Points(Points&& rhs) noexcept = default;
Points& Points::operator=(Points&& rhs) noexcept = default;

bool Points::load(){
    PointsComponent& pointscomp = getComponent<PointsComponent>();

    auto renderSystem = scene->getSystem<RenderSystem>();
    return renderSystem->loadPoints(entity, pointscomp, renderSystem->getScenePipelines());
}

void Points::setMaxPoints(unsigned int maxPoints){
    PointsComponent& pointscomp = getComponent<PointsComponent>();

    if (pointscomp.maxPoints != maxPoints){
        pointscomp.maxPoints = maxPoints;

        pointscomp.needReload = true;
    }
}

unsigned int Points::getMaxPoints() const{
    PointsComponent& pointscomp = getComponent<PointsComponent>();

    return pointscomp.maxPoints;
}

void Points::addPoint(PointData point){
    PointsComponent& pointscomp = getComponent<PointsComponent>();
    pointscomp.points.push_back(point);

    if (pointscomp.maxPoints < pointscomp.points.size()){
        pointscomp.maxPoints = pointscomp.maxPoints * 2;
        pointscomp.needReload = true;
    }

    if (point.textureRect != Rect(0,0,1,1) && !pointscomp.hasTextureRect){
        pointscomp.hasTextureRect = true;
        pointscomp.needReload = true;
    }

    pointscomp.needUpdate = true;
    markChanged<PointsComponent>();
}

void Points::addPoint(Vector3 position){
    PointData point = {};

    point.position = position;

    addPoint(point);
}

void Points::addPoint(float x, float y, float z){
   addPoint(Vector3(x, y, z));
}

void Points::addPoint(Vector3 position, Vector4 color){
    PointData point = {};

    point.position = position;
    point.color = color;

    addPoint(point);
}

void Points::addPoint(Vector3 position, Vector4 color, float size){
    PointData point = {};

    point.position = position;
    point.color = color;
    point.size = size;

    addPoint(point);
}

void Points::addPoint(Vector3 position, Vector4 color, float size, float rotation){
    PointData point = {};

    point.position = position;
    point.color = color;
    point.size = size;
    point.rotation = rotation;

    addPoint(point);
}

void Points::addPoint(Vector3 position, Vector4 color, float size, float rotation, Rect textureRect){
    PointData point = {};

    point.position = position;
    point.color = color;
    point.size = size;
    point.rotation = rotation;
    point.textureRect = textureRect;

    addPoint(point);
}

PointData& Points::getPoint(size_t index){
    PointsComponent& pointscomp = getComponent<PointsComponent>();
    return pointscomp.points.at(index);
}

void Points::updatePoint(size_t index, PointData point){
    PointsComponent& pointscomp = getComponent<PointsComponent>();
    pointscomp.points.at(index) = point;

    if (point.textureRect != Rect(0,0,1,1) && !pointscomp.hasTextureRect){
        pointscomp.hasTextureRect = true;
        pointscomp.needReload = true;
    }

    pointscomp.needUpdate = true;
    markChanged<PointsComponent>();
}

void Points::updatePoint(size_t index, Vector3 position){
    PointData point = getPoint(index);

    point.position = position;

    updatePoint(index, point);
}

void Points::updatePoint(size_t index, float x, float y, float z){
    updatePoint(index, Vector3(x, y, z));
}

void Points::updatePoint(size_t index, Vector3 position, Vector4 color){
    PointData point = getPoint(index);

    point.position = position;
    point.color = color;

    updatePoint(index, point);
}

void Points::updatePoint(size_t index, Vector3 position, Vector4 color, float size){
    PointData point = getPoint(index);

    point.position = position;
    point.color = color;
    point.size = size;

    updatePoint(index, point);
}

void Points::updatePoint(size_t index, Vector3 position, Vector4 color, float size, float rotation){
    PointData point = getPoint(index);

    point.position = position;
    point.color = color;
    point.size = size;
    point.rotation = rotation;

    updatePoint(index, point);
}

void Points::updatePoint(size_t index, Vector3 position, Vector4 color, float size, float rotation, Rect textureRect){
    PointData point = getPoint(index);

    point.position = position;
    point.color = color;
    point.size = size;
    point.rotation = rotation;
    point.textureRect = textureRect;

    updatePoint(index, point);
}

void Points::removePoint(size_t index){
    PointsComponent& pointscomp = getComponent<PointsComponent>();

    pointscomp.points.erase(pointscomp.points.begin() + index);

    pointscomp.needUpdate = true;
    markChanged<PointsComponent>();
}

bool Points::isPointVisible(size_t index){
    PointsComponent& pointscomp = getComponent<PointsComponent>();

    return pointscomp.points.at(index).visible;
}

void Points::setPointVisible(size_t index, bool visible) const{
    PointsComponent& pointscomp = getComponent<PointsComponent>();

    if (pointscomp.points.at(index).visible != visible){
        pointscomp.points.at(index).visible = visible;

        pointscomp.needUpdate = true;
        markChanged<PointsComponent>();
    }
}

void Points::updatePoints(){
    PointsComponent& pointscomp = getComponent<PointsComponent>();

    pointscomp.needUpdate = true;
    markChanged<PointsComponent>();
}

size_t Points::getNumPoints(){
    PointsComponent& pointscomp = getComponent<PointsComponent>();
    return pointscomp.points.size();
}

void Points::clearPoints(){
    PointsComponent& pointscomp = getComponent<PointsComponent>();
    pointscomp.points.clear();

    pointscomp.needUpdate = true;
    markChanged<PointsComponent>();
}

void Points::setTexture(const std::string& path){
    PointsComponent& pointscomp = getComponent<PointsComponent>();

    pointscomp.texture.setPath(path);

    pointscomp.needUpdateTexture = true;
    markChanged<PointsComponent>();
}

void Points::setTexture(const std::string& id, TextureData data){
    PointsComponent& pointscomp = getComponent<PointsComponent>();

    pointscomp.texture.setData(id, data);

    pointscomp.needUpdateTexture = true;
    markChanged<PointsComponent>();
}

void Points::setTexture(Framebuffer* framebuffer){
    PointsComponent& pointscomp = getComponent<PointsComponent>();

    pointscomp.texture.setFramebuffer(framebuffer);

    pointscomp.needUpdateTexture = true;
    markChanged<PointsComponent>();
}

void Points::setTransparent(bool transparent){
    PointsComponent& pointscomp = getComponent<PointsComponent>();

    pointscomp.transparent = transparent;
}

bool Points::isTransparent() const{
    PointsComponent& pointscomp = getComponent<PointsComponent>();

    return pointscomp.transparent;
}

void Points::setAutoTransparency(bool autoTransparency){
    PointsComponent& pointscomp = getComponent<PointsComponent>();

    pointscomp.autoTransparency = autoTransparency;
}

bool Points::isAutoTransparency() const{
    PointsComponent& pointscomp = getComponent<PointsComponent>();

    return pointscomp.autoTransparency;
}

void Points::addSpriteFrame(int id, const std::string& name, Rect rect){
    if (legacyParticles("addSpriteFrame")){
        if (id < 0 || (size_t)id >= particles::kMaxSpriteFrames){
            Log::error("Cannot set frame %s at id %i: a Particle System's sprites go from 0 to %zu", name.c_str(), id, particles::kMaxSpriteFrames - 1);
            return;
        }
        // an effect's sprites are UV rects: a pixel one would make it fail to compile
        if (!isUvRect(rect)){
            Log::error("Points:addSpriteFrame on '%s': frame %i not added, its rect is in UV units (0..1), got (%g, %g, %g, %g): divide a pixel rect by the texture size",
                       getName().c_str(), id, rect.getX(), rect.getY(), rect.getWidth(), rect.getHeight());
            return;
        }
        auto unit = [](float v){ return std::clamp(v, 0.0f, 1.0f); };
        ParticleSystem system(scene, entity);
        particles::ParticleEffectDocument document = system.getEffectDocument();
        particles::TextureSheetAnimationModule& sheet = document.textureSheetAnimation;
        if (sheet.sprites.size() <= (size_t)id)
            sheet.sprites.resize((size_t)id + 1);
        sheet.sprites[id] = {unit(rect.getX()), unit(rect.getY()), unit(rect.getWidth()), unit(rect.getHeight())};
        holdSprites(sheet);
        system.setEffectDocument(document);
        return;
    }
    PointsComponent& pointscomp = getComponent<PointsComponent>();
    if (pointscomp.framesRect.validIndex(id)){
        warnFrameUnits(pointscomp.frameUnitsWarned, rect);
        pointscomp.framesRect[id] = {name, rect};
        if ((unsigned int)(id + 1) > pointscomp.numFramesRect){
            pointscomp.numFramesRect = id + 1;
        }
        if (!pointscomp.hasTextureRect){
            pointscomp.hasTextureRect = true;
            pointscomp.needReload = true;
        }
    }else{
        Log::error("Cannot set frame %s at id %i: ids go from 0 to %zu", name.c_str(), id, ARRAY_INDEX_LIMIT - 1);
    }
}

void Points::addSpriteFrame(const std::string& name, float x, float y, float width, float height){
    if (legacyParticles("addSpriteFrame")){
        const size_t next = ParticleSystem(scene, entity).getEffectDocument().textureSheetAnimation.sprites.size();
        if (next >= particles::kMaxSpriteFrames){
            Log::error("Cannot add frame %s: a Particle System has at most %zu sprites", name.c_str(), particles::kMaxSpriteFrames);
            return;
        }
        addSpriteFrame((int)next, name, Rect(x, y, width, height));
        return;
    }
    PointsComponent& pointscomp = getComponent<PointsComponent>();

    int id = (int)pointscomp.numFramesRect;

    if (pointscomp.framesRect.validIndex(id)){
        addSpriteFrame(id, name, Rect(x, y, width, height));
    }else{
        Log::error("Cannot add frame %s: at most %zu frames", name.c_str(), ARRAY_INDEX_LIMIT);
    }
}

void Points::addSpriteFrame(float x, float y, float width, float height){
    addSpriteFrame("", x, y, width, height);
}

void Points::addSpriteFrame(Rect rect){
    addSpriteFrame(rect.getX(), rect.getY(), rect.getWidth(), rect.getHeight());
}

void Points::removeSpriteFrame(int id){
    if (legacyParticles("removeSpriteFrame")){
        ParticleSystem system(scene, entity);
        particles::ParticleEffectDocument document = system.getEffectDocument();
        std::vector<particles::SpriteRect>& sprites = document.textureSheetAnimation.sprites;
        if (id >= 0 && (size_t)id < sprites.size()){
            sprites.erase(sprites.begin() + id);
            holdSprites(document.textureSheetAnimation);
            system.setEffectDocument(document);
        }
        return;
    }
    PointsComponent& pointscomp = getComponent<PointsComponent>();
    if (id >= 0 && (unsigned int)id < pointscomp.numFramesRect){
        for (unsigned int i = id; i + 1 < pointscomp.numFramesRect; i++){
            pointscomp.framesRect[i] = pointscomp.framesRect[i + 1];
        }
        pointscomp.numFramesRect--;
        pointscomp.framesRect[pointscomp.numFramesRect] = SpriteFrameData{};

        if (pointscomp.numFramesRect == 0 && pointscomp.hasTextureRect){
            pointscomp.hasTextureRect = false;
            pointscomp.needReload = true;
        }
    }
}

void Points::removeSpriteFrame(const std::string& name){
    if (legacyParticles("removeSpriteFrame")){
        Log::warn("[Particles] Points:removeSpriteFrame(\"%s\") on '%s': a Particle System's sprites have no names; remove one by its index",
                  name.c_str(), getName().c_str());
        return;
    }
    PointsComponent& pointscomp = getComponent<PointsComponent>();

    for (int id = (int)pointscomp.numFramesRect - 1; id >= 0; id--){
        if (pointscomp.framesRect[id].name == name){
            removeSpriteFrame(id);
        }
    }
}

void Points::setCustomShader(const std::string& path){
    PointsComponent& points = getComponent<PointsComponent>();

    if (points.customShader != path){
        points.customShader = path;

        points.needReload = true;
    }
}

std::string Points::getCustomShader() const{
    PointsComponent& points = getComponent<PointsComponent>();

    return points.customShader;
}

void Points::setShaderUniform(const std::string& name, const Vector4& value){
    PointsComponent& points = getComponent<PointsComponent>();

    if (ShaderUniforms::set(points.shaderUniforms, name, value)){
        points.needUpdateShaderUniforms = true;
        markChanged<PointsComponent>();
    }
}

void Points::setShaderUniform(const std::string& name, const Vector3& value){
    setShaderUniform(name, Vector4(value.x, value.y, value.z, 0.0f));
}

void Points::setShaderUniform(const std::string& name, const Vector2& value){
    setShaderUniform(name, Vector4(value.x, value.y, 0.0f, 0.0f));
}

void Points::setShaderUniform(const std::string& name, float value){
    setShaderUniform(name, Vector4(value, 0.0f, 0.0f, 0.0f));
}

Vector4 Points::getShaderUniform(const std::string& name) const{
    PointsComponent& points = getComponent<PointsComponent>();

    return ShaderUniforms::get(points.shaderUniforms, name);
}

bool Points::removeShaderUniform(const std::string& name){
    PointsComponent& points = getComponent<PointsComponent>();

    if (!ShaderUniforms::remove(points.shaderUniforms, name))
        return false;

    points.needUpdateShaderUniforms = true;
    markChanged<PointsComponent>();
    return true;
}

Vector4 Points::getShaderParam(const std::string& name) const{
    PointsComponent& points = getComponent<PointsComponent>();

    return ShaderLibraryValues::getParam(points.shaderUniforms, points.customShader.empty() ? scene->getDefaultCustomShader(ShaderType::POINTS) : points.customShader.path, name);
}

bool Points::setShaderGlobalBinding(const std::string& member, const std::string& global){
    PointsComponent& points = getComponent<PointsComponent>();

    if (!ShaderGlobals::setBinding(points.shaderGlobalBindings, member, global))
        return false;
    points.needUpdateShaderUniforms = true;
    return true;
}

std::string Points::getShaderGlobalBinding(const std::string& member) const{
    const PointsComponent& points = getComponent<PointsComponent>();

    return ShaderGlobals::getBinding(points.shaderGlobalBindings, member);
}

void Points::setShaderTexture(const std::string& name, const std::string& path){
    PointsComponent& points = getComponent<PointsComponent>();

    // which textures it has is resolved at load
    if (ShaderLibraryValues::setTexture(points.shaderTextures, name, path))
        points.needReload = true;
}

std::string Points::getShaderTexture(const std::string& name) const{
    PointsComponent& points = getComponent<PointsComponent>();

    return ShaderLibraryValues::getTexture(points.shaderTextures, name);
}

void Points::setShaderFeature(const std::string& define, bool enabled){
    PointsComponent& points = getComponent<PointsComponent>();

    // features are compiled in: another shader variant
    if (ShaderLibraryValues::setFeature(points.shaderFeatures, points.customShader.empty() ? scene->getDefaultCustomShader(ShaderType::POINTS) : points.customShader.path, define, enabled))
        points.needReload = true;
}

bool Points::isShaderFeatureEnabled(const std::string& define) const{
    PointsComponent& points = getComponent<PointsComponent>();

    return ShaderLibraryValues::isFeatureEnabled(points.shaderFeatures, points.customShader.empty() ? scene->getDefaultCustomShader(ShaderType::POINTS) : points.customShader.path, define);
}

void Points::setBlendMode(MaterialBlendMode blendMode){
    if (legacyParticles("blendMode")){
        ParticleSystem system(scene, entity);
        particles::ParticleEffectDocument document = system.getEffectDocument();
        if (document.renderer.blend != particleBlendOf(blendMode)){
            document.renderer.blend = particleBlendOf(blendMode);
            system.setEffectDocument(document);
        }
        return;
    }
    PointsComponent& points = getComponent<PointsComponent>();

    if (points.blendMode != blendMode){
        points.blendMode = blendMode;
        points.needReload = true;
    }
}

MaterialBlendMode Points::getBlendMode() const{
    if (legacyParticles("blendMode")){
        return materialBlendOf(ParticleSystem(scene, entity).getEffectDocument().renderer.blend);
    }
    PointsComponent& points = getComponent<PointsComponent>();

    return points.blendMode;
}

void Points::setDepthWrite(MaterialDepthWrite depthWrite){
    if (legacyParticles("depthWrite")){
        ParticleSystem system(scene, entity);
        particles::ParticleEffectDocument document = system.getEffectDocument();
        // a particle draws without depth writes unless asked (the Renderer's default)
        const bool write = depthWrite == MaterialDepthWrite::ENABLED;
        if (document.renderer.depthWrite != write){
            document.renderer.depthWrite = write;
            system.setEffectDocument(document);
        }
        return;
    }
    PointsComponent& points = getComponent<PointsComponent>();

    if (points.depthWrite != depthWrite){
        points.depthWrite = depthWrite;
        points.needReload = true;
    }
}

MaterialDepthWrite Points::getDepthWrite() const{
    if (legacyParticles("depthWrite")){
        return ParticleSystem(scene, entity).getEffectDocument().renderer.depthWrite ? MaterialDepthWrite::ENABLED : MaterialDepthWrite::DISABLED;
    }
    PointsComponent& points = getComponent<PointsComponent>();

    return points.depthWrite;
}

void Points::setStencil(const MaterialStencil& stencil){
    PointsComponent& points = getComponent<PointsComponent>();

    if (points.stencil != stencil){
        points.stencil = stencil;
        points.needReload = true;
    }
}

MaterialStencil Points::getStencil() const{
    PointsComponent& points = getComponent<PointsComponent>();

    return points.stencil;
}

void Points::setColorWrite(int colorWrite){
    PointsComponent& points = getComponent<PointsComponent>();

    if (points.colorWrite != colorWrite){
        points.colorWrite = colorWrite;
        points.needReload = true;
    }
}

int Points::getColorWrite() const{
    PointsComponent& points = getComponent<PointsComponent>();

    return points.colorWrite;
}

void Points::setEnabled(bool enabled){
    getComponent<PointsComponent>().enabled = enabled;
}

bool Points::isEnabled() const{
    return getComponent<PointsComponent>().enabled;
}
