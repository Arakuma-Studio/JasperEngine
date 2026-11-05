// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef IMAGE_H
#define IMAGE_H

#include "RectTransform.h"
#include "texture/Material.h"

namespace jasperengine{
    class JASPERENGINE_API Image: public RectTransform{
    public:
        Image(Scene* scene);
        Image(Scene* scene, Entity entity);
        virtual ~Image();

        Image(const Image& rhs);
        Image& operator=(const Image& rhs);
        Image(Image&& rhs) noexcept;
        Image& operator=(Image&& rhs) noexcept;

        bool createImage();
        bool load();

        void setPatchMargin(unsigned int margin);
        void setPatchMargin(unsigned int marginLeft, unsigned int marginRight, unsigned int marginTop, unsigned int marginBottom);

        void setPatchMarginBottom(unsigned int marginBottom);
        void setPatchMarginLeft(unsigned int marginLeft);
        void setPatchMarginRight(unsigned int marginRight);
        void setPatchMarginTop(unsigned int marginTop);

        unsigned int getPatchMarginBottom() const;
        unsigned int getPatchMarginLeft() const;
        unsigned int getPatchMarginRight() const;
        unsigned int getPatchMarginTop() const;

        void setTexture(const std::string& path);
        void setTexture(const std::string& id, TextureData data);
        void setTexture(Framebuffer* framebuffer);

        void setTextureScaleFactor(float textureScaleFactor);
        float getTextureScaleFactor() const;

        void setColor(Vector4 color);
        void setColor(const float red, const float green, const float blue, const float alpha);
        void setColor(const float red, const float green, const float blue);
        void setAlpha(const float alpha);
        Vector4 getColor() const;
        float getAlpha() const;

        void setFlipY(bool flipY);
        bool isFlipY() const;

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

#endif //IMAGE_H