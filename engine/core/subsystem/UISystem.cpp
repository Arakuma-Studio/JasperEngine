// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "UISystem.h"
#include "component/ActionComponent.h"
#include "component/TweenComponent.h"
#include "component/ButtonComponent.h"
#include "component/CameraComponent.h"
#include "component/CanvasGroupComponent.h"
#include "component/ImageComponent.h"
#include "component/InputFieldComponent.h"
#include "component/LayoutGroupComponent.h"
#include "component/OnScreenControlComponents.h"
#include "component/PanelComponent.h"
#include "component/PointsComponent.h"
#include "component/PolygonComponent.h"
#include "component/ProgressbarComponent.h"
#include "component/SafeAreaComponent.h"
#include "component/ScrollbarComponent.h"
#include "component/TextComponent.h"
#include "component/TextEffectComponent.h"
#include "component/Transform.h"
#include "component/UIComponent.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <vector>
#include "Scene.h"
#include "Input.h"
#include "Engine.h"
#include "System.h"
#include "Screen.h"
#include "util/STBText.h"
#include "util/Color.h"
#include "util/StringUtils.h"
#include "pool/FontPool.h"

using namespace jasperengine;


UISystem::UISystem(Scene* scene): SubSystem(scene){
    signature.set(scene->getComponentId<RectTransformComponent>());
    // the types onComponentRemoved handles
    uiId = scene->getComponentId<UIComponent>();
    rectTransformId = scene->getComponentId<RectTransformComponent>();
    imageId = scene->getComponentId<ImageComponent>();
    transformId = scene->getComponentId<Transform>();
    buttonId = scene->getComponentId<ButtonComponent>();
    panelId = scene->getComponentId<PanelComponent>();
    scrollbarId = scene->getComponentId<ScrollbarComponent>();
    progressbarId = scene->getComponentId<ProgressbarComponent>();
    inputFieldId = scene->getComponentId<InputFieldComponent>();
    textId = scene->getComponentId<TextComponent>();
    canvasGroupId = scene->getComponentId<CanvasGroupComponent>();
    pointsId = scene->getComponentId<PointsComponent>();
    actionId = scene->getComponentId<ActionComponent>();
    onScreenStickId = scene->getComponentId<OnScreenStickComponent>();
    onScreenButtonId = scene->getComponentId<OnScreenButtonComponent>();
    touchAreaId = scene->getComponentId<TouchAreaComponent>();
    for (ComponentId id : {uiId, rectTransformId, imageId, transformId, buttonId, panelId, scrollbarId, progressbarId, inputFieldId, textId,
                           canvasGroupId, pointsId, actionId, onScreenStickId, onScreenButtonId, touchAreaId}){
        declareHookType(id);
    }

    lastUIFromClick = NULL_ENTITY;
    lastClickTime = -1.0;

    anchorReferenceWidth = 0;
    anchorReferenceHeight = 0;

    pendingImages = false;
}

UISystem::~UISystem(){
    // scene unload: the queued widgets and the helper fades go with it (their tweens are the
    // scene's entities, destroyed with it)
    interactabilityLost.clear();
    canvasGroupFades.clear();
}

