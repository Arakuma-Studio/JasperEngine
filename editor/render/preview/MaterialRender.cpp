// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MaterialRender.h"
#include "component/MeshComponent.h"
#include "component/SkyboxComponent.h"
#include "component/LightComponent.h"

#include "Configs.h"
#include "PreviewEnvironment.h"
#include "pool/TextureDataPool.h"
#include "pool/TexturePool.h"
#include "shader/ShaderManifest.h"
#include "util/CameraTextureLink.h"

using namespace jasperengine;

namespace {

    // the colour write a preview draws with: alpha added to a mask that writes colour but not
    // alpha (explicit, else the shader's @state colorwrite with these features)
    int previewColorWrite(int colorWrite, const std::string& shader, const std::vector<std::string>& features){
        int mask = colorWrite;
        if (mask < 0 && !shader.empty()){
            std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(shader);
            if (manifest && manifest->present)
                mask = manifest->resolveState(manifest->resolveFeatures(features)).colorWrite;
        }
        if (mask < 0 || (mask & MATERIAL_COLOR_WRITE_RGB) == 0 || (mask & MATERIAL_COLOR_WRITE_A) != 0)
            return colorWrite;
        return mask | MATERIAL_COLOR_WRITE_A;
    }

}

editor::MaterialRender::MaterialRender(){
    scene = new Scene(EntityPool::System);
    camera = new Camera(scene);
    light = new Light(scene);
    sky = new Skybox(scene);
    sphere = new Primitive(scene);

    scene->setBackgroundColor(0.0, 0.0, 0.0, 0.0);
    scene->setCamera(camera);

    sphere->createSphere(1.0);
    setReceiveIBL(receiveIBL);

    // interior environment used only as IBL light source; kept invisible so the
    // thumbnail keeps a transparent background (clean sphere icon on drag and drop)
    SkyboxComponent& skyComp = sky->getComponent<SkyboxComponent>();
    skyComp.visible = false;
    // the pool's faces while it has them (every MaterialRender, one per previewed material,
    // shares this id): setCubeDatas keeps those and drops new ones, whose pixels a TextureData
    // does not own, so decoding again leaked six images per preview (faces 4, 5, 1, 0, 2, 3 are
    // front, back, left, right, up, down)
    const std::shared_ptr<std::array<TextureData, 6>> cached = TextureDataPool::get("preview|environment");
    if (cached && TextureDataPool::hasTexturePixels(cached, 6)) {
        skyComp.texture.setCubeDatas("preview|environment",
            cached->at(4), cached->at(5), cached->at(1), cached->at(0), cached->at(2), cached->at(3));
    } else {
        skyComp.texture.setCubeDatas("preview|environment",
            TextureData((unsigned char*)preview_env_pz, preview_env_pz_len),  // front (+Z)
            TextureData((unsigned char*)preview_env_nz, preview_env_nz_len),  // back  (-Z)
            TextureData((unsigned char*)preview_env_nx, preview_env_nx_len),  // left  (-X)
            TextureData((unsigned char*)preview_env_px, preview_env_px_len),  // right (+X)
            TextureData((unsigned char*)preview_env_py, preview_env_py_len),  // up    (+Y)
            TextureData((unsigned char*)preview_env_ny, preview_env_ny_len)); // down  (-Y)
    }

    light->setDirection(-0.4, -0.5, -0.5);
    light->setIntensity(2.0);
    light->setType(LightType::DIRECTIONAL);

    camera->setPosition(0, 0, 3);
    camera->setTarget(0, 0, 0);
    camera->setType(CameraType::CAMERA_PERSPECTIVE);
    camera->setFramebufferSize(THUMBNAIL_SIZE, THUMBNAIL_SIZE);
    camera->setRenderToTexture(true);
}

editor::MaterialRender::~MaterialRender(){
    delete camera;
    delete light;
    delete sky;
    delete sphere;
    delete scene;
}

void editor::MaterialRender::applyMaterial(const Material& material, const std::string& meshShader){
    this->material = material;
    // the material's shader, its parameters, textures, features and render state are the
    // sphere's (Mesh::setMaterial reloads what the variant depends on)
    Material drawn = material;
    drawn.colorWrite = previewColorWrite(material.colorWrite, material.shader.empty() ? meshShader : material.shader.path,
                                         material.shaderFeatures);
    if (MaterialPass* pass = drawn.extraPass.editIfSet(); pass && !pass->shader.empty())
        pass->colorWrite = previewColorWrite(pass->colorWrite, pass->shader, pass->shaderFeatures);
    sphere->setCustomShader(meshShader);
    sphere->setMaterial(drawn);
}

const Material& editor::MaterialRender::getMaterial() const{
    return material;
}

std::string editor::MaterialRender::getMeshShader() const{
    return sphere->getCustomShader();
}

bool editor::MaterialRender::isPending() const{
    const MeshComponent& mesh = sphere->getComponent<MeshComponent>();
    if (!mesh.loaded || mesh.needReload || !mesh.loadCalled)
        return true;

    auto texturePending = [](const Texture& texture) {
        if (texture.isFramebuffer() || texture.empty() || CameraTextureLink::isCameraTexture(texture))
            return false; // framebuffer textures never enter the pool
        const std::string id = texture.getRenderId();
        return !id.empty() && !TexturePool::get(id);
    };
    const Material& material = mesh.submeshes[0].material;
    for (const Texture* texture : {&material.baseColorTexture, &material.emissiveTexture, &material.metallicRoughnessTexture,
                                   &material.occlusionTexture, &material.normalTexture}) {
        if (texturePending(*texture))
            return true;
    }
    for (const auto& shaderTexture : material.shaderTextures) {
        if (texturePending(shaderTexture.second))
            return true;
    }
    // the extra pass draws on the sphere too (outlines, silhouettes): its own textures
    for (const auto& shaderTexture : material.extraPass->shaderTextures) {
        if (texturePending(shaderTexture.second))
            return true;
    }
    return false;
}

void editor::MaterialRender::setReceiveIBL(bool value){
    MeshComponent& mesh = sphere->getComponent<MeshComponent>();
    if (mesh.receiveIBL != value){
        mesh.receiveIBL = value;
        mesh.needReload = true;
    }
    receiveIBL = value;
}

bool editor::MaterialRender::getReceiveIBL() const{
    return receiveIBL;
}

Framebuffer* editor::MaterialRender::getFramebuffer(){
    return camera->getFramebuffer();
}

Texture editor::MaterialRender::getTexture(){
    return Texture(getFramebuffer());
}

Scene* editor::MaterialRender::getScene(){
    return scene;
}
