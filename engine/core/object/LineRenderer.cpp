// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LineRenderer.h"
#include "shader/ShaderGlobals.h"
#include "shader/ShaderManifest.h"
#include "subsystem/RenderSystem.h"
#include "render/LineRibbonBuilder.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

LineRenderer::LineRenderer(Scene* scene): GameObject(scene){
    addComponent<LineRendererComponent>();
}

LineRenderer::LineRenderer(Scene* scene, Entity entity): GameObject(scene, entity){
}

LineRenderer::~LineRenderer(){

}

LineRenderer::LineRenderer(const LineRenderer& rhs) = default;
LineRenderer& LineRenderer::operator=(const LineRenderer& rhs) = default;
LineRenderer::LineRenderer(LineRenderer&& rhs) noexcept = default;
LineRenderer& LineRenderer::operator=(LineRenderer&& rhs) noexcept = default;

bool LineRenderer::load(){
    LineRendererComponent& linescomp = getComponent<LineRendererComponent>();

    auto renderSystem = scene->getSystem<RenderSystem>();
    return renderSystem->loadLines(entity, linescomp, renderSystem->getScenePipelines());
}

void LineRenderer::setMaxLines(unsigned int maxLines){
    LineRendererComponent& linescomp = getComponent<LineRendererComponent>();

    if (linescomp.maxLines != maxLines){
        linescomp.maxLines = maxLines;

        linescomp.needReload = true;
    }
}

unsigned int LineRenderer::getMaxLines() const{
    LineRendererComponent& linescomp = getComponent<LineRendererComponent>();

    return linescomp.maxLines;
}

void LineRenderer::addLine(LineData line){
    LineRendererComponent& linescomp = getComponent<LineRendererComponent>();

    linescomp.mode = LineRendererMode::Segments;
    linescomp.lines.push_back(line);
    linescomp.needUpdateRibbon = true;

    if (linescomp.maxLines < linescomp.lines.size()){
        linescomp.maxLines = linescomp.maxLines * 2;
        linescomp.needReload = true;
    }else{
        linescomp.needUpdateBuffer = true;
        markChanged<LineRendererComponent>();
    }
}

void LineRenderer::addLine(Vector3 pointA, Vector3 pointB){
    LineData line = {};

    line.pointA = pointA;
    line.pointB = pointB;

    addLine(line);
}

void LineRenderer::addLine(Vector3 pointA, Vector3 pointB, Vector3 color){
    LineData line = {};

    line.pointA = pointA;
    line.pointB = pointB;
    line.colorA = Vector4(color, line.colorA.z);
    line.colorB = Vector4(color, line.colorB.z);

    addLine(line);
}

void LineRenderer::addLine(Vector3 pointA, Vector3 pointB, Vector4 color){
    LineData line = {};

    line.pointA = pointA;
    line.pointB = pointB;
    line.colorA = color;
    line.colorB = color;

    addLine(line);
}

void LineRenderer::addLine(Vector3 pointA, Vector3 pointB, Vector4 colorA, Vector4 colorB){
    LineData line = {};

    line.pointA = pointA;
    line.pointB = pointB;
    line.colorA = colorA;
    line.colorB = colorB;

    addLine(line);
}

LineData& LineRenderer::getLine(size_t index){
    LineRendererComponent& linescomp = getComponent<LineRendererComponent>();

    return linescomp.lines.at(index);
}

void LineRenderer::updateLine(size_t index, LineData line){
    LineRendererComponent& linescomp = getComponent<LineRendererComponent>();

    linescomp.mode = LineRendererMode::Segments;
    if (linescomp.lines.at(index) != line){
        linescomp.lines.at(index) = line;

        linescomp.needUpdateBuffer = true;
        linescomp.needUpdateRibbon = true;
        markChanged<LineRendererComponent>();
    }
}

void LineRenderer::updateLine(size_t index, Vector3 pointA, Vector3 pointB){
    LineData line = getLine(index);

    line.pointA = pointA;
    line.pointB = pointB;

    updateLine(index, line);
}

void LineRenderer::updateLine(size_t index, Vector3 pointA, Vector3 pointB, Vector3 color){
    LineData line = getLine(index);

    line.pointA = pointA;
    line.pointB = pointB;
    line.colorA = Vector4(color, line.colorA.z);
    line.colorB = Vector4(color, line.colorB.z);

    updateLine(index, line);
}

