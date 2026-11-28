// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Text.h"
#include "localization/Localization.h"
#include "component/TextComponent.h"
#include "shader/ShaderGlobals.h"

#include <algorithm>
#include <cmath>
#include "shader/ShaderManifest.h"

#include "Log.h"
#include "util/Color.h"
#include "util/STBText.h"
#include "pixelart/BitmapFont.h"
#include "pixelart/BitmapText.h"
#include "pixelart/BitmapFontPool.h"
#include "pixelart/FontPixelReport.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/UISystem.h"

using namespace jasperengine;

Text::Text(Scene* scene): RectTransform(scene){
    addComponent<UIComponent>();
    addComponent<TextComponent>();
}

Text::Text(Scene* scene, Entity entity): RectTransform(scene, entity){
}

Text::~Text(){
}

Text::Text(const Text& rhs) = default;
Text& Text::operator=(const Text& rhs) = default;
Text::Text(Text&& rhs) noexcept = default;
Text& Text::operator=(Text&& rhs) noexcept = default;

bool Text::createText(){
    TextComponent& textcomp = getComponent<TextComponent>();
    UIComponent& ui = getComponent<UIComponent>();
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    return scene->getSystem<UISystem>()->createOrUpdateText(textcomp, ui, layout);
}

bool Text::updateLayout(){
    TextComponent& textcomp = getComponent<TextComponent>();
    UIComponent& ui = getComponent<UIComponent>();
    RectTransformComponent& layout = getComponent<RectTransformComponent>();

    return scene->getSystem<UISystem>()->updateTextLayout(textcomp, ui, layout);
}

unsigned int Text::getWidth() const{
    const_cast<Text*>(this)->updateLayout();
    return RectTransform::getWidth();
}

unsigned int Text::getHeight() const{
    const_cast<Text*>(this)->updateLayout();
    return RectTransform::getHeight();
}

bool Text::load(){
    UIComponent& ui = getComponent<UIComponent>();

    auto renderSystem = scene->getSystem<RenderSystem>();
    return renderSystem->loadUI(entity, ui, renderSystem->getScenePipelines(), true);
}

void Text::setLocalized(const std::string& key, const LocalizationArgs& args){
    TextComponent& textcomp = getComponent<TextComponent>();
    textcomp.localizationKey = key;
    textcomp.textArgs = args;
    // taken again at the next update, now that key or arguments changed
    textcomp.localizationGeneration = Localization::getGeneration() + 1;
    markChanged<TextComponent>();
}

void Text::setLocalizationKey(const std::string& key){
    setLocalized(key, getComponent<TextComponent>().textArgs);
}

std::string Text::getLocalizationKey() const{
    return getComponent<TextComponent>().localizationKey;
}

void Text::setFixedSize(bool fixedSize){
    TextComponent& textcomp = getComponent<TextComponent>();

    if (textcomp.fixedWidth != fixedSize || textcomp.fixedHeight != fixedSize){
        textcomp.fixedWidth = fixedSize;
        textcomp.fixedHeight = fixedSize;

        textcomp.needUpdateText = true;
        markChanged<TextComponent>();
    }
}

void Text::setFixedWidth(bool fixedWidth){
    TextComponent& textcomp = getComponent<TextComponent>();

    if (textcomp.fixedWidth != fixedWidth){
        textcomp.fixedWidth = fixedWidth;

        textcomp.needUpdateText = true;
        markChanged<TextComponent>();
    }
}

bool Text::isFixedWidth() const{
    TextComponent& textcomp = getComponent<TextComponent>();

    return textcomp.fixedWidth;
}

void Text::setFixedHeight(bool fixedHeight){
    TextComponent& textcomp = getComponent<TextComponent>();

    if (textcomp.fixedHeight != fixedHeight){
        textcomp.fixedHeight = fixedHeight;

        textcomp.needUpdateText = true;
        markChanged<TextComponent>();
    }
}

bool Text::isFixedHeight() const{
    TextComponent& textcomp = getComponent<TextComponent>();

    return textcomp.fixedHeight;
}

void Text::setMaxTextSize(unsigned int maxTextSize){
    TextComponent& textcomp = getComponent<TextComponent>();
    UIComponent& ui = getComponent<UIComponent>();

    if (textcomp.maxTextSize != maxTextSize){
        textcomp.maxTextSize = maxTextSize;

        textcomp.needReloadAtlas = true;
        textcomp.needUpdateText = true;
        markChanged<TextComponent>();
        ui.needReload = true;
    }
}

