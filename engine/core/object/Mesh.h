// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MESH_H
#define MESH_H

#include "GameObject.h"
#include "buffer/InterleavedBuffer.h"
#include "buffer/IndexBuffer.h"
#include "texture/Material.h"

namespace jasperengine{

    struct InstanceData;

    class JASPERENGINE_API Mesh: public GameObject{
    public:

        // component checkbox (Unity Behaviour.enabled): a disabled Mesh is skipped by its system
        void setEnabled(bool enabled);
        bool isEnabled() const;
        Mesh(Scene* scene);
        Mesh(Scene* scene, Entity entity);
        virtual ~Mesh();

        Mesh(const Mesh& rhs);
        Mesh& operator=(const Mesh& rhs);
        Mesh(Mesh&& rhs) noexcept;
        Mesh& operator=(Mesh&& rhs) noexcept;

        bool load();

        void setTexture(const std::string& path);
        void setTexture(const std::string& id, TextureData data);
        void setTexture(Framebuffer* framebuffer);

        // the normal map (tangent-space, OpenGL green up) of submesh 0 / of a submesh; "" removes
        // it. On sprites, tilemaps and polygons it answers Light 2Ds whose height is above 0;
        // same size and frame layout as the colour texture (a Tilemap: submesh i is the tileset
        // whose colour texture submesh i holds, one normal map per tileset texture, same size)
        void setNormalTexture(const std::string& path);
        void setNormalTexture(unsigned int submesh, const std::string& path);
        void setNormalTexture(const std::string& id, TextureData data);
        // submesh 0's path, "" when none (the id for a normal map set from data)
        std::string getNormalTexture() const;
        std::string getNormalTexture(unsigned int submesh) const;
        
        void setColor(Vector4 color);
        void setColor(const float red, const float green, const float blue, const float alpha);
        void setColor(const float red, const float green, const float blue);
        void setAlpha(const float alpha);
        Vector4 getColor() const;
        float getAlpha() const;

        void setMaterial(const Material& material);
        Material getMaterial() const;

        void setMaterial(unsigned int submesh, const Material& material);
        Material getMaterial(unsigned int submesh) const;

        void setPrimitiveType(PrimitiveType primitiveType);
        PrimitiveType getPrimitiveType() const;

        void setPrimitiveType(unsigned int submesh, PrimitiveType primitiveType);
        PrimitiveType getPrimitiveType(unsigned int submesh) const;

        void setFaceCulling(bool faceCulling);
        bool isFaceCulling() const;

        void setFaceCulling(unsigned int submesh, bool faceCulling);
        bool isFaceCulling(unsigned int submesh) const;

        void setCastShadowsWithTexture(bool castShadowsWithTexture);
        bool isCastShadowsWithTexture() const;

        void setCastShadowsWithTexture(unsigned int submesh, bool castShadowsWithTexture);
        bool isCastShadowsWithTexture(unsigned int submesh) const;

        void setCullingMode(CullingMode cullingMode);
        CullingMode getCullingMode() const;

        void setWindingOrder(WindingOrder windingOrder);
        WindingOrder getWindingOrder() const;

        AABB getAABB() const;
        AABB getVerticesAABB() const;
        AABB getWorldAABB() const;

        unsigned int getNumSubmeshes() const;

        void setReceiveLights(bool receiveLights);
        bool isReceiveLights() const;

        void setReceiveIBL(bool receiveIBL);
        bool isReceiveIBL() const;

        void setCastShadows(bool castShadows);
        bool isCastShadows() const;
        // cached in the static shadow layer: redrawn only when it changes (docs/rendering/shadows.md)
        void setStaticShadowCaster(bool staticShadowCaster);
        bool isStaticShadowCaster() const;

        void setReceiveShadows(bool receiveShadows);
        bool isReceiveShadows() const;

        void setLodEnabled(bool lodEnabled);
        bool isLodEnabled() const;

        void setLodBias(float lodBias);
        float getLodBias() const;

