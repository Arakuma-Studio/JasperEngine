// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LineRenderer.h"
#include "subsystem/RenderSystem.h"

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

    linescomp.lines.push_back(line);

    if (linescomp.maxLines < linescomp.lines.size()){
        linescomp.maxLines = linescomp.maxLines * 2;
        linescomp.needReload = true;
    }else{
        linescomp.needUpdateBuffer = true;
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

    if (linescomp.lines.at(index) != line){
        linescomp.lines.at(index) = line;

        linescomp.needUpdateBuffer = true;
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

    linescomp.lines.erase(linescomp.lines.begin() + index);

    linescomp.needUpdateBuffer = true;
}

void LineRenderer::updateLines(){
    LineRendererComponent& linescomp = getComponent<LineRendererComponent>();

    linescomp.needUpdateBuffer = true;
}

size_t LineRenderer::getNumLines(){
    LineRendererComponent& linescomp = getComponent<LineRendererComponent>();

    return linescomp.lines.size();
}

void LineRenderer::clearLines(){
    LineRendererComponent& linescomp = getComponent<LineRendererComponent>();

    if (linescomp.lines.size() > 0){
        linescomp.lines.clear();

        linescomp.needReload = true;
    }
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

    if (ShaderUniforms::set(lines.shaderUniforms, name, value))
        lines.needUpdateShaderUniforms = true;
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
    return true;
}

void LineRenderer::setEnabled(bool enabled){
    getComponent<LineRendererComponent>().enabled = enabled;
}

bool LineRenderer::isEnabled() const{
    return getComponent<LineRendererComponent>().enabled;
}