unsigned int Text::getMaxTextSize() const{
    TextComponent& textcomp = getComponent<TextComponent>();

    return textcomp.maxTextSize;
}

void Text::setText(const std::string &text){
    TextComponent& textcomp = getComponent<TextComponent>();

    if (textcomp.text != text){
        textcomp.text = text;
        textcomp.needUpdateText = true;
        markChanged<TextComponent>();
    }
}

std::string Text::getText() const{
    TextComponent& textcomp = getComponent<TextComponent>();

    return textcomp.text;
}

void Text::setFont(const std::string &font){
    setFont(0, font);
}

void Text::setFont(unsigned int index, const std::string &font){
    TextComponent& textcomp = getComponent<TextComponent>();

    if (index >= textcomp.font.size()){
        Log::error("Cannot use invalid font index: %u", index);
        return;
    }

    if (textcomp.font[index] != font){
        textcomp.font[index] = font;

        textcomp.needReloadAtlas = true;
        textcomp.needUpdateText = true;
        markChanged<TextComponent>();
    }
}

std::string Text::getFont() const{
    return getFont(0);
}

std::string Text::getFont(unsigned int index) const{
    TextComponent& textcomp = getComponent<TextComponent>();

    if (index >= textcomp.font.size()){
        Log::error("Cannot use invalid font index: %u", index);
        return "";
    }

    return textcomp.font[index];
}


void Text::setFontSize(unsigned int fontSize){
    TextComponent& textcomp = getComponent<TextComponent>();

    // the atlas is rebuilt for a new size only; scripts set it every frame
    if (textcomp.fontSize != fontSize){
        textcomp.fontSize = fontSize;

        textcomp.needReloadAtlas = true;
        textcomp.needUpdateText = true;
        markChanged<TextComponent>();
    }
}

unsigned int Text::getFontSize() const{
    TextComponent& textcomp = getComponent<TextComponent>();

    return textcomp.fontSize;
}

void Text::setMultiline(bool multiline){
    TextComponent& textcomp = getComponent<TextComponent>();

    textcomp.multiline = multiline;
    textcomp.needUpdateText = true;
    markChanged<TextComponent>();
}

bool Text::getMultiline() const{
    TextComponent& textcomp = getComponent<TextComponent>();

    return textcomp.multiline;
}

void Text::setColor(Vector4 color){
    UIComponent& ui = getComponent<UIComponent>();

    ui.color = Color::sRGBToLinear(color);
}

void Text::setColor(const float red, const float green, const float blue, const float alpha){
    setColor(Vector4(red, green, blue, alpha));
}

void Text::setColor(const float red, const float green, const float blue){
    setColor(Vector4(red, green, blue, getColor().w));
}

void Text::setAlpha(const float alpha){
    Vector4 color = getColor();
    setColor(Vector4(color.x, color.y, color.z, alpha));
}

Vector4 Text::getColor() const{
    UIComponent& ui = getComponent<UIComponent>();

    return Color::linearTosRGB(ui.color);
}

float Text::getAlpha() const{
    return getColor().w;
}

float Text::getAscent() const{
    TextComponent& textcomp = getComponent<TextComponent>();

    if (textcomp.bitmapText)
        return textcomp.bitmapText->getAscent(textcomp.pixelUnit);
    if (!textcomp.stbtext)
        return 0;
    else
        return textcomp.stbtext->getAscent();
}

float Text::getDescent() const{
    TextComponent& textcomp = getComponent<TextComponent>();

    // a bitmap font's descent: what its line holds below the baseline
    if (textcomp.bitmapText)
        return -(float)(textcomp.bitmapText->getFont()->getLineHeight() - textcomp.bitmapText->getFont()->getBaseline()) * textcomp.pixelUnit;
    if (!textcomp.stbtext)
        return 0;
    else
        return textcomp.stbtext->getDescent();
}

float Text::getLineGap() const{
    TextComponent& textcomp = getComponent<TextComponent>();

    if (textcomp.bitmapText)
        return 0;
    if (!textcomp.stbtext)
        return 0;
    else
        return textcomp.stbtext->getLineGap();
}

