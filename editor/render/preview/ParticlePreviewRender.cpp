// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticlePreviewRender.h"
#include "component/LightComponent.h"

#include "Engine.h"
#include "component/ParticleSystemComponent.h"
#include "subsystem/ParticleSubsystem.h"
#include "subsystem/ParticleSystemState.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

editor::ParticlePreviewRender::~ParticlePreviewRender(){
    clear();
}

void editor::ParticlePreviewRender::clear(){
    emitter.reset();
    delete camera;
    delete light;
    delete scene;
    camera = nullptr;
    light = nullptr;
    scene = nullptr;
    path.clear();
    center = Vector3(0.0f, 1.0f, 0.0f);
    radius = 3.0f;
}

void editor::ParticlePreviewRender::build(const std::string& assetPath, unsigned int size){
    if (scene && path == assetPath){
        return;
    }
    clear();
    path = assetPath;

    scene = new Scene();
    scene->setDefaultEntityPool(EntityPool::System);
    camera = new Camera(scene);
    light = new Light(scene);
    scene->setDefaultEntityPool(EntityPool::User);

    scene->setBackgroundColor(0.12f, 0.12f, 0.13f, 1.0f);
    scene->setCamera(camera);
    light->setDirection(-0.4f, -0.5f, -0.5f);
    light->setIntensity(4.0f);
    light->setType(LightType::DIRECTIONAL);

    emitter = std::make_unique<ParticleSystem>(scene);
    emitter->setEffect(assetPath);
    emitter->setPlayOnAwake(true);

    camera->setFramebufferSize(size, size);
    camera->setRenderToTexture(true);
    camera->setType(CameraType::CAMERA_PERSPECTIVE);
    camera->setAspect(1.0f);
    camera->setPosition(5.0f, 4.0f, 7.0f);
    camera->setTarget(center.x, center.y, center.z);
}

void editor::ParticlePreviewRender::restart(){
    if (emitter) emitter->restart();
}

uint32_t editor::ParticlePreviewRender::getParticleCount() const{
    return emitter ? emitter->getParticleCount() : 0;
}

void editor::ParticlePreviewRender::update(){
    if (!scene || !emitter){
        return;
    }
    // a one-shot effect plays again once it is over
    if (!emitter->isPlaying() && !emitter->isPaused() && emitter->getParticleCount() == 0){
        emitter->restart();
    }

    // ease the framing towards the particles' bounds
    const ParticleSystemComponent* particles = scene->findComponent<ParticleSystemComponent>(emitter->getEntity());
    if (particles && particles->state && particles->state->runtime.count() > 0){
        const particles::ParticleSystemState& state = *particles->state;
        const Vector3 lo(state.boundsMin[0], state.boundsMin[1], state.boundsMin[2]);
        const Vector3 hi(state.boundsMax[0], state.boundsMax[1], state.boundsMax[2]);
        const Vector3 targetCenter = (lo + hi) * 0.5f;
        const float targetRadius = std::max(0.5f, (hi - lo).length() * 0.5f);
        center = center + (targetCenter - center) * 0.05f;
        radius = radius + (targetRadius - radius) * 0.05f;
    }
    const float distance = radius / std::tan(22.5f * 3.14159265f / 180.0f);
    Vector3 offset(0.5f, 0.3f, 0.8f);
    offset.normalize();
    const Vector3 position = center + offset * distance;
    camera->setPosition(position.x, position.y, position.z);
    camera->setTarget(center.x, center.y, center.z);
    camera->setNearClip(std::max(0.01f, distance - radius * 3.0f));
    camera->setFarClip(distance + radius * 6.0f + 100.0f);

    Engine::executeSceneOnce(scene);
}

Texture editor::ParticlePreviewRender::getTexture(){
    return camera ? Texture(camera->getFramebuffer()) : Texture();
}