void LineRenderer::updateLine(size_t index, Vector3 pointA, Vector3 pointB, Vector4 color){
    LineData line = getLine(index);

    line.pointA = pointA;
    line.pointB = pointB;
    line.colorA = color;
    line.colorB = color;

    updateLine(index, line);
}

void LineRenderer::updateLine(size_t index, Vector3 pointA, Vector3 pointB, Vector4 colorA, Vector4 colorB){
    LineData line = getLine(index);

    line.pointA = pointA;
    line.pointB = pointB;
    line.colorA = colorA;
    line.colorB = colorB;

    updateLine(index, line);
}

void LineRenderer::updateLine(size_t index, Vector3 color){
    LineData line = getLine(index);

    line.colorA = Vector4(color, line.colorA.z);
    line.colorB = Vector4(color, line.colorB.z);

    updateLine(index, line);
}

void LineRenderer::updateLine(size_t index, Vector4 color){
    LineData line = getLine(index);

    line.colorA = color;
    line.colorB = color;

    updateLine(index, line);
}

void LineRenderer::updateLine(size_t index, Vector4 colorA, Vector4 colorB){
    LineData line = getLine(index);

    line.colorA = colorA;
    line.colorB = colorB;

    updateLine(index, line);
}

void LineRenderer::removeLine(size_t index){
    LineRendererComponent& linescomp = getComponent<LineRendererComponent>();

    linescomp.mode = LineRendererMode::Segments;
    linescomp.lines.erase(linescomp.lines.begin() + index);

    linescomp.needUpdateBuffer = true;
    linescomp.needUpdateRibbon = true;
    markChanged<LineRendererComponent>();
}

void LineRenderer::updateLines(){
    LineRendererComponent& linescomp = getComponent<LineRendererComponent>();

    linescomp.needUpdateBuffer = true;
    linescomp.needUpdateRibbon = true;
    markChanged<LineRendererComponent>();
}

size_t LineRenderer::getNumLines(){
    LineRendererComponent& linescomp = getComponent<LineRendererComponent>();

    return linescomp.lines.size();
}

void LineRenderer::clearLines(){
    LineRendererComponent& linescomp = getComponent<LineRendererComponent>();

    linescomp.mode = LineRendererMode::Segments;
    if (linescomp.lines.size() > 0){
        linescomp.lines.clear();

        linescomp.needReload = true;
        linescomp.needUpdateRibbon = true;
    }
}

// ---------------------------------------------------------------- Unity LineRenderer API

namespace {

    unsigned int clampFanVertices(int count){
        return (unsigned int)std::min<int>(std::max(count, 0), (int)LINE_RIBBON_MAX_FAN_VERTICES);
    }

    float nonNegative(float value){
        return (std::isfinite(value) && value > 0.0f) ? value : 0.0f;
    }

}

void LineRenderer::setMode(LineRendererMode mode){
    LineRendererComponent& lines = getComponent<LineRendererComponent>();
    if (lines.mode != mode){
        lines.mode = mode;
        lines.needUpdateRibbon = true;
    }
}

LineRendererMode LineRenderer::getMode() const{
    return getComponent<LineRendererComponent>().mode;
}

void LineRenderer::setPositionCount(int count){
    LineRendererComponent& lines = getComponent<LineRendererComponent>();
    lines.mode = LineRendererMode::Polyline;
    const size_t size = (size_t)std::max(count, 0);
    if (size != lines.positions.size()){
        const Vector3 fill = lines.positions.empty() ? Vector3(0.0f, 0.0f, 0.0f) : lines.positions.back();
        lines.positions.resize(size, fill);
    }
    lines.needUpdateRibbon = true;
}

int LineRenderer::getPositionCount() const{
    return (int)getComponent<LineRendererComponent>().positions.size();
}

bool LineRenderer::setPosition(int index, Vector3 position){
    LineRendererComponent& lines = getComponent<LineRendererComponent>();
    // out of range: refused, nothing changes (the mode neither)
    if (index < 0 || (size_t)index >= lines.positions.size())
        return false;
    lines.mode = LineRendererMode::Polyline;
    lines.positions[index] = position;
    lines.needUpdateRibbon = true;
    return true;
}

Vector3 LineRenderer::getPosition(int index) const{
    const LineRendererComponent& lines = getComponent<LineRendererComponent>();
    if (index < 0 || (size_t)index >= lines.positions.size())
        return Vector3(0.0f, 0.0f, 0.0f);
    return lines.positions[index];
}