int Text::getLineHeight() const{
    TextComponent& textcomp = getComponent<TextComponent>();

    if (textcomp.bitmapText)
        return (int)std::lround((float)textcomp.bitmapText->getFont()->getLineHeight() * textcomp.pixelUnit);
    if (!textcomp.stbtext)
        return 0;
    else
        return textcomp.stbtext->getLineHeight();
}

unsigned int Text::getNumChars() const{
    TextComponent& textcomp = getComponent<TextComponent>();

    return textcomp.charPositions.size();
}

Vector2 Text::getCharPosition(unsigned int index) const{
    TextComponent& textcomp = getComponent<TextComponent>();

    if (index < textcomp.charPositions.size()){
        return textcomp.charPositions[index];
    }

    throw std::out_of_range("charPositions out of range");
}

float Text::getCharWidth(uint32_t codepoint) const{
    TextComponent& textcomp = getComponent<TextComponent>();

    if (textcomp.bitmapText)
        return (float)textcomp.bitmapText->getFont()->advanceOf(codepoint) * textcomp.pixelUnit;
    if (!textcomp.stbtext)
        return 0;
    else
        return textcomp.stbtext->getCharWidth(codepoint);
}

void Text::setFlipY(bool flipY){
    UIComponent& ui = getComponent<UIComponent>();
    TextComponent& textcomp = getComponent<TextComponent>();

    ui.automaticFlipY = false;
    if (ui.flipY != flipY){
        ui.flipY = flipY;

        textcomp.needUpdateText = true;
        markChanged<TextComponent>();
    }
}

bool Text::isFlipY() const{
    UIComponent& ui = getComponent<UIComponent>();

    return ui.flipY;
}

void Text::setAnchorHorizontal(TextAnchorHorizontal alignment){
    TextComponent& textcomp = getComponent<TextComponent>();

    if (alignment != TextAnchorHorizontal::LEFT && alignment != TextAnchorHorizontal::CENTER && alignment != TextAnchorHorizontal::RIGHT){
        Log::error("Text horizontal anchor must be TextAnchor.Left, Center or Right (got %d)", (int)alignment);
        return;
    }
    if (textcomp.anchorHorizontal != alignment){
        textcomp.anchorHorizontal = alignment;
        textcomp.needUpdateText = true;
        markChanged<TextComponent>();
    }
}

TextAnchorHorizontal Text::getAnchorHorizontal() const{
    TextComponent& textcomp = getComponent<TextComponent>();

    return textcomp.anchorHorizontal;
}

void Text::setAnchorVertical(TextAnchorVertical alignment){
    TextComponent& textcomp = getComponent<TextComponent>();

    if ((int)alignment < (int)TextAnchorVertical::TOP || (int)alignment > (int)TextAnchorVertical::MIDLINE){
        Log::error("Text vertical anchor must be TextAnchor.Top, Middle, Bottom, Baseline, Capline or Midline (got %d)", (int)alignment);
        return;
    }
    if (textcomp.anchorVertical != alignment){
        textcomp.anchorVertical = alignment;
        textcomp.needUpdateText = true;
        markChanged<TextComponent>();
    }
}

TextAnchorVertical Text::getAnchorVertical() const{
    TextComponent& textcomp = getComponent<TextComponent>();

    return textcomp.anchorVertical;
}

void Text::setFontSizeUnit(TextFontSizeUnit unit){
    TextComponent& textcomp = getComponent<TextComponent>();

    if (unit != TextFontSizeUnit::PIXEL_HEIGHT && unit != TextFontSizeUnit::EM){
        Log::error("Text font size unit must be TextFontSizeUnit.PixelHeight or Em (got %d)", (int)unit);
        return;
    }
    // another scale is another atlas
    if (textcomp.fontSizeUnit != unit){
        textcomp.fontSizeUnit = unit;

        textcomp.needReloadAtlas = true;
        textcomp.needUpdateText = true;
        markChanged<TextComponent>();
    }
}

TextFontSizeUnit Text::getFontSizeUnit() const{
    TextComponent& textcomp = getComponent<TextComponent>();

    return textcomp.fontSizeUnit;
}

// pivotBaseline true was the Baseline alignment; false leaves another alignment as it is
void Text::setPivotBaseline(bool pivotBaseline){
    if (pivotBaseline)
        setAnchorVertical(TextAnchorVertical::BASELINE);
    else if (isPivotBaseline())
        setAnchorVertical(TextAnchorVertical::BOTTOM);
}

