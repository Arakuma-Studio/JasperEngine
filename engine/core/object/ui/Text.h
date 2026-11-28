// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TEXT_H
#define TEXT_H

#include "RectTransform.h"
#include "texture/Material.h"
#include "localization/LocalizationArgs.h"
#include "util/TextAlignment.h"

namespace jasperengine{
    class JASPERENGINE_API STBText;
    // component/TextComponent.h
    enum class TextAnchorHorizontal;
    enum class TextAnchorVertical;
    enum class TextFontSizeUnit;

    class JASPERENGINE_API Text: public RectTransform{

    public:
        Text(Scene* scene);
        Text(Scene* scene, Entity entity);
        virtual ~Text();

        Text(const Text& rhs);
        Text& operator=(const Text& rhs);
        Text(Text&& rhs) noexcept;
        Text& operator=(Text&& rhs) noexcept;

        bool createText();
        bool load();

        // the glyphs laid out now when the text, font or size changed since the last layout (S94
        // F25): width and height are final right after setText. False without a font to load.
        bool updateLayout();
        // the text's box, laid out first when it is dirty (RectTransform's are the stored size)
        unsigned int getWidth() const;
        unsigned int getHeight() const;

        void setFixedSize(bool fixedSize);

        void setFixedWidth(bool fixedWidth);
        bool isFixedWidth() const;

        void setFixedHeight(bool fixedHeight);
        bool isFixedHeight() const;

        void setMaxTextSize(unsigned int maxTextSize);
        unsigned int getMaxTextSize() const;

        void setText(const std::string& text);
        std::string getText() const;

        // Localization Key (game-ui-localization.md R16): the text follows the key's message in
        // the game language; the arguments are runtime only (not saved). "" keeps the text as is
        void setLocalized(const std::string& key, const LocalizationArgs& args = {});
        void setLocalizationKey(const std::string& key);
        std::string getLocalizationKey() const;

        // index 0 is the main font, the next ones are fallbacks for what it misses
        void setFont(const std::string& font);
        void setFont(unsigned int index, const std::string& font);
        std::string getFont() const;
        std::string getFont(unsigned int index) const;

        void setFontSize(unsigned int fontSize);
        unsigned int getFontSize() const;

        void setMultiline(bool multiline);
        bool getMultiline() const;

        void setColor(Vector4 color);
        void setColor(const float red, const float green, const float blue, const float alpha);
        void setColor(const float red, const float green, const float blue);
        void setAlpha(const float alpha);
        Vector4 getColor() const;
        float getAlpha() const;

        float getAscent() const;
        float getDescent() const;
        float getLineGap() const;
        int getLineHeight() const;
        unsigned int getNumChars() const;
        Vector2 getCharPosition(unsigned int index) const;
        float getCharWidth(uint32_t codepoint) const;

        void setFlipY(bool flipY);
        bool isFlipY() const;

        // Unity TextMesh's Anchor (S94 F24): the point of the text's box on the origin
        void setAnchorHorizontal(TextAnchorHorizontal alignment);
        TextAnchorHorizontal getAnchorHorizontal() const;
        void setAnchorVertical(TextAnchorVertical alignment);
        TextAnchorVertical getAnchorVertical() const;
        // the font size as ascender to descender (the default) or as the em square (S94 F26)
        void setFontSizeUnit(TextFontSizeUnit unit);
        TextFontSizeUnit getFontSizeUnit() const;

        // deprecated: the anchor before S94 (Baseline and Center)
        void setPivotBaseline(bool pivotBaseline);
        bool isPivotBaseline() const;

        void setPivotCentered(bool pivotCentered);
        bool isPivotCentered() const;

        // S93 R37, TextMeshPro's Alignment: each line inside the fixed width, the lines inside
        // the fixed height (without a fixed size the text's own box)
        void setHorizontalAlignment(TextHorizontalAlignment alignment);
        TextHorizontalAlignment getHorizontalAlignment() const;
        void setVerticalAlignment(TextVerticalAlignment alignment);
        TextVerticalAlignment getVerticalAlignment() const;
        // TMP Auto Size: the largest whole font size from fontSizeMin to fontSizeMax whose text
        // fits the fixed width and height (a vector font; fontSize is not used meanwhile)
        void setAutoSize(bool autoSize);
        bool isAutoSize() const;
        void setFontSizeMin(unsigned int size);
        unsigned int getFontSizeMin() const;
        void setFontSizeMax(unsigned int size);
        unsigned int getFontSizeMax() const;

        // Outline and shadow (TMP outline and underlay), in UI units; colours sRGB. Either
        // one switches the text to signed-distance glyphs.
        void setOutlineWidth(float outlineWidth);
        float getOutlineWidth() const;
        void setOutlineColor(Vector4 color);
        Vector4 getOutlineColor() const;
        void setShadowEnabled(bool shadowEnabled);
        bool isShadowEnabled() const;
        void setShadowColor(Vector4 color);
        Vector4 getShadowColor() const;
        void setShadowOffset(Vector2 offset);
        Vector2 getShadowOffset() const;
        void setShadowSoftness(float softness);
        float getShadowSoftness() const;
        void setShadowDilate(float dilate);
        float getShadowDilate() const;

        // Bitmap fonts (S51): the pixel outline's neighbours (4 or 8); the pixel scale k the
        // text draws at (0 for a vector font) and the font size that is (k x native size)
        void setOutlineNeighbors(int neighbors);
        int getOutlineNeighbors() const;
        int getPixelScale() const;
        unsigned int getEffectiveFontSize() const;
        // TMP maxVisibleCharacters: characters past it are not drawn (-1 all)
        void setVisibleCharacters(int visibleCharacters);
        int getVisibleCharacters() const;

        AABB getAABB() const;
        AABB getWorldAABB() const;

        void setCustomShader(const std::string& path);
        std::string getCustomShader() const;

        void setShaderUniform(const std::string& name, const Vector4& value);
        void setShaderUniform(const std::string& name, const Vector3& value);
        void setShaderUniform(const std::string& name, const Vector2& value);
        void setShaderUniform(const std::string& name, float value);
        Vector4 getShaderUniform(const std::string& name) const;
        bool removeShaderUniform(const std::string& name);

        // shader library: a @property value (set with setShaderUniform), else the manifest
        // default; the @texture uniforms by path ("" removes one: its default binds); the
        // enabled @feature and chosen @keywords defines
        Vector4 getShaderParam(const std::string& name) const;
        // a custom-block member that follows a global shader value (Shader.setGlobal*) instead of
        // its shader uniform; "" unbinds (false when nothing changed)
        bool setShaderGlobalBinding(const std::string& member, const std::string& global);
        std::string getShaderGlobalBinding(const std::string& member) const;
        void setShaderTexture(const std::string& name, const std::string& path);
        std::string getShaderTexture(const std::string& name) const;
        void setShaderFeature(const std::string& define, bool enabled);
        bool isShaderFeatureEnabled(const std::string& define) const;
        // AUTO keeps the built-in state: alpha blending, depth writes on
        void setBlendMode(MaterialBlendMode blendMode);
        MaterialBlendMode getBlendMode() const;
        void setDepthWrite(MaterialDepthWrite depthWrite);
        MaterialDepthWrite getDepthWrite() const;
        // stencil test/write and colour channels (MaterialColorWriteBits, -1 AUTO); AUTO takes
        // the shader's @state, else stencil off and every channel. getStencil gives a copy:
        // set it back after changing a field
        void setStencil(const MaterialStencil& stencil);
        MaterialStencil getStencil() const;
        void setColorWrite(int colorWrite);
        int getColorWrite() const;
    };
}

#endif //TEXT_H