void LineRenderer::setPositions(const std::vector<Vector3>& positions){
    LineRendererComponent& lines = getComponent<LineRendererComponent>();
    lines.mode = LineRendererMode::Polyline;
    lines.positions = positions;
    lines.needUpdateRibbon = true;
}

std::vector<Vector3> LineRenderer::getPositions() const{
    return getComponent<LineRendererComponent>().positions;
}

void LineRenderer::setLoop(bool loop){
    LineRendererComponent& lines = getComponent<LineRendererComponent>();
    if (lines.loop != loop){
        lines.loop = loop;
        lines.needUpdateRibbon = true;
    }
}

bool LineRenderer::getLoop() const{
    return getComponent<LineRendererComponent>().loop;
}

void LineRenderer::setUseWorldSpace(bool useWorldSpace){
    LineRendererComponent& lines = getComponent<LineRendererComponent>();
    if (lines.useWorldSpace != useWorldSpace){
        lines.useWorldSpace = useWorldSpace;
        lines.needUpdateRibbon = true;
    }
}

bool LineRenderer::getUseWorldSpace() const{
    return getComponent<LineRendererComponent>().useWorldSpace;
}

void LineRenderer::setWidthMultiplier(float widthMultiplier){
    LineRendererComponent& lines = getComponent<LineRendererComponent>();
    widthMultiplier = nonNegative(widthMultiplier);
    if (lines.widthMultiplier != widthMultiplier){
        // between 0 and a width the variant changes: the render system reloads it
        lines.widthMultiplier = widthMultiplier;
        lines.needUpdateRibbon = true;
    }
}

float LineRenderer::getWidthMultiplier() const{
    return getComponent<LineRendererComponent>().widthMultiplier;
}

void LineRenderer::setStartWidth(float width){
    LineRendererComponent& lines = getComponent<LineRendererComponent>();
    LineRibbonBuilder::setEndWidths(lines.widthMultiplier, lines.widthCurve, width, getEndWidth());
    lines.needUpdateRibbon = true;
}

float LineRenderer::getStartWidth() const{
    const LineRendererComponent& lines = getComponent<LineRendererComponent>();
    return LineRibbonBuilder::widthAt(lines.widthMultiplier, lines.widthCurve, 0.0f);
}

void LineRenderer::setEndWidth(float width){
    LineRendererComponent& lines = getComponent<LineRendererComponent>();
    LineRibbonBuilder::setEndWidths(lines.widthMultiplier, lines.widthCurve, getStartWidth(), width);
    lines.needUpdateRibbon = true;
}

float LineRenderer::getEndWidth() const{
    const LineRendererComponent& lines = getComponent<LineRendererComponent>();
    return LineRibbonBuilder::widthAt(lines.widthMultiplier, lines.widthCurve, 1.0f);
}

void LineRenderer::setWidthCurve(const upcurve::AnimationCurve& curve){
    LineRendererComponent& lines = getComponent<LineRendererComponent>();
    upcurve::AnimationCurve normalized = curve;
    normalized.normalize();
    if (lines.widthCurve != normalized){
        lines.widthCurve = normalized;
        lines.needUpdateRibbon = true;
    }
}

upcurve::AnimationCurve LineRenderer::getWidthCurve() const{
    return getComponent<LineRendererComponent>().widthCurve;
}

void LineRenderer::setWidthUnit(LineWidthUnit unit){
    LineRendererComponent& lines = getComponent<LineRendererComponent>();
    if (lines.widthUnit != unit){
        lines.widthUnit = unit;
        lines.needUpdateRibbon = true;
    }
}

LineWidthUnit LineRenderer::getWidthUnit() const{
    return getComponent<LineRendererComponent>().widthUnit;
}

void LineRenderer::setColorGradient(const upcurve::Gradient& gradient){
    LineRendererComponent& lines = getComponent<LineRendererComponent>();
    upcurve::Gradient normalized = gradient;
    normalized.normalize();
    if (lines.colorGradient != normalized){
        lines.colorGradient = normalized;
        lines.needUpdateRibbon = true;
    }
}

upcurve::Gradient LineRenderer::getColorGradient() const{
    return getComponent<LineRendererComponent>().colorGradient;
}

void LineRenderer::setStartColor(Vector4 color){
    LineRendererComponent& lines = getComponent<LineRendererComponent>();
    LineRibbonBuilder::setEndColor(lines.colorGradient, 0.0f, color);
    lines.needUpdateRibbon = true;
}