bool Text::isPivotBaseline() const{
    return getAnchorVertical() == TextAnchorVertical::BASELINE;
}

// pivotCentered true was the Center alignment; false leaves Right as it is
void Text::setPivotCentered(bool pivotCentered){
    if (pivotCentered)
        setAnchorHorizontal(TextAnchorHorizontal::CENTER);
    else if (isPivotCentered())
        setAnchorHorizontal(TextAnchorHorizontal::LEFT);
}

bool Text::isPivotCentered() const{
    return getAnchorHorizontal() == TextAnchorHorizontal::CENTER;
}

void Text::setHorizontalAlignment(TextHorizontalAlignment alignment){
    TextComponent& textcomp = getComponent<TextComponent>();

    if (textcomp.horizontalAlignment != alignment){
        textcomp.horizontalAlignment = alignment;
        textcomp.needUpdateText = true;
        markChanged<TextComponent>();
    }
}

TextHorizontalAlignment Text::getHorizontalAlignment() const{
    return getComponent<TextComponent>().horizontalAlignment;
}

void Text::setVerticalAlignment(TextVerticalAlignment alignment){
    TextComponent& textcomp = getComponent<TextComponent>();

    if (textcomp.verticalAlignment != alignment){
        textcomp.verticalAlignment = alignment;
        textcomp.needUpdateText = true;
        markChanged<TextComponent>();
    }
}

TextVerticalAlignment Text::getVerticalAlignment() const{
    return getComponent<TextComponent>().verticalAlignment;
}

void Text::setAutoSize(bool autoSize){
    TextComponent& textcomp = getComponent<TextComponent>();

    // (the fit, or leaving it, changes the atlas: the system reloads it then)
    if (textcomp.autoSize != autoSize){
        textcomp.autoSize = autoSize;
        textcomp.needUpdateText = true;
        markChanged<TextComponent>();
    }
}

bool Text::isAutoSize() const{
    return getComponent<TextComponent>().autoSize;
}

void Text::setFontSizeMin(unsigned int size){
    TextComponent& textcomp = getComponent<TextComponent>();

    if (textcomp.fontSizeMin != size){
        textcomp.fontSizeMin = size;
        textcomp.needUpdateText = true;
        markChanged<TextComponent>();
    }
}

unsigned int Text::getFontSizeMin() const{
    return getComponent<TextComponent>().fontSizeMin;
}

void Text::setFontSizeMax(unsigned int size){
    TextComponent& textcomp = getComponent<TextComponent>();

    if (textcomp.fontSizeMax != size){
        textcomp.fontSizeMax = size;
        textcomp.needUpdateText = true;
        markChanged<TextComponent>();
    }
}

unsigned int Text::getFontSizeMax() const{
    return getComponent<TextComponent>().fontSizeMax;
}

AABB Text::getAABB() const{
    UIComponent& ui = getComponent<UIComponent>();

    return ui.aabb;
}

AABB Text::getWorldAABB() const{
    UIComponent& ui = getComponent<UIComponent>();

    return ui.worldAABB;
}

void Text::setCustomShader(const std::string& path){
    UIComponent& ui = getComponent<UIComponent>();

    if (ui.customShader != path){
        ui.customShader = path;

        ui.needReload = true;
    }
}

std::string Text::getCustomShader() const{
    UIComponent& ui = getComponent<UIComponent>();

    return ui.customShader;
}

void Text::setShaderUniform(const std::string& name, const Vector4& value){
    UIComponent& ui = getComponent<UIComponent>();

    if (ShaderUniforms::set(ui.shaderUniforms, name, value)){
        ui.needUpdateShaderUniforms = true;
        markChanged<UIComponent>();
    }
}

void Text::setShaderUniform(const std::string& name, const Vector3& value){
    setShaderUniform(name, Vector4(value.x, value.y, value.z, 0.0f));
}

void Text::setShaderUniform(const std::string& name, const Vector2& value){
    setShaderUniform(name, Vector4(value.x, value.y, 0.0f, 0.0f));
}

void Text::setShaderUniform(const std::string& name, float value){
    setShaderUniform(name, Vector4(value, 0.0f, 0.0f, 0.0f));
}

