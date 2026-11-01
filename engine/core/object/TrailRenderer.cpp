// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TrailRenderer.h"
#include "shader/ShaderGlobals.h"
#include "shader/ShaderManifest.h"
#include "subsystem/TrailSystem.h"
#include "render/LineRibbonBuilder.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace {

    unsigned int clampFanVertices(int count){
        return (unsigned int)std::min<int>(std::max(count, 0), (int)LINE_RIBBON_MAX_FAN_VERTICES);
    }

    float nonNegative(float value){
        return (std::isfinite(value) && value > 0.0f) ? value : 0.0f;
    }

}

TrailRenderer::TrailRenderer(Scene* scene): GameObject(scene){
    addComponent<TrailRendererComponent>();
}

TrailRenderer::TrailRenderer(Scene* scene, Entity entity): GameObject(scene, entity){
}

TrailRenderer TrailRenderer::addTo(Scene* scene, Entity entity){
    if (!scene->findComponent<TrailRendererComponent>(entity)){
        scene->addComponent<TrailRendererComponent>(entity, {});
    }
    return TrailRenderer(scene, entity);
}

TrailRenderer::~TrailRenderer(){
}

TrailRenderer::TrailRenderer(const TrailRenderer& rhs) = default;
TrailRenderer& TrailRenderer::operator=(const TrailRenderer& rhs) = default;
TrailRenderer::TrailRenderer(TrailRenderer&& rhs) noexcept = default;
TrailRenderer& TrailRenderer::operator=(TrailRenderer&& rhs) noexcept = default;

void TrailRenderer::setEnabled(bool enabled){
    TrailRendererComponent& trail = getComponent<TrailRendererComponent>();
    // disabled: cleared now (re-enabled in the same frame, it draws no streak)
    if (!enabled){
        trail.clearPoints();
    }
    trail.enabled = enabled;
}

bool TrailRenderer::isEnabled() const{
    return getComponent<TrailRendererComponent>().enabled;
}

void TrailRenderer::setTime(float time){
    TrailRendererComponent& trail = getComponent<TrailRendererComponent>();
    trail.time = nonNegative(time);
    // the points' ages are over it: the width and colour along the trail change
    trail.needUpdateRibbon = true;
}

float TrailRenderer::getTime() const{
    return getComponent<TrailRendererComponent>().time;
}

void TrailRenderer::setMinVertexDistance(float distance){
    getComponent<TrailRendererComponent>().minVertexDistance = nonNegative(distance);
}

float TrailRenderer::getMinVertexDistance() const{
    return getComponent<TrailRendererComponent>().minVertexDistance;
}

void TrailRenderer::setEmitting(bool emitting){
    TrailRendererComponent& trail = getComponent<TrailRendererComponent>();
    trail.emitting = emitting;
    // the live head joins the ribbon only while emitting
    trail.needUpdateRibbon = true;
}

bool TrailRenderer::isEmitting() const{
    return getComponent<TrailRendererComponent>().emitting;
}

void TrailRenderer::setAutodestruct(bool autodestruct){
    getComponent<TrailRendererComponent>().autodestruct = autodestruct;
}

bool TrailRenderer::isAutodestruct() const{
    return getComponent<TrailRendererComponent>().autodestruct;
}

void TrailRenderer::clear(){
    TrailSimulation::clear(getComponent<TrailRendererComponent>());
}

int TrailRenderer::getPositionCount() const{
    return (int)getComponent<TrailRendererComponent>().points.size();
}

Vector3 TrailRenderer::getPosition(int index) const{
    const TrailRendererComponent& trail = getComponent<TrailRendererComponent>();
    if (index < 0 || (size_t)index >= trail.points.size())
        return Vector3(0.0f, 0.0f, 0.0f);
    return trail.points.at((size_t)index).position;
}

bool TrailRenderer::setPosition(int index, Vector3 position){
    TrailRendererComponent& trail = getComponent<TrailRendererComponent>();
    if (index < 0 || (size_t)index >= trail.points.size())
        return false;
    trail.points.at((size_t)index).position = position;
    trail.needUpdateRibbon = true;
    return true;
}

