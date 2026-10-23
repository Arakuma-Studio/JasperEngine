// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FluidSystem.h"

#include "Engine.h"
#include "Input.h"
#include "Log.h"
#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/ImageComponent.h"
#include "component/MeshComponent.h"
#include "component/RectTransformComponent.h"
#include "component/SpriteComponent.h"
#include "component/Transform.h"
#include "component/UIComponent.h"
#include "fluid/FluidSimulator.h"
#include "render/CameraRender.h"
#include "render/PipelineState.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace jasperengine;

struct FluidSystem::State {
    struct Entry {
        double time = 0.0;              // scaled seconds not simulated yet
        bool preview = false;           // steps while the system is paused (the Inspector's)
        bool mouseDown = false;         // the mouse drags this fluid (pointer 0)
        bool mousePressed = false;      // the left button was down at the last update
        std::set<int> touches;          // the touches dragging it (pointer 1 + touch id)
        unsigned long outputVersion = 0;
        bool lending = false;           // the entity's Sprite, Image or Mesh shows the output
        std::string lastError;
    };

    std::map<Entity, Entry> entries;
    CameraRender compositePass;
    bool materials = false;             // a MATERIAL output is lent this frame
};

namespace {

    // what shows a MATERIAL output: an Image's texture, else a Mesh's (a Sprite's) submesh 0
    // base colour. Its texture binding is made again (its own texture) when the output stops
    void rebindOwnTexture(Scene* scene, Entity entity){
        UIComponent* ui = scene->findComponent<UIComponent>(entity);
        if (ui && scene->findComponent<ImageComponent>(entity)){
            ui->needUpdateTexture = true;
            if (ui->texture.empty())
                ui->needReload = true;
        }else if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)){
            mesh->submeshes[0].needUpdateTexture = true;
        }
    }

    bool inside01(Vector2 p){
        return p.x >= 0.0f && p.x <= 1.0f && p.y >= 0.0f && p.y <= 1.0f;
    }

}

FluidSystem::FluidSystem(Scene* scene): SubSystem(scene), state(new State()){
}

FluidSystem::~FluidSystem(){
    delete state;
}

void FluidSystem::load(){
}

void FluidSystem::draw(){
    // the fluids draw in RenderSystem's frame passes (renderFluids)
}

void FluidSystem::destroy(){
    // a view loss or the scene's end: the GPU side goes, the fields restart with the next view
    if (!scene->isComponentRegistered<FluidSimulationComponent>())
        return;
    auto fluids = scene->getComponentArray<FluidSimulationComponent>();
    for (size_t i = 0; i < fluids->size(); i++){
        FluidSimulationComponent& fluid = fluids->getComponentFromIndex(i);
        if (fluid.simulator)
            fluid.simulator->release();
    }
}

float FluidSystem::getAspectRatio(Entity entity) const{
    if (!scene->isComponentRegistered<FluidSimulationComponent>())
        return 1.0f;
    const FluidSimulationComponent* fluid = scene->findComponent<FluidSimulationComponent>(entity);
    if (!fluid)
        return 1.0f;
    if (fluid->aspectRatio > 0.0f && std::isfinite(fluid->aspectRatio))
        return fluid->aspectRatio;
    if (fluid->output == FluidOutput::SCREEN){
        const int width = Engine::getCanvasWidth();
        const int height = Engine::getCanvasHeight();
        return (width > 0 && height > 0) ? (float)width / (float)height : 1.0f;
    }
    if (fluid->output == FluidOutput::MATERIAL){
        if (scene->findComponent<ImageComponent>(entity)){
            const RectTransformComponent* layout = scene->findComponent<RectTransformComponent>(entity);
            if (layout && layout->width > 0 && layout->height > 0)
                return (float)layout->width / (float)layout->height;
        }else if (const SpriteComponent* sprite = scene->findComponent<SpriteComponent>(entity)){
            // (a sprite sized by its texture would follow the fluid's own size: square)
            if (sprite->width > 0 && sprite->height > 0)
                return (float)sprite->width / (float)sprite->height;
        }
    }
    return 1.0f;
}