Vector4 Text::getShaderUniform(const std::string& name) const{
    UIComponent& ui = getComponent<UIComponent>();

    return ShaderUniforms::get(ui.shaderUniforms, name);
}

bool Text::removeShaderUniform(const std::string& name){
    UIComponent& ui = getComponent<UIComponent>();

    if (!ShaderUniforms::remove(ui.shaderUniforms, name))
        return false;

    ui.needUpdateShaderUniforms = true;
    markChanged<UIComponent>();
    return true;
}

Vector4 Text::getShaderParam(const std::string& name) const{
    UIComponent& ui = getComponent<UIComponent>();

    return ShaderLibraryValues::getParam(ui.shaderUniforms, ui.customShader.empty() ? scene->getDefaultCustomShader(ShaderType::UI) : ui.customShader.path, name);
}

bool Text::setShaderGlobalBinding(const std::string& member, const std::string& global){
    UIComponent& ui = getComponent<UIComponent>();

    if (!ShaderGlobals::setBinding(ui.shaderGlobalBindings, member, global))
        return false;
    ui.needUpdateShaderUniforms = true;
    return true;
}

std::string Text::getShaderGlobalBinding(const std::string& member) const{
    const UIComponent& ui = getComponent<UIComponent>();

    return ShaderGlobals::getBinding(ui.shaderGlobalBindings, member);
}

void Text::setShaderTexture(const std::string& name, const std::string& path){
    UIComponent& ui = getComponent<UIComponent>();

    // which textures it has is resolved at load
    if (ShaderLibraryValues::setTexture(ui.shaderTextures, name, path))
        ui.needReload = true;
}

std::string Text::getShaderTexture(const std::string& name) const{
    UIComponent& ui = getComponent<UIComponent>();

    return ShaderLibraryValues::getTexture(ui.shaderTextures, name);
}

void Text::setShaderFeature(const std::string& define, bool enabled){
    UIComponent& ui = getComponent<UIComponent>();

    // features are compiled in: another shader variant
    if (ShaderLibraryValues::setFeature(ui.shaderFeatures, ui.customShader.empty() ? scene->getDefaultCustomShader(ShaderType::UI) : ui.customShader.path, define, enabled))
        ui.needReload = true;
}

bool Text::isShaderFeatureEnabled(const std::string& define) const{
    UIComponent& ui = getComponent<UIComponent>();

    return ShaderLibraryValues::isFeatureEnabled(ui.shaderFeatures, ui.customShader.empty() ? scene->getDefaultCustomShader(ShaderType::UI) : ui.customShader.path, define);
}

void Text::setBlendMode(MaterialBlendMode blendMode){
    UIComponent& ui = getComponent<UIComponent>();

    if (ui.blendMode != blendMode){
        ui.blendMode = blendMode;
        ui.needReload = true;
    }
}

MaterialBlendMode Text::getBlendMode() const{
    UIComponent& ui = getComponent<UIComponent>();

    return ui.blendMode;
}

void Text::setDepthWrite(MaterialDepthWrite depthWrite){
    UIComponent& ui = getComponent<UIComponent>();

    if (ui.depthWrite != depthWrite){
        ui.depthWrite = depthWrite;
        ui.needReload = true;
    }
}

MaterialDepthWrite Text::getDepthWrite() const{
    UIComponent& ui = getComponent<UIComponent>();

    return ui.depthWrite;
}

void Text::setStencil(const MaterialStencil& stencil){
    UIComponent& ui = getComponent<UIComponent>();

    if (ui.stencil != stencil){
        ui.stencil = stencil;
        ui.needReload = true;
    }
}

MaterialStencil Text::getStencil() const{
    UIComponent& ui = getComponent<UIComponent>();

    return ui.stencil;
}

void Text::setColorWrite(int colorWrite){
    UIComponent& ui = getComponent<UIComponent>();

    if (ui.colorWrite != colorWrite){
        ui.colorWrite = colorWrite;
        ui.needReload = true;
    }
}

int Text::getColorWrite() const{
    UIComponent& ui = getComponent<UIComponent>();

    return ui.colorWrite;
}

