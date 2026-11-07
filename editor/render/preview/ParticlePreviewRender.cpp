// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticlePreviewRender.h"
#include "component/LightComponent.h"

#include "Engine.h"
#include "component/ParticleSystemComponent.h"
#include "component/SkyboxComponent.h"
#include "subsystem/ParticleSubsystem.h"
#include "subsystem/ParticleSystemState.h"

#include <algorithm>
#include <chrono>
#include <cmath>

using namespace jasperengine;

namespace {

    // the moving emitter: a circle of this radius, once around in this many seconds
    constexpr float kMovingRadius = 2.0f;
    constexpr double kMovingPeriod = 4.0;

}

bool editor::ParticlePreviewRender::StageSystem::operator==(const StageSystem& other) const{
    return name == other.name && effect == other.effect && position == other.position && rotation == other.rotation && scale == other.scale;
}

editor::ParticlePreviewRender::~ParticlePreviewRender(){
    clear();
}

void editor::ParticlePreviewRender::clear(){
    emitter.reset();
    systems.clear();
    root.reset();
    sky.reset();
    stageSystems.clear();
    // a frame queued for it must not draw it once it is gone
    if (scene) Engine::removeScene(scene);
    delete camera;
    delete light;
    delete scene;
    camera = nullptr;
    light = nullptr;
    scene = nullptr;
    path.clear();
    center = Vector3(0.0f, 1.0f, 0.0f);
    radius = 3.0f;
    stepped = false;
    width = height = 0;
    time = 0.0;
    seeking = manualSeek = false;
}

void editor::ParticlePreviewRender::createScene(unsigned int w, unsigned int h){
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

    width = w;
    height = h;
    camera->setFramebufferSize(w, h);
    camera->setRenderToTexture(true);
    camera->setType(CameraType::CAMERA_PERSPECTIVE);
    camera->setAspect(h > 0 ? (float)w / (float)h : 1.0f);
    camera->setPosition(5.0f, 4.0f, 7.0f);
    camera->setTarget(center.x, center.y, center.z);
}

void editor::ParticlePreviewRender::build(const std::string& assetPath, unsigned int size){
    if (scene && !stepped && path == assetPath){
        return;
    }
    clear();
    path = assetPath;
    createScene(size, size);

    emitter = std::make_unique<ParticleSystem>(scene);
    emitter->setEffect(assetPath);
    emitter->setPlayOnAwake(true);
}

void editor::ParticlePreviewRender::restart(){
    if (emitter) emitter->restart();
}

uint32_t editor::ParticlePreviewRender::getParticleCount() const{
    return emitter ? emitter->getParticleCount() : 0;
}

// eases the framing towards the particles' bounds (the stage: every system's), unless the stage holds it
void editor::ParticlePreviewRender::frameCamera(bool follow){
    if (follow){
        float lo[3] = {3.0e38f, 3.0e38f, 3.0e38f};
        float hi[3] = {-3.0e38f, -3.0e38f, -3.0e38f};
        bool any = false;
        auto add = [&](const ParticleSystem* system){
            const ParticleSystemComponent* particles = system ? scene->findComponent<ParticleSystemComponent>(system->getEntity()) : nullptr;
            if (!particles || !particles->state || !particles->state->hasBounds || particles->state->runtime.totalCount() == 0) return;
            for (int k = 0; k < 3; k++){
                lo[k] = std::min(lo[k], particles->state->boundsMin[k]);
                hi[k] = std::max(hi[k], particles->state->boundsMax[k]);
            }
            any = true;
        };
        add(emitter.get());
        for (const std::unique_ptr<ParticleSystem>& system : systems) add(system.get());
        if (any){
            const Vector3 targetCenter = (Vector3(lo[0], lo[1], lo[2]) + Vector3(hi[0], hi[1], hi[2])) * 0.5f;
            const float targetRadius = std::max(0.5f, (Vector3(hi[0], hi[1], hi[2]) - Vector3(lo[0], lo[1], lo[2])).length() * 0.5f);
            center = center + (targetCenter - center) * 0.05f;
            radius = radius + (targetRadius - radius) * 0.05f;
        }
    }
    const float distance = radius / std::tan(22.5f * 3.14159265f / 180.0f) * zoomFactor;
    Vector3 offset(0.5f, 0.3f, 0.8f);
    offset.normalize();
    if (stepped){
        // the stage's orbit turns the default view
        const float yawRad = yaw * 3.14159265f / 180.0f;
        const float pitchRad = std::clamp(std::asin(offset.y) + pitch * 3.14159265f / 180.0f, -1.5f, 1.5f);
        const float baseYaw = std::atan2(offset.x, offset.z) + yawRad;
        offset = Vector3(std::cos(pitchRad) * std::sin(baseYaw), std::sin(pitchRad), std::cos(pitchRad) * std::cos(baseYaw));
    }
    const Vector3 position = center + offset * distance;
    camera->setPosition(position.x, position.y, position.z);
    camera->setTarget(center.x, center.y, center.z);
    camera->setNearClip(std::max(0.01f, distance - radius * 3.0f));
    camera->setFarClip(distance + radius * 6.0f + 100.0f);
}