// a canvas point on the entity's Sprite or Image, in fluid coordinates (through the scene
// camera onto the quad's plane); false when it has none or misses the plane
static bool pointerOnQuad(Scene* scene, Entity entity, Vector2 canvasPoint, Vector2& fluidPoint){
    CameraComponent* camera = scene->findComponent<CameraComponent>(scene->getCamera());
    Transform* transform = scene->findComponent<Transform>(entity);
    const float canvasWidth = (float)Engine::getCanvasWidth();
    const float canvasHeight = (float)Engine::getCanvasHeight();
    if (!camera || !transform || canvasWidth <= 0.0f || canvasHeight <= 0.0f)
        return false;

    // the quad's local rectangle and which way its texture's rows run
    float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
    bool flipY = false;
    if (scene->findComponent<ImageComponent>(entity)){
        const UIComponent* ui = scene->findComponent<UIComponent>(entity);
        const RectTransformComponent* layout = scene->findComponent<RectTransformComponent>(entity);
        if (!ui || !layout)
            return false;
        maxX = (float)layout->width;
        maxY = (float)layout->height;
        flipY = ui->flipY;
    }else if (const SpriteComponent* sprite = scene->findComponent<SpriteComponent>(entity)){
        const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        if (!mesh || mesh->aabb.isNull())
            return false;
        minX = mesh->aabb.getMinimum().x;
        minY = mesh->aabb.getMinimum().y;
        maxX = mesh->aabb.getMaximum().x;
        maxY = mesh->aabb.getMaximum().y;
        flipY = sprite->flipY;
    }else{
        return false;
    }
    if (maxX - minX <= 1e-6f || maxY - minY <= 1e-6f)
        return false;

    // the pointer's ray (Camera::screenToRay) in the quad's space
    const float nx = (2.0f * canvasPoint.x) / canvasWidth - 1.0f;
    const float ny = -((2.0f * canvasPoint.y) / canvasHeight - 1.0f);
    const Matrix4 toLocal = transform->modelMatrix.inverse() * camera->viewProjectionMatrix.inverse();
    Vector4 nearPoint = toLocal * Vector4(nx, ny, -1.0f, 1.0f);
    Vector4 farPoint = toLocal * Vector4(nx, ny, 1.0f, 1.0f);
    if (std::fabs(nearPoint.w) < 1e-12f || std::fabs(farPoint.w) < 1e-12f)
        return false;
    nearPoint.divideByW();
    farPoint.divideByW();
    const float dz = farPoint.z - nearPoint.z;
    if (std::fabs(dz) < 1e-12f)
        return false;
    const float t = -nearPoint.z / dz;
    const float lx = nearPoint.x + (farPoint.x - nearPoint.x) * t;
    const float ly = nearPoint.y + (farPoint.y - nearPoint.y) * t;

    // a flipped quad samples its texture's first row at its local top (the output's is the
    // fluid's top)
    const float tx = (lx - minX) / (maxX - minX);
    const float ty = (ly - minY) / (maxY - minY);
    fluidPoint = Vector2(tx, flipY ? ty : 1.0f - ty);
    return true;
}

