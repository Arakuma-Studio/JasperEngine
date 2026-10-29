// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HtmlUITexture.h"

#include "Log.h"
#include "render/SystemRender.h"

using namespace jasperengine;

HtmlUITexture::~HtmlUITexture(){
    if (ownedRender && !dead){
        ownedRender->destroyTexture();
    }
    for (std::unique_ptr<TextureRender>& plane : planes){
        if (plane && !dead)
            plane->destroyTexture();
    }
}

std::unique_ptr<HtmlUITexture> HtmlUITexture::createDynamic(const std::string& label, int width, int height,
                                                            ColorFormat format, TextureFilter filter){
    std::unique_ptr<HtmlUITexture> texture = std::make_unique<HtmlUITexture>();
    texture->kind = Kind::DYNAMIC;
    texture->ownedRender = std::make_unique<TextureRender>();
    if (!texture->ownedRender->createDynamicTexture(label, width, height, format, filter)){
        Log::error("[HTML UI] could not create the %i x %i dynamic texture '%s'", width, height, label.c_str());
        return nullptr;
    }
    texture->render = texture->ownedRender.get();
    texture->width = width;
    texture->height = height;
    return texture;
}

bool HtmlUITexture::canUpdate() const{
    return kind == Kind::DYNAMIC && render && !dead && lastUpdateFrame != SystemRender::getFrameCount();
}

bool HtmlUITexture::update(const void* data, size_t size){
    if (!canUpdate() || !data || size == 0)
        return false;

    // past the frame's stream staging buffer: not uploaded (false), may be tried again
    if (!render->updateTexture(data, size))
        return false;
    lastUpdateFrame = SystemRender::getFrameCount();
    return true;
}