void editor::ParticlePreviewRender::update(){
    if (!scene || !emitter){
        return;
    }
    // a one-shot effect plays again once it is over
    if (!emitter->isPlaying() && !emitter->isPaused() && emitter->getParticleCount() == 0){
        emitter->restart();
    }
    frameCamera(true);
    Engine::executeSceneOnce(scene);
}

Texture editor::ParticlePreviewRender::getTexture(){
    return camera ? Texture(camera->getFramebuffer()) : Texture();
}

// ---------------------------------------------------------------- the stage

void editor::ParticlePreviewRender::buildStage(const std::vector<StageSystem>& list, unsigned int w, unsigned int h){
    w = std::max(16u, w);
    h = std::max(16u, h);
    if (scene && stepped && stageSystems == list){
        if (w != width || h != height){
            width = w;
            height = h;
            camera->setFramebufferSize(w, h);
            camera->setAspect((float)w / (float)h);
        }
        return;
    }
    const float keepYaw = yaw, keepPitch = pitch, keepZoom = zoomFactor;
    const Background keepBackground = background;
    const bool keepMoving = moving;
    const uint32_t keepSeed = seed;
    clear();
    stepped = true;
    yaw = keepYaw;
    pitch = keepPitch;
    zoomFactor = keepZoom;
    background = keepBackground;
    moving = keepMoving;
    seed = keepSeed;
    createScene(w, h);
    // the caller drives every tick (the subsystem binds the effects, simulates nothing)
    scene->getSystem<ParticleSubsystem>()->setPaused(true);

    stageSystems = list;
    root = std::make_unique<GameObject>(scene);
    root->setName("Stage");
    for (size_t i = 0; i < list.size(); i++){
        auto system = std::make_unique<ParticleSystem>(scene);
        system->setName(list[i].name.empty() ? "System" : list[i].name);
        root->addChild(system.get());
        system->setPosition(list[i].position);
        system->setRotation(list[i].rotation);
        system->setScale(list[i].scale);
        system->setEffect(list[i].effect);
        system->setPlayOnAwake(true);
        system->setSeed(seed + (uint32_t)i);
        systems.push_back(std::move(system));
    }
    placeRoot(0.0);
    if (background != Background::Dark) setBackground(background, nullptr);
}

Entity editor::ParticlePreviewRender::systemEntity(size_t index) const{
    return index < systems.size() ? systems[index]->getEntity() : NULL_ENTITY;
}

Vector3 editor::ParticlePreviewRender::movingOffset(double at){
    const double angle = at / kMovingPeriod * 6.283185307179586;
    return Vector3(kMovingRadius * (float)std::cos(angle), 0.0f, kMovingRadius * (float)std::sin(angle));
}

void editor::ParticlePreviewRender::placeRoot(double at){
    if (!root) return;
    root->setPosition(moving ? movingOffset(at) : Vector3(0.0f, 0.0f, 0.0f));
}