std::vector<Vector3> TrailRenderer::getPositions() const{
    const TrailRendererComponent& trail = getComponent<TrailRendererComponent>();
    std::vector<Vector3> positions;
    positions.reserve(trail.points.size());
    for (size_t i = 0; i < trail.points.size(); i++){
        positions.push_back(trail.points.at(i).position);
    }
    return positions;
}

void TrailRenderer::addPosition(Vector3 position){
    std::shared_ptr<TrailSystem> system = scene->getSystem<TrailSystem>();
    TrailSimulation::addPosition(getComponent<TrailRendererComponent>(), position, system ? system->getTime() : 0.0f);
}

void TrailRenderer::addPositions(const std::vector<Vector3>& positions){
    for (const Vector3& position : positions){
        addPosition(position);
    }
}

void TrailRenderer::setWidthMultiplier(float widthMultiplier){
    TrailRendererComponent& trail = getComponent<TrailRendererComponent>();
    trail.widthMultiplier = nonNegative(widthMultiplier);
    trail.needUpdateRibbon = true;
}

float TrailRenderer::getWidthMultiplier() const{
    return getComponent<TrailRendererComponent>().widthMultiplier;
}

void TrailRenderer::setStartWidth(float width){
    TrailRendererComponent& trail = getComponent<TrailRendererComponent>();
    LineRibbonBuilder::setEndWidths(trail.widthMultiplier, trail.widthCurve, width, getEndWidth());
    trail.needUpdateRibbon = true;
}

float TrailRenderer::getStartWidth() const{
    const TrailRendererComponent& trail = getComponent<TrailRendererComponent>();
    return LineRibbonBuilder::widthAt(trail.widthMultiplier, trail.widthCurve, 0.0f);
}

void TrailRenderer::setEndWidth(float width){
    TrailRendererComponent& trail = getComponent<TrailRendererComponent>();
    LineRibbonBuilder::setEndWidths(trail.widthMultiplier, trail.widthCurve, getStartWidth(), width);
    trail.needUpdateRibbon = true;
}

float TrailRenderer::getEndWidth() const{
    const TrailRendererComponent& trail = getComponent<TrailRendererComponent>();
    return LineRibbonBuilder::widthAt(trail.widthMultiplier, trail.widthCurve, 1.0f);
}

void TrailRenderer::setWidthCurve(const upcurve::AnimationCurve& curve){
    TrailRendererComponent& trail = getComponent<TrailRendererComponent>();
    trail.widthCurve = curve;
    trail.widthCurve.normalize();
    trail.needUpdateRibbon = true;
}

upcurve::AnimationCurve TrailRenderer::getWidthCurve() const{
    return getComponent<TrailRendererComponent>().widthCurve;
}

void TrailRenderer::setWidthUnit(LineWidthUnit unit){
    TrailRendererComponent& trail = getComponent<TrailRendererComponent>();
    trail.widthUnit = unit;
    trail.needUpdateRibbon = true;
}

LineWidthUnit TrailRenderer::getWidthUnit() const{
    return getComponent<TrailRendererComponent>().widthUnit;
}

void TrailRenderer::setColorGradient(const upcurve::Gradient& gradient){
    TrailRendererComponent& trail = getComponent<TrailRendererComponent>();
    trail.colorGradient = gradient;
    trail.colorGradient.normalize();
    trail.needUpdateRibbon = true;
}

upcurve::Gradient TrailRenderer::getColorGradient() const{
    return getComponent<TrailRendererComponent>().colorGradient;
}

void TrailRenderer::setStartColor(Vector4 color){
    TrailRendererComponent& trail = getComponent<TrailRendererComponent>();
    LineRibbonBuilder::setEndColor(trail.colorGradient, 0.0f, color);
    trail.needUpdateRibbon = true;
}

Vector4 TrailRenderer::getStartColor() const{
    return LineRibbonBuilder::colorAt(getComponent<TrailRendererComponent>().colorGradient, 0.0f);
}

