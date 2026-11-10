// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteFxPreview.h"

#include "Engine.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
#include "object/Sprite.h"
#include "pool/TexturePool.h"
#include "shader/ShaderManifest.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

using namespace jasperengine;

namespace {

    // what a thumbnail shows, as text: a change of any of it draws it again
    std::string spriteFxThumbnailKey(const Material& material, const std::string& feature, const Rect& frame, int texelsX, int texelsY){
        std::ostringstream key;
        key << feature << '|' << material.shader.path << '|' << material.baseColorTexture.getPath() << '|' << material.baseColorTexture.getRenderId()
            << '|' << frame.getX() << ',' << frame.getY() << ',' << frame.getWidth() << ',' << frame.getHeight() << '|' << texelsX << 'x' << texelsY;
        for (const std::string& define : material.shaderFeatures)
            key << '|' << define;
        for (const auto& entry : material.shaderParams)
            key << '|' << entry.first << '=' << entry.second.x << ',' << entry.second.y << ',' << entry.second.z << ',' << entry.second.w;
        for (const auto& entry : material.shaderTextures)
            key << '|' << entry.first << '=' << entry.second.getPath();
        return key.str();
    }

}

editor::SpriteFxThumbnail::SpriteFxThumbnail(){
    scene = new Scene(EntityPool::System);
    camera = new Camera(scene);
    sprite = new Sprite(scene);
    scene->setBackgroundColor(0.0f, 0.0f, 0.0f, 0.0f);
    scene->setCamera(camera);
    // one unit a pixel of the thumbnail
    const float half = SIZE * 0.5f;
    camera->setType(CameraType::CAMERA_ORTHO);
    camera->setOrtho(-half, half, -half, half, -100.0f, 100.0f);
    camera->setPosition(0.0f, 0.0f, 10.0f);
    camera->setTarget(0.0f, 0.0f, 0.0f);
    camera->setFramebufferSize(SIZE, SIZE);
    camera->setFramebufferFilter(TextureFilter::NEAREST);
    camera->setRenderToTexture(true);
    sprite->setPivotPreset(PivotPreset::CENTER);
    sprite->setPixelsPerUnit(1.0f);
}

editor::SpriteFxThumbnail::~SpriteFxThumbnail(){
    delete sprite;
    delete camera;
    delete scene;
}

void editor::SpriteFxThumbnail::update(const Material& material, const std::string& feature, const Rect& frameRect, int frameTexelsX, int frameTexelsY){
    const std::string next = spriteFxThumbnailKey(material, feature, frameRect, frameTexelsX, frameTexelsY);
    if (next == key && rendered && !isPending())
        return;
    if (next != key){
        key = next;
        // only this feature (and its keyword choice), at the material's values
        Material drawn = material;
        std::vector<std::string> features = {feature};
        if (std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(material.shader)){
            for (const ShaderManifestKeywords& group : manifest->keywords){
                if (group.feature != feature)
                    continue;
                const std::string choice = manifest->getKeywordChoice(group, material.shaderFeatures);
                if (!choice.empty())
                    features.push_back(choice);
            }
        }
        drawn.shaderFeatures = features;
        drawn.extraPass = MaterialPass();
        // the largest whole zoom that keeps the frame and what grows around it in the square
        const int reach = 4;
        const int zoom = std::max(1, SIZE / std::max(1, std::max(frameTexelsX, frameTexelsY) + 2 * reach));
        sprite->setSize((unsigned int)(std::max(1, frameTexelsX) * zoom), (unsigned int)(std::max(1, frameTexelsY) * zoom));
        sprite->setTextureRect(frameRect);
        MeshComponent& mesh = sprite->getComponent<MeshComponent>();
        mesh.submeshes[0].material = drawn;
        mesh.submeshes[0].material.baseColorTexture.setMinFilter(TextureFilter::NEAREST);
        mesh.submeshes[0].material.baseColorTexture.setMagFilter(TextureFilter::NEAREST);
        mesh.submeshes[0].needUpdateTexture = true;
        mesh.needReload = true;
        sprite->getComponent<SpriteComponent>().needUpdateSprite = true;
    }
    Engine::executeSceneOnce(scene);
    rendered = true;
}