double editor::ParticlePreviewRender::tickSeconds() const{
    if (!systems.empty() && scene){
        const ParticleSystemComponent* particles = scene->findComponent<ParticleSystemComponent>(systems[0]->getEntity());
        if (particles && particles->state && particles->state->effect && particles->state->effect->document.tickRate > 0){
            return 1.0 / particles->state->effect->document.tickRate;
        }
    }
    return 1.0 / 30.0;
}

void editor::ParticlePreviewRender::setSeed(uint32_t value){
    seed = value == 0 ? 1 : value;
    for (size_t i = 0; i < systems.size(); i++){
        systems[i]->setSeed(seed + (uint32_t)i);
    }
}

void editor::ParticlePreviewRender::invalidate(){
    if (!scene) return;
    std::shared_ptr<ParticleSubsystem> particles = scene->getSystem<ParticleSubsystem>();
    for (const std::unique_ptr<ParticleSystem>& system : systems){
        // (getState binds the effect again first: the checkpoints go after the rebind)
        if (particles::ParticleSystemState* state = particles->getState(system->getEntity())){
            if (state->preview) state->preview->invalidate();
        }
    }
}

void editor::ParticlePreviewRender::seek(double to){
    if (!scene || !stepped) return;
    std::shared_ptr<ParticleSubsystem> particles = scene->getSystem<ParticleSubsystem>();
    seekTarget = std::max(0.0, to);
    seeking = true;
    manualSeek = moving;
    if (manualSeek){
        // the motion is part of the run: replayed tick by tick from the start
        manualTime = 0.0;
        placeRoot(0.0);
        for (const std::unique_ptr<ParticleSystem>& system : systems){
            particles->restart(system->getEntity());
        }
    }else{
        placeRoot(0.0);
        for (const std::unique_ptr<ParticleSystem>& system : systems){
            particles->previewSeek(system->getEntity(), seekTarget);
        }
    }
}

bool editor::ParticlePreviewRender::updateSeek(double budgetMs){
    if (!seeking) return true;
    std::shared_ptr<ParticleSubsystem> particles = scene->getSystem<ParticleSubsystem>();
    if (manualSeek){
        const auto begin = std::chrono::steady_clock::now();
        const double tick = tickSeconds();
        while (manualTime < seekTarget){
            const double dt = std::min(tick, seekTarget - manualTime);
            manualTime += dt;
            placeRoot(manualTime);
            for (const std::unique_ptr<ParticleSystem>& system : systems){
                particles->previewAdvance(system->getEntity(), dt);
            }
            if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count() >= budgetMs) break;
        }
        seeking = manualTime < seekTarget;
    }else{
        bool done = true;
        const double share = budgetMs / (double)std::max<size_t>(1, systems.size());
        for (const std::unique_ptr<ParticleSystem>& system : systems){
            done = particles->previewUpdate(system->getEntity(), share) && done;
        }
        seeking = !done;
    }
    if (!seeking) time = seekTarget;
    return !seeking;
}

float editor::ParticlePreviewRender::seekProgress() const{
    if (!seeking || seekTarget <= 0.0) return 1.0f;
    if (manualSeek) return (float)std::clamp(manualTime / seekTarget, 0.0, 1.0);
    double least = 1.0;
    for (const std::unique_ptr<ParticleSystem>& system : systems){
        const ParticleSystemComponent* particles = scene->findComponent<ParticleSystemComponent>(system->getEntity());
        if (particles && particles->state && particles->state->preview && particles->state->preview->pending()){
            least = std::min(least, particles->state->preview->simulatedSeconds() / seekTarget);
        }
    }
    return (float)std::clamp(least, 0.0, 1.0);
}

void editor::ParticlePreviewRender::advance(double dt){
    if (!scene || !stepped || seeking || dt <= 0.0) return;
    std::shared_ptr<ParticleSubsystem> particles = scene->getSystem<ParticleSubsystem>();
    time += dt;
    placeRoot(time);
    for (const std::unique_ptr<ParticleSystem>& system : systems){
        particles->previewAdvance(system->getEntity(), dt);
    }
}

void editor::ParticlePreviewRender::step(){
    advance(tickSeconds());
}

void editor::ParticlePreviewRender::setMovingEmitter(bool value){
    moving = value;
    placeRoot(time);
}