Vector4 LineRenderer::getStartColor() const{
    return LineRibbonBuilder::colorAt(getComponent<LineRendererComponent>().colorGradient, 0.0f);
}

void LineRenderer::setEndColor(Vector4 color){
    LineRendererComponent& lines = getComponent<LineRendererComponent>();
    LineRibbonBuilder::setEndColor(lines.colorGradient, 1.0f, color);
    lines.needUpdateRibbon = true;
}

Vector4 LineRenderer::getEndColor() const{
    return LineRibbonBuilder::colorAt(getComponent<LineRendererComponent>().colorGradient, 1.0f);
}

void LineRenderer::setNumCornerVertices(int count){
    LineRendererComponent& lines = getComponent<LineRendererComponent>();
    lines.numCornerVertices = clampFanVertices(count);
    lines.needUpdateRibbon = true;
}

int LineRenderer::getNumCornerVertices() const{
    return (int)getComponent<LineRendererComponent>().numCornerVertices;
}

void LineRenderer::setNumCapVertices(int count){
    LineRendererComponent& lines = getComponent<LineRendererComponent>();
    lines.numCapVertices = clampFanVertices(count);
    lines.needUpdateRibbon = true;
}

int LineRenderer::getNumCapVertices() const{
    return (int)getComponent<LineRendererComponent>().numCapVertices;
}

void LineRenderer::setAlignment(LineAlignment alignment){
    getComponent<LineRendererComponent>().alignment = alignment;
}

LineAlignment LineRenderer::getAlignment() const{
    return getComponent<LineRendererComponent>().alignment;
}

void LineRenderer::setTextureMode(LineTextureMode mode){
    LineRendererComponent& lines = getComponent<LineRendererComponent>();
    if (lines.textureMode != mode){
        lines.textureMode = mode;
        lines.needUpdateRibbon = true;
    }
}

LineTextureMode LineRenderer::getTextureMode() const{
    return getComponent<LineRendererComponent>().textureMode;
}

void LineRenderer::setTextureScale(Vector2 scale){
    LineRendererComponent& lines = getComponent<LineRendererComponent>();
    if (lines.textureScale != scale){
        lines.textureScale = scale;
        lines.needUpdateRibbon = true;
    }
}

Vector2 LineRenderer::getTextureScale() const{
    return getComponent<LineRendererComponent>().textureScale;
}

void LineRenderer::setTexture(const std::string& path){
    setShaderTexture(LINE_BASE_TEXTURE_UNIFORM, path);
}

std::string LineRenderer::getTexture() const{
    return getShaderTexture(LINE_BASE_TEXTURE_UNIFORM);
}

void LineRenderer::setSortPriority(int priority){
    LineRendererComponent& lines = getComponent<LineRendererComponent>();
    lines.sortPriority = priority;
}

int LineRenderer::getSortPriority() const{
    return getComponent<LineRendererComponent>().sortPriority;
}

void LineRenderer::setCustomShader(const std::string& path){
    LineRendererComponent& lines = getComponent<LineRendererComponent>();

    if (lines.customShader != path){
        lines.customShader = path;

        lines.needReload = true;
    }
}

std::string LineRenderer::getCustomShader() const{
    LineRendererComponent& lines = getComponent<LineRendererComponent>();

    return lines.customShader;
}

void LineRenderer::setShaderUniform(const std::string& name, const Vector4& value){
    LineRendererComponent& lines = getComponent<LineRendererComponent>();

    if (ShaderUniforms::set(lines.shaderUniforms, name, value)){
        lines.needUpdateShaderUniforms = true;
        markChanged<LineRendererComponent>();
    }
}

void LineRenderer::setShaderUniform(const std::string& name, const Vector3& value){
    setShaderUniform(name, Vector4(value.x, value.y, value.z, 0.0f));
}

void LineRenderer::setShaderUniform(const std::string& name, const Vector2& value){
    setShaderUniform(name, Vector4(value.x, value.y, 0.0f, 0.0f));
}

void LineRenderer::setShaderUniform(const std::string& name, float value){
    setShaderUniform(name, Vector4(value, 0.0f, 0.0f, 0.0f));
}

Vector4 LineRenderer::getShaderUniform(const std::string& name) const{
    LineRendererComponent& lines = getComponent<LineRendererComponent>();

    return ShaderUniforms::get(lines.shaderUniforms, name);
}