// colours and offsets reach the shader each frame; a width the atlas padding does not cover
// rebuilds the atlas (UISystem::getTextOptions)
void Text::setOutlineNeighbors(int neighbors){
    TextComponent& textcomp = getComponent<TextComponent>();
    const int value = neighbors == 4 ? 4 : 8;
    if (textcomp.outlineNeighbors != value){
        textcomp.outlineNeighbors = value;
        textcomp.needUpdateText = true;
    }
}

int Text::getOutlineNeighbors() const{
    return getComponent<TextComponent>().outlineNeighbors;
}

int Text::getPixelScale() const{
    const TextComponent& textcomp = getComponent<TextComponent>();
    if (!pixelart::isBitmapFontPath(textcomp.font[0]))
        return 0;
    if (textcomp.bitmapText)
        return pixelart::pixelScaleFor(effectiveFontSize(textcomp), textcomp.bitmapText->getFont()->getNativeSize());
    // not loaded yet: the font's own native size
    std::shared_ptr<pixelart::BitmapFont> font = pixelart::BitmapFontPool::get(textcomp.font[0]);
    return font ? pixelart::pixelScaleFor(textcomp.fontSize, font->getNativeSize()) : 0;
}

unsigned int Text::getEffectiveFontSize() const{
    const TextComponent& textcomp = getComponent<TextComponent>();
    const int k = getPixelScale();
    // a vector font: the size Auto Size fitted, else fontSize (a bitmap font's k holds its fit)
    if (k == 0)
        return effectiveFontSize(textcomp);
    std::shared_ptr<pixelart::BitmapFont> font = textcomp.bitmapText ? textcomp.bitmapText->getFont() : pixelart::BitmapFontPool::get(textcomp.font[0]);
    return font ? (unsigned int)(k * font->getNativeSize()) : textcomp.fontSize;
}

void Text::setOutlineWidth(float outlineWidth){
    TextComponent& textcomp = getComponent<TextComponent>();
    if (textcomp.outlineWidth != outlineWidth){
        textcomp.outlineWidth = std::isfinite(outlineWidth) ? std::max(outlineWidth, 0.0f) : 0.0f;
        textcomp.needUpdateText = true;
    }
}

float Text::getOutlineWidth() const{
    return getComponent<TextComponent>().outlineWidth;
}

void Text::setOutlineColor(Vector4 color){
    getComponent<TextComponent>().outlineColor = Color::sRGBToLinear(color);
}

Vector4 Text::getOutlineColor() const{
    return Color::linearTosRGB(getComponent<TextComponent>().outlineColor);
}

void Text::setShadowEnabled(bool shadowEnabled){
    TextComponent& textcomp = getComponent<TextComponent>();
    if (textcomp.shadowEnabled != shadowEnabled){
        textcomp.shadowEnabled = shadowEnabled;
        textcomp.needUpdateText = true;
    }
}

bool Text::isShadowEnabled() const{
    return getComponent<TextComponent>().shadowEnabled;
}

void Text::setShadowColor(Vector4 color){
    getComponent<TextComponent>().shadowColor = Color::sRGBToLinear(color);
}

Vector4 Text::getShadowColor() const{
    return Color::linearTosRGB(getComponent<TextComponent>().shadowColor);
}

void Text::setShadowOffset(Vector2 offset){
    getComponent<TextComponent>().shadowOffset = offset;
}

Vector2 Text::getShadowOffset() const{
    return getComponent<TextComponent>().shadowOffset;
}

void Text::setShadowSoftness(float softness){
    getComponent<TextComponent>().shadowSoftness = std::isfinite(softness) ? std::max(softness, 0.0f) : 0.0f;
}

float Text::getShadowSoftness() const{
    return getComponent<TextComponent>().shadowSoftness;
}

void Text::setShadowDilate(float dilate){
    getComponent<TextComponent>().shadowDilate = std::isfinite(dilate) ? std::max(dilate, 0.0f) : 0.0f;
}

float Text::getShadowDilate() const{
    return getComponent<TextComponent>().shadowDilate;
}

void Text::setVisibleCharacters(int visibleCharacters){
    TextComponent& textcomp = getComponent<TextComponent>();
    visibleCharacters = std::max(visibleCharacters, -1);
    if (textcomp.visibleCharacters != visibleCharacters){
        textcomp.visibleCharacters = visibleCharacters;
        textcomp.needUpdateText = true;
    }
}

int Text::getVisibleCharacters() const{
    return getComponent<TextComponent>().visibleCharacters;
}