void TrailRenderer::setEndColor(Vector4 color){
    TrailRendererComponent& trail = getComponent<TrailRendererComponent>();
    LineRibbonBuilder::setEndColor(trail.colorGradient, 1.0f, color);
    trail.needUpdateRibbon = true;
}

Vector4 TrailRenderer::getEndColor() const{
    return LineRibbonBuilder::colorAt(getComponent<TrailRendererComponent>().colorGradient, 1.0f);
}

void TrailRenderer::setNumCornerVertices(int count){
    TrailRendererComponent& trail = getComponent<TrailRendererComponent>();
    trail.numCornerVertices = clampFanVertices(count);
    trail.needUpdateRibbon = true;
}

int TrailRenderer::getNumCornerVertices() const{
    return (int)getComponent<TrailRendererComponent>().numCornerVertices;
}

void TrailRenderer::setNumCapVertices(int count){
    TrailRendererComponent& trail = getComponent<TrailRendererComponent>();
    trail.numCapVertices = clampFanVertices(count);
    trail.needUpdateRibbon = true;
}

int TrailRenderer::getNumCapVertices() const{
    return (int)getComponent<TrailRendererComponent>().numCapVertices;
}

void TrailRenderer::setAlignment(LineAlignment alignment){
    getComponent<TrailRendererComponent>().alignment = alignment;
}

LineAlignment TrailRenderer::getAlignment() const{
    return getComponent<TrailRendererComponent>().alignment;
}

void TrailRenderer::setTextureMode(LineTextureMode mode){
    TrailRendererComponent& trail = getComponent<TrailRendererComponent>();
    trail.textureMode = mode;
    trail.needUpdateRibbon = true;
}

LineTextureMode TrailRenderer::getTextureMode() const{
    return getComponent<TrailRendererComponent>().textureMode;
}

void TrailRenderer::setTextureScale(Vector2 scale){
    TrailRendererComponent& trail = getComponent<TrailRendererComponent>();
    trail.textureScale = scale;
    trail.needUpdateRibbon = true;
}

Vector2 TrailRenderer::getTextureScale() const{
    return getComponent<TrailRendererComponent>().textureScale;
}

void TrailRenderer::setTexture(const std::string& path){
    setShaderTexture(LINE_BASE_TEXTURE_UNIFORM, path);
}

std::string TrailRenderer::getTexture() const{
    return getShaderTexture(LINE_BASE_TEXTURE_UNIFORM);
}

void TrailRenderer::setSortPriority(int priority){
    getComponent<TrailRendererComponent>().sortPriority = priority;
}

int TrailRenderer::getSortPriority() const{
    return getComponent<TrailRendererComponent>().sortPriority;
}

void TrailRenderer::setCustomShader(const std::string& path){
    TrailRendererComponent& trail = getComponent<TrailRendererComponent>();
    if (trail.customShader != path){
        trail.customShader = path;
        trail.needReload = true;
    }
}

std::string TrailRenderer::getCustomShader() const{
    return getComponent<TrailRendererComponent>().customShader;
}

void TrailRenderer::setShaderUniform(const std::string& name, const Vector4& value){
    TrailRendererComponent& trail = getComponent<TrailRendererComponent>();
    if (ShaderUniforms::set(trail.shaderUniforms, name, value))
        trail.needUpdateShaderUniforms = true;
}

void TrailRenderer::setShaderUniform(const std::string& name, const Vector3& value){
    setShaderUniform(name, Vector4(value.x, value.y, value.z, 0.0f));
}

void TrailRenderer::setShaderUniform(const std::string& name, const Vector2& value){
    setShaderUniform(name, Vector4(value.x, value.y, 0.0f, 0.0f));
}

void TrailRenderer::setShaderUniform(const std::string& name, float value){
    setShaderUniform(name, Vector4(value, 0.0f, 0.0f, 0.0f));
}

Vector4 TrailRenderer::getShaderUniform(const std::string& name) const{
    return ShaderUniforms::get(getComponent<TrailRendererComponent>().shaderUniforms, name);
}

