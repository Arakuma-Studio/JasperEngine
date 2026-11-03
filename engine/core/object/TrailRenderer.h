// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Trail Renderer object (Lua `TrailRenderer`, `object:getTrailRenderer()`): Unity's
// TrailRenderer on a GameObject (lines and trails spec 6.4, 6.6). TrailRenderer(scene) makes a
// new GameObject with one; TrailRenderer(scene, entity) adds the component when the entity has
// none (Lua `object:getTrailRenderer()` returns nil instead). Main thread. Positions are the
// stored points, oldest first (index 0, Unity's order); the live head (where the object is)
// is not one of them. Values are clamped silently (time, widths, minVertexDistance >= 0,
// vertex counts 0..90).

#ifndef TRAIL_RENDERER_H
#define TRAIL_RENDERER_H

#include "GameObject.h"
#include "component/TrailRendererComponent.h"
#include <string>
#include <vector>

namespace jasperengine{

    class JASPERENGINE_API TrailRenderer: public GameObject{
    public:
        // the GameObject's own position stays reachable next to the points' (Unity's
        // transform.position vs SetPosition(index, position))
        using GameObject::setPosition;
        using GameObject::getPosition;

        // a new GameObject with a Trail Renderer
        TrailRenderer(Scene* scene);
        // the Trail Renderer of an entity that has one (a handle: it adds nothing)
        TrailRenderer(Scene* scene, Entity entity);
        // Unity's AddComponent<TrailRenderer>(): the entity's Trail Renderer, added when it has none
        static TrailRenderer addTo(Scene* scene, Entity entity);
        virtual ~TrailRenderer();

        TrailRenderer(const TrailRenderer& rhs);
        TrailRenderer& operator=(const TrailRenderer& rhs);
        TrailRenderer(TrailRenderer&& rhs) noexcept;
        TrailRenderer& operator=(TrailRenderer&& rhs) noexcept;

        // component checkbox: a disabled trail is cleared
        void setEnabled(bool enabled);
        bool isEnabled() const;

        void setTime(float time);
        float getTime() const;
        void setMinVertexDistance(float distance);
        float getMinVertexDistance() const;
        void setEmitting(bool emitting);
        bool isEmitting() const;
        void setAutodestruct(bool autodestruct);
        bool isAutodestruct() const;

        // removes every point (Unity Clear()): call it after teleporting the object
        void clear();
        // stored points, oldest first
        int getPositionCount() const;
        Vector3 getPosition(int index) const;
        // false (nothing changed) out of range
        bool setPosition(int index, Vector3 position);
        std::vector<Vector3> getPositions() const;
        // adds a point now (the newest), as if the object had been there
        void addPosition(Vector3 position);
        void addPositions(const std::vector<Vector3>& positions);

        void setWidthMultiplier(float widthMultiplier);
        float getWidthMultiplier() const;
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

        // shader: as LineRenderer's
        void setCustomShader(const std::string& path);
        std::string getCustomShader() const;
        void setShaderUniform(const std::string& name, const Vector4& value);
        void setShaderUniform(const std::string& name, const Vector3& value);
        void setShaderUniform(const std::string& name, const Vector2& value);
        void setShaderUniform(const std::string& name, float value);
        Vector4 getShaderUniform(const std::string& name) const;
        bool removeShaderUniform(const std::string& name);
        Vector4 getShaderParam(const std::string& name) const;
        // a custom-block member that follows a global shader value (Shader.setGlobal*) instead of
        // its shader uniform; "" unbinds (false when nothing changed)
        bool setShaderGlobalBinding(const std::string& member, const std::string& global);
        std::string getShaderGlobalBinding(const std::string& member) const;
        void setShaderTexture(const std::string& name, const std::string& path);
        std::string getShaderTexture(const std::string& name) const;
        void setShaderFeature(const std::string& define, bool enabled);
        bool isShaderFeatureEnabled(const std::string& define) const;
        // AUTO: alpha blending, no depth writes
        void setBlendMode(MaterialBlendMode blendMode);
        MaterialBlendMode getBlendMode() const;
        void setDepthWrite(MaterialDepthWrite depthWrite);
        MaterialDepthWrite getDepthWrite() const;
        void setStencil(const MaterialStencil& stencil);
        MaterialStencil getStencil() const;
        void setColorWrite(int colorWrite);
        int getColorWrite() const;
    };
}

#endif //TRAIL_RENDERER_H