bool LineRenderer::removeShaderUniform(const std::string& name){
    LineRendererComponent& lines = getComponent<LineRendererComponent>();

    if (!ShaderUniforms::remove(lines.shaderUniforms, name))
        return false;

    lines.needUpdateShaderUniforms = true;
    markChanged<LineRendererComponent>();
    return true;
}

Vector4 LineRenderer::getShaderParam(const std::string& name) const{
    LineRendererComponent& lines = getComponent<LineRendererComponent>();

    return ShaderLibraryValues::getParam(lines.shaderUniforms, lines.customShader.empty() ? scene->getDefaultCustomShader(ShaderType::LINES) : lines.customShader.path, name);
}

bool LineRenderer::setShaderGlobalBinding(const std::string& member, const std::string& global){
    LineRendererComponent& lines = getComponent<LineRendererComponent>();

    if (!ShaderGlobals::setBinding(lines.shaderGlobalBindings, member, global))
        return false;
    lines.needUpdateShaderUniforms = true;
    return true;
}

std::string LineRenderer::getShaderGlobalBinding(const std::string& member) const{
    const LineRendererComponent& lines = getComponent<LineRendererComponent>();

    return ShaderGlobals::getBinding(lines.shaderGlobalBindings, member);
}

void LineRenderer::setShaderTexture(const std::string& name, const std::string& path){
    LineRendererComponent& lines = getComponent<LineRendererComponent>();

    // which textures it has is resolved at load
    if (ShaderLibraryValues::setTexture(lines.shaderTextures, name, path))
        lines.needReload = true;
}

std::string LineRenderer::getShaderTexture(const std::string& name) const{
    LineRendererComponent& lines = getComponent<LineRendererComponent>();

    return ShaderLibraryValues::getTexture(lines.shaderTextures, name);
}

void LineRenderer::setShaderFeature(const std::string& define, bool enabled){
    LineRendererComponent& lines = getComponent<LineRendererComponent>();

    // features are compiled in: another shader variant
    if (ShaderLibraryValues::setFeature(lines.shaderFeatures, lines.customShader.empty() ? scene->getDefaultCustomShader(ShaderType::LINES) : lines.customShader.path, define, enabled))
        lines.needReload = true;
}

bool LineRenderer::isShaderFeatureEnabled(const std::string& define) const{
    LineRendererComponent& lines = getComponent<LineRendererComponent>();

    return ShaderLibraryValues::isFeatureEnabled(lines.shaderFeatures, lines.customShader.empty() ? scene->getDefaultCustomShader(ShaderType::LINES) : lines.customShader.path, define);
}

void LineRenderer::setBlendMode(MaterialBlendMode blendMode){
    LineRendererComponent& lines = getComponent<LineRendererComponent>();

    if (lines.blendMode != blendMode){
        lines.blendMode = blendMode;
        lines.needReload = true;
    }
}

MaterialBlendMode LineRenderer::getBlendMode() const{
    LineRendererComponent& lines = getComponent<LineRendererComponent>();

    return lines.blendMode;
}

void LineRenderer::setDepthWrite(MaterialDepthWrite depthWrite){
    LineRendererComponent& lines = getComponent<LineRendererComponent>();

    if (lines.depthWrite != depthWrite){
        lines.depthWrite = depthWrite;
        lines.needReload = true;
    }
}

MaterialDepthWrite LineRenderer::getDepthWrite() const{
    LineRendererComponent& lines = getComponent<LineRendererComponent>();

    return lines.depthWrite;
}

void LineRenderer::setStencil(const MaterialStencil& stencil){
    LineRendererComponent& lines = getComponent<LineRendererComponent>();

    if (lines.stencil != stencil){
        lines.stencil = stencil;
        lines.needReload = true;
    }
}

MaterialStencil LineRenderer::getStencil() const{
    LineRendererComponent& lines = getComponent<LineRendererComponent>();

    return lines.stencil;
}

void LineRenderer::setColorWrite(int colorWrite){
    LineRendererComponent& lines = getComponent<LineRendererComponent>();

    if (lines.colorWrite != colorWrite){
        lines.colorWrite = colorWrite;
        lines.needReload = true;
    }
}

int LineRenderer::getColorWrite() const{
    LineRendererComponent& lines = getComponent<LineRendererComponent>();

    return lines.colorWrite;
}

void LineRenderer::setEnabled(bool enabled){
    getComponent<LineRendererComponent>().enabled = enabled;
}

bool LineRenderer::isEnabled() const{
    return getComponent<LineRendererComponent>().enabled;
}
