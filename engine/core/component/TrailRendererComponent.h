// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TRAIL_RENDERER_COMPONENT_H
#define TRAIL_RENDERER_COMPONENT_H

// Unity's TrailRenderer (lines and trails spec 6.4): a fading ribbon behind a moving
// GameObject. It lives in world space and draws from its own loop (RenderSystem), so it
// coexists with the Mesh, Sprite or other renderer of its GameObject. TrailSystem samples the
// transform once per frame (scaled game time); the render system builds and draws the ribbon
// like a thick Line Renderer's.

#include "asset/AssetRef.h"
#include "component/LineRendererComponent.h"
#include "math/Vector3.h"
#include <string>
#include <memory>
#include <utility>
#include <vector>

namespace jasperengine{

    // a stored trail point: where the object was and when (TrailSystem's clock, seconds)
    struct TrailPoint{
        Vector3 position;
        float birthTime = 0.0f;
    };

    // The stored points of a trail, oldest first: a ring whose capacity grows by powers of
    // two, so a moving trail neither allocates nor shifts per frame. Runtime only
    class JASPERENGINE_API TrailPointRing{
    private:
        std::vector<TrailPoint> ring;
        size_t first = 0;
        size_t count = 0;

    public:
        size_t size() const { return count; }
        bool empty() const { return count == 0; }
        size_t capacity() const { return ring.size(); }
        // 0 is the oldest
        const TrailPoint& at(size_t index) const { return ring[(first + index) % ring.size()]; }
        TrailPoint& at(size_t index) { return ring[(first + index) % ring.size()]; }
        const TrailPoint& newest() const { return at(count - 1); }

        void push(const TrailPoint& point);
        void popOldest();
        void clear();
        // releases the storage
        void reset();
    };

    struct JASPERENGINE_API TrailRendererComponent{
        // component checkbox: a disabled trail is cleared and draws nothing
        bool enabled = true;

        // seconds a point lives (Unity time)
        float time = 5.0f;
        // a point is added when the object moved this far from the newest one
        float minVertexDistance = 0.1f;
        // width = widthMultiplier x widthCurve over the trail (0 = the head, 1 = the tail)
        float widthMultiplier = 1.0f;
        Curve widthCurve = Curve::constant(1.0f);
        LineWidthUnit widthUnit = LineWidthUnit::World;
        // over the trail by age: 0 = the head, 1 = the oldest point
        Gradient colorGradient = Gradient::twoColors(Vector3(1.0f, 1.0f, 1.0f), Vector3(1.0f, 1.0f, 1.0f));
        unsigned int numCornerVertices = 0;
        unsigned int numCapVertices = 0;
        LineAlignment alignment = LineAlignment::View;
        LineTextureMode textureMode = LineTextureMode::Stretch;
        Vector2 textureScale = Vector2(1.0f, 1.0f);
        // new points are added while on (the stored ones keep fading either way)
        bool emitting = true;
        // the GameObject is destroyed once it has had no points and not moved for `time`
        bool autodestruct = false;
        // transparent queue order (Unity's sorting order), lower first
        int sortPriority = 0;
        // Sorting Layer (a SortingLayers id) and Order in Layer (spec S52 R2): the 2D draw order
        // and the transparent queue's first keys; 0 and 0 draw as before
        int32_t sortingLayer = 0;
        int32_t sortingOrder = 0;   // Order in Layer, -32768..32767

        // shader: as LineRendererComponent (the built-in line shader's ribbon variant, a
        // library lines shader or a fork); AUTO blend is alpha, AUTO depth write off
        ShaderRef customShader;
        uint16_t customShaderId = 0;
        ShaderUniformValues shaderUniforms;
        bool needUpdateShaderUniforms = false;
        CustomUniformBlock customVSParams;
        CustomUniformBlock customFSParams;
        std::vector<std::pair<std::string, Texture>> shaderTextures;
        std::vector<std::string> shaderFeatures;
        MaterialBlendMode blendMode = MaterialBlendMode::AUTO;
        MaterialDepthWrite depthWrite = MaterialDepthWrite::AUTO;
        MaterialStencil stencil;
        int colorWrite = -1;
        // custom-block members that follow a global shader value (Shader.setGlobal*): member
        // -> global name, over shaderUniforms and the manifest default; globalsVersionSeen is
        // ShaderGlobals::getVersion() when the blocks were last written with them
        ShaderGlobalBindings shaderGlobalBindings;
        uint64_t globalsVersionSeen = 0;

        // runtime: the points and the clock state (TrailSystem), the GPU objects and the ribbon
        TrailPointRing points;
        // where the head was last frame, the seconds it has had no points and not moved
        Vector3 lastHeadPosition;
        bool hasLastHeadPosition = false;
        float idleTime = 0.0f;
        bool warnedCapacity = false;
        // autodestruct asked GameObject.destroy (it runs at the end of the frame)
        bool destroyRequested = false;
        bool needUpdateRibbon = true;

        // forgets every point and where the head was (Unity Clear(); TrailSimulation::clear)
        void clearPoints(){
            points.clear();
            hasLastHeadPosition = false;
            idleTime = 0.0f;
            needUpdateRibbon = true;
        }

        bool loaded = false;
        bool loadCalled = false;
        bool needReload = false;
        bool needUpdateBuffer = false;
        uint64_t loadedRibbonKey = 0;
        // the loaded shader's @state priority= (added to sortPriority in the transparent queue)
        int queuePriority = 0;
        // the loaded ribbon blends (transparent queue) rather than drawing opaque in place
        bool transparent = true;
        ExternalBuffer buffer;
        LineRibbonData ribbon;
        ObjectRender render;
        std::shared_ptr<ShaderRender> shader;
        uint64_t shaderProperties = 0;
        int slotVSParams = -1;
        std::shared_ptr<const ShaderManifest> shaderManifest;
        ShaderLibraryBindings libraryBindings;
        // the world bounds of the drawn ribbon (with its width), null while it draws nothing
        AABB worldAABB;
    };

}

#endif //TRAIL_RENDERER_COMPONENT_H
