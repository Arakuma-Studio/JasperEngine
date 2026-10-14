// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SKYBOX_H
#define SKYBOX_H

#include "object/EntityHandle.h"

namespace jasperengine{
    class JASPERENGINE_API Skybox: public EntityHandle{

    public:
        Skybox(Scene* scene);
        Skybox(Scene* scene, Entity entity);
        virtual ~Skybox();

        bool load();

        void setTextures(const std::string& id,
                        TextureData texturePositiveX, TextureData textureNegativeX,
                        TextureData texturePositiveY, TextureData textureNegativeY,
                        TextureData texturePositiveZ, TextureData textureNegativeZ);

        void setTextures(const std::string& texturePositiveX, const std::string& textureNegativeX,
                        const std::string& texturePositiveY, const std::string& textureNegativeY,
                        const std::string& texturePositiveZ, const std::string& textureNegativeZ);

        void setTexture(const std::string& texture);

        void setTexturePositiveX(const std::string& texture);
        void setTextureNegativeX(const std::string& texture);
        void setTexturePositiveY(const std::string& texture);
        void setTextureNegativeY(const std::string& texture);
        void setTexturePositiveZ(const std::string& texture);
        void setTextureNegativeZ(const std::string& texture);

        void setColor(Vector4 color);
        void setColor(const float r, const float g, const float b);
        void setColor(const float r, const float g, const float b, const float a);
        void setAlpha(const float alpha);
        Vector4 getColor() const;
        float getAlpha() const;

        void setRotation(float angle);
        float getRotation() const;

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
        void setShaderTexture(const std::string& name, const std::string& path);
        std::string getShaderTexture(const std::string& name) const;
        void setShaderFeature(const std::string& define, bool enabled);
        bool isShaderFeatureEnabled(const std::string& define) const;
    };
}

#endif //SKYBOX_H