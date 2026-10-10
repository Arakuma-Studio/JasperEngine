// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Skybox.h"
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

    sky.texture.setCubeDatas(id, texturePositiveX, textureNegativeX, texturePositiveY, textureNegativeY, texturePositiveZ, textureNegativeZ);

    sky.needUpdateTexture = true;
}

void Skybox::setTextures(const std::string& texturePositiveX, const std::string& textureNegativeX,
                        const std::string& texturePositiveY, const std::string& textureNegativeY,
                        const std::string& texturePositiveZ, const std::string& textureNegativeZ){
    
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    sky.texture.setCubePaths(texturePositiveX, textureNegativeX, texturePositiveY, textureNegativeY, texturePositiveZ, textureNegativeZ);

    sky.needUpdateTexture = true;
}

void Skybox::setTexture(const std::string& texture){
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    sky.texture.setCubeMap(texture);

    sky.needUpdateTexture = true;
}

void Skybox::setTexturePositiveX(const std::string& texture){
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    sky.texture.setCubePath(0, texture);

    sky.needUpdateTexture = true;
}

void Skybox::setTextureNegativeX(const std::string& texture){
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    sky.texture.setCubePath(1, texture);

    sky.needUpdateTexture = true;
}

void Skybox::setTexturePositiveY(const std::string& texture){
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    sky.texture.setCubePath(2, texture);

    sky.needUpdateTexture = true;
}

void Skybox::setTextureNegativeY(const std::string& texture){
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    sky.texture.setCubePath(3, texture);

    sky.needUpdateTexture = true;
}

void Skybox::setTexturePositiveZ(const std::string& texture){
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    sky.texture.setCubePath(4, texture);

    sky.needUpdateTexture = true;
}

void Skybox::setTextureNegativeZ(const std::string& texture){
    SkyboxComponent& sky = getComponent<SkyboxComponent>();

    sky.texture.setCubePath(5, texture);

    sky.needUpdateTexture = true;
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

    if (ShaderUniforms::set(sky.shaderUniforms, name, value))
        sky.needUpdateShaderUniforms = true;
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
    return true;
}