bool TrailRenderer::removeShaderUniform(const std::string& name){
    TrailRendererComponent& trail = getComponent<TrailRendererComponent>();
    if (!ShaderUniforms::remove(trail.shaderUniforms, name))
        return false;
    trail.needUpdateShaderUniforms = true;
    return true;
}

Vector4 TrailRenderer::getShaderParam(const std::string& name) const{
    const TrailRendererComponent& trail = getComponent<TrailRendererComponent>();
    return ShaderLibraryValues::getParam(trail.shaderUniforms, trail.customShader.empty() ? scene->getDefaultCustomShader(ShaderType::LINES) : trail.customShader, name);
}

bool TrailRenderer::setShaderGlobalBinding(const std::string& member, const std::string& global){
    TrailRendererComponent& trail = getComponent<TrailRendererComponent>();

    if (!ShaderGlobals::setBinding(trail.shaderGlobalBindings, member, global))
        return false;
    trail.needUpdateShaderUniforms = true;
    return true;
}

std::string TrailRenderer::getShaderGlobalBinding(const std::string& member) const{
    const TrailRendererComponent& trail = getComponent<TrailRendererComponent>();

    return ShaderGlobals::getBinding(trail.shaderGlobalBindings, member);
}

void TrailRenderer::setShaderTexture(const std::string& name, const std::string& path){
    TrailRendererComponent& trail = getComponent<TrailRendererComponent>();
    // which textures it has is resolved at load
    if (ShaderLibraryValues::setTexture(trail.shaderTextures, name, path))
        trail.needReload = true;
}

std::string TrailRenderer::getShaderTexture(const std::string& name) const{
    return ShaderLibraryValues::getTexture(getComponent<TrailRendererComponent>().shaderTextures, name);
}

void TrailRenderer::setShaderFeature(const std::string& define, bool enabled){
    TrailRendererComponent& trail = getComponent<TrailRendererComponent>();
    if (ShaderLibraryValues::setFeature(trail.shaderFeatures, trail.customShader.empty() ? scene->getDefaultCustomShader(ShaderType::LINES) : trail.customShader, define, enabled))
        trail.needReload = true;
}

bool TrailRenderer::isShaderFeatureEnabled(const std::string& define) const{
    const TrailRendererComponent& trail = getComponent<TrailRendererComponent>();
    return ShaderLibraryValues::isFeatureEnabled(trail.shaderFeatures, trail.customShader.empty() ? scene->getDefaultCustomShader(ShaderType::LINES) : trail.customShader, define);
}

void TrailRenderer::setBlendMode(MaterialBlendMode blendMode){
    TrailRendererComponent& trail = getComponent<TrailRendererComponent>();
    if (trail.blendMode != blendMode){
        trail.blendMode = blendMode;
        trail.needReload = true;
    }
}

MaterialBlendMode TrailRenderer::getBlendMode() const{
    return getComponent<TrailRendererComponent>().blendMode;
}

void TrailRenderer::setDepthWrite(MaterialDepthWrite depthWrite){
    TrailRendererComponent& trail = getComponent<TrailRendererComponent>();
    if (trail.depthWrite != depthWrite){
        trail.depthWrite = depthWrite;
        trail.needReload = true;
    }
}

MaterialDepthWrite TrailRenderer::getDepthWrite() const{
    return getComponent<TrailRendererComponent>().depthWrite;
}

void TrailRenderer::setStencil(const MaterialStencil& stencil){
    TrailRendererComponent& trail = getComponent<TrailRendererComponent>();
    if (trail.stencil != stencil){
        trail.stencil = stencil;
        trail.needReload = true;
    }
}

MaterialStencil TrailRenderer::getStencil() const{
    return getComponent<TrailRendererComponent>().stencil;
}

void TrailRenderer::setColorWrite(int colorWrite){
    TrailRendererComponent& trail = getComponent<TrailRendererComponent>();
    if (trail.colorWrite != colorWrite){
        trail.colorWrite = colorWrite;
        trail.needReload = true;
    }
}

int TrailRenderer::getColorWrite() const{
    return getComponent<TrailRendererComponent>().colorWrite;
}
