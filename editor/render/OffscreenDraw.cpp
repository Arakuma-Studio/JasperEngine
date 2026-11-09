// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "OffscreenDraw.h"

#include "Scene.h"
#include "render/ObjectRender.h"
#include "subsystem/RenderSystem.h"
#include "texture/Framebuffer.h"
#include "util/GraphicUtils.h"

using namespace jasperengine;

editor::OffscreenDraw editor::drawOffscreen(uint32_t viewId, Framebuffer& target, const std::vector<Engine::ViewLayer>& layers, bool& warmed,
                                            std::vector<uint8_t>& rgba, std::string& waitingFor,
                                            const std::function<bool(std::string&)>& ready){
    if (!Engine::isViewLoaded()){
        waitingFor = "the graphics view";
        return OffscreenDraw::Wait;
    }
    auto loaded = [&](){
        for (const Engine::ViewLayer& layer : layers){
            if (layer.scene && !layer.scene->getSystem<RenderSystem>()->isAllLoaded()) return false;
        }
        return true;
    };
    if (!loaded()){
        waitingFor = "the scene's loading";
        return OffscreenDraw::Wait;
    }
    if (!warmed){
        // the first draw adds the cameras' pipelines; the objects load them on the next update
        Engine::drawView(viewId, &target, layers);
        warmed = true;
        waitingFor = "the first draw's pipelines";
        return OffscreenDraw::Wait;
    }
    if (ready && !ready(waitingFor)){
        return OffscreenDraw::Wait;
    }
    // a pipeline still being made draws nothing: never read that back
    const uint32_t waitingBefore = ObjectRender::getWaitingDrawCount();
    Engine::drawView(viewId, &target, layers);
    if (ObjectRender::getWaitingDrawCount() != waitingBefore){
        waitingFor = "pipelines being compiled";
        return OffscreenDraw::Wait;
    }
    if (!GraphicUtils::readFramebufferRGBA(&target, rgba)){
        waitingFor = "the read-back";
        return OffscreenDraw::Wait;
    }
    return OffscreenDraw::Done;
}
