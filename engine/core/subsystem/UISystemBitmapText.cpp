// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The Text component with a bitmap font (docs/specs/S51-pixel-fonts.md): integer pixel scale
// (R7), font pixels of whole device pixels (R8), NEAREST sampling from the font's own atlas (R9),
// and pixel outlines and shadows from the styled glyph cache (R10, R12).

#include "UISystem.h"
#include "component/CameraComponent.h"
#include "component/RectTransformComponent.h"
#include "component/TextComponent.h"
#include "component/UIComponent.h"

#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "pixelart/BitmapFont.h"
#include "pixelart/BitmapFontPool.h"
#include "pixelart/BitmapText.h"
#include "pixelart/BitmapTextCache.h"
#include "pixelart/FontPixelReport.h"
#include "util/Color.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace{
    // linear colour to the sRGB bytes the cache page stores (sampled as an sRGB texture)
    uint32_t bitmapStyleColor(const Vector4& linear){
        const Vector3 srgb = Color::linearTosRGB(Vector3(linear.x, linear.y, linear.z));
        auto byte = [](float v){ return (uint32_t)std::lround(std::clamp(std::isfinite(v) ? v : 0.0f, 0.0f, 1.0f) * 255.0f); };
        return byte(srgb.x) | (byte(srgb.y) << 8) | (byte(srgb.z) << 16) | (byte(linear.w) << 24);
    }

    // R10: whole font pixels; softness and dilate do not apply
    pixelart::BitmapTextStyle bitmapStyleOf(const TextComponent& text){
        pixelart::BitmapTextStyle style;
        const float width = std::isfinite(text.outlineWidth) ? text.outlineWidth : 0.0f;
        style.outlineWidth = std::clamp((int)std::lround(width), 0, 2);
        style.outlineNeighbors = text.outlineNeighbors == 4 ? 4 : 8;
        style.outlineColor = style.outlineWidth > 0 ? bitmapStyleColor(text.outlineColor) : 0u;
        style.shadow = text.shadowEnabled;
        if (style.shadow){
            auto whole = [](float v){ return std::isfinite(v) ? std::clamp((int)std::lround(v), -64, 64) : 0; };
            style.shadowX = whole(text.shadowOffset.x);
            style.shadowY = whole(text.shadowOffset.y);
            style.shadowColor = bitmapStyleColor(text.shadowColor);
        }
        return style;
    }
}

float UISystem::getBitmapTextUnit(int pixelScale) const{
    const int k = std::max(1, pixelScale);
    // world text under a Pixels Per Unit: a font pixel is k base pixels, k / PPU units (S41 D2)
    const int ppu = Engine::getPixelsPerUnit();
    if (ppu > 1){
        CameraComponent* camera = scene->findComponent<CameraComponent>(scene->getCamera());
        if (camera && camera->type != CameraType::CAMERA_UI)
            return (float)k / (float)ppu;
    }
    const float s = getTextRasterScale();
    if (s == 1.0f)
        return (float)k;
    const int d = std::max(1, (int)std::lround((float)k * s));
    return (float)d / s;
}

bool UISystem::loadBitmapFont(TextComponent& text, UIComponent& ui){
    std::vector<std::string> paths;
    for (const AssetRef& font : text.font)
        paths.push_back(font.path);
    std::string key;
    std::shared_ptr<pixelart::BitmapFont> font = pixelart::BitmapFontPool::getChain(paths, nullptr, &key);
    if (!font){
        // still a pixel font: the built-in one draws until the file loads
        Log::error("Cannot load bitmap font: %s", text.font[0].c_str());
        key = pixelart::kBuiltinPixelFont;
        font = pixelart::BitmapFontPool::get(key);
    }
    text.bitmapText = std::make_shared<pixelart::BitmapText>(font, key, pixelart::BitmapFontPool::generation(key));
    text.bitmapText->poolRevision = pixelart::BitmapFontPool::revision();
    text.stbtext.reset();
    text.atlasVersion = 0;
    text.rasterScale = 1.0f;
    text.sdfPadding = 0;
    text.bitmapSizeNoted = false;
    ui.textSdf = false;
    ui.pixelOriginSnap = true;

    text.needReloadAtlas = false;
    text.loaded = true;
    return true;
}