bool editor::SpriteFxThumbnail::isPending() const{
    const MeshComponent& mesh = sprite->getComponent<MeshComponent>();
    if (!mesh.loaded || mesh.needReload)
        return true;
    for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
        if (mesh.submeshes[s].needUpdateTexture)
            return true;
    }
    return !camera->getFramebuffer() || !camera->getFramebuffer()->isCreated();
}

Texture editor::SpriteFxThumbnail::getTexture(){
    return Texture(camera->getFramebuffer());
}

// ---------------------------------------------------------------- the scrub preview

void editor::SpriteFxScrub::scrub(Scene* scene, uint32_t sceneId, Entity entity, unsigned int submesh, const std::string& member, float value){
    if (active && (this->scene != scene || this->entity != entity || this->submesh != submesh || this->member != member))
        stop();
    if (!active){
        MeshComponent* mesh = scene ? scene->findComponent<MeshComponent>(entity) : nullptr;
        if (!mesh || submesh >= mesh->numSubmeshes)
            return;
        const ShaderUniformValues& values = mesh->submeshes[submesh].material.shaderParams;
        hadValue = false;
        for (const auto& entry : values){
            if (entry.first == member){
                hadValue = true;
                original = entry.second;
            }
        }
        this->scene = scene;
        this->sceneId = sceneId;
        this->entity = entity;
        this->submesh = submesh;
        this->member = member;
        active = true;
    }
    playing = false;
    write(value);
}

void editor::SpriteFxScrub::play(Scene* scene, uint32_t sceneId, Entity entity, unsigned int submesh, const std::string& member,
                                 float from, float to, float seconds){
    scrub(scene, sceneId, entity, submesh, member, from);
    if (!active)
        return;
    playing = true;
    this->from = from;
    this->to = to;
    this->seconds = std::max(seconds, 0.01f);
    elapsed = 0.0f;
}

void editor::SpriteFxScrub::update(float deltaSeconds){
    if (!active || !playing)
        return;
    elapsed += std::max(deltaSeconds, 0.0f);
    if (elapsed >= seconds){
        stop();
        return;
    }
    write(from + (to - from) * (elapsed / seconds));
}

void editor::SpriteFxScrub::write(float shown){
    MeshComponent* mesh = scene ? scene->findComponent<MeshComponent>(entity) : nullptr;
    if (!mesh || submesh >= mesh->numSubmeshes){
        active = playing = false;
        return;
    }
    value = shown;
    ShaderUniformValues& values = mesh->submeshes[submesh].material.shaderParams;
    bool found = false;
    for (auto& entry : values){
        if (entry.first == member){
            entry.second = Vector4(shown, 0.0f, 0.0f, 0.0f);
            found = true;
        }
    }
    if (!found)
        values.push_back({member, Vector4(shown, 0.0f, 0.0f, 0.0f)});
    mesh->needUpdateShaderUniforms = true;
}

void editor::SpriteFxScrub::stop(){
    if (!active)
        return;
    MeshComponent* mesh = scene ? scene->findComponent<MeshComponent>(entity) : nullptr;
    if (mesh && submesh < mesh->numSubmeshes){
        ShaderUniformValues& values = mesh->submeshes[submesh].material.shaderParams;
        if (hadValue){
            for (auto& entry : values){
                if (entry.first == member)
                    entry.second = original;
            }
        }else{
            values.erase(std::remove_if(values.begin(), values.end(), [&](const auto& entry){ return entry.first == member; }), values.end());
        }
        mesh->needUpdateShaderUniforms = true;
    }
    active = playing = false;
    scene = nullptr;
    entity = NULL_ENTITY;
    member.clear();
}