void FluidSystem::update(double dt){
    if (!scene->isComponentRegistered<FluidSimulationComponent>())
        return;
    auto fluids = scene->getComponentArray<FluidSimulationComponent>();
    for (size_t i = 0; i < fluids->size(); i++){
        const Entity entity = fluids->getEntity(i);
        FluidSimulationComponent& fluid = fluids->getComponentFromIndex(i);
        State::Entry& entry = state->entries[entity];
        if (!fluid.enabled || !scene->isEntityActiveInHierarchy(entity))
            continue;
        if (!fluid.simulator)
            fluid.simulator = std::make_shared<FluidSimulator>();

        // the scene's time (none in a paused scene, the editor's edit mode, unless previewed)
        const bool running = !paused || entry.preview;
        if (running && dt > 0.0)
            entry.time += dt * (double)std::max(fluid.timeScale, 0.0f);

        // the pointer: Pavel's mouse and touches, over the view (SCREEN) or the quad (MATERIAL)
        FluidSimulator& simulator = *fluid.simulator;
        const bool onScreen = fluid.output == FluidOutput::SCREEN;
        const bool onQuad = fluid.output == FluidOutput::MATERIAL &&
                            (scene->findComponent<ImageComponent>(entity) || scene->findComponent<SpriteComponent>(entity));
        if (!fluid.interactive || !running || !(onScreen || onQuad)){
            if (entry.mouseDown || !entry.touches.empty())
                simulator.pointerCancelAll();
            entry.mouseDown = false;
            entry.touches.clear();
            entry.mousePressed = Input::isMousePressed(D_MOUSE_BUTTON_LEFT);
            continue;
        }
        auto toFluid = [&](Vector2 canvasPoint, Vector2& fluidPoint) -> bool {
            if (onScreen){
                const float width = (float)Engine::getCanvasWidth();
                const float height = (float)Engine::getCanvasHeight();
                if (width <= 0.0f || height <= 0.0f)
                    return false;
                fluidPoint = Vector2(canvasPoint.x / width, 1.0f - canvasPoint.y / height);
                return true;
            }
            return pointerOnQuad(scene, entity, canvasPoint, fluidPoint);
        };

        const std::vector<Touch> touches = Input::getTouches();
        const bool pressed = Input::isMousePressed(D_MOUSE_BUTTON_LEFT);
        if (!touches.empty() || !entry.touches.empty()){
            // touches (a mouse the platform mirrors from them aside)
            std::set<int> now;
            for (const Touch& touch : touches){
                const int id = 1 + touch.pointer;
                now.insert(id);
                Vector2 point;
                const bool hit = toFluid(touch.position, point);
                if (entry.touches.count(id)){
                    if (hit)
                        simulator.pointerMove(id, point);
                }else if (hit && inside01(point)){
                    simulator.pointerDown(id, point);
                    entry.touches.insert(id);
                }
            }
            for (auto it = entry.touches.begin(); it != entry.touches.end();){
                if (!now.count(*it)){
                    simulator.pointerUp(*it);
                    it = entry.touches.erase(it);
                }else{
                    ++it;
                }
            }
            if (entry.mouseDown){
                simulator.pointerUp(0);
                entry.mouseDown = false;
            }
        }else{
            Vector2 point;
            const bool hit = toFluid(Input::getMousePosition(), point);
            if (pressed){
                if (entry.mouseDown){
                    if (hit)
                        simulator.pointerMove(0, point);
                }else if (!entry.mousePressed && hit && inside01(point)){
                    // a press on the fluid (a drag coming in from outside is not one)
                    simulator.pointerDown(0, point);
                    entry.mouseDown = true;
                }
            }else if (entry.mouseDown){
                simulator.pointerUp(0);
                entry.mouseDown = false;
            }
        }
        entry.mousePressed = pressed;
    }
}

void FluidSystem::renderFluids(){
    state->materials = false;
    if (!scene->isComponentRegistered<FluidSimulationComponent>())
        return;
    bool materials = false;
    auto fluids = scene->getComponentArray<FluidSimulationComponent>();
    for (size_t i = 0; i < fluids->size(); i++){
        const Entity entity = fluids->getEntity(i);
        FluidSimulationComponent& fluid = fluids->getComponentFromIndex(i);
        State::Entry& entry = state->entries[entity];
        const bool active = fluid.enabled && scene->isEntityActiveInHierarchy(entity);
        if (!active){
            if (entry.lending)
                rebindOwnTexture(scene, entity);
            entry.lending = false;
            continue;
        }
        if (!fluid.simulator)
            fluid.simulator = std::make_shared<FluidSimulator>();

        const bool drawn = fluid.simulator->update(fluid, getAspectRatio(entity), entry.time);
        entry.time = 0.0;
        if (!drawn){
            const std::string& error = fluid.simulator->getError();
            // (shaders building is no error: the fluid starts once they are)
            if (!error.empty() && error != entry.lastError && error.find("being compiled") == std::string::npos)
                Log::error("Fluid Simulation '%s': %s", scene->getEntityName(entity).c_str(), error.c_str());
            entry.lastError = error;
        }else{
            entry.lastError.clear();
        }

        Framebuffer* output = fluid.simulator->getOutputFramebuffer();
        if (output->isCreated() && output->getVersion() != entry.outputVersion){
            entry.outputVersion = output->getVersion();
            fluid.revision++;
        }
        // MATERIAL: lent at draw time (getMaterialTexture) while there is an image to lend
        const bool lending = fluid.output == FluidOutput::MATERIAL && drawn && output->isCreated();
        if (entry.lending != lending){
            // an Image without a texture of its own has no texture to sample: loaded again
            // with one (RenderSystem::loadUI asks getMaterialTexture), and again without
            UIComponent* ui = scene->findComponent<UIComponent>(entity);
            if (ui && ui->texture.empty() && scene->findComponent<ImageComponent>(entity))
                ui->needReload = true;
        }
        if (entry.lending && !lending)
            rebindOwnTexture(scene, entity);
        entry.lending = lending;
        materials = materials || lending;
    }
    state->materials = materials;
}

