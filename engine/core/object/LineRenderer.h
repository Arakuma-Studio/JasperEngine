// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LINE_RENDERER_H
#define LINE_RENDERER_H

#include "GameObject.h"
#include "component/LineRendererComponent.h"
#include <vector>

namespace jasperengine{

    class JASPERENGINE_API LineRenderer: public GameObject{
    public:
        // the GameObject's own position stays reachable next to the points' (Unity's
        // transform.position vs SetPosition(index, position))
        using GameObject::setPosition;
        using GameObject::getPosition;


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

        // Unity's LineRenderer (lines and trails spec 6.6). The polyline API (positionCount,
        // setPosition(s)) switches the mode to Polyline, the segment API above (addLine,
        // updateLine, removeLine, clearLines) to Segments; both lists are kept, the mode's
        // draws. Values are clamped silently (widths >= 0, vertex counts 0..90)
        void setMode(LineRendererMode mode);
        LineRendererMode getMode() const;
        // growing fills with the last point (or zero)
        void setPositionCount(int count);
        int getPositionCount() const;
        // 0-based like Unity; false (nothing changed) out of range
        bool setPosition(int index, Vector3 position);
        Vector3 getPosition(int index) const;
        void setPositions(const std::vector<Vector3>& positions);
        std::vector<Vector3> getPositions() const;
        void setLoop(bool loop);
        bool getLoop() const;
        void setUseWorldSpace(bool useWorldSpace);
        bool getUseWorldSpace() const;
        // 0 = the 1-pixel hairline
        void setWidthMultiplier(float widthMultiplier);
        float getWidthMultiplier() const;
        // Unity shortcuts: the width at the start / end (multiplier x curve); setting one makes
        // the curve linear between them with the multiplier 1
        void setStartWidth(float width);
        float getStartWidth() const;
        void setEndWidth(float width);
        float getEndWidth() const;
        void setWidthCurve(const Curve& curve);
        Curve getWidthCurve() const;
        void setWidthUnit(LineWidthUnit unit);
        LineWidthUnit getWidthUnit() const;
        void setColorGradient(const Gradient& gradient);
        Gradient getColorGradient() const;
        // Unity shortcuts on the gradient's ends (sRGB colour, alpha)
        void setStartColor(Vector4 color);
        Vector4 getStartColor() const;
        void setEndColor(Vector4 color);
        Vector4 getEndColor() const;
        void setNumCornerVertices(int count);
        int getNumCornerVertices() const;
        void setNumCapVertices(int count);
        int getNumCapVertices() const;
        void setAlignment(LineAlignment alignment);
        LineAlignment getAlignment() const;
        void setTextureMode(LineTextureMode mode);
        LineTextureMode getTextureMode() const;
        void setTextureScale(Vector2 scale);
        Vector2 getTextureScale() const;
        // the base texture: a shortcut for setShaderTexture("u_baseTexture", path)
        void setTexture(const std::string& path);
        std::string getTexture() const;
        void setSortPriority(int priority);
        int getSortPriority() const;

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

#endif //LINE_RENDERER_H