        // spec S54 R9: Fade Mode, Animate Cross-fading, Fade Transition Width (0..1) and Cull
        // Below Screen Height (share of the view, 0..1; 0 never culls)
        void setLodFadeMode(LodFadeMode mode);
        LodFadeMode getLodFadeMode() const;
        void setLodAnimateCrossFading(bool animate);
        bool isLodAnimateCrossFading() const;
        void setLodFadeTransitionWidth(float width);
        float getLodFadeTransitionWidth() const;
        void setLodCullBelow(float share);
        float getLodCullBelow() const;
        // draws this level in every view (-1: automatic again); the level the main view drew
        // last (LOD_CULLED: culled for its size)
        void forceLod(int level);
        int getForcedLod() const;
        int getCurrentLod() const;

        void setRenderInReflectionProbes(bool renderInReflectionProbes);
        bool isRenderInReflectionProbes() const;

        void setShadowsBillboard(bool shadowsBillboard);
        bool isShadowsBillboard() const;

        void setTransparent(bool transparent);
        bool isTransparent() const;

        void setAutoTransparency(bool autoTransparency);
        bool isAutoTransparency() const;

        void setCustomShader(const std::string& path);
        std::string getCustomShader() const;

        void setCustomDepthShader(const std::string& path);
        std::string getCustomDepthShader() const;

        void setShaderUniform(const std::string& name, const Vector4& value);
        void setShaderUniform(const std::string& name, const Vector3& value);
        void setShaderUniform(const std::string& name, const Vector2& value);
        void setShaderUniform(const std::string& name, float value);
        Vector4 getShaderUniform(const std::string& name) const;
        bool removeShaderUniform(const std::string& name);

        void setAsMirror();
        void setAsMirror(Vector3 normal);
        void removeMirror();
        bool isMirror() const;

        void createInstancedMesh();
        void removeInstancedMesh();
        bool hasInstancedMesh() const;

        void setInstancedBillboard(bool billboard, bool cylindrical);

        void setInstancedBillboard(bool billboard);
        bool isInstancedBillboard() const;

        void setInstancedCylindricalBillboard(bool cylindricalBillboard);
        bool isInstancedCylindricalBillboard() const;

        void setMaxInstances(unsigned int maxInstances);
        unsigned int getMaxInstances() const;

        // instances shrink to nothing between fadeStart and fadeEnd (model-space distances)
        void setDistanceFade(bool distanceFade);
        bool isDistanceFade() const;

        void setFadeRange(float fadeStart, float fadeEnd);
        void setFadeStart(float fadeStart);
        float getFadeStart() const;
        void setFadeEnd(float fadeEnd);
        float getFadeEnd() const;

        void setCullInstances(bool cullInstances);
        bool isCullInstances() const;
        void setCullDistance(float distance);
        float getCullDistance() const;

        void addInstance(InstanceData instance);
        void addInstance(Vector3 position);
        void addInstance(float x, float y, float z);
        void addInstance(Vector3 position, Quaternion rotation, Vector3 scale);
        void addInstance(Vector3 position, Quaternion rotation, Vector3 scale, Vector4 color);
        void addInstance(Vector3 position, Quaternion rotation, Vector3 scale, Vector4 color, Rect textureRect);

        InstanceData& getInstance(size_t index);

        // writes everything but vat0/vat1 (the Vertex Animator's state of the instance)
        void updateInstance(size_t index, InstanceData instance);
        void updateInstance(size_t index, Vector3 position);
        void updateInstance(size_t index, float x, float y, float z);
        void updateInstance(size_t index, Vector3 position, Quaternion rotation, Vector3 scale);
        void updateInstance(size_t index, Vector3 position, Quaternion rotation, Vector3 scale, Vector4 color);
        void updateInstance(size_t index, Vector3 position, Quaternion rotation, Vector3 scale, Vector4 color, Rect textureRect);

        void removeInstance(size_t index);

        bool isInstanceVisible(size_t index);
        void setInstanceVisible(size_t index, bool visible) const;

        void updateInstances();
        size_t getNumInstances();

        void clearInstances();
    };
}

#endif //MESH_H