bool FluidSystem::hasScreenFluids() const{
    if (!scene->isComponentRegistered<FluidSimulationComponent>())
        return false;
    auto fluids = scene->getComponentArray<FluidSimulationComponent>();
    for (size_t i = 0; i < fluids->size(); i++){
        const FluidSimulationComponent& fluid = fluids->getComponentFromIndex(i);
        if (fluid.output == FluidOutput::SCREEN && fluid.enabled && fluid.simulator && fluid.simulator->getOutputFramebuffer()->isCreated() &&
            scene->isEntityActiveInHierarchy(fluids->getEntity(i)))
            return true;
    }
    return false;
}

void FluidSystem::compositeScreen(FramebufferRender* target, const Rect& viewRect, bool flipY){
    if (!hasScreenFluids())
        return;
    CameraRender& pass = state->compositePass;
    pass.setColorActions(LoadAction::LOAD, StoreAction::STORE);
    pass.setDepthStencilActions(LoadAction::LOAD, StoreAction::STORE);
    pass.setLabel("fluid composite");
    if (target)
        pass.startRenderPass(target);
    else
        pass.startRenderPass();
    pass.applyViewport(viewRect);

    PipelineState composite;
    composite.target = targetFormatFor(target ? PIP_RTT : PIP_DEFAULT);
    auto fluids = scene->getComponentArray<FluidSimulationComponent>();
    for (size_t i = 0; i < fluids->size(); i++){
        FluidSimulationComponent& fluid = fluids->getComponentFromIndex(i);
        if (fluid.output != FluidOutput::SCREEN || !fluid.enabled || !fluid.simulator || !scene->isEntityActiveInHierarchy(fluids->getEntity(i)))
            continue;
        fluid.simulator->drawComposite(composite, flipY);
    }
    pass.endRenderPass();
}

bool FluidSystem::hasMaterialFluids() const{
    return state->materials;
}

TextureRender* FluidSystem::getMaterialTexture(Entity entity) const{
    if (!state->materials)
        return nullptr;
    auto it = state->entries.find(entity);
    if (it == state->entries.end() || !it->second.lending)
        return nullptr;
    const FluidSimulationComponent* fluid = scene->findComponent<FluidSimulationComponent>(entity);
    if (!fluid || !fluid->simulator || fluid->output != FluidOutput::MATERIAL || !fluid->enabled)
        return nullptr;
    Framebuffer* output = fluid->simulator->getOutputFramebuffer();
    return output->isCreated() ? &output->getRender().getColorTexture() : nullptr;
}

void FluidSystem::onComponentAdded(Entity entity, ComponentId componentId){
    if (!scene->isComponentRegistered<FluidSimulationComponent>() || componentId != scene->getComponentId<FluidSimulationComponent>())
        return;
    // the simulator itself is made at the first update (lazily); a fresh entry restarts
    state->entries[entity] = State::Entry();
}

void FluidSystem::onComponentRemoved(Entity entity, ComponentId componentId){
    if (!scene->isComponentRegistered<FluidSimulationComponent>() || componentId != scene->getComponentId<FluidSimulationComponent>())
        return;
    FluidSimulationComponent* fluid = scene->findComponent<FluidSimulationComponent>(entity);
    auto it = state->entries.find(entity);
    if (fluid){
        if (it != state->entries.end() && it->second.lending)
            rebindOwnTexture(scene, entity);
        if (fluid->simulator){
            fluid->simulator->release();
            fluid->simulator.reset();
        }
    }
    if (it != state->entries.end())
        state->entries.erase(it);
}

void FluidSystem::setPreview(Entity entity, bool preview){
    state->entries[entity].preview = preview;
}

bool FluidSystem::isPreview(Entity entity) const{
    auto it = state->entries.find(entity);
    return it != state->entries.end() && it->second.preview;
}
