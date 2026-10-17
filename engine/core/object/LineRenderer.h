// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LINE_RENDERER_H
#define LINE_RENDERER_H

#include "GameObject.h"

namespace jasperengine{

    class JASPERENGINE_API LineRenderer: public GameObject{
    public:

        // component checkbox (Unity Behaviour.enabled): a disabled Lines is skipped by its system
        void setEnabled(bool enabled);
        bool isEnabled() const;
        LineRenderer(Scene* scene);
        LineRenderer(Scene* scene, Entity entity);
        virtual ~LineRenderer();

        LineRenderer(const LineRenderer& rhs);
        LineRenderer& operator=(const LineRenderer& rhs);
        LineRenderer(LineRenderer&& rhs) noexcept;
        LineRenderer& operator=(LineRenderer&& rhs) noexcept;

        bool load();

        void setMaxLines(unsigned int maxLines);
        unsigned int getMaxLines() const;

        void addLine(LineData line);
        void addLine(Vector3 pointA, Vector3 pointB);
        void addLine(Vector3 pointA, Vector3 pointB, Vector3 color);
        void addLine(Vector3 pointA, Vector3 pointB, Vector4 color);
        void addLine(Vector3 pointA, Vector3 pointB, Vector4 colorA, Vector4 colorB);

        LineData& getLine(size_t index);

        void updateLine(size_t index, LineData line);
        void updateLine(size_t index, Vector3 pointA, Vector3 pointB);
        void updateLine(size_t index, Vector3 pointA, Vector3 pointB, Vector3 color);
        void updateLine(size_t index, Vector3 pointA, Vector3 pointB, Vector4 color);
        void updateLine(size_t index, Vector3 pointA, Vector3 pointB, Vector4 colorA, Vector4 colorB);
        void updateLine(size_t index, Vector3 color);
        void updateLine(size_t index, Vector4 color);
        void updateLine(size_t index, Vector4 colorA, Vector4 colorB);

        void removeLine(size_t index);

        void updateLines();
        size_t getNumLines();

        void clearLines();

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

#endif //LINE_RENDERER_H