void UISystem::createBitmapText(TextComponent& text, UIComponent& ui, RectTransformComponent& layout){
    const pixelart::BitmapFont& font = *text.bitmapText->getFont();
    const int k = pixelart::pixelScaleFor(text.fontSize, font.getNativeSize());
    const float unit = getBitmapTextUnit(k);
    text.pixelScale = k;
    text.pixelUnit = unit;

    const pixelart::BitmapTextStyle style = bitmapStyleOf(text);
    const bool cached = style.hasBack() || font.getPages().size() > 1;
    const bool colors = text.vertexColor || cached;
    const unsigned int quadsPerCharacter = style.hasBack() ? 2 : 1;

    ui.primitiveType = PrimitiveType::TRIANGLES;
    ui.buffer.clear();
    ui.buffer.addAttribute(AttributeType::POSITION, 3);
    ui.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
    // a Text Effect, or the cache's two layers, colour each glyph
    if (colors)
        ui.buffer.addAttribute(AttributeType::COLOR, 4);
    ui.buffer.setUsage(BufferUsage::DYNAMIC);
    ui.vertexColorAbsolute = colors;
    text.buildCount++;

    ui.indices.clear();
    ui.indices.setUsage(BufferUsage::DYNAMIC);

    if (text.text.length() > text.maxTextSize){
        unsigned int newSize = std::max(text.maxTextSize, 1u);
        while (newSize < text.text.length())
            newSize *= 2;
        Log::warn("Text is bigger than maxTextSize: %i. Increasing it to: %i", text.maxTextSize, newSize);
        text.maxTextSize = newSize;
    }
    if (ui.loaded && (size_t)text.maxTextSize * 4 * quadsPerCharacter * ui.buffer.getStride() > ui.gpuBufferBytes)
        ui.needReload = true;
    ui.minBufferCount = text.maxTextSize * 4 * quadsPerCharacter;
    ui.minIndicesCount = text.maxTextSize * 6 * quadsPerCharacter;

    pixelart::BitmapTextOptions options;
    options.unit = unit;
    options.fixedWidth = text.fixedWidth;
    options.fixedHeight = text.fixedHeight;
    options.multiline = text.multiline;
    options.invert = ui.flipY;
    options.style = style;
    options.vertexColor = colors;
    options.faceColor = Vector3(ui.color.x, ui.color.y, ui.color.z);

    std::vector<uint16_t> indices;
    text.bitmapText->createText(text.text, &ui.buffer, indices, text.charPositions, text.charExtents, layout.width, layout.height, options,
                                &text.glyphInfo);

    // the font's atlas (or the cache page), sampled NEAREST without mips (R9)
    const bool rgba = text.bitmapText->isColoredTexture();
    if (ui.textRgba != rgba){
        ui.textRgba = rgba;
        if (ui.loaded)
            ui.needReload = true;
    }
    if (TextureData* data = text.bitmapText->getTexture()){
        const std::string id = text.bitmapText->getTextureId();
        if (ui.texture.getId() != id){
            ui.texture.setData(id, *data);
            ui.needUpdateTexture = true;
        }
    }
    if (ui.texture.getMinFilter() != TextureFilter::NEAREST || ui.texture.getMagFilter() != TextureFilter::NEAREST){
        ui.texture.setMinFilter(TextureFilter::NEAREST);
        ui.texture.setMagFilter(TextureFilter::NEAREST);
        ui.needUpdateTexture = true;
    }
    ui.textSdf = false;
    ui.pixelOriginSnap = true;
    text.bitmapFaceColor = options.faceColor;

    // R7: Lua and MCP may set any size; development builds say once what it draws at
    if (!text.bitmapSizeNoted && Engine::isDevelopmentBuild() && (int)text.fontSize != k * font.getNativeSize()){
        text.bitmapSizeNoted = true;
        Log::print("Text font size %u is not a multiple of its bitmap font's %d px: drawn at %d (x%d)", text.fontSize, font.getNativeSize(),
                  k * font.getNativeSize(), k);
    }

    finishTextGeometry(text, ui, layout, text.bitmapText->getAscent(unit), unit, false, indices);
}

void UISystem::updateBitmapText(TextComponent& text, UIComponent& ui){
    if (!text.loaded || !text.bitmapText || text.needReloadAtlas)
        return;
    pixelart::BitmapText& bitmap = *text.bitmapText;

    // a font file changed on disk (S05): load again
    const uint32_t revision = pixelart::BitmapFontPool::revision();
    if (bitmap.poolRevision != revision){
        bitmap.poolRevision = revision;
        if (pixelart::BitmapFontPool::generation(bitmap.getChainKey()) != bitmap.getGeneration()){
            text.needReloadAtlas = true;
            return;
        }
    }

    // a new canvas scale, font size or style builds the quads again (the atlas stays)
    const int k = pixelart::pixelScaleFor(text.fontSize, bitmap.getFont()->getNativeSize());
    if (k != text.pixelScale || getBitmapTextUnit(k) != text.pixelUnit || bitmapStyleOf(text) != bitmap.getLastStyle())
        text.needUpdateText = true;

    if (!bitmap.usesCache())
        return;
    if (bitmap.isCacheStale()){
        text.needUpdateText = true;
        return;
    }
    pixelart::BitmapTextCache::get().touch(bitmap.getCachePage());

    // D5: the Text colour reaches the face quads as vertex colours, never the cache
    const Vector3 face(ui.color.x, ui.color.y, ui.color.z);
    if (!text.vertexColor && !text.needUpdateText && !(face == text.bitmapFaceColor)){
        Attribute* colors = ui.buffer.getAttribute(AttributeType::COLOR);
        if (colors){
            const unsigned int first = (unsigned int)bitmap.getBackQuads() * 4;
            for (unsigned int v = first; v < colors->getCount(); v++)
                ui.buffer.setVector4(v, colors, Vector4(face, 1.0f));
            ui.needUpdateBuffer = ui.loaded;
        }
        text.bitmapFaceColor = face;
    }
}