bool UISystem::createImagePatches(ImageComponent& img, UIComponent& ui, RectTransformComponent& layout){
    // before the first load, see RenderSystem::loadUI
    ui.texture.setAlphaBorderAuto(true);

    unsigned int texWidth = ui.texture.getWidth();
    unsigned int texHeight = ui.texture.getHeight();

    if ((texWidth == 0 || texHeight == 0) && !ui.texture.empty()){
        // Reuse cached texture metadata when available. Reloading here is only
        // needed for first-time loads, not when resize just rebuilds patches.
        TextureLoadResult texResult = ui.texture.load();
        if (texResult.state == ResourceLoadState::Finished){
            texWidth = ui.texture.getWidth();
            texHeight = ui.texture.getHeight();
        }else if (texResult.state == ResourceLoadState::Loading){
            return false;
        }
    }

    if (texWidth == 0 || texHeight == 0){
        texWidth = layout.width;
        texHeight = layout.height;
    }

    if (layout.width == 0 && layout.height == 0){
        layout.width = texWidth;
        layout.height = texHeight;
    }

    ui.primitiveType = PrimitiveType::TRIANGLES;

    ui.buffer.clear();
    ui.buffer.addAttribute(AttributeType::POSITION, 3);
    ui.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
    ui.buffer.addAttribute(AttributeType::COLOR, 4);
    ui.buffer.setUsage(BufferUsage::DYNAMIC);

    ui.indices.clear();
    ui.indices.setUsage(BufferUsage::DYNAMIC);

    Attribute* atrVertex = ui.buffer.getAttribute(AttributeType::POSITION);

    ui.buffer.addVector3(atrVertex, Vector3(0, 0, 0)); //0
    ui.buffer.addVector3(atrVertex, Vector3(layout.width, 0, 0)); //1
    ui.buffer.addVector3(atrVertex, Vector3(layout.width,  layout.height, 0)); //2
    ui.buffer.addVector3(atrVertex, Vector3(0,  layout.height, 0)); //3

    ui.buffer.addVector3(atrVertex, Vector3(img.patchMarginLeft, img.patchMarginTop, 0)); //4
    ui.buffer.addVector3(atrVertex, Vector3(layout.width-img.patchMarginRight, img.patchMarginTop, 0)); //5
    ui.buffer.addVector3(atrVertex, Vector3(layout.width-img.patchMarginRight,  layout.height-img.patchMarginBottom, 0)); //6
    ui.buffer.addVector3(atrVertex, Vector3(img.patchMarginLeft,  layout.height-img.patchMarginBottom, 0)); //7

    ui.buffer.addVector3(atrVertex, Vector3(img.patchMarginLeft, 0, 0)); //8
    ui.buffer.addVector3(atrVertex, Vector3(0, img.patchMarginTop, 0)); //9

    ui.buffer.addVector3(atrVertex, Vector3(layout.width-img.patchMarginRight, 0, 0)); //10
    ui.buffer.addVector3(atrVertex, Vector3(layout.width, img.patchMarginTop, 0)); //11

    ui.buffer.addVector3(atrVertex, Vector3(layout.width-img.patchMarginRight, layout.height, 0)); //12
    ui.buffer.addVector3(atrVertex, Vector3(layout.width, layout.height-img.patchMarginBottom, 0)); //13

    ui.buffer.addVector3(atrVertex, Vector3(img.patchMarginLeft, layout.height, 0)); //14
    ui.buffer.addVector3(atrVertex, Vector3(0, layout.height-img.patchMarginBottom, 0)); //15

    Attribute* atrTexcoord = ui.buffer.getAttribute(AttributeType::TEXCOORD1);

    float texCutRatioW = 0;
    float texCutRatioH = 0;
    if (texWidth != 0 && texHeight != 0){
        texCutRatioW = 1.0 / texWidth * img.textureScaleFactor;
        texCutRatioH = 1.0 / texHeight * img.textureScaleFactor;
    }

    float x0 = texCutRatioW;
    float x1 = 1.0-texCutRatioW;
    float y0 = texCutRatioH;
    float y1 = 1.0-texCutRatioH;

    ui.buffer.addVector2(atrTexcoord, Vector2(x0, y0));
    ui.buffer.addVector2(atrTexcoord, Vector2(x1, y0));
    ui.buffer.addVector2(atrTexcoord, Vector2(x1, y1));
    ui.buffer.addVector2(atrTexcoord, Vector2(x0, y1));

    ui.buffer.addVector2(atrTexcoord, Vector2(img.patchMarginLeft/(float)texWidth, img.patchMarginTop/(float)texHeight));
    ui.buffer.addVector2(atrTexcoord, Vector2(x1-(img.patchMarginRight/(float)texWidth), img.patchMarginTop/(float)texHeight));
    ui.buffer.addVector2(atrTexcoord, Vector2(x1-(img.patchMarginRight/(float)texWidth), y1-(img.patchMarginBottom/(float)texHeight)));
    ui.buffer.addVector2(atrTexcoord, Vector2(img.patchMarginLeft/(float)texWidth, y1-(img.patchMarginBottom/(float)texHeight)));

    ui.buffer.addVector2(atrTexcoord, Vector2(img.patchMarginLeft/(float)texWidth, y0));
    ui.buffer.addVector2(atrTexcoord, Vector2(x0, img.patchMarginTop/(float)texHeight));

    ui.buffer.addVector2(atrTexcoord, Vector2(x1-(img.patchMarginRight/(float)texWidth), y0));
    ui.buffer.addVector2(atrTexcoord, Vector2(x1, img.patchMarginTop/(float)texHeight));

    ui.buffer.addVector2(atrTexcoord, Vector2(x1-(img.patchMarginRight/(float)texWidth), y1));
    ui.buffer.addVector2(atrTexcoord, Vector2(x1, y1-(img.patchMarginBottom/(float)texHeight)));

    ui.buffer.addVector2(atrTexcoord, Vector2((img.patchMarginLeft/(float)texWidth), y1));
    ui.buffer.addVector2(atrTexcoord, Vector2(x0, y1-(img.patchMarginBottom/(float)texHeight)));

    if (ui.flipY){
        for (int i = 0; i < ui.buffer.getCount(); i++){
            Vector2 uv = ui.buffer.getVector2(atrTexcoord, i);
            uv.y = 1.0 - uv.y;
            ui.buffer.setVector2(i, atrTexcoord, uv);
        }
    }

    Attribute* atrColor = ui.buffer.getAttribute(AttributeType::COLOR);

    for (int i = 0; i < ui.buffer.getCount(); i++){
        ui.buffer.addVector4(atrColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    }

    static const uint16_t indices_array[] = {
        4,  5,  6, // Center
        4,  6,  7,

        0,  8,  4, // Top-left
        0,  4,  9,

        8, 10,  5, // Top
        8,  5,  4,

        10, 1, 11, // Top-right
        10, 11, 5,

        5, 11, 13, // Right
        5, 13,  6,

        6, 13,  2, // Bottom-right
        6,  2, 12,

        7,  6, 12, // Bottom
        7, 12, 14,

        15, 7, 14, // Bottom-left
        15, 14, 3,

        9,  4,  7, // Left
        9,  7, 15
    };

    ui.indices.setValues(
        0, ui.indices.getAttribute(AttributeType::INDEX),
        54, (char*)&indices_array[0], sizeof(uint16_t));

    calculateUIAABB(ui);

    if (ui.loaded)
        ui.needUpdateBuffer = true;

    return true;
}

// an outline or a shadow needs the signed-distance atlas
static bool hasTextOutlineOrShadow(const TextComponent& text){
    return (std::isfinite(text.outlineWidth) && text.outlineWidth > 0.0f) || text.shadowEnabled;
}

STBTextOptions UISystem::getTextOptions(const TextComponent& text) const{
    STBTextOptions options;
    const float fontSize = (float)std::max(text.fontSize, 1u);

    // whole raster pixels, so a resize reuses an atlas once the size settles
    const float scale = getTextRasterScale();
    float rasterSize = fontSize;
    if (scale != 1.0f)
        rasterSize = std::max(1.0f, std::round(fontSize * scale));

    if (hasTextOutlineOrShadow(text)){
        // a distance field of small glyphs keeps its corners at 32 pixels and more
        rasterSize = std::max(rasterSize, 32.0f);
        options.rasterScale = rasterSize / fontSize;

        float extent = std::max(text.outlineWidth, 0.0f);
        if (text.shadowEnabled){
            const float offset = std::max(std::fabs(text.shadowOffset.x), std::fabs(text.shadowOffset.y));
            extent = std::max(extent, std::max(text.shadowSoftness, 0.0f) + std::max(text.shadowDilate, 0.0f) + offset);
        }
        if (!std::isfinite(extent))
            extent = 0.0f;
        int padding = (int)std::ceil(extent * options.rasterScale) + 2;
        // steps of 4 pixels, so dragging a width does not make an atlas per value
        padding = std::clamp(((padding + 3) / 4) * 4, 4, 64);
        options.sdfPadding = padding;
    }else if (rasterSize != fontSize){
        options.rasterScale = rasterSize / fontSize;
    }

    return options;
}

std::string UISystem::getFontId(const TextComponent& text) const{
    return getFontId(text, getTextOptions(text));
}

std::string UISystem::getFontId(const TextComponent& text, const STBTextOptions& options) const{
    std::string fontId = text.font[0];
    if (fontId.empty())
        fontId = "font";

    //two texts only share an atlas when they fall back the same way
    for (size_t i = 1; i < text.font.size(); i++){
        if (!text.font[i].empty())
            fontId += std::string(";") + text.font[i];
    }

    fontId += std::string("|") + std::to_string(text.fontSize);
    // another raster size or an SDF is another atlas (plain texts keep their old id)
    if (options.rasterScale != 1.0f)
        fontId += std::string("@") + std::to_string((int)std::lround((float)text.fontSize * options.rasterScale));
    if (options.sdfPadding > 0)
        fontId += std::string("~sdf") + std::to_string(options.sdfPadding);
    return fontId;
}

std::string UISystem::getFontTextureId(const TextComponent& text) const{
    // Every atlas change gets its own texture. The previous one stays untouched for
    // the texts still using it, instead of being rebuilt while bound, and is
    // released when the last of them is rebuilt.
    return getFontId(text) + std::string("#") + std::to_string(text.stbtext->getAtlasVersion());
}

bool UISystem::isFontAtlasStale(const TextComponent& text) const{
    //another text using the same font can have added glyphs to the atlas
    return text.loaded && text.stbtext && text.stbtext->getAtlasVersion() != text.atlasVersion;
}

void UISystem::syncFontAtlas(TextComponent& text, UIComponent& ui){
    if (!text.stbtext || text.stbtext->getAtlasVersion() == text.atlasVersion)
        return;

    TextureData* atlas = text.stbtext->getTextureData();
    if (!atlas)
        return;

    ui.texture.setData(getFontTextureId(text), *atlas);

    text.atlasVersion = text.stbtext->getAtlasVersion();
    ui.needUpdateTexture = true;
}

bool UISystem::loadFontAtlas(TextComponent& text, UIComponent& ui, RectTransformComponent& layout){
    const STBTextOptions options = getTextOptions(text);
    std::string fontId = getFontId(text, options);

    text.stbtext = FontPool::get(fontId);
    if (!text.stbtext){
        std::vector<std::string> fallbacks;
        for (size_t i = 1; i < text.font.size(); i++){
            if (!text.font[i].empty())
                fallbacks.push_back(text.font[i]);
        }

        text.stbtext = FontPool::get(fontId, text.font[0], fallbacks, text.fontSize, options);
        if (!text.stbtext) {
            Log::error("Cannot load font atlas from: %s", text.font[0].c_str());
            return false;
        }
    }

    //texture is published by syncFontAtlas, after createText rasterized the glyphs
    text.atlasVersion = 0;
    text.rasterScale = options.rasterScale;
    text.sdfPadding = options.sdfPadding;

    text.needReloadAtlas = false;
    text.loaded = true;

    return true;
}

// glyph quads are (x0,y0) (x1,y0) (x1,y1) (x0,y1), with y0 < y1 in both flipY modes
static void clipTextQuad(Buffer& buffer, Attribute* atrVertice, Attribute* atrTexcoord, int firstVertex, const Rect& clip, bool clipWidth, bool clipHeight){
    Vector3 p0 = buffer.getVector3(atrVertice, firstVertex);
    Vector3 p1 = buffer.getVector3(atrVertice, firstVertex + 1);
    Vector3 p2 = buffer.getVector3(atrVertice, firstVertex + 2);
    Vector3 p3 = buffer.getVector3(atrVertice, firstVertex + 3);

    Vector2 uv0 = buffer.getVector2(atrTexcoord, firstVertex);
    Vector2 uv1 = buffer.getVector2(atrTexcoord, firstVertex + 1);
    Vector2 uv2 = buffer.getVector2(atrTexcoord, firstVertex + 2);
    Vector2 uv3 = buffer.getVector2(atrTexcoord, firstVertex + 3);

    float left = clip.getX();
    float right = clip.getX() + clip.getWidth();
    float bottom = clip.getY();
    float top = clip.getY() + clip.getHeight();

    if ((clipWidth && (p1.x <= left || p0.x >= right)) || (clipHeight && (p2.y <= bottom || p0.y >= top))){
        for (int i = 0; i < 4; i++){
            buffer.setVector3(firstVertex + i, atrVertice, Vector3(0, 0, 0));
        }
        return;
    }

    if (clipWidth && p0.x < left){
        float ratio = (left - p0.x) / (p1.x - p0.x);
        uv0.x = uv0.x + (uv1.x - uv0.x) * ratio;
        uv3.x = uv3.x + (uv2.x - uv3.x) * ratio;
        p0.x = left;
        p3.x = left;
    }

    if (clipWidth && p1.x > right){
        float ratio = (right - p0.x) / (p1.x - p0.x);
        uv1.x = uv0.x + (uv1.x - uv0.x) * ratio;
        uv2.x = uv3.x + (uv2.x - uv3.x) * ratio;
        p1.x = right;
        p2.x = right;
    }

    if (clipHeight && p0.y < bottom){
        float ratio = (bottom - p0.y) / (p2.y - p0.y);
        uv0.y = uv0.y + (uv3.y - uv0.y) * ratio;
        uv1.y = uv1.y + (uv2.y - uv1.y) * ratio;
        p0.y = bottom;
        p1.y = bottom;
    }

    if (clipHeight && p2.y > top){
        float ratio = (top - p0.y) / (p2.y - p0.y);
        uv3.y = uv0.y + (uv3.y - uv0.y) * ratio;
        uv2.y = uv1.y + (uv2.y - uv1.y) * ratio;
        p2.y = top;
        p3.y = top;
    }

    buffer.setVector3(firstVertex, atrVertice, p0);
    buffer.setVector3(firstVertex + 1, atrVertice, p1);
    buffer.setVector3(firstVertex + 2, atrVertice, p2);
    buffer.setVector3(firstVertex + 3, atrVertice, p3);

    buffer.setVector2(firstVertex, atrTexcoord, uv0);
    buffer.setVector2(firstVertex + 1, atrTexcoord, uv1);
    buffer.setVector2(firstVertex + 2, atrTexcoord, uv2);
    buffer.setVector2(firstVertex + 3, atrTexcoord, uv3);
}

// the glyph quads of characters from `count` on (characters: the clusters in logical order)
// collapse to a point
void UISystem::hideTextGlyphsAfter(TextComponent& text, UIComponent& ui, size_t count){
    Attribute* atrVertice = ui.buffer.getAttribute(AttributeType::POSITION);
    if (!atrVertice)
        return;

    // the character index of each cluster start, in logical order
    std::vector<uint32_t> clusters;
    clusters.reserve(text.glyphInfo.size());
    for (const TextGlyphInfo& info : text.glyphInfo)
        clusters.push_back(info.cluster);
    std::sort(clusters.begin(), clusters.end());
    clusters.erase(std::unique(clusters.begin(), clusters.end()), clusters.end());

    for (size_t g = 0; g < text.glyphInfo.size(); g++){
        const size_t character = (size_t)(std::lower_bound(clusters.begin(), clusters.end(), text.glyphInfo[g].cluster) - clusters.begin());
        if (character < count)
            continue;
        const int first = (int)(g * 4);
        if (first + 3 >= (int)ui.buffer.getCount())
            break;
        const Vector3 corner = ui.buffer.getVector3(atrVertice, first);
        for (int v = 0; v < 4; v++)
            ui.buffer.setVector3(first + v, atrVertice, corner);
    }
}

void UISystem::updateTextShaderParams(TextComponent& text, UIComponent& ui){
    ui.textSdf = text.sdfPadding > 0 && text.stbtext;
    if (!ui.textSdf)
        return;

    const float raster = text.rasterScale;
    const bool outline = std::isfinite(text.outlineWidth) && text.outlineWidth > 0.0f;
    const Vector4 outlineColor = outline ? text.outlineColor : Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    const Vector4 shadowColor = text.shadowEnabled ? text.shadowColor : Vector4(0.0f, 0.0f, 0.0f, 0.0f);

    // colours linear as ui.color, sizes in atlas pixels, within the glyph's padding (the
    // padding is capped: wider outlines and shadows stop at its edge, not at a neighbour glyph)
    const float reach = std::max((float)text.sdfPadding - 2.0f, 0.0f);
    const float softness = std::min(std::max(text.shadowSoftness, 0.0f) * raster, reach);
    const float dilate = std::min(std::max(text.shadowDilate, 0.0f) * raster, reach - softness);
    ui.textParams[0] = outlineColor;
    ui.textParams[1] = shadowColor;
    ui.textParams[2] = Vector4(outline ? std::min(text.outlineWidth * raster, reach) : 0.0f,
                               softness,
                               dilate,
                               text.stbtext->getSdfDistanceScale());
    float u = 0.0f, v = 0.0f;
    if (TextureData* atlas = text.stbtext->getTextureData()){
        if (atlas->getWidth() > 0 && atlas->getHeight() > 0 && text.shadowEnabled){
            u = std::clamp(text.shadowOffset.x * raster, -reach, reach) / (float)atlas->getWidth();
            v = std::clamp(text.shadowOffset.y * raster, -reach, reach) / (float)atlas->getHeight();
        }
    }
    ui.textParams[3] = Vector4(u, v, 0.0f, 0.0f);
}

void UISystem::createText(TextComponent& text, UIComponent& ui, RectTransformComponent& layout){

    ui.primitiveType = PrimitiveType::TRIANGLES;

    ui.buffer.clear();
    ui.buffer.addAttribute(AttributeType::POSITION, 3);
    ui.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
    // a Text Effect colours each glyph (plain texts keep their two attributes)
    if (text.vertexColor)
        ui.buffer.addAttribute(AttributeType::COLOR, 4);
    ui.buffer.setUsage(BufferUsage::DYNAMIC);
    ui.vertexColorAbsolute = text.vertexColor;
    text.buildCount++;

    ui.indices.clear();
    ui.indices.setUsage(BufferUsage::DYNAMIC);

    std::vector<uint16_t> indices_array;

    if (text.text.length() > text.maxTextSize){
        unsigned int newSize = text.maxTextSize;
        if (newSize == 0) newSize = 1;
        while (newSize < text.text.length()){
            newSize *= 2;
        }

        Log::warn("Text is bigger than maxTextSize: %i. Increasing it to: %i", text.maxTextSize, newSize);
        text.maxTextSize = newSize;
        if (ui.loaded){
            ui.needReload = true;
        }
    }

    ui.minBufferCount = text.maxTextSize * 4;
    ui.minIndicesCount = text.maxTextSize * 6;

    text.stbtext->createText(text.text, &ui.buffer, indices_array, text.charPositions, text.charExtents, layout.width, layout.height, text.fixedWidth, text.fixedHeight, text.multiline, ui.flipY,
                             &text.glyphInfo);

    syncFontAtlas(text, ui);
    updateTextShaderParams(text, ui);

    float baselineOffsetY = ui.flipY ? (layout.height - text.stbtext->getAscent()) : text.stbtext->getAscent();
    float offsetX = text.pivotCentered ? -(layout.width / 2.0f) : 0.0f;
    float offsetY = text.pivotBaseline ? 0.0f : baselineOffsetY;

    if (text.pivotCentered || !text.pivotBaseline){
        Attribute* atrVertice = ui.buffer.getAttribute(AttributeType::POSITION);
        for (int i = 0; i < ui.buffer.getCount(); i++){
            Vector3 vertice = ui.buffer.getVector3(atrVertice, i);

            vertice.x = vertice.x + offsetX;
            vertice.y = vertice.y + offsetY;

            ui.buffer.setVector3(i, atrVertice, vertice);
        }
    }

    // a size not resolved yet would clip every glyph away
    bool clipWidth = text.fixedWidth && layout.width > 0;
    bool clipHeight = text.fixedHeight && layout.height > 0;

    if (clipWidth || clipHeight){
        // the layout rect follows the same pivot offset applied to the glyphs
        Rect clip(offsetX, offsetY - baselineOffsetY, layout.width, layout.height);

        Attribute* atrVertice = ui.buffer.getAttribute(AttributeType::POSITION);
        Attribute* atrTexcoord = ui.buffer.getAttribute(AttributeType::TEXCOORD1);
        for (int i = 0; i + 3 < ui.buffer.getCount(); i += 4){
            clipTextQuad(ui.buffer, atrVertice, atrTexcoord, i, clip, clipWidth, clipHeight);
        }
    }

    // TMP maxVisibleCharacters: the glyphs of later characters collapse (layout unchanged)
    if (text.visibleCharacters >= 0){
        hideTextGlyphsAfter(text, ui, (size_t)text.visibleCharacters);
    }

    if (text.vertexColor){
        // the Text Effect writes the colours (rgb, and its alpha factor); until then the text's own
        Attribute* atrColor = ui.buffer.getAttribute(AttributeType::COLOR);
        const unsigned int vertices = ui.buffer.getAttribute(AttributeType::POSITION)->getCount();
        for (unsigned int i = 0; i < vertices; i++){
            ui.buffer.addVector4(atrColor, Vector4(ui.color.x, ui.color.y, ui.color.z, 1.0f));
        }
    }

    ui.indices.setValues(
            0, ui.indices.getAttribute(AttributeType::INDEX),
            indices_array.size(), (char*)&indices_array[0], sizeof(uint16_t));

    calculateUIAABB(ui);

    if (ui.loaded)
        ui.needUpdateBuffer = true;
}

void UISystem::createButtonObjects(Entity entity, ButtonComponent& button){
    if (button.label != NULL_ENTITY && !scene->findComponent<TextComponent>(button.label)){
        button.label = NULL_ENTITY;
    }

    if (button.label == NULL_ENTITY){
        button.label = scene->createEntity();

        scene->addComponent<Transform>(button.label);
        scene->addComponent<RectTransformComponent>(button.label);
        scene->addComponent<UIComponent>(button.label);
        scene->addComponent<TextComponent>(button.label);

        scene->addEntityChild(entity, button.label, false);

        UIComponent& labelui = scene->getComponent<UIComponent>(button.label);
        RectTransformComponent& labellayout = scene->getComponent<RectTransformComponent>(button.label);

        labelui.color = Vector4(0.0, 0.0, 0.0, 1.0);
        labellayout.ignoreEvents = true;
        labellayout.anchorPreset = AnchorPreset::CENTER;
        labellayout.usingAnchors = true;
    }
}

void UISystem::createPanelObjects(Entity entity, PanelComponent& panel){
    if (panel.headerimage != NULL_ENTITY && !scene->findComponent<ImageComponent>(panel.headerimage)){
        panel.headerimage = NULL_ENTITY;
    }
    if (panel.headercontainer != NULL_ENTITY && !scene->findComponent<LayoutGroupComponent>(panel.headercontainer)){
        panel.headercontainer = NULL_ENTITY;
    }
    if (panel.headertext != NULL_ENTITY && !scene->findComponent<TextComponent>(panel.headertext)){
        panel.headertext = NULL_ENTITY;
    }
    if (panel.headerimage == NULL_ENTITY){
        panel.headercontainer = NULL_ENTITY;
        panel.headertext = NULL_ENTITY;
    }else if (panel.headercontainer == NULL_ENTITY){
        panel.headertext = NULL_ENTITY;
    }

    if (panel.headerimage == NULL_ENTITY){
        panel.headerimage = scene->createEntity();

        scene->addComponent<Transform>(panel.headerimage);
        scene->addComponent<RectTransformComponent>(panel.headerimage);
        scene->addComponent<UIComponent>(panel.headerimage);
        scene->addComponent<ImageComponent>(panel.headerimage);

        scene->addEntityChild(entity, panel.headerimage, false);

        UIComponent& headerui = scene->getComponent<UIComponent>(panel.headerimage);
        RectTransformComponent& headerimagelayout = scene->getComponent<RectTransformComponent>(panel.headerimage);

        headerui.color = Vector4(0, 0, 0, 0);
        headerimagelayout.ignoreEvents = true;
        headerimagelayout.ignoreScissor = true;
        headerimagelayout.usingAnchors = true;
        // same of TOP_WIDE
        headerimagelayout.anchorPointLeft = 0;
        headerimagelayout.anchorPointTop = 0;
        headerimagelayout.anchorPointRight = 1;
        headerimagelayout.anchorPointBottom = 0;
    }
    if (panel.headercontainer == NULL_ENTITY){
        panel.headercontainer = scene->createEntity();

        scene->addComponent<Transform>(panel.headercontainer);
        scene->addComponent<RectTransformComponent>(panel.headercontainer);
        scene->addComponent<LayoutGroupComponent>(panel.headercontainer);

        scene->addEntityChild(panel.headerimage, panel.headercontainer, false);

        LayoutGroupComponent& containerui = scene->getComponent<LayoutGroupComponent>(panel.headercontainer);
        RectTransformComponent& containerlayout = scene->getComponent<RectTransformComponent>(panel.headercontainer);

        containerlayout.ignoreEvents = true;
        containerlayout.anchorPreset = AnchorPreset::FULL_LAYOUT;
        containerlayout.ignoreScissor = true;
        containerlayout.usingAnchors = true;
        containerui.type = LayoutGroupType::HORIZONTAL;
    }
    if (panel.headertext == NULL_ENTITY){
        panel.headertext = scene->createEntity();

        scene->addComponent<Transform>(panel.headertext);
        scene->addComponent<RectTransformComponent>(panel.headertext);
        scene->addComponent<UIComponent>(panel.headertext);
        scene->addComponent<TextComponent>(panel.headertext);

        scene->addEntityChild(panel.headercontainer, panel.headertext, false);

        UIComponent& titleui = scene->getComponent<UIComponent>(panel.headertext);
        RectTransformComponent& titlelayout = scene->getComponent<RectTransformComponent>(panel.headertext);

        titleui.color = Vector4(0.0, 0.0, 0.0, 1.0);
        titlelayout.ignoreEvents = true;
        titlelayout.anchorPreset = panel.titleAnchorPreset;
        titlelayout.usingAnchors = true;
    }
}

void UISystem::createScrollbarObjects(Entity entity, ScrollbarComponent& scrollbar){
    if (scrollbar.bar != NULL_ENTITY && !scene->findComponent<ImageComponent>(scrollbar.bar)){
        scrollbar.bar = NULL_ENTITY;
    }

    if (scrollbar.bar == NULL_ENTITY){
        scrollbar.bar = scene->createEntity();

        scene->addComponent<Transform>(scrollbar.bar);
        scene->addComponent<RectTransformComponent>(scrollbar.bar);
        scene->addComponent<UIComponent>(scrollbar.bar);
        scene->addComponent<ImageComponent>(scrollbar.bar);

        scene->addEntityChild(entity, scrollbar.bar, false);

        RectTransformComponent& barlayout = scene->getComponent<RectTransformComponent>(scrollbar.bar);

        barlayout.ignoreEvents = true;
    }
}

void UISystem::createProgressbarObjects(Entity entity, ProgressbarComponent& progressbar){
    if (progressbar.fill != NULL_ENTITY && !scene->findComponent<ImageComponent>(progressbar.fill)){
        progressbar.fill = NULL_ENTITY;
    }

    if (progressbar.fill == NULL_ENTITY){
        progressbar.fill = scene->createEntity();

        scene->addComponent<Transform>(progressbar.fill);
        scene->addComponent<RectTransformComponent>(progressbar.fill);
        scene->addComponent<UIComponent>(progressbar.fill);
        scene->addComponent<ImageComponent>(progressbar.fill);

        scene->addEntityChild(entity, progressbar.fill, false);

        RectTransformComponent& filllayout = scene->getComponent<RectTransformComponent>(progressbar.fill);

        filllayout.ignoreEvents = true;
    }
}

void UISystem::createInputFieldObjects(Entity entity, InputFieldComponent& inputfield){
    if (inputfield.text != NULL_ENTITY && !scene->findComponent<TextComponent>(inputfield.text)){
        inputfield.text = NULL_ENTITY;
    }
    if (inputfield.selection != NULL_ENTITY && !scene->findComponent<PolygonComponent>(inputfield.selection)){
        inputfield.selection = NULL_ENTITY;
    }
    if (inputfield.cursor != NULL_ENTITY && !scene->findComponent<PolygonComponent>(inputfield.cursor)){
        inputfield.cursor = NULL_ENTITY;
    }

    if (inputfield.text == NULL_ENTITY){
        inputfield.text = scene->createEntity();

        scene->addComponent<Transform>(inputfield.text);
        scene->addComponent<RectTransformComponent>(inputfield.text);
        scene->addComponent<UIComponent>(inputfield.text);
        scene->addComponent<TextComponent>(inputfield.text);

        scene->addEntityChild(entity, inputfield.text, false);

        RectTransformComponent& textlayout = scene->getComponent<RectTransformComponent>(inputfield.text);
        UIComponent& textui = scene->getComponent<UIComponent>(inputfield.text);
        textui.color = Vector4(0.0, 0.0, 0.0, 1.0);
        textlayout.ignoreEvents = true;
    }

    if (inputfield.selection == NULL_ENTITY){
        inputfield.selection = scene->createEntity();

        scene->addComponent<Transform>(inputfield.selection);
        scene->addComponent<RectTransformComponent>(inputfield.selection);
        scene->addComponent<UIComponent>(inputfield.selection);
        scene->addComponent<PolygonComponent>(inputfield.selection);

        scene->addEntityChild(entity, inputfield.selection, false);

        RectTransformComponent& selectionlayout = scene->getComponent<RectTransformComponent>(inputfield.selection);
        selectionlayout.ignoreEvents = true;
    }

    if (inputfield.cursor == NULL_ENTITY){
        inputfield.cursor = scene->createEntity();

        scene->addComponent<Transform>(inputfield.cursor);
        scene->addComponent<RectTransformComponent>(inputfield.cursor);
        scene->addComponent<UIComponent>(inputfield.cursor);
        scene->addComponent<PolygonComponent>(inputfield.cursor);

        scene->addEntityChild(entity, inputfield.cursor, false);

        RectTransformComponent& cursorlayout = scene->getComponent<RectTransformComponent>(inputfield.cursor);

        cursorlayout.ignoreEvents = true;
    }
}

void UISystem::applyButtonVisual(ButtonComponent& button, UIComponent& ui){
    Texture targetTexture;
    Vector4 targetColor;

    // the own flag or a non-interactable Canvas Group (Unity Selectable.IsInteractable)
    if (button.disabled || !ui.groupInteractable){
        targetTexture = button.textureDisabled;
        targetColor = button.colorDisabled;
    }else if (button.pressed){
        targetTexture = button.texturePressed;
        targetColor = button.colorPressed;
    }else if (button.hovered){
        bool hasHoveredTexture = !button.textureHovered.empty() &&
            (!button.textureHovered.getId().empty() || button.textureHovered.isFramebuffer());
        targetTexture = hasHoveredTexture ? button.textureHovered : button.textureNormal;
        targetColor = button.colorHovered;
    }else{
        targetTexture = button.textureNormal;
        targetColor = button.colorNormal;
    }

    if (ui.texture != targetTexture){
        ui.texture = targetTexture;
        ui.needUpdateTexture = true;
    }
    ui.color = targetColor;
}

void UISystem::updateButton(Entity entity, ButtonComponent& button, ImageComponent& img, UIComponent& ui, RectTransformComponent& layout){
    if (!ui.loaded){
        // resolved here so that the copies swapped into ui.texture already match it
        button.textureNormal.setAlphaBorderAuto(true);
        button.textureHovered.setAlphaBorderAuto(true);
        button.texturePressed.setAlphaBorderAuto(true);
        button.textureDisabled.setAlphaBorderAuto(true);

        if (!button.textureNormal.load()){
            button.textureNormal = ui.texture;
        }
        Vector4 oldColorNormal = button.colorNormal;
        if (button.colorNormal == Vector4(1.0, 1.0, 1.0, 1.0)){
            button.colorNormal = ui.color;
        }
        if (button.colorHovered == oldColorNormal){
            button.colorHovered = button.colorNormal;
        }
        button.textureNormal.load();
        button.textureHovered.load();
        button.texturePressed.load();
        button.textureDisabled.load();
    }

    if (button.label == NULL_ENTITY || !scene->findComponent<TextComponent>(button.label)){
        applyButtonVisual(button, ui);
        return;
    }

    TextComponent& labeltext = scene->getComponent<TextComponent>(button.label);
    UIComponent& labelui = scene->getComponent<UIComponent>(button.label);
    RectTransformComponent& labellayout = scene->getComponent<RectTransformComponent>(button.label);

    labeltext.needUpdateText = true;
    createOrUpdateText(labeltext, labelui, labellayout);

    applyButtonVisual(button, ui);
}

void UISystem::updatePanel(Entity entity, PanelComponent& panel, ImageComponent& img, UIComponent& ui, RectTransformComponent& layout){
    if (panel.minWidth > layout.width){
        panel.minWidth = layout.width;
    }
    if (panel.minHeight > layout.height){
        panel.minHeight = layout.height;
    }
    if (panel.minWidth < (img.patchMarginLeft + img.patchMarginRight + 1)){
        panel.minWidth = static_cast<unsigned int>(img.patchMarginLeft + img.patchMarginRight + 1);
    }
    if (panel.minHeight < (img.patchMarginTop + img.patchMarginBottom + 1)){
        panel.minHeight = static_cast<unsigned int>(img.patchMarginTop + img.patchMarginBottom + 1);
    }

    if (panel.headerimage != NULL_ENTITY && scene->findComponent<RectTransformComponent>(panel.headerimage)){
        RectTransformComponent& headerimagelayout = scene->getComponent<RectTransformComponent>(panel.headerimage);

        if (panel.defaultHeaderMargin){
            panel.headerMarginLeft = img.patchMarginLeft;
            panel.headerMarginRight = img.patchMarginRight;
            panel.headerMarginTop = img.patchMarginBottom;
            panel.headerMarginBottom = img.patchMarginBottom;
        }

        headerimagelayout.anchorOffsetLeft = panel.headerMarginLeft;
        headerimagelayout.anchorOffsetTop = panel.headerMarginTop;
        headerimagelayout.anchorOffsetRight = -panel.headerMarginRight;
        headerimagelayout.anchorOffsetBottom = img.patchMarginTop - panel.headerMarginBottom;
    }

    if (panel.headertext != NULL_ENTITY && scene->findComponent<TextComponent>(panel.headertext)){
        RectTransformComponent& titlelayout = scene->getComponent<RectTransformComponent>(panel.headertext);
        UIComponent& titleui = scene->getComponent<UIComponent>(panel.headertext);
        TextComponent& headertext = scene->getComponent<TextComponent>(panel.headertext);

        titlelayout.anchorPreset = panel.titleAnchorPreset;
        headertext.needUpdateText = true;
        createOrUpdateText(headertext, titleui, titlelayout);
    }
}

void UISystem::updateScrollbar(ScrollbarComponent& scrollbar, RectTransformComponent& layout){
    float barSize = scrollbar.barSize;
    float step = scrollbar.step;

    if (barSize > 1){
        barSize = 1;
        step = 0;
    }else if (barSize < 0){
        barSize = 0;
    }

    if (step > 1){
        step = 1;
    }else if (step < 0){
        step = 0;
    }

    bool changed = (step != scrollbar.step);
    scrollbar.barSize = barSize;
    scrollbar.step = step;

    if (scrollbar.bar != NULL_ENTITY && scene->findComponent<RectTransformComponent>(scrollbar.bar)){
        RectTransformComponent& barlayout = scene->getComponent<RectTransformComponent>(scrollbar.bar);

        float innerHeight = std::max(0.0f, (float)layout.height - scrollbar.barMarginTop - scrollbar.barMarginBottom);
        float innerWidth = std::max(0.0f, (float)layout.width - scrollbar.barMarginLeft - scrollbar.barMarginRight);

        float barSizePixel = 0;
        float trackStartNorm = 0;
        float trackEndNorm = 1;
        float halfBarParent = 0;
        unsigned int oldBarWidth = barlayout.width;
        unsigned int oldBarHeight = barlayout.height;

        if (scrollbar.type == ScrollbarType::VERTICAL){
            barSizePixel = innerHeight * scrollbar.barSize;
            barlayout.width = innerWidth;
            barlayout.height = barSizePixel;
            trackStartNorm = layout.height > 0 ? scrollbar.barMarginTop / (float)layout.height : 0;
            trackEndNorm = layout.height > 0 ? 1.0f - scrollbar.barMarginBottom / (float)layout.height : 1;
            halfBarParent = layout.height > 0 ? (barSizePixel / 2.0f) / layout.height : 0;
        }else if (scrollbar.type == ScrollbarType::HORIZONTAL){
            barSizePixel = innerWidth * scrollbar.barSize;
            barlayout.width = barSizePixel;
            barlayout.height = innerHeight;
            trackStartNorm = layout.width > 0 ? scrollbar.barMarginLeft / (float)layout.width : 0;
            trackEndNorm = layout.width > 0 ? 1.0f - scrollbar.barMarginRight / (float)layout.width : 1;
            halfBarParent = layout.width > 0 ? (barSizePixel / 2.0f) / layout.width : 0;
        }

        if (barlayout.width != oldBarWidth || barlayout.height != oldBarHeight){
            barlayout.needUpdateSizes = true;
        }

        float movableStart = trackStartNorm + halfBarParent;
        float movableEnd = trackEndNorm - halfBarParent;
        float pos = movableStart;
        if (movableEnd > movableStart){
            pos = movableStart + scrollbar.step * (movableEnd - movableStart);
        }

        if (scrollbar.type == ScrollbarType::VERTICAL){
            barlayout.anchorPointLeft = 0;
            barlayout.anchorPointTop = pos;
            barlayout.anchorPointRight = 1;
            barlayout.anchorPointBottom = pos;
            barlayout.anchorOffsetLeft = scrollbar.barMarginLeft;
            barlayout.anchorOffsetTop = -floor(barlayout.height / 2.0);
            barlayout.anchorOffsetRight = -scrollbar.barMarginRight;
            barlayout.anchorOffsetBottom = ceil(barlayout.height / 2.0);
        }else if (scrollbar.type == ScrollbarType::HORIZONTAL){
            barlayout.anchorPointLeft = pos;
            barlayout.anchorPointTop = 0;
            barlayout.anchorPointRight = pos;
            barlayout.anchorPointBottom = 1;
            barlayout.anchorOffsetLeft = -floor(barlayout.width / 2.0);
            barlayout.anchorOffsetTop = scrollbar.barMarginTop;
            barlayout.anchorOffsetRight = ceil(barlayout.width / 2.0);
            barlayout.anchorOffsetBottom = -scrollbar.barMarginBottom;
        }
        barlayout.anchorPreset = AnchorPreset::NONE;
        barlayout.usingAnchors = true;
    }

    if (changed)
        scrollbar.onChange.call(step);
}

void UISystem::updateProgressbar(Entity entity, ProgressbarComponent& progressbar, ImageComponent& img, UIComponent& ui, RectTransformComponent& layout){
    if (progressbar.value > 1){
        progressbar.value = 1;
    }else if (progressbar.value < 0){
        progressbar.value = 0;
    }

    if (progressbar.fill == NULL_ENTITY || !scene->findComponent<RectTransformComponent>(progressbar.fill)){
        return;
    }

    RectTransformComponent& filllayout = scene->getComponent<RectTransformComponent>(progressbar.fill);

    if (progressbar.type == ProgressbarType::HORIZONTAL){
        filllayout.anchorPointLeft = 0;
        filllayout.anchorPointTop = 0;
        filllayout.anchorPointRight = progressbar.value;
        filllayout.anchorPointBottom = 1;
        filllayout.anchorOffsetLeft = progressbar.fillMarginLeft;
        filllayout.anchorOffsetTop = progressbar.fillMarginTop;
        filllayout.anchorOffsetRight = progressbar.value >= 1.0f ? -progressbar.fillMarginRight : 0;
        filllayout.anchorOffsetBottom = -progressbar.fillMarginBottom;
    }else if (progressbar.type == ProgressbarType::VERTICAL){
        filllayout.anchorPointLeft = 0;
        filllayout.anchorPointTop = 1 - progressbar.value;
        filllayout.anchorPointRight = 1;
        filllayout.anchorPointBottom = 1;
        filllayout.anchorOffsetLeft = progressbar.fillMarginLeft;
        filllayout.anchorOffsetTop = progressbar.value >= 1.0f ? progressbar.fillMarginTop : 0;
        filllayout.anchorOffsetRight = -progressbar.fillMarginRight;
        filllayout.anchorOffsetBottom = -progressbar.fillMarginBottom;
    }
    filllayout.anchorPreset = AnchorPreset::NONE;
    filllayout.usingAnchors = true;
}

bool UISystem::inputFieldHasSelection(const InputFieldComponent& inputfield) const{
    return inputfield.selectionAnchor != inputfield.cursorIndex;
}

int UISystem::inputFieldSelectionStart(const InputFieldComponent& inputfield) const{
    return std::min(inputfield.selectionAnchor, inputfield.cursorIndex);
}

int UISystem::inputFieldSelectionEnd(const InputFieldComponent& inputfield) const{
    return std::max(inputfield.selectionAnchor, inputfield.cursorIndex);
}

void UISystem::inputFieldClampCursor(InputFieldComponent& inputfield, size_t numCodepoints) const{
    int maxIndex = static_cast<int>(numCodepoints);
    inputfield.cursorIndex = std::clamp(inputfield.cursorIndex, 0, maxIndex);
    if (!inputFieldHasSelection(inputfield)){
        inputfield.selectionAnchor = inputfield.cursorIndex;
    }else{
        inputfield.selectionAnchor = std::clamp(inputfield.selectionAnchor, 0, maxIndex);
    }
}

int UISystem::inputFieldVisualStep(const TextComponent& text, int cursorIndex, int direction) const{
    size_t numChars = text.charExtents.size();
    if (numChars == 0){
        return cursorIndex + direction;
    }

    //arrows follow the screen, like the click that places the caret. Where the
    //direction changes two stops land on the same pixel and both must stay reachable,
    //so the logical index breaks the tie and keeps the order total
    std::vector<std::pair<float, int>> stops;
    stops.reserve(numChars + 1);

    for (int i = 0; i <= (int)numChars; i++){
        stops.push_back({inputFieldCursorPixelX(text, i), i});
    }

    std::sort(stops.begin(), stops.end());

    for (size_t i = 0; i < stops.size(); i++){
        if (stops[i].second != cursorIndex)
            continue;

        if (direction < 0){
            return (i > 0) ? stops[i - 1].second : cursorIndex;
        }
        return (i + 1 < stops.size()) ? stops[i + 1].second : cursorIndex;
    }

    return cursorIndex + direction;
}

float UISystem::inputFieldCursorPixelX(const TextComponent& text, int cursorIndex) const{
    size_t numChars = text.charExtents.size();
    if (numChars == 0){
        return 0;
    }

    //the caret sits after the previous codepoint, which in an RTL run is its left side
    if (cursorIndex > 0){
        const CharExtent& previous = text.charExtents[std::min((size_t)cursorIndex, numChars) - 1];
        return previous.rtl ? previous.left : previous.right;
    }

    const CharExtent& first = text.charExtents[0];
    return first.rtl ? first.right : first.left;
}

int UISystem::inputFieldFindCursorIndexFromX(const TextComponent& text, float x) const{
    size_t numChars = text.charExtents.size();
    if (numChars == 0){
        return 0;
    }

    //visual order does not follow logical order, the codepoint under the pointer has
    //to be searched for instead of accumulated
    size_t nearest = 0;
    float nearestDistance = std::numeric_limits<float>::max();

    for (size_t i = 0; i < numChars; i++){
        const CharExtent& extent = text.charExtents[i];

        float distance = 0.0f;
        if (x < extent.left){
            distance = extent.left - x;
        }else if (x > extent.right){
            distance = x - extent.right;
        }

        if (distance < nearestDistance){
            nearestDistance = distance;
            nearest = i;
            if (distance == 0.0f)
                break;
        }
    }

    const CharExtent& extent = text.charExtents[nearest];
    float middle = (extent.left + extent.right) * 0.5f;
    bool afterChar = extent.rtl ? (x < middle) : (x > middle);

    return static_cast<int>(afterChar ? nearest + 1 : nearest);
}

void UISystem::inputFieldSelectionRects(const TextComponent& text, int start, int end, std::vector<Rect>& rects) const{
    rects.clear();

    size_t numChars = text.charExtents.size();
    if (start >= end || numChars == 0){
        return;
    }

    size_t from = std::min((size_t)std::max(start, 0), numChars);
    size_t to = std::min((size_t)std::max(end, 0), numChars);

    //contiguous in logical order can still be several pieces on screen, so the spans
    //are merged by position
    std::vector<Rect> spans;
    for (size_t i = from; i < to; i++){
        const CharExtent& extent = text.charExtents[i];
        if (extent.right <= extent.left)
            continue;

        spans.push_back(Rect(extent.left, extent.y, extent.right - extent.left, 0));
    }

    if (spans.empty()){
        return;
    }

    std::sort(spans.begin(), spans.end(), [](const Rect& a, const Rect& b){
        return a.getX() < b.getX();
    });

    Rect current = spans[0];
    for (size_t i = 1; i < spans.size(); i++){
        float currentRight = current.getX() + current.getWidth();
        if (spans[i].getX() <= currentRight + 0.5f){
            float right = std::max(currentRight, spans[i].getX() + spans[i].getWidth());
            current.setWidth(right - current.getX());
        }else{
            rects.push_back(current);
            current = spans[i];
        }
    }
    rects.push_back(current);
}

std::string UISystem::inputFieldMaskText(const std::string& text, char maskChar) const{
    return std::string(StringUtils::countCodepoints(text), maskChar);
}

void UISystem::inputFieldDeleteSelection(InputFieldComponent& inputfield, TextComponent& text){
    if (!inputFieldHasSelection(inputfield)){
        return;
    }

    StringUtils::eraseCodepointsUtf8(text.text,
        static_cast<size_t>(inputFieldSelectionStart(inputfield)),
        static_cast<size_t>(inputFieldSelectionEnd(inputfield)));
    inputfield.cursorIndex = inputFieldSelectionStart(inputfield);
    inputfield.selectionAnchor = inputfield.cursorIndex;
}

void UISystem::inputFieldResetBlink(InputFieldComponent& inputfield) const{
    inputfield.cursorBlinkTimer = 0;
    if (Transform* cursortransform = scene->findComponent<Transform>(inputfield.cursor)){
        cursortransform->visible = true;
    }
}

InputFieldComponent* UISystem::findInputFieldForTextChild(Entity textEntity) const{
    Transform* transform = scene->findComponent<Transform>(textEntity);
    if (!transform || transform->parent == NULL_ENTITY){
        return nullptr;
    }

    InputFieldComponent* inputfield = scene->findComponent<InputFieldComponent>(transform->parent);
    if (!inputfield || inputfield->text != textEntity){
        return nullptr;
    }

    return inputfield;
}

void UISystem::updateInputField(Entity entity, InputFieldComponent& inputfield, ImageComponent& img, UIComponent& ui, RectTransformComponent& layout){
    if (inputfield.text == NULL_ENTITY || !scene->findComponent<TextComponent>(inputfield.text)){
        return;
    }

    Transform& texttransform = scene->getComponent<Transform>(inputfield.text);
    RectTransformComponent& textlayout = scene->getComponent<RectTransformComponent>(inputfield.text);
    UIComponent& textui = scene->getComponent<UIComponent>(inputfield.text);
    TextComponent& text = scene->getComponent<TextComponent>(inputfield.text);

    inputFieldClampCursor(inputfield, StringUtils::countCodepoints(text.text));

    std::string storedText = text.text;
    Vector4 storedColor = textui.color;
    bool showingPlaceholder = storedText.empty() && !inputfield.placeholder.empty() && !ui.focused;
    bool showingPassword = inputfield.password && !storedText.empty();

    if (showingPlaceholder){
        text.text = inputfield.placeholder;
        textui.color = inputfield.placeholderColor;
    }else if (showingPassword){
        text.text = inputFieldMaskText(storedText, inputfield.passwordChar);
    }

    text.multiline = false;
    text.needUpdateText = true;
    createOrUpdateText(text, textui, textlayout);

    text.text = storedText;
    textui.color = storedColor;

    if (layout.height == 0){
        layout.height = textlayout.height + img.patchMarginTop + img.patchMarginBottom;
        layout.needUpdateSizes = true;
    }

    int heightArea = layout.height - img.patchMarginTop - img.patchMarginBottom;
    int widthArea = layout.width - img.patchMarginRight - img.patchMarginLeft - inputfield.cursorWidth;

    float cursorPixelX = inputFieldCursorPixelX(text, inputfield.cursorIndex);
    if (cursorPixelX - inputfield.scrollOffset > widthArea){
        inputfield.scrollOffset = cursorPixelX - widthArea;
    }
    if (cursorPixelX - inputfield.scrollOffset < 0){
        inputfield.scrollOffset = cursorPixelX;
    }
    if (textlayout.width - inputfield.scrollOffset < widthArea && textlayout.width > widthArea){
        inputfield.scrollOffset = textlayout.width - widthArea;
    }
    if (inputfield.scrollOffset < 0){
        inputfield.scrollOffset = 0;
    }

    float textX = static_cast<float>(img.patchMarginLeft) - inputfield.scrollOffset;
    float textY = static_cast<float>(img.patchMarginTop) + (heightArea / 2.0f) - (textlayout.height / 2.0f);

    Vector3 textPosition = Vector3(textX, textY, 0);
    if (texttransform.position != textPosition){
        texttransform.position = textPosition;
        texttransform.needUpdate = true;
    }

    if (inputfield.selection != NULL_ENTITY && scene->findComponent<PolygonComponent>(inputfield.selection)){
        Transform& selectiontransform = scene->getComponent<Transform>(inputfield.selection);
        RectTransformComponent& selectionlayout = scene->getComponent<RectTransformComponent>(inputfield.selection);
        UIComponent& selectionui = scene->getComponent<UIComponent>(inputfield.selection);
        PolygonComponent& selection = scene->getComponent<PolygonComponent>(inputfield.selection);

        bool hasSelection = ui.focused && !inputfield.disabled && inputFieldHasSelection(inputfield) && !showingPlaceholder;

        std::vector<Rect> selectionRects;
        if (hasSelection){
            inputFieldSelectionRects(text, inputFieldSelectionStart(inputfield), inputFieldSelectionEnd(inputfield), selectionRects);
            hasSelection = !selectionRects.empty();
        }

        selectiontransform.visible = hasSelection;

        if (hasSelection){
            createOrUpdatePolygon(selection, selectionui, selectionlayout);

            float selectionHeight = static_cast<float>(textlayout.height);
            Vector4 white = Vector4(1.0, 1.0, 1.0, 1.0);

            selection.points.clear();
            for (const Rect& rect : selectionRects){
                float left = rect.getX();
                float right = left + rect.getWidth();

                //a repeated vertex on each side of a gap keeps the bridging triangles
                //degenerate, so one strip can hold several rectangles
                if (!selection.points.empty()){
                    selection.points.push_back(selection.points.back());
                    selection.points.push_back({Vector3(left, 0, 0), white});
                }

                selection.points.push_back({Vector3(left, 0, 0), white});
                selection.points.push_back({Vector3(right, 0, 0), white});
                selection.points.push_back({Vector3(left, selectionHeight, 0), white});
                selection.points.push_back({Vector3(right, selectionHeight, 0), white});
            }

            selectionui.color = inputfield.selectionColor;
            selectiontransform.position = Vector3(textX, textY, 0.0);
            selectiontransform.needUpdate = true;
            selection.needUpdatePolygon = true;
        }
    }

    if (inputfield.cursor != NULL_ENTITY && scene->findComponent<PolygonComponent>(inputfield.cursor)){
        Transform& cursortransform = scene->getComponent<Transform>(inputfield.cursor);
        RectTransformComponent& cursorlayout = scene->getComponent<RectTransformComponent>(inputfield.cursor);
        UIComponent& cursorui = scene->getComponent<UIComponent>(inputfield.cursor);
        PolygonComponent& cursor = scene->getComponent<PolygonComponent>(inputfield.cursor);

        bool showCursor = ui.focused && !inputfield.disabled && !showingPlaceholder;
        if (!showCursor){
            cursortransform.visible = false;
        }

        if (showCursor){
            createOrUpdatePolygon(cursor, cursorui, cursorlayout);

            float cursorHeight = static_cast<float>(textlayout.height);

            cursor.points.clear();
            cursor.points.push_back({Vector3(0, 0, 0), Vector4(1.0, 1.0, 1.0, 1.0)});
            cursor.points.push_back({Vector3(inputfield.cursorWidth, 0, 0), Vector4(1.0, 1.0, 1.0, 1.0)});
            cursor.points.push_back({Vector3(0, cursorHeight, 0), Vector4(1.0, 1.0, 1.0, 1.0)});
            cursor.points.push_back({Vector3(inputfield.cursorWidth, cursorHeight, 0), Vector4(1.0, 1.0, 1.0, 1.0)});

            float cursorX = textX + cursorPixelX;
            float cursorY = static_cast<float>(img.patchMarginTop) + (heightArea / 2.0f) - (cursorHeight / 2.0f);

            cursorui.color = inputfield.cursorColor;
            cursortransform.position = Vector3(cursorX, cursorY, 0.0);
            cursortransform.needUpdate = true;
            cursor.needUpdatePolygon = true;
        }
    }
}

void UISystem::blinkCursorInputField(double dt, InputFieldComponent& inputfield, UIComponent& ui){
    if (inputfield.cursor == NULL_ENTITY || !scene->findComponent<Transform>(inputfield.cursor)){
        return;
    }

    Transform& cursortransform = scene->getComponent<Transform>(inputfield.cursor);

    if (!ui.focused || inputfield.disabled){
        cursortransform.visible = false;
        inputfield.cursorBlinkTimer = 0;
        return;
    }

    float period = inputfield.cursorBlink;
    if (period <= 0.0f){
        cursortransform.visible = true;
        inputfield.cursorBlinkTimer = 0;
        return;
    }

    inputfield.cursorBlinkTimer += static_cast<float>(dt);
    float cycle = period * 2.0f;
    if (inputfield.cursorBlinkTimer >= cycle){
        inputfield.cursorBlinkTimer = std::fmod(inputfield.cursorBlinkTimer, cycle);
    }

    cursortransform.visible = inputfield.cursorBlinkTimer < period;
}

void UISystem::setInputFieldCursorFromLocalX(InputFieldComponent& inputfield, TextComponent& text, ImageComponent& img, float localX, bool extendSelection){
    float clickTextX = localX - img.patchMarginLeft + inputfield.scrollOffset;
    inputfield.cursorIndex = inputFieldFindCursorIndexFromX(text, clickTextX);
    if (!extendSelection){
        inputfield.selectionAnchor = inputfield.cursorIndex;
    }
    inputFieldResetBlink(inputfield);
    inputfield.needUpdateInputField = true;
}

bool UISystem::handleInputFieldCharInput(InputFieldComponent& inputfield, TextComponent& text, UIComponent& ui, char32_t codepoint){
    if (inputfield.disabled){
        return false;
    }

    if (codepoint == '\x1b'){
        ui.focused = false;
        ui.onLostFocus.call();
        inputfield.needUpdateInputField = true;
        return true;
    }

    if (codepoint == '\r' || codepoint == '\n'){
        inputfield.onSubmit.call();
        return true;
    }

    if (codepoint == '\b'){
        if (inputFieldHasSelection(inputfield)){
            inputFieldDeleteSelection(inputfield, text);
        }else if (inputfield.cursorIndex > 0){
            StringUtils::eraseCodepointsUtf8(text.text,
                static_cast<size_t>(inputfield.cursorIndex - 1),
                static_cast<size_t>(inputfield.cursorIndex));
            inputfield.cursorIndex--;
            inputfield.selectionAnchor = inputfield.cursorIndex;
        }
    }else if (codepoint >= 32 || codepoint == '\t'){
        std::string insertText = StringUtils::toUTF8(codepoint);
        size_t insertCount = StringUtils::countCodepoints(insertText);
        size_t currentCount = StringUtils::countCodepoints(text.text);
        size_t selectionSize = inputFieldHasSelection(inputfield)
            ? static_cast<size_t>(inputFieldSelectionEnd(inputfield) - inputFieldSelectionStart(inputfield))
            : 0;

        if (text.maxTextSize > 0 && currentCount - selectionSize + insertCount > text.maxTextSize){
            return true;
        }

        if (inputFieldHasSelection(inputfield)){
            inputFieldDeleteSelection(inputfield, text);
        }

        StringUtils::insertUtf8AtCodepoint(text.text, static_cast<size_t>(inputfield.cursorIndex), insertText);
        inputfield.cursorIndex += static_cast<int>(insertCount);
        inputfield.selectionAnchor = inputfield.cursorIndex;
    }else{
        return false;
    }

    text.needUpdateText = true;
    inputFieldResetBlink(inputfield);
    inputfield.needUpdateInputField = true;
    inputfield.onChange.call();
    return true;
}

bool UISystem::handleInputFieldKeyDown(InputFieldComponent& inputfield, TextComponent& text, UIComponent& ui, int key, bool repeat, int mods){
    if (inputfield.disabled){
        return false;
    }

    bool shift = (mods & D_MODIFIER_SHIFT) != 0;
    bool control = (mods & D_MODIFIER_CONTROL) != 0;
    size_t numCodepoints = StringUtils::countCodepoints(text.text);
    bool changed = false;

    if (control && key == D_KEY_A){
        inputfield.selectionAnchor = 0;
        inputfield.cursorIndex = static_cast<int>(numCodepoints);
        inputFieldResetBlink(inputfield);
        inputfield.needUpdateInputField = true;
        return true;
    }

    auto moveCursor = [&](int newIndex){
        inputfield.cursorIndex = std::clamp(newIndex, 0, static_cast<int>(numCodepoints));
        if (!shift){
            inputfield.selectionAnchor = inputfield.cursorIndex;
        }
        inputFieldResetBlink(inputfield);
        inputfield.needUpdateInputField = true;
        changed = true;
    };

    if (key == D_KEY_LEFT){
        moveCursor(inputFieldVisualStep(text, inputfield.cursorIndex, -1));
    }else if (key == D_KEY_RIGHT){
        moveCursor(inputFieldVisualStep(text, inputfield.cursorIndex, 1));
    }else if (key == D_KEY_HOME){
        moveCursor(0);
    }else if (key == D_KEY_END){
        moveCursor(static_cast<int>(numCodepoints));
    }else if (key == D_KEY_DELETE){
        if (inputFieldHasSelection(inputfield)){
            inputFieldDeleteSelection(inputfield, text);
            changed = true;
        }else if (inputfield.cursorIndex < static_cast<int>(numCodepoints)){
            StringUtils::eraseCodepointsUtf8(text.text,
                static_cast<size_t>(inputfield.cursorIndex),
                static_cast<size_t>(inputfield.cursorIndex + 1));
            changed = true;
        }
    }else{
        return false;
    }

    if (changed){
        text.needUpdateText = true;
        inputfield.onChange.call();
        return true;
    }

    return changed;
}

void UISystem::createUIPolygon(PolygonComponent& polygon, UIComponent& ui, RectTransformComponent& layout){

    ui.primitiveType = PrimitiveType::TRIANGLE_STRIP;

    ui.buffer.clear();
    ui.buffer.addAttribute(AttributeType::POSITION, 3);
    ui.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
    ui.buffer.addAttribute(AttributeType::COLOR, 4);
    ui.buffer.setUsage(BufferUsage::DYNAMIC);

    ui.indices.clear();

    for (int i = 0; i < polygon.points.size(); i++){
        ui.buffer.addVector3(AttributeType::POSITION, polygon.points[i].position);
        ui.buffer.addVector3(AttributeType::NORMAL, Vector3(0.0f, 0.0f, 1.0f));
        ui.buffer.addVector4(AttributeType::COLOR, polygon.points[i].color);
    }

    // Generation texcoords (lowest(), not min(): min() is the smallest positive float, so a
    // polygon left of or below its origin measured to 0 instead of to its own edge)
    float min_X = std::numeric_limits<float>::max();
    float max_X = std::numeric_limits<float>::lowest();
    float min_Y = std::numeric_limits<float>::max();
    float max_Y = std::numeric_limits<float>::lowest();

    Attribute* attVertex = ui.buffer.getAttribute(AttributeType::POSITION);

    for (unsigned int i = 0; i < ui.buffer.getCount(); i++){
        min_X = fmin(min_X, ui.buffer.getFloat(attVertex, i, 0));
        min_Y = fmin(min_Y, ui.buffer.getFloat(attVertex, i, 1));
        max_X = fmax(max_X, ui.buffer.getFloat(attVertex, i, 0));
        max_Y = fmax(max_Y, ui.buffer.getFloat(attVertex, i, 1));
    }

    double k_X = 1/(max_X - min_X);
    double k_Y = 1/(max_Y - min_Y);

    float u = 0;
    float v = 0;

    for ( unsigned int i = 0; i < ui.buffer.getCount(); i++){
        u = (ui.buffer.getFloat(attVertex, i, 0) - min_X) * k_X;
        v = (ui.buffer.getFloat(attVertex, i, 1) - min_Y) * k_Y;

        if (ui.flipY)
            v = 1.0 - v;
        ui.buffer.addVector2(AttributeType::TEXCOORD1, Vector2(u, v));
    }

    layout.width = static_cast<unsigned int>(max_X - min_X);
    layout.height = static_cast<unsigned int>(max_Y - min_Y);

    calculateUIAABB(ui);

    if (ui.loaded)
        ui.needUpdateBuffer = true;
}

bool UISystem::createOrUpdatePolygon(PolygonComponent& polygon, UIComponent& ui, RectTransformComponent& layout){
    if (polygon.needUpdatePolygon){
        if (ui.automaticFlipY){
            CameraComponent& camera = scene->getComponent<CameraComponent>(scene->getCamera());
            changeFlipY(ui, camera);
        }

        createUIPolygon(polygon, ui, layout);

        polygon.needUpdatePolygon = false;
    }

    return true;
}

bool UISystem::createOrUpdateImage(ImageComponent& img, UIComponent& ui, RectTransformComponent& layout){
    if (img.needUpdatePatches){
        if (ui.automaticFlipY){
            CameraComponent& camera = scene->getComponent<CameraComponent>(scene->getCamera());
            changeFlipY(ui, camera);
        }

        if (createImagePatches(img, ui, layout)){
            img.needUpdatePatches = false;
        }else{
            return false;
        }
    }

    return true;
}

Vector2 UISystem::getTextMinSize(TextComponent& text){
    int minHeight = 0;
    float minWidth = 0;

    if (text.stbtext){
        minHeight = text.stbtext->getLineHeight();
        minWidth = text.stbtext->getCharWidth('W');
    }

    return Vector2(minWidth, minHeight);
}

void UISystem::checkTextAtlasOptions(TextComponent& text, UIComponent& ui){
    if (text.loaded && !text.needReloadAtlas){
        const STBTextOptions options = getTextOptions(text);
        if (options.rasterScale != text.rasterScale || options.sdfPadding != text.sdfPadding){
            text.needReloadAtlas = true;
            // another shader variant (SDF on or off)
            if ((options.sdfPadding > 0) != (text.sdfPadding > 0) && ui.loaded)
                ui.needReload = true;
        }
    }
}

bool UISystem::createOrUpdateText(TextComponent& text, UIComponent& ui, RectTransformComponent& layout){
    // the canvas scale or the outline/shadow changed the atlas it needs
    checkTextAtlasOptions(text, ui);

    if (text.loaded && text.needReloadAtlas){
        destroyText(text);
    }

    if (isFontAtlasStale(text)){
        text.needUpdateText = true;
    }

    // colours, offsets and softness reach the shader without a rebuild
    if (!text.needUpdateText && text.loaded && ui.textSdf){
        updateTextShaderParams(text, ui);
    }

    if (text.needUpdateText){
        if (ui.automaticFlipY){
            CameraComponent& camera = scene->getComponent<CameraComponent>(scene->getCamera());
            changeFlipY(ui, camera);
        }

        if (!text.loaded){
            loadFontAtlas(text, ui, layout);
        }
        createText(text, ui, layout);

        text.needUpdateText = false;
    }

    return true;
}

// --- Layout / anchor ---

Entity UISystem::findNearestLayoutParent(Entity startParent) const{
    Entity ancestor = startParent;
    while (ancestor != NULL_ENTITY) {
        if (scene->findComponent<RectTransformComponent>(ancestor)) {
            return ancestor;
        }
        Transform* ancestorTransform = scene->findComponent<Transform>(ancestor);
        ancestor = ancestorTransform ? ancestorTransform->parent : NULL_ENTITY;
    }
    return NULL_ENTITY;
}

Rect UISystem::transformRectToWorldAxisAligned(const Rect& rect, const Matrix4& matrix){
    const Vector3 corners[4] = {
        matrix * Vector3(rect.getX(), rect.getY(), 0.0f),
        matrix * Vector3(rect.getX() + rect.getWidth(), rect.getY(), 0.0f),
        matrix * Vector3(rect.getX(), rect.getY() + rect.getHeight(), 0.0f),
        matrix * Vector3(rect.getX() + rect.getWidth(), rect.getY() + rect.getHeight(), 0.0f),
    };

    float minX = corners[0].x;
    float maxX = corners[0].x;
    float minY = corners[0].y;
    float maxY = corners[0].y;

    for (int i = 1; i < 4; ++i) {
        minX = std::min(minX, corners[i].x);
        maxX = std::max(maxX, corners[i].x);
        minY = std::min(minY, corners[i].y);
        maxY = std::max(maxY, corners[i].y);
    }

    return Rect(minX, minY, maxX - minX, maxY - minY);
}

Vector3 UISystem::mapLayoutPointToParentLocalSpace(float posX, float posY, Entity layoutParent, Entity targetParent) const{
    if (layoutParent == NULL_ENTITY || targetParent == NULL_ENTITY || layoutParent == targetParent) {
        return Vector3(posX, posY, 0.0f);
    }

    const Transform& layoutTransform = scene->getComponent<Transform>(layoutParent);
    const Transform& targetTransform = scene->getComponent<Transform>(targetParent);

    Vector3 worldPoint = layoutTransform.modelMatrix * Vector3(posX, posY, 0.0f);
    Matrix4 targetToLayout = targetTransform.modelMatrix.inverse();
    if (!targetToLayout.isValid()) {
        return Vector3(posX, posY, 0.0f);
    }

    return targetToLayout * worldPoint;
}

Vector3 UISystem::mapParentLocalPointToLayoutSpace(float posX, float posY, Entity layoutParent, Entity sourceParent) const{
    if (layoutParent == NULL_ENTITY || sourceParent == NULL_ENTITY || layoutParent == sourceParent) {
        return Vector3(posX, posY, 0.0f);
    }

    const Transform& layoutTransform = scene->getComponent<Transform>(layoutParent);
    const Transform& sourceTransform = scene->getComponent<Transform>(sourceParent);

    Vector3 worldPoint = sourceTransform.modelMatrix * Vector3(posX, posY, 0.0f);
    Matrix4 layoutToWorld = layoutTransform.modelMatrix.inverse();
    if (!layoutToWorld.isValid()) {
        return Vector3(posX, posY, 0.0f);
    }

    return layoutToWorld * worldPoint;
}

Vector2 UISystem::parentToLayoutSpace(Entity entity, const Vector2& point) const{
    float x = point.x;
    float y = point.y;
    Transform* transform = scene->findComponent<Transform>(entity);
    if (transform && transform->parent != NULL_ENTITY){
        convertLocalParentSpaceToLayoutSpace(transform->parent, findNearestLayoutParent(transform->parent), x, y);
    }
    return Vector2(x, y);
}

Vector2 UISystem::layoutToParentSpace(Entity entity, const Vector2& point) const{
    float x = point.x;
    float y = point.y;
    Transform* transform = scene->findComponent<Transform>(entity);
    if (transform && transform->parent != NULL_ENTITY){
        convertLayoutSpaceToLocalParentSpace(transform->parent, findNearestLayoutParent(transform->parent), x, y);
    }
    return Vector2(x, y);
}

void UISystem::convertLayoutSpaceToLocalParentSpace(Entity transformParent, Entity layoutParent, float& posX, float& posY) const{
    Vector3 localPoint = mapLayoutPointToParentLocalSpace(posX, posY, layoutParent, transformParent);
    posX = localPoint.x;
    posY = localPoint.y;
}

void UISystem::convertLocalParentSpaceToLayoutSpace(Entity transformParent, Entity layoutParent, float& posX, float& posY) const{
    Vector3 layoutPoint = mapParentLocalPointToLayoutSpace(posX, posY, layoutParent, transformParent);
    posX = layoutPoint.x;
    posY = layoutPoint.y;
}

Rect UISystem::getAnchorReferenceRect(const RectTransformComponent& layout, const Transform& transform, bool worldPosition){
    auto canvasRect = [this]() {
        const Vector2 size = getCanvasSize();
        return Rect(0, 0, size.x, size.y);
    };

    if (transform.parent == NULL_ENTITY){
        return canvasRect();
    }

    // Prefab roots and other non-UI parents have no RectTransform; walk up to the nearest layout parent.
    Entity layoutParent = findNearestLayoutParent(transform.parent);

    if (layoutParent == NULL_ENTITY){
        Rect boxRect = canvasRect();
        if (worldPosition && transform.parent != NULL_ENTITY) {
            const Transform* parentTransform = scene->findComponent<Transform>(transform.parent);
            if (parentTransform){
                boxRect = transformRectToWorldAxisAligned(boxRect, parentTransform->modelMatrix);
            }
        }
        return boxRect;
    }

    RectTransformComponent* parentlayout = scene->findComponent<RectTransformComponent>(layoutParent);
    Rect boxRect = Rect(0, 0, parentlayout->width, parentlayout->height);

    LayoutGroupComponent* parentcontainer = scene->findComponent<LayoutGroupComponent>(layoutParent);
    if (parentcontainer){
        boxRect = parentcontainer->boxes[layout.containerBoxIndex].rect;
    }

    ImageComponent* parentimage = scene->findComponent<ImageComponent>(layoutParent);
    if (parentimage && !layout.ignoreScissor){
        boxRect.setX(boxRect.getX() + parentimage->patchMarginLeft);
        boxRect.setWidth(boxRect.getWidth() - parentimage->patchMarginRight - parentimage->patchMarginLeft);
        boxRect.setY(boxRect.getY() + parentimage->patchMarginTop);
        boxRect.setHeight(boxRect.getHeight() - parentimage->patchMarginBottom - parentimage->patchMarginTop);
    }

    if (worldPosition){
        const Transform& layoutTransform = scene->getComponent<Transform>(layoutParent);
        boxRect = transformRectToWorldAxisAligned(boxRect, layoutTransform.modelMatrix);
    }

    return boxRect;
}

void UISystem::applySafeArea(SafeAreaComponent& safeArea, RectTransformComponent& layout, const Transform& transform){
    // the anchor reference in world axes (the canvas for a root element); a rotated or scaled
    // parent gets the safe area axis-aligned
    const Rect reference = getAnchorReferenceRect(layout, transform, true);
    if (reference.getWidth() <= 0.0f || reference.getHeight() <= 0.0f)
        return;

    // Screen's canvas to this scene's UI units: the same fractions of the root canvas (the
    // Canvas Scaler's UI size when it scales; the editor lays edit-mode UI out for the project canvas)
    const Vector2 rootCanvas = getCanvasSize();
    const float canvasWidth = rootCanvas.x;
    const float canvasHeight = rootCanvas.y;
    const float screenWidth = Screen::getSafeAreaCanvasWidth();
    const float screenHeight = Screen::getSafeAreaCanvasHeight();
    const float sx = screenWidth > 0.0f ? canvasWidth / screenWidth : 1.0f;
    const float sy = screenHeight > 0.0f ? canvasHeight / screenHeight : 1.0f;
    const Rect safe = safeArea.includeExtraInsets ? Screen::getSafeArea() : Screen::getDeviceSafeArea();
    const float safeLeft = safe.getX() * sx;
    const float safeRight = (safe.getX() + safe.getWidth()) * sx;
    // Screen's rectangle is top-left; a 2D (y-up) camera puts layout's "top" at the screen's
    // bottom: there the screen's bottom edge is the smaller layout y
    const Entity cameraEntity = scene->getCamera();
    const CameraComponent* camera = cameraEntity != NULL_ENTITY ? scene->findComponent<CameraComponent>(cameraEntity) : nullptr;
    const bool yUp = camera && camera->type != CameraType::CAMERA_UI;
    const float screenTop = safe.getY() * sy;
    const float screenBottom = (safe.getY() + safe.getHeight()) * sy;
    const float safeTop = yUp ? canvasHeight - screenBottom : screenTop;
    const float safeBottom = yUp ? canvasHeight - screenTop : screenBottom;
    const bool followTop = yUp ? safeArea.bottom : safeArea.top;
    const bool followBottom = yUp ? safeArea.top : safeArea.bottom;

    auto fraction = [](float value, float origin, float size){
        return std::clamp((value - origin) / size, 0.0f, 1.0f);
    };
    const float anchorLeft = safeArea.left ? fraction(safeLeft, reference.getX(), reference.getWidth()) : 0.0f;
    const float anchorTop = followTop ? fraction(safeTop, reference.getY(), reference.getHeight()) : 0.0f;
    const float anchorRight = safeArea.right ? fraction(safeRight, reference.getX(), reference.getWidth()) : 1.0f;
    const float anchorBottom = followBottom ? fraction(safeBottom, reference.getY(), reference.getHeight()) : 1.0f;

    layout.usingAnchors = true;
    layout.anchorPreset = AnchorPreset::NONE;
    layout.anchorPointLeft = anchorLeft;
    layout.anchorPointTop = anchorTop;
    layout.anchorPointRight = std::max(anchorRight, anchorLeft);
    layout.anchorPointBottom = std::max(anchorBottom, anchorTop);
    layout.anchorOffsetLeft = 0;
    layout.anchorOffsetTop = 0;
    layout.anchorOffsetRight = 0;
    layout.anchorOffsetBottom = 0;
    layout.positionOffset = Vector2(0.0f, 0.0f);
    layout.needUpdateAnchorOffsets = false;
    safeArea.appliedRevision = Screen::getSafeAreaRevision();
}

void UISystem::setAnchorReferenceSize(float width, float height){
    this->anchorReferenceWidth = width;
    this->anchorReferenceHeight = height;
}

void UISystem::clearAnchorReferenceSize(){
    this->anchorReferenceWidth = 0;
    this->anchorReferenceHeight = 0;
}

float UISystem::getAnchorReferenceWidth() const{
    return anchorReferenceWidth;
}

float UISystem::getAnchorReferenceHeight() const{
    return anchorReferenceHeight;
}

bool UISystem::reducedMotion = false;

void UISystem::setTextEffectsHeld(bool held){
    if (textEffectsHeld == held)
        return;
    textEffectsHeld = held;

    // Play and Stop start each effect again as a loaded scene starts it (a preview ends)
    auto effects = scene->getComponentArray<TextEffectComponent>();
    for (size_t i = 0; i < effects->size(); i++){
        TextEffectComponent& effect = effects->getComponentFromIndex(i);
        effect.previewInEditor = false;
        effect.wasActive = false;
        effect.playing = false;
        effect.needApply = true;
    }
}

bool UISystem::isTextEffectsHeld() const{
    return textEffectsHeld;
}

namespace {

    // TMP Effect's word separators
    bool isWordSeparator(uint32_t c){
        if (c == 0x20 || c == 0x09 || c == 0x0A || c == 0x0D || c == 0xA0 || c == 0x3000 || (c >= 0x2000 && c <= 0x200B))
            return true;
        return c < 128 && std::ispunct((int)c);
    }

    // the codepoints forWords names: each whole-word occurrence of its space-separated words
    std::vector<bool> codepointsOfWords(const std::vector<uint32_t>& text, const std::string& forWords){
        std::vector<bool> marked(text.size(), false);
        bool invalid = false;
        std::vector<uint32_t> words = StringUtils::decodeUtf8ToCodepoints(forWords, invalid);
        size_t start = 0;
        while (start < words.size()){
            while (start < words.size() && isWordSeparator(words[start]) && (words[start] == 0x20 || words[start] == 0x09))
                start++;
            size_t end = start;
            while (end < words.size() && words[end] != 0x20 && words[end] != 0x09)
                end++;
            const size_t length = end - start;
            if (length > 0){
                for (size_t at = 0; at + length <= text.size(); at++){
                    if (!std::equal(words.begin() + start, words.begin() + end, text.begin() + at))
                        continue;
                    const bool before = at == 0 || isWordSeparator(text[at - 1]);
                    const bool after = at + length >= text.size() || isWordSeparator(text[at + length]);
                    if (before && after){
                        for (size_t k = at; k < at + length; k++)
                            marked[k] = true;
                    }
                }
            }
            start = end;
        }
        return marked;
    }

}

void UISystem::applyTextEffect(Entity entity, TextEffectComponent& effect, TextComponent& text, UIComponent& ui,
                               std::vector<int>& revealedEvents, bool& completed){
    (void)entity;
    Attribute* positions = ui.buffer.getAttribute(AttributeType::POSITION);
    Attribute* colors = ui.buffer.getAttribute(AttributeType::COLOR);
    if (!positions || !colors)
        return;

    const unsigned int vertexCount = std::min<unsigned int>(positions->getCount(), (unsigned int)effect.basePositions.size());
    const Vector4 base = effect.baseColor;   // linear
    const Vector3 plainRgb(base.x, base.y, base.z);

    // the plain text: the base vertices and its colour
    if (!effect.playing){
        for (unsigned int v = 0; v < vertexCount; v++){
            ui.buffer.setVector3(v, positions, effect.basePositions[v]);
            ui.buffer.setVector4(v, colors, Vector4(plainRgb, 1.0f));
        }
        effect.applied = false;
        ui.needUpdateBuffer = ui.loaded;
        return;
    }

    // the characters it animates: by range, or the whole words of forWords
    std::vector<uint32_t> clusters;
    clusters.reserve(text.glyphInfo.size());
    for (const TextGlyphInfo& info : text.glyphInfo)
        clusters.push_back(info.cluster);
    std::sort(clusters.begin(), clusters.end());
    clusters.erase(std::unique(clusters.begin(), clusters.end()), clusters.end());

    std::vector<bool> wordCodepoints;
    const bool byWords = !effect.forWords.empty();
    if (byWords){
        bool invalid = false;
        wordCodepoints = codepointsOfWords(StringUtils::decodeUtf8ToCodepoints(text.text, invalid), effect.forWords);
    }
    const int rangeStart = std::max(effect.startIndex, 0);
    const int rangeEnd = (effect.length < 0) ? std::numeric_limits<int>::max() : rangeStart + effect.length;

    // the glyph's index among the animated visible glyphs (-1: not animated or no ink)
    const size_t glyphCount = std::min(text.glyphInfo.size(), (size_t)(vertexCount / 4));
    // the effect's index of each glyph in reading (logical) order, so a right-to-left text
    // types from its first character too
    std::vector<int> order(glyphCount, -1);
    std::vector<std::pair<uint32_t, size_t>> selectedGlyphs;
    for (size_t g = 0; g < glyphCount; g++){
        const Vector3& p0 = effect.basePositions[g * 4];
        const Vector3& p2 = effect.basePositions[g * 4 + 2];
        if (p0.x == p2.x || p0.y == p2.y)
            continue;
        const uint32_t cluster = text.glyphInfo[g].cluster;
        bool selected;
        if (byWords){
            selected = cluster < wordCodepoints.size() && wordCodepoints[cluster];
        }else{
            const int character = (int)(std::lower_bound(clusters.begin(), clusters.end(), cluster) - clusters.begin());
            selected = character >= rangeStart && character < rangeEnd;
        }
        if (selected)
            selectedGlyphs.push_back({cluster, g});
    }
    std::stable_sort(selectedGlyphs.begin(), selectedGlyphs.end(),
                     [](const std::pair<uint32_t, size_t>& a, const std::pair<uint32_t, size_t>& b){ return a.first < b.first; });
    const int count = (int)selectedGlyphs.size();
    for (int i = 0; i < count; i++)
        order[selectedGlyphs[(size_t)i].second] = i;
    // TMP Effect times glyphs over the characters, spaces included: the selection's and the text's
    const int textCharacters = (int)clusters.size();
    int characters = 0;
    if (byWords){
        characters = (int)std::count(wordCodepoints.begin(), wordCodepoints.end(), true);
    }else{
        characters = std::max(0, std::min(rangeEnd, textCharacters) - std::min(rangeStart, textCharacters));
    }

    const bool reduced = UISystem::isReducedMotion();
    const float fontSize = (float)text.fontSize;
    const Vector4 baseSRGB(Color::linearTosRGB(plainRgb), base.w);
    // y up under an orthographic camera (flipped text): the effects work y down
    const float ySign = ui.flipY ? -1.0f : 1.0f;

    for (size_t g = 0; g < glyphCount; g++){
        const unsigned int first = (unsigned int)(g * 4);
        if (order[g] < 0){
            for (unsigned int v = 0; v < 4; v++){
                ui.buffer.setVector3(first + v, positions, effect.basePositions[first + v]);
                ui.buffer.setVector4(first + v, colors, Vector4(plainRgb, 1.0f));
            }
            continue;
        }

        // corners top-left, top-right, bottom-right, bottom-left of the quad (x0,y0) (x1,y0)
        // (x1,y1) (x0,y1); flipped, the first two are the bottom ones
        static const int straight[4] = {0, 1, 2, 3};
        static const int flipped[4] = {3, 2, 1, 0};
        const int* corner = ui.flipY ? flipped : straight;

        TextEffectGlyphState state;
        for (int c = 0; c < 4; c++){
            const Vector3& p = effect.basePositions[first + corner[c]];
            state.corners[c] = Vector2(p.x, p.y * ySign);
        }
        state.color = baseSRGB;

        for (const TextEffectLayer& layer : effect.layers){
            const float layerTime = (float)(TextEffects::isOneShot(layer.type) ? effect.cycleTime : effect.time);
            TextEffects::applyLayer(layer, order[g], count, layerTime, fontSize, reduced, state, characters, textCharacters);
        }

        const Vector3 rgb = Color::sRGBToLinear(Vector3(state.color.x, state.color.y, state.color.z));
        const float alpha = (baseSRGB.w > 0.0f) ? state.color.w / baseSRGB.w : state.color.w;
        for (int c = 0; c < 4; c++){
            const Vector3& p = effect.basePositions[first + corner[c]];
            ui.buffer.setVector3(first + corner[c], positions, Vector3(state.corners[c].x, state.corners[c].y * ySign, p.z));
            ui.buffer.setVector4(first + corner[c], colors, Vector4(rgb, alpha));
        }
    }

    // a typewriter's events: each character once, in order, as it appears
    for (const TextEffectLayer& layer : effect.layers){
        if (!layer.enabled || layer.type != TextEffectType::Typewriter)
            continue;
        int shown = 0;
        while (shown < count && TextEffects::revealTime(layer, shown, count) <= (float)effect.cycleTime)
            shown++;
        for (int i = effect.revealed; i < shown; i++)
            revealedEvents.push_back(i);
        effect.revealed = std::max(effect.revealed, shown);
        break;
    }

    // the one-shot layers finished: every glyph reached its end (a layer reduced motion holds
    // still is done at once)
    bool anyOneShot = false;
    const float oneShotLength = TextEffects::effectLength(effect.layers.data(), effect.layers.size(), count, reduced, anyOneShot);
    if (anyOneShot && count > 0 && !effect.finished && effect.cycleTime >= (double)oneShotLength){
        effect.finished = true;
        completed = true;
    }

    effect.applied = true;
    ui.needUpdateBuffer = ui.loaded;
}

void UISystem::updateTextEffects(double dt){
    auto effects = scene->getComponentArray<TextEffectComponent>();
    if (effects->size() == 0){
        lastTextEffectsTime = 0.0;
        return;
    }

    // the Inspector's preview runs on the wall clock (edit mode updates with no delta)
    const double now = Engine::getSystemTime();
    const double previewDt = (lastTextEffectsTime > 0.0) ? std::min(now - lastTextEffectsTime, 0.25) : 0.0;
    lastTextEffectsTime = now;
    // the frame's delta before Engine.timeScale (dt is the scaled one)
    const double timeScale = Engine::getTimeScale();
    const double unscaledDt = (timeScale > 0.0) ? dt / timeScale : (double)Engine::getUnscaledDeltatime();

    struct Pending {
        Entity entity;
        std::vector<int> revealed;
        bool completed;
    };
    std::vector<Pending> pending;

    for (size_t i = 0; i < effects->size(); i++){
        Entity entity = effects->getEntity(i);
        TextEffectComponent& effect = effects->getComponentFromIndex(i);
        TextComponent* text = scene->findComponent<TextComponent>(entity);
        UIComponent* ui = scene->findComponent<UIComponent>(entity);
        if (!text || !ui || !text->loaded || !text->vertexColor || !ui->buffer.getAttribute(AttributeType::COLOR))
            continue;

        const bool held = textEffectsHeld && !effect.previewInEditor;
        const bool active = effect.enabled && scene->isEntityActiveInHierarchy(entity) && !held;

        // a rebuilt text is the new base; a new text starts its one-shot layers again
        bool changed = effect.needApply;
        if (effect.appliedBuild != text->buildCount){
            effect.appliedBuild = text->buildCount;
            Attribute* positions = ui->buffer.getAttribute(AttributeType::POSITION);
            effect.basePositions.resize(positions->getCount());
            for (unsigned int v = 0; v < positions->getCount(); v++)
                effect.basePositions[v] = ui->buffer.getVector3(positions, v);
            if (effect.appliedText != text->text || effect.appliedFontSize != text->fontSize){
                effect.appliedText = text->text;
                effect.appliedFontSize = text->fontSize;
                effect.cycleTime = 0.0;
                effect.revealed = 0;
                effect.finished = false;
            }
            changed = true;
        }
        if (ui->color != effect.baseColor){
            effect.baseColor = ui->color;
            changed = true;
        }

        if (!active){
            // hidden, disabled or held: the plain text, and it starts again when shown
            // (a colour or text change too: the vertices hold the absolute colour)
            if (effect.wasActive || effect.applied || changed){
                effect.wasActive = false;
                const bool wasPlaying = effect.playing;
                effect.playing = false;
                std::vector<int> none;
                bool done = false;
                applyTextEffect(entity, effect, *text, *ui, none, done);
                effect.playing = wasPlaying;
            }
            effect.needApply = false;
            continue;
        }

        if (!effect.wasActive){
            effect.wasActive = true;
            if (effect.autoPlay || effect.previewInEditor){
                effect.started = true;
                effect.playing = true;
                effect.needRestart = true;
            }
        }
        if (effect.needRestart){
            effect.needRestart = false;
            effect.time = 0.0;
            effect.cycleTime = 0.0;
            effect.revealed = 0;
            effect.finished = false;
            changed = true;
        }

        // looping layers keep it moving; one-shot ones until they finish
        bool looping = false;
        for (const TextEffectLayer& layer : effect.layers){
            if (layer.enabled && layer.type != TextEffectType::None && !TextEffects::isOneShot(layer.type) &&
                !(UISystem::isReducedMotion() && TextEffects::isMotion(layer.type)))
                looping = true;
        }

        if (effect.playing){
            const double step = effect.previewInEditor ? previewDt : (effect.useUnscaledTime ? unscaledDt : dt);
            if (step > 0.0 && (looping || !effect.finished)){
                effect.time += step;
                if (!effect.finished)
                    effect.cycleTime += step;
                changed = true;
            }
        }

        if (!changed && effect.applied == effect.playing)
            continue;

        Pending events{entity, {}, false};
        applyTextEffect(entity, effect, *text, *ui, events.revealed, events.completed);
        effect.needApply = false;

        // TMP Repeat: the one-shot layers again, the looping ones carry on (not one that reduced
        // motion holds still: it would complete every frame)
        bool anyOneShot = false;
        const bool repeats = TextEffects::effectLength(effect.layers.data(), effect.layers.size(), 1, UISystem::isReducedMotion(), anyOneShot) > 0.0f;
        if (events.completed && effect.loop && repeats){
            effect.cycleTime = 0.0;
            effect.revealed = 0;
            effect.finished = false;
        }
        if (!events.revealed.empty() || events.completed)
            pending.push_back(std::move(events));
    }

    // the callbacks may change the registry: after the pass, each looked up again
    for (const Pending& events : pending){
        for (int index : events.revealed){
            TextEffectComponent* effect = scene->findComponent<TextEffectComponent>(events.entity);
            if (!effect)
                break;
            effect->onCharacterRevealed.call(index);
        }
        if (events.completed){
            if (TextEffectComponent* effect = scene->findComponent<TextEffectComponent>(events.entity))
                effect->onComplete.call();
        }
    }
}

void UISystem::setReducedMotion(bool reducedMotion){
    UISystem::reducedMotion = reducedMotion;
}

bool UISystem::isReducedMotion(){
    return reducedMotion;
}

void UISystem::toUISpace(float& x, float& y) const{
    if (scene->isUIScaled()){
        const Vector2 ui = scene->canvasToUI(Vector2(x, y));
        x = ui.x;
        y = ui.y;
    }
}

Vector2 UISystem::getCanvasSize() const{
    Vector2 size((float)Engine::getCanvasWidth(), (float)Engine::getCanvasHeight());
    if (scene->isUIScaled()){
        size = Vector2(scene->getCanvasScale().uiWidth, scene->getCanvasScale().uiHeight);
    }

    if (anchorReferenceWidth != 0)
        size.x = anchorReferenceWidth;
    if (anchorReferenceHeight != 0)
        size.y = anchorReferenceHeight;

    return size;
}

float UISystem::getTextRasterScale() const{
    if (!scene->isUIScaled())
        return 1.0f;
    const float scale = scene->getCanvasScale().scaleFactor;
    return (scale > 0.0f) ? scale : 1.0f;
}

void UISystem::applyAnchorPreset(RectTransformComponent& layout){
    if (layout.anchorPreset == AnchorPreset::TOP_LEFT){
        layout.anchorPointLeft = 0;
        layout.anchorPointTop = 0;
        layout.anchorPointRight = 0;
        layout.anchorPointBottom = 0;
        layout.anchorOffsetLeft = 0;
        layout.anchorOffsetTop = 0;
        layout.anchorOffsetRight = layout.width;
        layout.anchorOffsetBottom = layout.height;
    }else if (layout.anchorPreset == AnchorPreset::TOP_RIGHT){
        layout.anchorPointLeft = 1;
        layout.anchorPointTop = 0;
        layout.anchorPointRight = 1;
        layout.anchorPointBottom = 0;
        layout.anchorOffsetLeft = -layout.width;
        layout.anchorOffsetTop = 0;
        layout.anchorOffsetRight = 0;
        layout.anchorOffsetBottom = layout.height;
    }else if (layout.anchorPreset == AnchorPreset::BOTTOM_RIGHT){
        layout.anchorPointLeft = 1;
        layout.anchorPointTop = 1;
        layout.anchorPointRight = 1;
        layout.anchorPointBottom = 1;
        layout.anchorOffsetLeft = -layout.width;
        layout.anchorOffsetTop = -layout.height;
        layout.anchorOffsetRight = 0;
        layout.anchorOffsetBottom = 0;
    }else if (layout.anchorPreset == AnchorPreset::BOTTOM_LEFT){
        layout.anchorPointLeft = 0;
        layout.anchorPointTop = 1;
        layout.anchorPointRight = 0;
        layout.anchorPointBottom = 1;
        layout.anchorOffsetLeft = 0;
        layout.anchorOffsetTop = -layout.height;
        layout.anchorOffsetRight = layout.width;
        layout.anchorOffsetBottom = 0;
    }else if (layout.anchorPreset == AnchorPreset::CENTER_LEFT){
        layout.anchorPointLeft = 0;
        layout.anchorPointTop = 0.5;
        layout.anchorPointRight = 0;
        layout.anchorPointBottom = 0.5;
        layout.anchorOffsetLeft = 0;
        layout.anchorOffsetTop = -floor(layout.height / 2.0);
        layout.anchorOffsetRight = layout.width;
        layout.anchorOffsetBottom = ceil(layout.height / 2.0);
    }else if (layout.anchorPreset == AnchorPreset::CENTER_TOP){
        layout.anchorPointLeft = 0.5;
        layout.anchorPointTop = 0;
        layout.anchorPointRight = 0.5;
        layout.anchorPointBottom = 0;
        layout.anchorOffsetLeft = -floor(layout.width / 2.0);
        layout.anchorOffsetTop = 0;
        layout.anchorOffsetRight = ceil(layout.width / 2.0);
        layout.anchorOffsetBottom = layout.height;
    }else if (layout.anchorPreset == AnchorPreset::CENTER_RIGHT){
        layout.anchorPointLeft = 1;
        layout.anchorPointTop = 0.5;
        layout.anchorPointRight = 1;
        layout.anchorPointBottom = 0.5;
        layout.anchorOffsetLeft = -layout.width;
        layout.anchorOffsetTop = -floor(layout.height / 2.0);
        layout.anchorOffsetRight = 0;
        layout.anchorOffsetBottom = ceil(layout.height / 2.0);
    }else if (layout.anchorPreset == AnchorPreset::CENTER_BOTTOM){
        layout.anchorPointLeft = 0.5;
        layout.anchorPointTop = 1;
        layout.anchorPointRight = 0.5;
        layout.anchorPointBottom = 1;
        layout.anchorOffsetLeft = -floor(layout.width / 2.0);
        layout.anchorOffsetTop = -layout.height;
        layout.anchorOffsetRight = ceil(layout.width / 2.0);
        layout.anchorOffsetBottom = 0;
    }else if (layout.anchorPreset == AnchorPreset::CENTER){
        layout.anchorPointLeft = 0.5;
        layout.anchorPointTop = 0.5;
        layout.anchorPointRight = 0.5;
        layout.anchorPointBottom = 0.5;
        layout.anchorOffsetLeft = -floor(layout.width / 2.0);
        layout.anchorOffsetTop = -floor(layout.height / 2.0);
        layout.anchorOffsetRight = ceil(layout.width / 2.0);
        layout.anchorOffsetBottom = ceil(layout.height / 2.0);
    }else if (layout.anchorPreset == AnchorPreset::LEFT_WIDE){
        layout.anchorPointLeft = 0;
        layout.anchorPointTop = 0;
        layout.anchorPointRight = 0;
        layout.anchorPointBottom = 1;
        layout.anchorOffsetLeft = 0;
        layout.anchorOffsetTop = 0;
        layout.anchorOffsetRight = layout.width;
        layout.anchorOffsetBottom = 0;
    }else if (layout.anchorPreset == AnchorPreset::TOP_WIDE){
        layout.anchorPointLeft = 0;
        layout.anchorPointTop = 0;
        layout.anchorPointRight = 1;
        layout.anchorPointBottom = 0;
        layout.anchorOffsetLeft = 0;
        layout.anchorOffsetTop = 0;
        layout.anchorOffsetRight = 0;
        layout.anchorOffsetBottom = layout.height;
    }else if (layout.anchorPreset == AnchorPreset::RIGHT_WIDE){
        layout.anchorPointLeft = 1;
        layout.anchorPointTop = 0;
        layout.anchorPointRight = 1;
        layout.anchorPointBottom = 1;
        layout.anchorOffsetLeft = -layout.width;
        layout.anchorOffsetTop = 0;
        layout.anchorOffsetRight = 0;
        layout.anchorOffsetBottom = 0;
    }else if (layout.anchorPreset == AnchorPreset::BOTTOM_WIDE){
        layout.anchorPointLeft = 0;
        layout.anchorPointTop = 1;
        layout.anchorPointRight = 1;
        layout.anchorPointBottom = 1;
        layout.anchorOffsetLeft = 0;
        layout.anchorOffsetTop = -layout.height;
        layout.anchorOffsetRight = 0;
        layout.anchorOffsetBottom = 0;
    }else if (layout.anchorPreset == AnchorPreset::VERTICAL_CENTER_WIDE){
        layout.anchorPointLeft = 0.5;
        layout.anchorPointTop = 0;
        layout.anchorPointRight = 0.5;
        layout.anchorPointBottom = 1;
        layout.anchorOffsetLeft = -floor(layout.width / 2.0);
        layout.anchorOffsetTop = 0;
        layout.anchorOffsetRight = ceil(layout.width / 2.0);
        layout.anchorOffsetBottom = 0;
    }else if (layout.anchorPreset == AnchorPreset::HORIZONTAL_CENTER_WIDE){
        layout.anchorPointLeft = 0;
        layout.anchorPointTop = 0.5;
        layout.anchorPointRight = 1;
        layout.anchorPointBottom = 0.5;
        layout.anchorOffsetLeft = 0;
        layout.anchorOffsetTop = -floor(layout.height / 2.0);
        layout.anchorOffsetRight = 0;
        layout.anchorOffsetBottom = ceil(layout.height / 2.0);
    }else if (layout.anchorPreset == AnchorPreset::FULL_LAYOUT){
        layout.anchorPointLeft = 0;
        layout.anchorPointTop = 0;
        layout.anchorPointRight = 1;
        layout.anchorPointBottom = 1;
        layout.anchorOffsetLeft = 0;
        layout.anchorOffsetTop = 0;
        layout.anchorOffsetRight = 0;
        layout.anchorOffsetBottom = 0;
    }
}

void UISystem::changeFlipY(UIComponent& ui, CameraComponent& camera){
    // framebuffer textures need no special case: offscreen passes render with a
    // Y-flipped projection on GL, so they are top-left origin on every backend
    ui.flipY = false;
    if (camera.type != CameraType::CAMERA_UI){
        ui.flipY = !ui.flipY;
    }
}

void UISystem::destroyText(TextComponent& text){
    text.loaded = false;
    text.needReloadAtlas = false;

    text.needUpdateText = true;

    text.atlasVersion = 0;

    if (text.stbtext){
        // an atlas of a raster scale or a distance field is one of many (each canvas scale,
        // each padding): it goes with its last text; plain atlases stay pooled as before
        if (text.rasterScale != 1.0f || text.sdfPadding > 0)
            FontPool::release(text.stbtext);
        else
            text.stbtext.reset();
    }
}

void UISystem::destroyButton(ButtonComponent& button){
    button.textureNormal.destroy();
    button.textureHovered.destroy();
    button.texturePressed.destroy();
    button.textureDisabled.destroy();

    button.needUpdateButton = true;
}

bool UISystem::isEntityChild(Entity child, Entity parent) const{
    if (child == NULL_ENTITY || parent == NULL_ENTITY){
        return false;
    }

    Transform* transform = scene->findComponent<Transform>(child);
    return transform && transform->parent == parent;
}

void UISystem::load(){
    canvasGroupsStructuralDirty = true;
    update(0);
}

void UISystem::destroy(){
    auto layouts = scene->getComponentArray<RectTransformComponent>();
    for (int i = 0; i < layouts->size(); i++) {
        Entity entity = layouts->getEntity(i);
        Signature signature = scene->getSignature(entity);

        if (signature.test(scene->getComponentId<TextComponent>())) {
            TextComponent &text = scene->getComponent<TextComponent>(entity);

            destroyText(text);
        }
        if (signature.test(scene->getComponentId<ButtonComponent>())) {
            ButtonComponent &button = scene->getComponent<ButtonComponent>(entity);

            if (Engine::isViewLoaded()){
                destroyButton(button);
            }else{
                // Keep in-memory state textures available after view recreation.
                button.needUpdateButton = true;
            }
        }
    }
}

void UISystem::draw(){
    // before RenderSystem draws: a write after the update (onPostUpdate) shows this frame
    syncCanvasGroups();
}

void UISystem::createOrUpdateUiComponent(RectTransformComponent& layout, Entity entity, Signature signature){
    if (signature.test(scene->getComponentId<UIComponent>())){
        UIComponent& ui = scene->getComponent<UIComponent>(entity);
        // replaced or deserialized without its geometry (not saved for images, texts or polygons)
        const bool noGeometry = !ui.buffer.getAttribute(AttributeType::POSITION);

        // Texts
        if (signature.test(scene->getComponentId<TextComponent>())){
            TextComponent& text = scene->getComponent<TextComponent>(entity);

            if (noGeometry) text.needUpdateText = true;

            // a Text Effect needs a colour per vertex: another buffer layout and shader variant
            const bool vertexColor = scene->findComponent<TextEffectComponent>(entity) != nullptr;
            if (text.vertexColor != vertexColor){
                text.vertexColor = vertexColor;
                text.needUpdateText = true;
                if (ui.loaded)
                    ui.needReload = true;
            }

            if (InputFieldComponent* ownerEdit = findInputFieldForTextChild(entity)){
                // (its input field rebuilds it: a new canvas scale or outline reaches it too)
                checkTextAtlasOptions(text, ui);
                if (text.needUpdateText || text.needReloadAtlas || isFontAtlasStale(text)){
                    ownerEdit->needUpdateInputField = true;
                }else if (text.loaded && ui.textSdf){
                    updateTextShaderParams(text, ui);
                }
            }else{
                createOrUpdateText(text, ui, layout);
            }
        }

        // UI Polygons
        if (signature.test(scene->getComponentId<PolygonComponent>())){
            PolygonComponent& polygon = scene->getComponent<PolygonComponent>(entity);

            if (noGeometry) polygon.needUpdatePolygon = true;

            createOrUpdatePolygon(polygon, ui, layout);
        }

        // Images
        if (signature.test(scene->getComponentId<ImageComponent>())){
            ImageComponent& img = scene->getComponent<ImageComponent>(entity);

            if (noGeometry) img.needUpdatePatches = true;

            if (!createOrUpdateImage(img, ui, layout)) pendingImages = true;

            // Buttons
            if (signature.test(scene->getComponentId<ButtonComponent>())){
                ButtonComponent& button = scene->getComponent<ButtonComponent>(entity);

                if (button.needUpdateButton){
                    updateButton(entity, button, img, ui, layout);

                    button.needUpdateButton = false;
                }
            }

            // Panels
            if (signature.test(scene->getComponentId<PanelComponent>())){
                PanelComponent& panel = scene->getComponent<PanelComponent>(entity);

                if (panel.needUpdatePanel){
                    updatePanel(entity, panel, img, ui, layout);

                    panel.needUpdatePanel = false;
                }
            }

            // Scrollbar
            if (signature.test(scene->getComponentId<ScrollbarComponent>())){
                ScrollbarComponent& scrollbar = scene->getComponent<ScrollbarComponent>(entity);

                if (scrollbar.needUpdateScrollbar){
                    scrollbar.needUpdateScrollbar = false;
                    // onChange can invalidate img, ui and layout: leave the remaining
                    // updates for the next frame, their flags are still set
                    updateScrollbar(scrollbar, layout);
                    return;
                }
            }

            // Progressbar
            if (signature.test(scene->getComponentId<ProgressbarComponent>())){
                ProgressbarComponent& progressbar = scene->getComponent<ProgressbarComponent>(entity);

                if (progressbar.needUpdateProgressbar){
                    updateProgressbar(entity, progressbar, img, ui, layout);

                    progressbar.needUpdateProgressbar = false;
                }
            }

            // Inputfields
            if (signature.test(scene->getComponentId<InputFieldComponent>())){
                InputFieldComponent& inputfield = scene->getComponent<InputFieldComponent>(entity);

                if (inputfield.needUpdateInputField){
                    updateInputField(entity, inputfield, img, ui, layout);

                    inputfield.needUpdateInputField = false;
                }
            }
        }
    }
}

void UISystem::getPanelEdges(const PanelComponent& panel, const RectTransformComponent& layout, const Transform& transform, const RectTransformComponent& headerlayout,  Rect& edgeRight, Rect& edgeRightBottom, Rect& edgeBottom, Rect& edgeLeftBottom, Rect& edgeLeft){
    int minX;
    int minY;
    int width;
    int height;

    Vector2 scaledSize = Vector2(layout.width * transform.worldScale.x, layout.height * transform.worldScale.y);
    Vector2 scaledResizeSize = Vector2(panel.resizeMargin * transform.worldScale.x, panel.resizeMargin * transform.worldScale.y);
    float scaledHeaderHeight = headerlayout.height * transform.worldScale.y;

    // right
    minX = transform.worldPosition.x + scaledSize.x - scaledResizeSize.x;
    minY = transform.worldPosition.y + scaledHeaderHeight;
    width = scaledResizeSize.x;
    height = scaledSize.y - scaledResizeSize.y - scaledHeaderHeight - 1;
    edgeRight = Rect(minX, minY, width, height);

    // right-bottom
    minX = transform.worldPosition.x + scaledSize.x - scaledResizeSize.x;
    minY = transform.worldPosition.y + scaledSize.y - scaledResizeSize.y;
    width = scaledResizeSize.x;
    height = scaledResizeSize.y;
    edgeRightBottom = Rect(minX, minY, width, height);

    // bottom
    minX = transform.worldPosition.x + scaledResizeSize.x + 1;
    minY = transform.worldPosition.y + scaledSize.y - scaledResizeSize.y;
    width = scaledSize.x - 2;
    height = scaledResizeSize.y;
    edgeBottom = Rect(minX, minY, width, height);

    // left-bottom
    minX = transform.worldPosition.x;
    minY = transform.worldPosition.y + scaledSize.y - scaledResizeSize.y;
    width = scaledResizeSize.x;
    height = scaledResizeSize.y;
    edgeLeftBottom = Rect(minX, minY, width, height);

    // left
    minX = transform.worldPosition.x;
    minY = transform.worldPosition.y + scaledHeaderHeight;
    width = scaledResizeSize.x;
    height = scaledSize.y - scaledResizeSize.y - scaledHeaderHeight - 1;
    edgeLeft = Rect(minX, minY, width, height);
}

Rect UISystem::fitOnPanel(Rect uiRect, Entity parentPanel){
    Transform& paneltransform =  scene->getComponent<Transform>(parentPanel);
    RectTransformComponent& panellayout =  scene->getComponent<RectTransformComponent>(parentPanel);
    ImageComponent& panelimage =  scene->getComponent<ImageComponent>(parentPanel);

    float x = paneltransform.worldPosition.x + (panelimage.patchMarginLeft * paneltransform.worldScale.x);
    float y = paneltransform.worldPosition.y + (panelimage.patchMarginTop * paneltransform.worldScale.y);
    float width = (panellayout.width - (panelimage.patchMarginLeft + panelimage.patchMarginRight)) * paneltransform.worldScale.x;
    float height = (panellayout.height - (panelimage.patchMarginTop + panelimage.patchMarginBottom)) * paneltransform.worldScale.y;

    return uiRect.fitOnRect(Rect(x, y, width, height));
}

void UISystem::calculateUIAABB(UIComponent& ui){
    // seeded empty: AABB::ZERO is a finite box at the origin that every merge would keep
    ui.aabb.setNull();
    Attribute* vertexAttr = ui.buffer.getAttribute(AttributeType::POSITION);
    if (vertexAttr){
        for (int i = 0; i < ui.buffer.getCount(); i++){
            Vector3 vertice = ui.buffer.getVector3(vertexAttr, i);

            ui.aabb.merge(vertice);
        }
    }
    if (ui.aabb.isNull())
        ui.aabb = AABB::ZERO;

    ui.needUpdateAABB = true;
}

void UISystem::update(double dt){
    pendingImages = false;

    syncCanvasGroups();

    if (paused) {
        return;
    }

    // what a Canvas Group change or a finished fade calls back, outside any draw or raycast
    processInteractabilityLost();
    processCanvasGroupFades();

    // need to be ordered by Transform
    auto layouts = scene->getComponentArray<RectTransformComponent>();

    for (int i = 0; i < layouts->size(); i++){
        RectTransformComponent& layout = layouts->getComponentFromIndex(i);
        Entity entity = layouts->getEntity(i);
        Signature signature = scene->getSignature(entity);

        // a deactivated or disabled element cannot keep the keyboard focus
        if (signature.test(scene->getComponentId<UIComponent>())){
            UIComponent& ui = scene->getComponent<UIComponent>(entity);
            if (ui.focused && !isUIActive(entity, signature)){
                ui.focused = false;
                if (InputFieldComponent* inputfield = scene->findComponent<InputFieldComponent>(entity)){
                    inputfield->needUpdateInputField = true;
                }
                ui.onLostFocus.call();
                // the callback may have changed the registry and moved the layout; an
                // inactive element takes no layout space, so it just waits a frame
                continue;
            }
        }

        if (signature.test(scene->getComponentId<LayoutGroupComponent>())){
            LayoutGroupComponent& container = scene->getComponent<LayoutGroupComponent>(entity);
            // reseting all container boxes
            for (int b = 0; b < container.numBoxes; b++){
                container.boxes[b].layout = NULL_ENTITY;
            }
            container.numBoxes = 0;
        }

        if (signature.test(scene->getComponentId<Transform>())){
            Transform& transform = scene->getComponent<Transform>(entity);
            Entity layoutParentEntity = findNearestLayoutParent(transform.parent);
            if (layoutParentEntity != NULL_ENTITY){
                RectTransformComponent* parentlayout = scene->findComponent<RectTransformComponent>(layoutParentEntity);
                LayoutGroupComponent* parentcontainer = scene->findComponent<LayoutGroupComponent>(layoutParentEntity);
                // like Unity layout groups, an inactive child takes no space
                if (parentcontainer && transform.visible && scene->isEntityActiveInHierarchy(entity)){
                    if (parentcontainer->numBoxes < MAX_CONTAINER_BOXES){
                        layout.containerBoxIndex = parentcontainer->numBoxes;
                        if (!layout.usingAnchors){
                            layout.anchorPreset = AnchorPreset::TOP_LEFT;
                            layout.usingAnchors = true;
                        }
                        parentcontainer->boxes[layout.containerBoxIndex].layout = entity;

                        parentcontainer->numBoxes = parentcontainer->numBoxes + 1;
                    }else{
                        transform.parent = NULL_ENTITY;
                        scene->hierarchyChanged();
                        Log::error("The UI container has exceeded the maximum allowed of %i children. Please, increase MAX_CONTAINER_BOXES value.", MAX_CONTAINER_BOXES);
                    }
                }

                PanelComponent* parentpanel = scene->findComponent<PanelComponent>(layoutParentEntity);
                if (parentpanel){
                    layout.panel = layoutParentEntity;
                }

                if (parentlayout->panel != NULL_ENTITY){
                    layout.panel = parentlayout->panel;
                }
            }
        }
    }

    // reverse Transform order to get sizes
    // iterating over a snapshot because createOrUpdateUiComponent can run user callbacks
    // that add or remove UI entities, reordering the component array
    std::vector<Entity> layoutEntities;
    layoutEntities.reserve(layouts->size());
    for (int i = layouts->size()-1; i >= 0; i--){
        layoutEntities.push_back(layouts->getEntity(i));
    }

    for (Entity entity : layoutEntities){
        RectTransformComponent* layoutPtr = scene->findComponent<RectTransformComponent>(entity);
        if (!layoutPtr){
            continue;
        }

        createOrUpdateUiComponent(*layoutPtr, entity, scene->getSignature(entity));

        layoutPtr = scene->findComponent<RectTransformComponent>(entity);
        if (!layoutPtr){
            continue;
        }

        RectTransformComponent& layout = *layoutPtr;
        Signature signature = scene->getSignature(entity);

        if (signature.test(scene->getComponentId<LayoutGroupComponent>())){
            LayoutGroupComponent& container = scene->getComponent<LayoutGroupComponent>(entity);

            // reset cached intrinsic minimums for this frame
            container.contentMinWidth = 0;
            container.contentMinHeight = 0;

            // configuring all container boxes
            if (container.numBoxes > 0){

                container.fixedWidth = 0;
                container.fixedHeight = 0;
                container.numBoxExpand = 0;
                container.maxWidth = 0;
                container.maxHeight = 0;
                // Content-min totals exclude children whose size is derived from parent via anchors
                // to avoid a circular dependency that prevents the container from shrinking
                int contentMinTotalWidth = 0;
                int contentMinTotalHeight = 0;
                unsigned int contentMinMaxWidth = 0;
                unsigned int contentMinMaxHeight = 0;
                for (int b = 0; b < container.numBoxes; b++){
                    if (container.boxes[b].layout != NULL_ENTITY){
                        if (!container.boxes[b].expand){
                            container.fixedWidth += container.boxes[b].rect.getWidth();
                        }
                        if (!container.boxes[b].expand){
                            container.fixedHeight += container.boxes[b].rect.getHeight();
                        }
                        container.maxHeight = std::max(container.maxHeight, (unsigned int)container.boxes[b].rect.getHeight());
                        container.maxWidth = std::max(container.maxWidth, (unsigned int)container.boxes[b].rect.getWidth());

                        // Check if child size is anchor-derived from parent in each dimension
                        RectTransformComponent* childLayout = scene->findComponent<RectTransformComponent>(container.boxes[b].layout);
                        bool widthFromParent = childLayout && childLayout->usingAnchors &&
                            (childLayout->anchorPointLeft != childLayout->anchorPointRight);
                        bool heightFromParent = childLayout && childLayout->usingAnchors &&
                            (childLayout->anchorPointTop != childLayout->anchorPointBottom);

                        // If child is anchor-derived but is itself a Container, use its intrinsic
                        // content minimum so the parent cannot shrink below the child's needs
                        LayoutGroupComponent* childContainer = scene->findComponent<LayoutGroupComponent>(container.boxes[b].layout);

                        if (!widthFromParent){
                            contentMinTotalWidth += container.boxes[b].rect.getWidth();
                            contentMinMaxWidth = std::max(contentMinMaxWidth, (unsigned int)container.boxes[b].rect.getWidth());
                        } else if (childContainer) {
                            contentMinTotalWidth += childContainer->contentMinWidth;
                            contentMinMaxWidth = std::max(contentMinMaxWidth, childContainer->contentMinWidth);
                        }
                        if (!heightFromParent){
                            contentMinTotalHeight += container.boxes[b].rect.getHeight();
                            contentMinMaxHeight = std::max(contentMinMaxHeight, (unsigned int)container.boxes[b].rect.getHeight());
                        } else if (childContainer) {
                            contentMinTotalHeight += childContainer->contentMinHeight;
                            contentMinMaxHeight = std::max(contentMinMaxHeight, childContainer->contentMinHeight);
                        }
                    }
                    if (container.boxes[b].expand){
                        container.numBoxExpand++;
                    }
                }


                if (container.type == LayoutGroupType::HORIZONTAL){
                    layout.width = (layout.width > contentMinTotalWidth)? layout.width : contentMinTotalWidth;
                    layout.height = (layout.height > contentMinMaxHeight)? layout.height : contentMinMaxHeight;
                }else if (container.type == LayoutGroupType::VERTICAL){
                    layout.width = (layout.width > contentMinMaxWidth)? layout.width : contentMinMaxWidth;
                    layout.height = (layout.height > contentMinTotalHeight)? layout.height : contentMinTotalHeight;
                }else if (container.type == LayoutGroupType::HORIZONTAL_WRAP){
                    layout.width = (layout.width > container.maxWidth)? layout.width : container.maxWidth;
                    unsigned int effCellW = (container.wrapCellWidth > 0) ? container.wrapCellWidth : container.maxWidth;
                    unsigned int effCellH = (container.wrapCellHeight > 0) ? container.wrapCellHeight : container.maxHeight;
                    if (effCellW > 0 && effCellH > 0){
                        int itemsPerRow = layout.width / effCellW;
                        if (itemsPerRow < 1) itemsPerRow = 1;
                        int numRows = (container.numBoxes + itemsPerRow - 1) / itemsPerRow;
                        unsigned int minHeight = numRows * effCellH;
                        layout.height = (layout.height > minHeight)? layout.height : minHeight;
                    }else{
                        layout.height = (layout.height > container.maxHeight)? layout.height : container.maxHeight;
                    }
                }else if (container.type == LayoutGroupType::VERTICAL_WRAP){
                    layout.height = (layout.height > container.maxHeight)? layout.height : container.maxHeight;
                    unsigned int effCellW = (container.wrapCellWidth > 0) ? container.wrapCellWidth : container.maxWidth;
                    unsigned int effCellH = (container.wrapCellHeight > 0) ? container.wrapCellHeight : container.maxHeight;
                    if (effCellH > 0 && effCellW > 0){
                        int itemsPerCol = layout.height / effCellH;
                        if (itemsPerCol < 1) itemsPerCol = 1;
                        int numCols = (container.numBoxes + itemsPerCol - 1) / itemsPerCol;
                        unsigned int minWidth = numCols * effCellW;
                        layout.width = (layout.width > minWidth)? layout.width : minWidth;
                    }else{
                        layout.width = (layout.width > container.maxWidth)? layout.width : container.maxWidth;
                    }
                }

                if (layout.width < 1) layout.width = 1;
                if (layout.height < 1) layout.height = 1;

                // Store the intrinsic content minimum for parent containers to use
                if (container.type == LayoutGroupType::HORIZONTAL){
                    container.contentMinWidth = contentMinTotalWidth;
                    container.contentMinHeight = contentMinMaxHeight;
                } else if (container.type == LayoutGroupType::VERTICAL){
                    container.contentMinWidth = contentMinMaxWidth;
                    container.contentMinHeight = contentMinTotalHeight;
                } else if (container.type == LayoutGroupType::HORIZONTAL_WRAP){
                    container.contentMinWidth = contentMinMaxWidth;
                    container.contentMinHeight = contentMinMaxHeight;
                } else if (container.type == LayoutGroupType::VERTICAL_WRAP){
                    container.contentMinWidth = contentMinMaxWidth;
                    container.contentMinHeight = contentMinMaxHeight;
                }
            }
        }

        if (signature.test(scene->getComponentId<Transform>())){
            Transform& transform = scene->getComponent<Transform>(entity);
            Entity layoutParentEntity = findNearestLayoutParent(transform.parent);
            LayoutGroupComponent* parentcontainer = layoutParentEntity != NULL_ENTITY
                ? scene->findComponent<LayoutGroupComponent>(layoutParentEntity)
                : nullptr;
            if (parentcontainer && layout.containerBoxIndex >= 0){
                parentcontainer->boxes[layout.containerBoxIndex].rect = Rect(0, 0, layout.width, layout.height);
            }
        }
    }

    for (int i = 0; i < layouts->size(); i++){
        RectTransformComponent& layout = layouts->getComponentFromIndex(i);
        Entity entity = layouts->getEntity(i);
        Signature signature = scene->getSignature(entity);

        if (signature.test(scene->getComponentId<Transform>())){
            Transform& transform = scene->getComponent<Transform>(entity);

            // a Safe Area panel's anchors follow Screen.safeArea; an inactive one keeps what it had
            if (signature.test(scene->getComponentId<SafeAreaComponent>())){
                SafeAreaComponent& safeArea = scene->getComponent<SafeAreaComponent>(entity);
                if (safeArea.enabled && scene->isEntityActiveInHierarchy(entity)){
                    applySafeArea(safeArea, layout, transform);
                }
            }

            if (layout.usingAnchors){
                if (layout.anchorPointRight < layout.anchorPointLeft)
                    layout.anchorPointRight = layout.anchorPointLeft;
                if (layout.anchorPointBottom < layout.anchorPointTop)
                    layout.anchorPointBottom = layout.anchorPointTop;

                applyAnchorPreset(layout);
            }

            Rect anchorRefRect = getAnchorReferenceRect(layout, transform, false);
            float abAnchorLeft = (anchorRefRect.getWidth() * layout.anchorPointLeft) + anchorRefRect.getX();
            float abAnchorRight = (anchorRefRect.getWidth() * layout.anchorPointRight) + anchorRefRect.getX();
            float abAnchorTop = (anchorRefRect.getHeight() * layout.anchorPointTop) + anchorRefRect.getY();
            float abAnchorBottom = (anchorRefRect.getHeight() * layout.anchorPointBottom) + anchorRefRect.getY();

            if (layout.usingAnchors){

                if (layout.anchorPreset == AnchorPreset::NONE && layout.needUpdateAnchorOffsets){
                    // Convert current manual transform/size back into anchor offsets
                    float rawPosX = transform.position.x;
                    float rawPosY = transform.position.y;

                    if (transform.parent != NULL_ENTITY){
                        Entity layoutParentEntity = findNearestLayoutParent(transform.parent);
                        convertLocalParentSpaceToLayoutSpace(transform.parent, layoutParentEntity, rawPosX, rawPosY);
                    }

                    rawPosX -= layout.positionOffset.x;
                    rawPosY -= layout.positionOffset.y;

                    layout.anchorOffsetLeft = rawPosX - abAnchorLeft;
                    layout.anchorOffsetTop = rawPosY - abAnchorTop;

                    layout.anchorOffsetRight = layout.width - (abAnchorRight - abAnchorLeft) + layout.anchorOffsetLeft;
                    layout.anchorOffsetBottom = layout.height - (abAnchorBottom - abAnchorTop) + layout.anchorOffsetTop;
                }

                layout.needUpdateAnchorOffsets = false;

                float posX = abAnchorLeft + layout.anchorOffsetLeft;
                float posY = abAnchorTop + layout.anchorOffsetTop;

                float computedWidth = abAnchorRight - posX + layout.anchorOffsetRight;
                float computedHeight = abAnchorBottom - posY + layout.anchorOffsetBottom;

                if (computedWidth < 1.0f) computedWidth = 1.0f;
                if (computedHeight < 1.0f) computedHeight = 1.0f;
                unsigned int width = static_cast<unsigned int>(std::ceil(computedWidth));
                unsigned int height = static_cast<unsigned int>(std::ceil(computedHeight));

                if (width != layout.width || height != layout.height){
                    layout.width = width;
                    layout.height = height;
                    layout.needUpdateSizes = true;
                }

                posX += layout.positionOffset.x;
                posY += layout.positionOffset.y;

                if (transform.parent != NULL_ENTITY){
                    Entity layoutParentEntity = findNearestLayoutParent(transform.parent);
                    convertLayoutSpaceToLocalParentSpace(transform.parent, layoutParentEntity, posX, posY);
                }

                if (posX != transform.position.x || posY != transform.position.y){
                    transform.position.x = posX;
                    transform.position.y = posY;
                    transform.needUpdate = true;
                }

            }else{
                float layoutPosX = transform.position.x;
                float layoutPosY = transform.position.y;

                if (transform.parent != NULL_ENTITY){
                    Entity layoutParentEntity = findNearestLayoutParent(transform.parent);
                    convertLocalParentSpaceToLayoutSpace(transform.parent, layoutParentEntity, layoutPosX, layoutPosY);
                }

                layout.anchorOffsetLeft = layoutPosX - abAnchorLeft;
                layout.anchorOffsetTop = layoutPosY - abAnchorTop;
                layout.anchorOffsetRight = layout.width + layoutPosX - abAnchorRight;
                layout.anchorOffsetBottom = layout.height + layoutPosY - abAnchorBottom;
            }
        }

        if (signature.test(scene->getComponentId<LayoutGroupComponent>())){
            LayoutGroupComponent& container = scene->getComponent<LayoutGroupComponent>(entity);
            int numObjInLine = 0;

            // Resolve effective cell sizes: custom override or auto from children
            unsigned int effectiveCellWidth = (container.wrapCellWidth > 0) ? container.wrapCellWidth : container.maxWidth;
            unsigned int effectiveCellHeight = (container.wrapCellHeight > 0) ? container.wrapCellHeight : container.maxHeight;

            if (container.type == LayoutGroupType::HORIZONTAL_WRAP){
                if (effectiveCellWidth > 0){
                    numObjInLine = floor((float)layout.width / (float)effectiveCellWidth);
                }
                if (numObjInLine < 1) numObjInLine = 1;
            }else if (container.type == LayoutGroupType::VERTICAL_WRAP){
                if (effectiveCellHeight > 0){
                    numObjInLine = floor((float)layout.height / (float)effectiveCellHeight);
                }
                if (numObjInLine < 1) numObjInLine = 1;
            }
            // configuring all container boxes
            if (container.numBoxes > 0){

                // Pre-compute sizes for expand boxes in HORIZONTAL/VERTICAL using
                // freeze-and-redistribute: non-reducible boxes (images) that exceed
                // their equal share are frozen at their natural size; the remaining
                // space is distributed equally among the rest.
                int resolved[MAX_CONTAINER_BOXES];
                std::fill(resolved, resolved + container.numBoxes, -1);
                if (container.type == LayoutGroupType::HORIZONTAL || container.type == LayoutGroupType::VERTICAL){
                    bool horiz = (container.type == LayoutGroupType::HORIZONTAL);
                    int available = horiz
                        ? static_cast<int>(layout.width)  - static_cast<int>(container.fixedWidth)
                        : static_cast<int>(layout.height) - static_cast<int>(container.fixedHeight);
                    bool frozen[MAX_CONTAINER_BOXES] = {};
                    int frozenTotal = 0, frozenCount = 0;
                    bool changed = true;
                    while (changed && available > 0){
                        changed = false;
                        int freeCount = (int)container.numBoxExpand - frozenCount;
                        if (freeCount <= 0) break;
                        int share = (available - frozenTotal) / freeCount;
                        for (int b = 0; b < container.numBoxes; b++){
                            if (!frozen[b] && container.boxes[b].layout != NULL_ENTITY && container.boxes[b].expand){
                                bool ar = scene->findComponent<LayoutGroupComponent>(container.boxes[b].layout) != nullptr;
                                int nat = horiz ? (int)container.boxes[b].rect.getWidth() : (int)container.boxes[b].rect.getHeight();
                                if (!ar && nat > share){
                                    frozen[b] = true; frozenTotal += nat; frozenCount++; changed = true; break;
                                }
                            }
                        }
                    }
                    int fc = (int)container.numBoxExpand - frozenCount;
                    int rem = available - frozenTotal;
                    int base = (fc > 0 && rem > 0) ? rem / fc : 1;
                    int leftover = (fc > 0 && rem > 0) ? rem % fc : 0;
                    int fi = 0;
                    for (int b = 0; b < container.numBoxes; b++){
                        if (container.boxes[b].layout != NULL_ENTITY && container.boxes[b].expand){
                            int nat = horiz ? (int)container.boxes[b].rect.getWidth() : (int)container.boxes[b].rect.getHeight();
                            resolved[b] = frozen[b] ? nat : std::max(1, base + (fi++ < leftover ? 1 : 0));
                        }
                    }
                }

                for (int b = 0; b < container.numBoxes; b++){
                    if (container.boxes[b].layout != NULL_ENTITY){
                        if (container.type == LayoutGroupType::HORIZONTAL){
                            if (b > 0){
                                container.boxes[b].rect.setX(container.boxes[b-1].rect.getX() + container.boxes[b-1].rect.getWidth());
                            }

                            if (container.boxes[b].expand && container.numBoxExpand > 0){
                                if (resolved[b] >= 0){
                                    container.boxes[b].rect.setWidth(resolved[b]);
                                }
                            }
                            container.boxes[b].rect.setHeight(layout.height);
                        }else if (container.type == LayoutGroupType::VERTICAL){
                            if (b > 0){
                                container.boxes[b].rect.setY(container.boxes[b-1].rect.getY() + container.boxes[b-1].rect.getHeight());
                            }

                            if (container.boxes[b].expand && container.numBoxExpand > 0){
                                if (resolved[b] >= 0){
                                    container.boxes[b].rect.setHeight(resolved[b]);
                                }
                            }
                            container.boxes[b].rect.setWidth(layout.width);
                        }else if (container.type == LayoutGroupType::HORIZONTAL_WRAP){
                            if (container.useAllWrapSpace){
                                int line = b / numObjInLine;
                                int lineIndex = b % numObjInLine;
                                int firstInLine = line * numObjInLine;
                                int itemsInLine = std::min(numObjInLine, static_cast<int>(container.numBoxes) - firstInLine);

                                if (itemsInLine > 0){
                                    int baseWidth = layout.width / itemsInLine;
                                    int remainder = layout.width % itemsInLine;

                                    int lineX = 0;
                                    for (int item = 0; item < lineIndex; item++){
                                        lineX += baseWidth + ((item < remainder) ? 1 : 0);
                                    }

                                    int itemWidth = baseWidth + ((lineIndex < remainder) ? 1 : 0);

                                    container.boxes[b].rect.setX(lineX);
                                    container.boxes[b].rect.setY(line * effectiveCellHeight);
                                    container.boxes[b].rect.setWidth(itemWidth);
                                }
                            }else{
                                // compute box width using signed math to avoid unsigned underflow
                                int containerW = static_cast<int>(layout.width);
                                int cellW = static_cast<int>(effectiveCellWidth);
                                int boxWidth = static_cast<int>(container.boxes[b].rect.getWidth());

                                if (container.boxes[b].expand && numObjInLine > 0 && cellW > 0){
                                    int diff = containerW - (numObjInLine * cellW);
                                    boxWidth = cellW + diff / numObjInLine;
                                }
                                boxWidth = std::clamp(boxWidth, 1, std::max(1, containerW));

                                container.boxes[b].rect.setWidth(boxWidth);

                                if (b == 0){
                                    container.boxes[b].rect.setX(0);
                                    container.boxes[b].rect.setY(0);
                                }else{
                                    int nextX = static_cast<int>(container.boxes[b-1].rect.getX() + container.boxes[b-1].rect.getWidth());
                                    int nextY = static_cast<int>(container.boxes[b-1].rect.getY());

                                    if (nextX + boxWidth > containerW){
                                        nextX = 0;
                                        nextY = static_cast<int>(container.boxes[b-1].rect.getY()) + static_cast<int>(effectiveCellHeight);
                                    }

                                    container.boxes[b].rect.setX(nextX);
                                    container.boxes[b].rect.setY(nextY);
                                }
                            }
                            container.boxes[b].rect.setHeight(effectiveCellHeight);
                        }else if (container.type == LayoutGroupType::VERTICAL_WRAP){
                            if (container.useAllWrapSpace){
                                int column = b / numObjInLine;
                                int columnIndex = b % numObjInLine;
                                int firstInColumn = column * numObjInLine;
                                int itemsInColumn = std::min(numObjInLine, static_cast<int>(container.numBoxes) - firstInColumn);

                                if (itemsInColumn > 0){
                                    int baseHeight = layout.height / itemsInColumn;
                                    int remainder = layout.height % itemsInColumn;

                                    int columnY = 0;
                                    for (int item = 0; item < columnIndex; item++){
                                        columnY += baseHeight + ((item < remainder) ? 1 : 0);
                                    }

                                    int itemHeight = baseHeight + ((columnIndex < remainder) ? 1 : 0);

                                    container.boxes[b].rect.setX(column * effectiveCellWidth);
                                    container.boxes[b].rect.setY(columnY);
                                    container.boxes[b].rect.setHeight(itemHeight);
                                }
                            }else{
                                // compute box height using signed math to avoid unsigned underflow
                                int containerH = static_cast<int>(layout.height);
                                int cellH = static_cast<int>(effectiveCellHeight);
                                int boxHeight = static_cast<int>(container.boxes[b].rect.getHeight());

                                if (container.boxes[b].expand && numObjInLine > 0 && cellH > 0){
                                    int diff = containerH - (numObjInLine * cellH);
                                    boxHeight = cellH + diff / numObjInLine;
                                }
                                boxHeight = std::clamp(boxHeight, 1, std::max(1, containerH));

                                container.boxes[b].rect.setHeight(boxHeight);

                                if (b == 0){
                                    container.boxes[b].rect.setX(0);
                                    container.boxes[b].rect.setY(0);
                                }else{
                                    int nextX = static_cast<int>(container.boxes[b-1].rect.getX());
                                    int nextY = static_cast<int>(container.boxes[b-1].rect.getY() + container.boxes[b-1].rect.getHeight());

                                    if (nextY + boxHeight > containerH){
                                        nextX = static_cast<int>(container.boxes[b-1].rect.getX()) + static_cast<int>(effectiveCellWidth);
                                        nextY = 0;
                                    }

                                    container.boxes[b].rect.setX(nextX);
                                    container.boxes[b].rect.setY(nextY);
                                }
                            }
                            container.boxes[b].rect.setWidth(effectiveCellWidth);
                        }
                    }
                }
            }
        }

        bool resized = layout.needUpdateSizes;
        unsigned int oldWidth = layout.width;
        unsigned int oldHeight = layout.height;

        if (layout.needUpdateSizes){
            if (signature.test(scene->getComponentId<ImageComponent>())){
                ImageComponent& img = scene->getComponent<ImageComponent>(entity);

                img.needUpdatePatches = true;
            }

            if (signature.test(scene->getComponentId<TextComponent>())){
                TextComponent& text = scene->getComponent<TextComponent>(entity);

                Vector2 minSize = getTextMinSize(text);
                if (layout.width < minSize.x) layout.width = minSize.x;
                if (layout.height < minSize.y) layout.height = minSize.y;

                if (InputFieldComponent* ownerEdit = findInputFieldForTextChild(entity)){
                    ownerEdit->needUpdateInputField = true;
                }else{
                    text.needUpdateText = true;
                }
            }

            if (signature.test(scene->getComponentId<ScrollbarComponent>())){
                ScrollbarComponent& scrollbar = scene->getComponent<ScrollbarComponent>(entity);

                scrollbar.needUpdateScrollbar = true;
            }

            if (signature.test(scene->getComponentId<ProgressbarComponent>())){
                ProgressbarComponent& progressbar = scene->getComponent<ProgressbarComponent>(entity);

                progressbar.needUpdateProgressbar = true;
            }

            layout.needUpdateSizes = false;
        }

        createOrUpdateUiComponent(layout, entity, signature);

        // the pivot is a share of the size, which a text may also have changed
        resized = resized || layout.width != oldWidth || layout.height != oldHeight;
        if (resized && layout.pivot != Vector2(0, 0) && signature.test(scene->getComponentId<Transform>())){
            scene->getComponent<Transform>(entity).needUpdate = true;
        }
        
    }

    auto inputfields = scene->getComponentArray<InputFieldComponent>();
    for (int i = 0; i < inputfields->size(); i++){
        Entity entity = inputfields->getEntity(i);
        UIComponent* ui = scene->findComponent<UIComponent>(entity);
        if (ui){
            blinkCursorInputField(dt, inputfields->getComponentFromIndex(i), *ui);
        }
    }

    // the on-screen controls (UISystemPointer.cpp): visibility, the Touch Areas' frame drags,
    // presses of elements that went inactive cancelled
    updateControls();
    // after the layout: the glyphs of this frame are built
    updateTextEffects(dt);
}

bool UISystem::hasPendingImages() const{
    return pendingImages;
}

bool UISystem::isInputFieldFocused(){
    auto inputfields = scene->getComponentArray<InputFieldComponent>();
    for (int i = 0; i < inputfields->size(); i++){
        Entity entity = inputfields->getEntity(i);
        Signature signature = scene->getSignature(entity);
        if (signature.test(scene->getComponentId<UIComponent>())){
            UIComponent& ui = scene->getComponent<UIComponent>(entity);
            if (ui.focused){
                return true;
            }
        }
    }
    return false;
}

bool UISystem::eventOnCharInput(char32_t codepoint){
    // what the event handlers destroy goes when the dispatch is over
    StructuralDeferScope deferScope(*scene);
    auto layouts = scene->getComponentArray<RectTransformComponent>();
    for (int i = 0; i < layouts->size(); i++){
        Entity entity = layouts->getEntity(i);
        Signature signature = scene->getSignature(entity);
        if (signature.test(scene->getComponentId<InputFieldComponent>()) && signature.test(scene->getComponentId<UIComponent>())){
            InputFieldComponent& inputfield = scene->getComponent<InputFieldComponent>(entity);
            UIComponent& ui = scene->getComponent<UIComponent>(entity);

            // a field that became non-interactable (own flag or Canvas Group) takes no typing,
            // also before the next update drops its focus
            if (ui.focused && isUIActive(entity, signature) && isInteractable(entity)){
                TextComponent* text = scene->findComponent<TextComponent>(inputfield.text);
                if (text && handleInputFieldCharInput(inputfield, *text, ui, codepoint)){
                    return true;
                }
            }
        }
    }

    return false;
}

bool UISystem::eventOnKeyDown(int key, bool repeat, int mods){
    // what the event handlers destroy goes when the dispatch is over
    StructuralDeferScope deferScope(*scene);
    auto layouts = scene->getComponentArray<RectTransformComponent>();
    for (int i = 0; i < layouts->size(); i++){
        Entity entity = layouts->getEntity(i);
        Signature signature = scene->getSignature(entity);
        if (signature.test(scene->getComponentId<InputFieldComponent>()) && signature.test(scene->getComponentId<UIComponent>())){
            InputFieldComponent& inputfield = scene->getComponent<InputFieldComponent>(entity);
            UIComponent& ui = scene->getComponent<UIComponent>(entity);

            // a field that became non-interactable (own flag or Canvas Group) takes no typing,
            // also before the next update drops its focus
            if (ui.focused && isUIActive(entity, signature) && isInteractable(entity)){
                TextComponent* text = scene->findComponent<TextComponent>(inputfield.text);
                if (text && handleInputFieldKeyDown(inputfield, *text, ui, key, repeat, mods)){
                    return true;
                }
            }
        }
    }

    return false;
}

bool UISystem::isUIActive(Entity entity, Signature signature) const {
    if (!scene->isEntityActiveInHierarchy(entity))
        return false;
    if (signature.test(scene->getComponentId<UIComponent>()))
        return scene->getComponent<UIComponent>(entity).enabled;
    return true;
}

bool UISystem::isLiveUIEntity(Entity entity) const {
    if (entity == NULL_ENTITY || !scene->isEntityCreated(entity))
        return false;
    return isLiveUIEntity(entity, scene->getSignature(entity));
}

bool UISystem::isLiveUIEntity(Entity entity, Signature signature) const {
    return entity != NULL_ENTITY
        && scene->isEntityCreated(entity)
        && signature.test(scene->getComponentId<Transform>())
        && signature.test(scene->getComponentId<UIComponent>());
}

bool UISystem::isCoordInside(float x, float y, Transform& transform, RectTransformComponent& layout){
    Vector3 point = transform.worldRotation.getRotationMatrix() * Vector3(x, y, 0);

    if (point.x >= (transform.worldPosition.x) &&
        point.x <= (transform.worldPosition.x + abs(layout.width * transform.worldScale.x)) &&
        point.y >= (transform.worldPosition.y) &&
        point.y <= (transform.worldPosition.y + abs(layout.height * transform.worldScale.y))) {
        return true;
    }
    return false;
}

void UISystem::onComponentRemoved(Entity entity, ComponentId componentId) {
    if (componentId == canvasGroupId ||
        componentId == uiId ||
        componentId == pointsId ||
        componentId == rectTransformId ||
        componentId == transformId) {
        canvasGroupsStructuralDirty = true;
    }
    if (componentId == canvasGroupId) {
        // the group's helper fade is cancelled with it (brief: cancelled with the owner)
        for (size_t i = 0; i < canvasGroupFades.size(); i++) {
            if (canvasGroupFades[i].group == entity) {
                CanvasGroupFade fade = std::move(canvasGroupFades[i]);
                canvasGroupFades.erase(canvasGroupFades.begin() + i);
                finishCanvasGroupFade(std::move(fade), false);
                break;
            }
        }
    } else if (componentId == actionId) {
        // a helper fade's tween destroyed by someone else: the fade is over, unfinished
        for (size_t i = 0; i < canvasGroupFades.size(); i++) {
            if (canvasGroupFades[i].tween == entity) {
                canvasGroupFades.erase(canvasGroupFades.begin() + i);
                break;
            }
        }
    }

    if (componentId == uiId ||
        componentId == rectTransformId ||
        componentId == imageId ||
        componentId == transformId) {
        // a destroyed element: the pointer states drop it without callbacks (D6)
        for (auto& pair : pointers) {
            UIPointerState& state = pair.second;
            for (Entity* target : {&state.hit, &state.hover, &state.rawPress, &state.press, &state.drag, &state.widget,
                                   &state.buttonPressed, &state.panelGesture, &state.scrollbar, &state.inputFieldSelecting}) {
                if (*target == entity)
                    *target = NULL_ENTITY;
            }
            if (state.drag == NULL_ENTITY)
                state.dragging = false;
            if (state.widget == NULL_ENTITY) {
                state.legacyDragging = false;
                state.internalGesture = false;
            }
            state.enterChain.erase(std::remove(state.enterChain.begin(), state.enterChain.end(), entity), state.enterChain.end());
        }
        for (auto& pair : clickRecords) {
            if (pair.second.element == entity)
                pair.second.element = NULL_ENTITY;
        }
        if (entity == lastUIFromClick) {
            lastUIFromClick = NULL_ENTITY;
            lastClickTime = -1.0;
        }
    }

    // an on-screen control goes: what it holds and the input it wrote go with it
    if (componentId == onScreenStickId ||
        componentId == onScreenButtonId ||
        componentId == touchAreaId ||
        componentId == transformId) {
        for (auto& pair : pointers) {
            if (pair.second.control == entity)
                pair.second.control = NULL_ENTITY;
            if (pair.second.enteredButton == entity)
                pair.second.enteredButton = NULL_ENTITY;
        }
        clearControlSources(entity);
        controlOwners.erase(entity);
    }

    if (componentId == buttonId) {
        ButtonComponent& button = scene->getComponent<ButtonComponent>(entity);
        if (button.label != NULL_ENTITY){
            if (isEntityChild(button.label, entity)){
                scene->destroyEntity(button.label);
            }
            button.label = NULL_ENTITY;
        }
        destroyButton(button);
    } else if (componentId == panelId) {
        PanelComponent& panel = scene->getComponent<PanelComponent>(entity);
        if (panel.headertext != NULL_ENTITY){
            if (isEntityChild(panel.headertext, entity)){
                scene->destroyEntity(panel.headertext);
            }
            panel.headertext = NULL_ENTITY;
        }
        if (panel.headercontainer != NULL_ENTITY){
            if (isEntityChild(panel.headercontainer, entity)){
                scene->destroyEntity(panel.headercontainer);
            }
            panel.headercontainer = NULL_ENTITY;
        }
        if (panel.headerimage != NULL_ENTITY){
            if (isEntityChild(panel.headerimage, entity)){
                scene->destroyEntity(panel.headerimage);
            }
            panel.headerimage = NULL_ENTITY;
        }
    } else if (componentId == scrollbarId) {
        ScrollbarComponent& scrollbar = scene->getComponent<ScrollbarComponent>(entity);
        if (scrollbar.bar != NULL_ENTITY){
            if (isEntityChild(scrollbar.bar, entity)){
                scene->destroyEntity(scrollbar.bar);
            }
            scrollbar.bar = NULL_ENTITY;
        }
    } else if (componentId == progressbarId) {
        ProgressbarComponent& progressbar = scene->getComponent<ProgressbarComponent>(entity);
        if (progressbar.fill != NULL_ENTITY){
            if (isEntityChild(progressbar.fill, entity)){
                scene->destroyEntity(progressbar.fill);
            }
            progressbar.fill = NULL_ENTITY;
        }
    } else if (componentId == inputFieldId) {
        InputFieldComponent& inputfield = scene->getComponent<InputFieldComponent>(entity);
        if (inputfield.text != NULL_ENTITY){
            if (isEntityChild(inputfield.text, entity)){
                scene->destroyEntity(inputfield.text);
            }
            inputfield.text = NULL_ENTITY;
        }
        if (inputfield.selection != NULL_ENTITY){
            if (isEntityChild(inputfield.selection, entity)){
                scene->destroyEntity(inputfield.selection);
            }
            inputfield.selection = NULL_ENTITY;
        }
        if (inputfield.cursor != NULL_ENTITY){
            if (isEntityChild(inputfield.cursor, entity)){
                scene->destroyEntity(inputfield.cursor);
            }
            inputfield.cursor = NULL_ENTITY;
        }
    } else if (componentId == textId) {
        TextComponent& text = scene->getComponent<TextComponent>(entity);
        destroyText(text);
    }
}

// ---------------------------------------------------------------------------------------
// Canvas Groups (ui canvas group spec 7.2, 7.4)
// ---------------------------------------------------------------------------------------

static constexpr uint8_t CANVAS_GROUP_MAX_LAYER_DEPTH = 4;

static bool canvasGroupChanged(const CanvasGroupComponent& group){
    const CanvasGroupComponent::Seen& seen = group.seen;
    return !seen.valid || seen.enabled != group.enabled || seen.alpha != group.alpha ||
           seen.interactable != group.interactable || seen.blocks != group.blocksRaycasts ||
           seen.ignore != group.ignoreParentGroups || seen.renderMode != group.renderMode;
}

static void markCanvasGroupSeen(CanvasGroupComponent& group){
    group.seen.valid = true;
    group.seen.enabled = group.enabled;
    group.seen.alpha = group.alpha;
    group.seen.interactable = group.interactable;
    group.seen.blocks = group.blocksRaycasts;
    group.seen.ignore = group.ignoreParentGroups;
    group.seen.renderMode = group.renderMode;
}

bool UISystem::sanitizeCanvasGroup(Entity entity, CanvasGroupComponent& group){
    if (std::isnan(group.alpha)){
        // rejected: the last value seen stays
        group.alpha = group.seen.valid ? group.seen.alpha : 1.0f;
        if (!group.warnedInvalidAlpha){
            group.warnedInvalidAlpha = true;
            Log::warn("Canvas Group of '%s': alpha NaN rejected, kept %g", scene->getEntityName(entity).c_str(), group.alpha);
        }
        return true;
    }
    if (group.alpha < 0.0f){
        group.alpha = 0.0f;
        return true;
    }
    if (group.alpha > 1.0f){
        group.alpha = 1.0f;
        return true;
    }
    return false;
}

UISystem::CanvasGroupEntry UISystem::combineCanvasGroup(const CanvasGroupEntry& parent, Entity entity, Signature signature, CanvasGroupComponent* group, bool warn){
    CanvasGroupEntry entry = parent;
    entry.underRect = parent.underRect || signature.test(scene->getComponentId<RectTransformComponent>());

    if (!group || !group->enabled){
        return entry;
    }

    const bool ignore = group->ignoreParentGroups;
    entry.alpha = group->alpha * (ignore ? 1.0f : parent.alpha);
    // the chain's alpha up to this group, relative to the enclosing layer
    const float chainAlpha = group->alpha * (ignore ? 1.0f : parent.layerAlpha);
    entry.layerAlpha = chainAlpha;
    entry.interactable = group->interactable && (ignore || parent.interactable);
    entry.blocks = group->blocksRaycasts && (ignore || parent.blocks);
    entry.group = entity;

    group->layerAlpha = chainAlpha;
    bool layered = false;
    if (group->renderMode == CanvasGroupRenderMode::FLATTEN && chainAlpha > 0.0f && chainAlpha < 1.0f && group->subtreeDraws >= 2){
        if (parent.layerDepth < CANVAS_GROUP_MAX_LAYER_DEPTH){
            layered = true;
        }else if (warn){
            Log::warn("Canvas Group '%s': more than %i nested Flatten layers, drawn Per Element",
                      scene->getEntityName(entity).c_str(), (int)CANVAS_GROUP_MAX_LAYER_DEPTH);
        }
    }
    group->layered = layered;
    if (layered){
        // the layer starts here: the group's own element and its subtree draw into it at
        // their alpha relative to it, and the layer is composited at layerAlpha
        entry.layerAlpha = 1.0f;
        entry.layerDepth = parent.layerDepth + 1;
    }

    return entry;
}

void UISystem::applyCanvasGroupEntry(Entity entity, Signature signature, const CanvasGroupEntry& entry){
    if (signature.test(scene->getComponentId<UIComponent>())){
        UIComponent& ui = scene->getComponent<UIComponent>(entity);
        ui.groupAlpha = entry.alpha;
        ui.groupLayerAlpha = entry.layerAlpha;
        ui.groupBlocksRaycasts = entry.blocks;
        ui.groupEntity = entry.group;
        if (ui.groupInteractable != entry.interactable){
            ui.groupInteractable = entry.interactable;
            // the visual changes now; the widgets drop what they hold in the next update
            if (signature.test(scene->getComponentId<ButtonComponent>())){
                applyButtonVisual(scene->getComponent<ButtonComponent>(entity), ui);
            }
            if (signature.test(scene->getComponentId<InputFieldComponent>())){
                scene->getComponent<InputFieldComponent>(entity).needUpdateInputField = true;
            }
            if (!entry.interactable){
                interactabilityLost.push_back(entity);
            }
        }
    }
    if (signature.test(scene->getComponentId<PointsComponent>())){
        PointsComponent& points = scene->getComponent<PointsComponent>(entity);
        const float alpha = entry.underRect ? entry.alpha : 1.0f;
        if (points.uiGroupAlpha != alpha){
            points.uiGroupAlpha = alpha;
            points.needUpdate = true;
            points.needUpdateGroupAlpha = true;
        }
    }
}

void UISystem::canvasGroupFullPass(){
    auto transforms = scene->getComponentArray<Transform>();
    const size_t count = transforms->size();
    const ComponentId groupId = scene->getComponentId<CanvasGroupComponent>();
    const ComponentId uiId = scene->getComponentId<UIComponent>();

    canvasGroupStats.fullPasses++;
    canvasGroupStats.visited += count;

    // parents by array index (a parent always comes before its children); siblings follow
    // each other, so the last parent found answers most lookups
    canvasGroupParents.assign(count, SIZE_MAX);
    Entity lastParent = NULL_ENTITY;
    size_t lastParentIndex = SIZE_MAX;
    for (size_t i = 0; i < count; i++){
        const Entity parent = transforms->getComponentFromIndex(i).parent;
        if (parent == NULL_ENTITY)
            continue;
        if (parent != lastParent){
            lastParent = parent;
            lastParentIndex = transforms->hasEntity(parent) ? transforms->getIndex(parent) : SIZE_MAX;
        }
        if (lastParentIndex < i){
            canvasGroupParents[i] = lastParentIndex;
        }
    }

    // UI elements per branch, children first (the FLATTEN leaf rule; UI particles draw in the
    // colour pass, not in a layer): only needed when a group can make a layer
    bool anyFlatten = false;
    auto groups = scene->getComponentArray<CanvasGroupComponent>();
    for (size_t i = 0; i < groups->size() && !anyFlatten; i++){
        anyFlatten = groups->getComponentFromIndex(i).renderMode == CanvasGroupRenderMode::FLATTEN;
    }
    if (anyFlatten){
        std::vector<uint32_t> draws(count, 0);
        for (size_t i = count; i-- > 0;){
            const Entity entity = transforms->getEntity(i);
            const Signature signature = scene->getSignature(entity);
            if (signature.test(uiId)){
                draws[i]++;
            }
            if (signature.test(groupId)){
                scene->getComponent<CanvasGroupComponent>(entity).subtreeDraws = draws[i];
            }
            if (canvasGroupParents[i] != SIZE_MAX){
                draws[canvasGroupParents[i]] += draws[i];
            }
        }
    }

    canvasGroupScratch.assign(count, CanvasGroupEntry());
    for (size_t i = 0; i < count; i++){
        const Entity entity = transforms->getEntity(i);
        const Signature signature = scene->getSignature(entity);
        const CanvasGroupEntry parent = canvasGroupParents[i] != SIZE_MAX ? canvasGroupScratch[canvasGroupParents[i]] : CanvasGroupEntry();
        CanvasGroupComponent* group = signature.test(groupId) ? &scene->getComponent<CanvasGroupComponent>(entity) : nullptr;
        const bool warn = group && canvasGroupChanged(*group);
        canvasGroupScratch[i] = combineCanvasGroup(parent, entity, signature, group, warn);
        applyCanvasGroupEntry(entity, signature, canvasGroupScratch[i]);
    }

    for (size_t i = 0; i < groups->size(); i++){
        markCanvasGroupSeen(groups->getComponentFromIndex(i));
    }
    canvasGroupsApplied = true;
}

void UISystem::canvasGroupSubtreePass(Entity groupEntity){
    auto transforms = scene->getComponentArray<Transform>();
    if (!transforms->hasEntity(groupEntity)){
        return;
    }
    const ComponentId groupId = scene->getComponentId<CanvasGroupComponent>();

    // the state the group inherits: its ancestors folded from the root down (O(depth))
    std::vector<Entity> ancestors;
    Entity parentEntity = transforms->getComponent(groupEntity).parent;
    while (parentEntity != NULL_ENTITY && transforms->hasEntity(parentEntity) && ancestors.size() < 4096){
        ancestors.push_back(parentEntity);
        parentEntity = transforms->getComponent(parentEntity).parent;
    }
    CanvasGroupEntry inherited;
    for (size_t a = ancestors.size(); a-- > 0;){
        const Entity ancestor = ancestors[a];
        const Signature signature = scene->getSignature(ancestor);
        CanvasGroupComponent* group = signature.test(groupId) ? &scene->getComponent<CanvasGroupComponent>(ancestor) : nullptr;
        inherited = combineCanvasGroup(inherited, ancestor, signature, group, false);
    }

    const size_t first = transforms->getIndex(groupEntity);
    const size_t last = scene->findBranchLastIndex(groupEntity);

    canvasGroupStats.subtreePasses++;
    canvasGroupStats.visited += last - first + 1;

    canvasGroupScratch.assign(last - first + 1, CanvasGroupEntry());
    for (size_t i = first; i <= last; i++){
        const Entity entity = transforms->getEntity(i);
        const Signature signature = scene->getSignature(entity);
        CanvasGroupEntry parent = inherited;
        if (i > first){
            const Entity parentOfEntity = transforms->getComponentFromIndex(i).parent;
            const size_t parentIndex = transforms->getIndex(parentOfEntity);
            if (parentIndex >= first && parentIndex < i){
                parent = canvasGroupScratch[parentIndex - first];
            }
        }
        CanvasGroupComponent* group = signature.test(groupId) ? &scene->getComponent<CanvasGroupComponent>(entity) : nullptr;
        const bool warn = group && canvasGroupChanged(*group);
        canvasGroupScratch[i - first] = combineCanvasGroup(parent, entity, signature, group, warn);
        applyCanvasGroupEntry(entity, signature, canvasGroupScratch[i - first]);
        if (group){
            markCanvasGroupSeen(*group);
        }
    }
    canvasGroupsApplied = true;
}

void UISystem::canvasGroupResetPass(){
    canvasGroupStats.fullPasses++;
    const CanvasGroupEntry none;
    auto uis = scene->getComponentArray<UIComponent>();
    for (size_t i = 0; i < uis->size(); i++){
        const Entity entity = uis->getEntity(i);
        applyCanvasGroupEntry(entity, scene->getSignature(entity), none);
    }
    auto points = scene->getComponentArray<PointsComponent>();
    for (size_t i = 0; i < points->size(); i++){
        const Entity entity = points->getEntity(i);
        applyCanvasGroupEntry(entity, scene->getSignature(entity), none);
    }
    canvasGroupStats.visited += uis->size() + points->size();
    canvasGroupsApplied = false;
}

void UISystem::syncCanvasGroups(){
    if (scene->hierarchyRevision() != canvasGroupsRevision){
        canvasGroupsRevision = scene->hierarchyRevision();
        canvasGroupsStructuralDirty = true;
    }

    auto groups = scene->getComponentArray<CanvasGroupComponent>();
    if (groups->size() == 0){
        // with no group nothing runs; the last one removed resets the elements once
        if (canvasGroupsApplied){
            canvasGroupResetPass();
        }
        canvasGroupsStructuralDirty = false;
        return;
    }

    canvasGroupDirty.clear();
    for (size_t i = 0; i < groups->size(); i++){
        CanvasGroupComponent& group = groups->getComponentFromIndex(i);
        sanitizeCanvasGroup(groups->getEntity(i), group);
        if (canvasGroupChanged(group)){
            canvasGroupDirty.push_back(groups->getEntity(i));
            // a render mode change counts the subtree's UI elements again (the FLATTEN leaf
            // rule; the full pass counts them only while some group is FLATTEN)
            if (group.seen.valid && group.seen.renderMode != group.renderMode){
                canvasGroupsStructuralDirty = true;
            }
        }
    }

    if (canvasGroupsStructuralDirty){
        canvasGroupsStructuralDirty = false;
        canvasGroupFullPass();
        return;
    }

    if (canvasGroupDirty.empty()){
        return;
    }

    // outermost first: a dirty group inside a walked branch was covered by that walk
    auto transforms = scene->getComponentArray<Transform>();
    std::vector<std::pair<size_t, Entity>> ordered;
    ordered.reserve(canvasGroupDirty.size());
    for (Entity entity : canvasGroupDirty){
        if (transforms->hasEntity(entity)){
            ordered.push_back({transforms->getIndex(entity), entity});
        }else{
            // a group without a Transform is in no hierarchy
            markCanvasGroupSeen(scene->getComponent<CanvasGroupComponent>(entity));
        }
    }
    std::sort(ordered.begin(), ordered.end());
    size_t coveredUntil = 0;
    bool anyWalk = false;
    for (const auto& item : ordered){
        if (anyWalk && item.first <= coveredUntil){
            continue;
        }
        CanvasGroupComponent* group = scene->findComponent<CanvasGroupComponent>(item.second);
        if (!group || !canvasGroupChanged(*group)){
            continue;
        }
        canvasGroupSubtreePass(item.second);
        coveredUntil = scene->findBranchLastIndex(item.second);
        anyWalk = true;
    }
}

CanvasGroupState UISystem::getCanvasGroupState(Entity entity) const{
    CanvasGroupState state;
    if (entity == NULL_ENTITY || !scene->isEntityCreated(entity)){
        return state;
    }
    Entity current = entity;
    int guard = 0;
    while (current != NULL_ENTITY && guard++ < 4096){
        const CanvasGroupComponent* group = scene->findComponent<CanvasGroupComponent>(current);
        if (group && group->enabled){
            const float alpha = std::isnan(group->alpha) ? 1.0f : std::clamp(group->alpha, 0.0f, 1.0f);
            state.alpha *= alpha;
            state.interactable = state.interactable && group->interactable;
            state.blocksRaycasts = state.blocksRaycasts && group->blocksRaycasts;
            if (state.group == NULL_ENTITY){
                state.group = current;
            }
            if (group->ignoreParentGroups){
                break;
            }
        }
        const Transform* transform = scene->findComponent<Transform>(current);
        current = transform ? transform->parent : NULL_ENTITY;
    }
    return state;
}

bool UISystem::isInteractable(Entity entity) const{
    if (entity == NULL_ENTITY || !scene->isEntityCreated(entity)){
        return false;
    }
    if (const UIComponent* ui = scene->findComponent<UIComponent>(entity)){
        if (!ui->groupInteractable){
            return false;
        }
    }
    if (const ButtonComponent* button = scene->findComponent<ButtonComponent>(entity)){
        if (button->disabled){
            return false;
        }
    }
    if (const InputFieldComponent* inputfield = scene->findComponent<InputFieldComponent>(entity)){
        if (inputfield->disabled){
            return false;
        }
    }
    return true;
}

const CanvasGroupStats& UISystem::getCanvasGroupStats() const{
    return canvasGroupStats;
}

void UISystem::resetCanvasGroupStats(){
    canvasGroupStats = CanvasGroupStats();
}

// Ui canvas group spec 7.2: interactability lost while a widget held a press, the focus or
// a gesture: a D6 cancel for every pointer pressing the widget (ui-multitouch), the focus goes
void UISystem::processInteractabilityLost(){
    if (interactabilityLost.empty()){
        return;
    }
    std::vector<Entity> entities;
    entities.swap(interactabilityLost);

    for (Entity entity : entities){
        if (!isLiveUIEntity(entity)){
            continue;
        }
        UIComponent* ui = scene->findComponent<UIComponent>(entity);
        if (!ui || ui->groupInteractable){
            continue;
        }

        // the pointers pressing it: cancelled (no click; the Button releases, the gestures end)
        std::vector<int> pressing;
        for (const auto& pair : pointers){
            const UIPointerState& state = pair.second;
            if (state.down && (state.widget == entity || state.buttonPressed == entity || state.panelGesture == entity ||
                               state.scrollbar == entity || state.inputFieldSelecting == entity || state.control == entity)){
                pressing.push_back(pair.first);
            }
        }
        for (int pointerId : pressing){
            endPress(pointerId, false);
        }
        if (!isLiveUIEntity(entity)){
            continue;
        }
        ui = scene->findComponent<UIComponent>(entity);

        if (InputFieldComponent* inputfield = scene->findComponent<InputFieldComponent>(entity)){
            if (ui->focused){
                ui->focused = false;
                inputfield->needUpdateInputField = true;
                System::instance().hideVirtualKeyboard();
                ui->onLostFocus.call();
                if (!isLiveUIEntity(entity)){
                    continue;
                }
            }
        }
    }
}

void UISystem::startCanvasGroupFade(Entity group, Entity tween, float endAlpha, bool deactivate, std::function<void()> onComplete){
    // the previous helper fade of the group stops (no fighting tweens)
    for (size_t i = 0; i < canvasGroupFades.size(); i++){
        if (canvasGroupFades[i].group == group){
            CanvasGroupFade previous = std::move(canvasGroupFades[i]);
            canvasGroupFades.erase(canvasGroupFades.begin() + i);
            finishCanvasGroupFade(std::move(previous), false);
            break;
        }
    }

    CanvasGroupFade fade;
    fade.group = group;
    fade.tween = tween;
    fade.endAlpha = endAlpha;
    fade.deactivate = deactivate;
    fade.onComplete = std::move(onComplete);
    canvasGroupFades.push_back(std::move(fade));

    if (ActionComponent* action = scene->findComponent<ActionComponent>(tween)){
        Scene* fadeScene = scene;
        action->onStop.add("canvasGroupFade", std::function<void()>([fadeScene, tween](){
            const TweenComponent* timed = fadeScene->findComponent<TweenComponent>(tween);
            // onStop runs before the tween is rewound: a finished tween is at its end
            const bool atEnd = timed && timed->time >= 1.0f;
            fadeScene->getSystem<UISystem>()->onCanvasGroupFadeStopped(tween, atEnd);
        }));
    }
}

void UISystem::onCanvasGroupFadeStopped(Entity tween, bool atEnd){
    for (CanvasGroupFade& fade : canvasGroupFades){
        if (fade.tween == tween && !fade.finished){
            fade.finished = true;
            fade.completed = atEnd;
        }
    }
}

void UISystem::finishCanvasGroupFade(CanvasGroupFade fade, bool runCompletion){
    if (fade.tween != NULL_ENTITY && scene->isEntityCreated(fade.tween)){
        if (ActionComponent* action = scene->findComponent<ActionComponent>(fade.tween)){
            action->onStop.remove("canvasGroupFade");
        }
        scene->destroyEntity(fade.tween);
    }
    if (!runCompletion){
        return;
    }
    // the final value was written by the tween (or stopCanvasGroupFade): then onComplete,
    // then the deactivation
    if (fade.onComplete){
        fade.onComplete();
    }
    if (fade.deactivate && fade.group != NULL_ENTITY && scene->isEntityCreated(fade.group)){
        scene->setEntityActive(fade.group, false);
    }
}

void UISystem::stopCanvasGroupFade(Entity group, bool jumpToEnd){
    for (size_t i = 0; i < canvasGroupFades.size(); i++){
        if (canvasGroupFades[i].group == group){
            CanvasGroupFade fade = std::move(canvasGroupFades[i]);
            canvasGroupFades.erase(canvasGroupFades.begin() + i);
            if (jumpToEnd){
                if (CanvasGroupComponent* component = scene->findComponent<CanvasGroupComponent>(group)){
                    component->alpha = fade.endAlpha;
                }
            }
            finishCanvasGroupFade(std::move(fade), jumpToEnd);
            return;
        }
    }
}

bool UISystem::isCanvasGroupFading(Entity group) const{
    for (const CanvasGroupFade& fade : canvasGroupFades){
        if (fade.group == group && !fade.finished){
            return true;
        }
    }
    return false;
}

void UISystem::processCanvasGroupFades(){
    for (size_t i = 0; i < canvasGroupFades.size();){
        if (!canvasGroupFades[i].finished){
            i++;
            continue;
        }
        CanvasGroupFade fade = std::move(canvasGroupFades[i]);
        canvasGroupFades.erase(canvasGroupFades.begin() + i);
        const bool completed = fade.completed;
        // (a callback may start another fade: the list is re-read from the same index)
        finishCanvasGroupFade(std::move(fade), completed);
    }
}

Entity UISystem::raycast(Vector2 point){
    syncCanvasGroups();
    return raycastHit(point);
}

void UISystem::onComponentAdded(Entity entity, ComponentId componentId){
    (void)entity;
    if (componentId == scene->getComponentId<CanvasGroupComponent>() ||
        componentId == scene->getComponentId<UIComponent>() ||
        componentId == scene->getComponentId<PointsComponent>() ||
        componentId == scene->getComponentId<RectTransformComponent>() ||
        componentId == scene->getComponentId<Transform>()){
        canvasGroupsStructuralDirty = true;
    }
}