void editor::ParticlePreviewRender::setBackground(Background value, Scene* sceneWithSky){
    background = value;
    if (!scene) return;
    if (sky) sky->getComponent<SkyboxComponent>().visible = false;
    switch (value){
        case Background::Dark: scene->setBackgroundColor(0.12f, 0.12f, 0.13f, 1.0f); break;
        case Background::Light: scene->setBackgroundColor(0.78f, 0.78f, 0.80f, 1.0f); break;
        // the window draws the checker under the image: the stage clears to nothing
        case Background::Checker: scene->setBackgroundColor(0.0f, 0.0f, 0.0f, 0.0f); break;
        case Background::Black: scene->setBackgroundColor(0.0f, 0.0f, 0.0f, 1.0f); break;
        case Background::SceneSky: {
            scene->setBackgroundColor(0.12f, 0.12f, 0.13f, 1.0f);
            const SkyboxComponent* source = nullptr;
            if (sceneWithSky && sceneWithSky->isComponentRegistered<SkyboxComponent>()){
                auto skies = sceneWithSky->getComponentArray<SkyboxComponent>();
                if (skies->size() > 0) source = &skies->getComponentFromIndex(0);
            }
            if (!source){
                // a scene without a sky: its background colour
                if (sceneWithSky){
                    const Vector4 color = sceneWithSky->getBackgroundColor();
                    scene->setBackgroundColor(color.x, color.y, color.z, 1.0f);
                }
                break;
            }
            if (!sky){
                scene->setDefaultEntityPool(EntityPool::System);
                sky = std::make_unique<Skybox>(scene);
                scene->setDefaultEntityPool(EntityPool::User);
            }
            SkyboxComponent& target = sky->getComponent<SkyboxComponent>();
            if (target.texture.getId() != source->texture.getId()){
                target.texture = source->texture;
                target.needReload = true;
            }
            target.color = source->color;
            target.rotation = source->rotation;
            target.visible = true;
            break;
        }
    }
}

void editor::ParticlePreviewRender::orbit(float yawDegrees, float pitchDegrees){
    yaw += yawDegrees;
    pitch = std::clamp(pitch + pitchDegrees, -80.0f, 80.0f);
}

void editor::ParticlePreviewRender::zoom(float factor){
    zoomFactor = std::clamp(zoomFactor * factor, 0.1f, 10.0f);
}

void editor::ParticlePreviewRender::resetView(){
    yaw = pitch = 0.0f;
    zoomFactor = 1.0f;
}

editor::ParticlePreviewRender::StageStats editor::ParticlePreviewRender::stats(){
    StageStats out;
    if (!scene) return out;
    std::shared_ptr<ParticleSubsystem> particles = scene->getSystem<ParticleSubsystem>();
    for (const std::unique_ptr<ParticleSystem>& system : systems){
        const ParticleSystemStats s = particles->getStats(system->getEntity());
        if (!s.valid) continue;
        out.live += s.live;
        out.capacity += s.capacity;
        out.simulateMs += s.simulateMicros / 1000.0;
        out.renderMs += s.renderMicros / 1000.0;
        out.stateHash = (out.stateHash * 1099511628211ull) ^ particles->getStateHash(system->getEntity());
    }
    return out;
}

void editor::ParticlePreviewRender::setExposed(const std::string& name, const Vector4& value){
    for (const std::unique_ptr<ParticleSystem>& system : systems){
        const particles::ParticleEffectDocument document = system->getEffectDocument();
        for (const particles::ExposedParameter& parameter : document.exposed){
            if (parameter.name != name) continue;
            switch (parameter.type){
                case particles::ExposedType::Float: system->setFloat(name, value.x); break;
                case particles::ExposedType::Vector3: system->setVector3(name, Vector3(value.x, value.y, value.z)); break;
                case particles::ExposedType::Color: system->setColor(name, value); break;
            }
        }
    }
}

void editor::ParticlePreviewRender::sendTrigger(const std::string& name){
    for (const std::unique_ptr<ParticleSystem>& system : systems){
        system->sendTrigger(name);
    }
}

void editor::ParticlePreviewRender::render(){
    if (!scene) return;
    frameCamera(true);
    Engine::executeSceneOnce(scene);
}
