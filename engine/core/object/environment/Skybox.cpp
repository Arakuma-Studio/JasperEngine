// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Skybox.h"
#include "shader/ShaderGlobals.h"
#include "shader/ShaderManifest.h"
#include "subsystem/RenderSystem.h"

using namespace jasperengine;

Skybox::Skybox(Scene* scene): EntityHandle(scene){
    addComponent<SkyboxComponent>();
}

Skybox::Skybox(Scene* scene, Entity entity): EntityHandle(scene, entity){
}

Skybox::~Skybox(){
}

Skybox::Skybox(const Skybox& rhs) = default;
Skybox& Skybox::operator=(const Skybox& rhs) = default;
Skybox::Skybox(Skybox&& rhs) noexcept = default;
Skybox& Skybox::operator=(Skybox&& rhs) noexcept = default;

bool Skybox::load(){
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    auto renderSystem = scene->getSystem<RenderSystem>();
    return renderSystem->loadSky(entity, sky, renderSystem->getScenePipelines());
}

void Skybox::setTextures(const std::string& id,
                        TextureData texturePositiveX, TextureData textureNegativeX,
                        TextureData texturePositiveY, TextureData textureNegativeY,
                        TextureData texturePositiveZ, TextureData textureNegativeZ){

    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    // front, back, left, right, up, down
    sky.texture.setCubeDatas(id, texturePositiveZ, textureNegativeZ, textureNegativeX, texturePositiveX, texturePositiveY, textureNegativeY);

    sky.needUpdateTexture = true;
    markChanged<SkyboxComponent>();
}

void Skybox::setTextures(const std::string& texturePositiveX, const std::string& textureNegativeX,
                        const std::string& texturePositiveY, const std::string& textureNegativeY,
                        const std::string& texturePositiveZ, const std::string& textureNegativeZ){
    
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    // front, back, left, right, up, down
    sky.texture.setCubePaths(texturePositiveZ, textureNegativeZ, textureNegativeX, texturePositiveX, texturePositiveY, textureNegativeY);

    sky.needUpdateTexture = true;
    markChanged<SkyboxComponent>();
}

void Skybox::setTexture(const std::string& texture){
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    sky.texture.setCubeMap(texture);

    sky.needUpdateTexture = true;
    markChanged<SkyboxComponent>();
}

void Skybox::setTexturePositiveX(const std::string& texture){
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    sky.texture.setCubePath(0, texture);

    sky.needUpdateTexture = true;
    markChanged<SkyboxComponent>();
}

void Skybox::setTextureNegativeX(const std::string& texture){
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    sky.texture.setCubePath(1, texture);

    sky.needUpdateTexture = true;
    markChanged<SkyboxComponent>();
}

void Skybox::setTexturePositiveY(const std::string& texture){
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    sky.texture.setCubePath(2, texture);

    sky.needUpdateTexture = true;
    markChanged<SkyboxComponent>();
}

void Skybox::setTextureNegativeY(const std::string& texture){
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    sky.texture.setCubePath(3, texture);

    sky.needUpdateTexture = true;
    markChanged<SkyboxComponent>();
}

void Skybox::setTexturePositiveZ(const std::string& texture){
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    sky.texture.setCubePath(4, texture);

    sky.needUpdateTexture = true;
    markChanged<SkyboxComponent>();
}

void Skybox::setTextureNegativeZ(const std::string& texture){
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    sky.texture.setCubePath(5, texture);

    sky.needUpdateTexture = true;
    markChanged<SkyboxComponent>();
}

void Skybox::setColor(Vector4 color){
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    sky.color = color;
}

void Skybox::setColor(const float r, const float g, const float b){
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    sky.color = Vector4(r, g, b, sky.color.w);
}

void Skybox::setColor(const float r, const float g, const float b, const float a){
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    sky.color = Vector4(r, g, b, a);
}

void Skybox::setAlpha(const float alpha){
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    sky.color = Vector4(sky.color.x, sky.color.y, sky.color.z, alpha);
}

Vector4 Skybox::getColor() const{
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    return sky.color;
}

float Skybox::getAlpha() const{
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    return sky.color.w;
}

void Skybox::setRotation(float angle){
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    sky.rotation = angle;
    sky.needUpdateSky = true;
    markChanged<SkyboxComponent>();
}

float Skybox::getRotation() const{
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    return sky.rotation;
}

void Skybox::setCustomShader(const std::string& path){
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    if (sky.customShader != path){
        sky.customShader = path;

        sky.needReload = true;
    }
}

std::string Skybox::getCustomShader() const{
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    return sky.customShader;
}

void Skybox::setShaderUniform(const std::string& name, const Vector4& value){
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    if (ShaderUniforms::set(sky.shaderUniforms, name, value)){
        sky.needUpdateShaderUniforms = true;
        markChanged<SkyboxComponent>();
    }
}

void Skybox::setShaderUniform(const std::string& name, const Vector3& value){
    setShaderUniform(name, Vector4(value.x, value.y, value.z, 0.0f));
}

void Skybox::setShaderUniform(const std::string& name, const Vector2& value){
    setShaderUniform(name, Vector4(value.x, value.y, 0.0f, 0.0f));
}

void Skybox::setShaderUniform(const std::string& name, float value){
    setShaderUniform(name, Vector4(value, 0.0f, 0.0f, 0.0f));
}

Vector4 Skybox::getShaderUniform(const std::string& name) const{
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    return ShaderUniforms::get(sky.shaderUniforms, name);
}

bool Skybox::removeShaderUniform(const std::string& name){
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    if (!ShaderUniforms::remove(sky.shaderUniforms, name))
        return false;

    sky.needUpdateShaderUniforms = true;
    markChanged<SkyboxComponent>();
    return true;
}

Vector4 Skybox::getShaderParam(const std::string& name) const{
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    return ShaderLibraryValues::getParam(sky.shaderUniforms, sky.customShader.empty() ? scene->getDefaultCustomShader(ShaderType::SKYBOX) : sky.customShader.path, name);
}

bool Skybox::setShaderGlobalBinding(const std::string& member, const std::string& global){
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    if (!ShaderGlobals::setBinding(sky.shaderGlobalBindings, member, global))
        return false;
    sky.needUpdateShaderUniforms = true;
    return true;
}

std::string Skybox::getShaderGlobalBinding(const std::string& member) const{
    const SkyboxComponent& sky = getComponent<SkyboxComponent>();

    return ShaderGlobals::getBinding(sky.shaderGlobalBindings, member);
}

void Skybox::setShaderTexture(const std::string& name, const std::string& path){
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    // which textures it has is resolved at load
    if (ShaderLibraryValues::setTexture(sky.shaderTextures, name, path))
        sky.needReload = true;
}

std::string Skybox::getShaderTexture(const std::string& name) const{
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    return ShaderLibraryValues::getTexture(sky.shaderTextures, name);
}

void Skybox::setShaderFeature(const std::string& define, bool enabled){
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    // features are compiled in: another shader variant
    if (ShaderLibraryValues::setFeature(sky.shaderFeatures, sky.customShader.empty() ? scene->getDefaultCustomShader(ShaderType::SKYBOX) : sky.customShader.path, define, enabled))
        sky.needReload = true;
}

bool Skybox::isShaderFeatureEnabled(const std::string& define) const{
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    return ShaderLibraryValues::isFeatureEnabled(sky.shaderFeatures, sky.customShader.empty() ? scene->getDefaultCustomShader(ShaderType::SKYBOX) : sky.customShader.path, define);
}
