// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LINE_RENDERER_COMPONENT_H
#define LINE_RENDERER_COMPONENT_H

#include "buffer/Buffer.h"
#include "buffer/ExternalBuffer.h"
#include "math/Curve.h"
#include "render/LineRibbonBuilder.h"
#include "render/ObjectRender.h"
#include "render/ShaderRender.h"
#include "shader/ShaderUniforms.h"
#include "texture/Material.h"
#include "Engine.h"
#include <string>
#include <memory>

namespace jasperengine{

    struct LineData{
        Vector3 pointA = Vector3(0,0,0);
        Vector4 colorA = Vector4(1,1,1,1);

        Vector3 pointB = Vector3(0,0,0);
        Vector4 colorB = Vector4(1,1,1,1);

        bool operator==(const LineData& other) const {
            return pointA == other.pointA &&
                colorA == other.colorA &&
                pointB == other.pointB &&
                colorB == other.colorB;
        }

        bool operator!=(const LineData& other) const {
            return !(*this == other);
        }
    };

    // which data a Line Renderer draws: the independent A-B segments of `lines` (debug lines,
    // per-end colours) or Unity's polyline `positions`
    enum class LineRendererMode : int { Segments = 0, Polyline = 1 };

    // the base texture uniform of the built-in line shader (and of library lines shaders that
    // sample one): LineRenderer::setTexture is a shortcut for shaderTextures[it]
    constexpr const char* LINE_BASE_TEXTURE_UNIFORM = "u_baseTexture";

    // The ribbon (thick line) of a Line Renderer or a Trail Renderer: what the render system
    // builds (LineRibbonBuilder) and uploads. Runtime only, never serialized or copied by value
    // into another object's GPU state
    struct LineRibbonData{
        std::vector<LineRibbonVertex> vertices;
        std::vector<uint32_t> indices;
        // the indices as uint16 while the vertex capacity fits them
        std::vector<uint16_t> indices16;
        ExternalBuffer indexBuffer;
        // capacity of the GPU buffers (powers of two); growing past it reloads
        unsigned int maxVertices = 0;
        unsigned int maxIndices = 0;
        bool index32 = false;
        unsigned int indexCount = 0;
        // centre line length and bounds (of the points, in their space), largest half width
        float length = 0.0f;
        AABB bounds;
        float maxHalfWidth = 0.0f;
        // the frame the buffer shrank or was sized last (shrinks after 5 s below a quarter)
        float lowUseTime = 0.0f;
        // whether the shader drawing it has a manifest (the per-frame variant check; looked up
        // again when the shader changes and at each load)
        std::string manifestShader;
        bool hasManifest = false;
        bool manifestKnown = false;
    };

    struct JASPERENGINE_API LineRendererComponent{
        // component checkbox: its system skips a disabled component
        bool enabled = true;

        bool loaded = false;
        bool loadCalled = false;

        ExternalBuffer buffer;

        // Segments mode: independent A-B segments (per-end colours)
        std::vector<LineData> lines;

        unsigned int maxLines = 100;

        // Unity's LineRenderer (lines and trails spec 6.2). Defaults keep old scenes as they
        // were: no width is the 1-pixel hairline of `lines`
        LineRendererMode mode = LineRendererMode::Segments;
        // Polyline mode: the points (model space, world space with useWorldSpace)
        std::vector<Vector3> positions;
        // closes the polyline (last point joined back to the first)
        bool loop = false;
        // the points are world positions (the transform is ignored); Unity's default is true,
        // the editor menu and create_entity make new lines with it
        bool useWorldSpace = false;
        // the width is widthMultiplier x widthCurve over the normalized length; 0 = the
        // hairline (today's 1-pixel LINES primitive)
        float widthMultiplier = 0.0f;
        Curve widthCurve = Curve::constant(1.0f);
        LineWidthUnit widthUnit = LineWidthUnit::World;
        // Polyline mode: over the normalized length (Segments mode uses the segment colours)
        Gradient colorGradient = Gradient::twoColors(Vector3(1.0f, 1.0f, 1.0f), Vector3(1.0f, 1.0f, 1.0f));
        // 0 = miter joint (bevel past 4x the half width), else a round joint (0..90)
        unsigned int numCornerVertices = 0;
        // 0 = flat butt caps, else round caps (0..90)
        unsigned int numCapVertices = 0;
        LineAlignment alignment = LineAlignment::View;
        LineTextureMode textureMode = LineTextureMode::Stretch;
        // u scale (Tile: repeats per world unit), v scale
        Vector2 textureScale = Vector2(1.0f, 1.0f);
        // transparent queue order (Unity's sorting order), lower first
        int sortPriority = 0;
        // Sorting Layer (a SortingLayers id) and Order in Layer (spec S52 R2): the 2D draw order
        // and the transparent queue's first keys; 0 and 0 draw as before
        int32_t sortingLayer = 0;
        int32_t sortingOrder = 0;   // Order in Layer, -32768..32767

        // runtime: the variant loaded (ribbon or hairline), the ribbon, the hairline segments
        // built from a width-0 polyline, invalid points warned about once
        bool loadedRibbon = false;
        uint64_t loadedRibbonKey = 0;
        // the loaded shader's @state priority= (added to sortPriority in the transparent queue)
        int queuePriority = 0;
        // the loaded ribbon blends (transparent queue) rather than drawing opaque in place
        bool transparent = false;
        bool needUpdateRibbon = true;
        LineRibbonData ribbon;
        std::vector<LineData> polylineSegments;
        bool warnedInvalidPoints = false;

        ObjectRender render;
        std::shared_ptr<ShaderRender> shader;
        uint64_t shaderProperties = 0;
        // optional custom shader: "library:<name>" or a project-relative fork base path
        // (no extension), empty = built-in. customShaderId is the resolved id for ShaderPool keys.
        ShaderRef customShader;
        uint16_t customShaderId = 0;
        int slotVSParams = -1;
        // values of the fork's custom uniform blocks, by member name
        ShaderUniformValues shaderUniforms;
        bool needUpdateShaderUniforms = false;
        // custom-block members that follow a global shader value (Shader.setGlobal*): member
        // -> global name, over shaderUniforms and the manifest default; globalsVersionSeen is
        // ShaderGlobals::getVersion() when the blocks were last written with them
        ShaderGlobalBindings shaderGlobalBindings;
        uint64_t globalsVersionSeen = 0;
        CustomUniformBlock customVSParams;
        CustomUniformBlock customFSParams;
        // shader library: textures by @texture uniform name and enabled @feature/@keywords
        // defines of the shader above (library reference or fork)
        std::vector<std::pair<std::string, Texture>> shaderTextures;
        std::vector<std::string> shaderFeatures;
        // AUTO keeps the built-in state (alpha blend, depth write on; a ribbon writes no depth)
        MaterialBlendMode blendMode = MaterialBlendMode::AUTO;
        MaterialDepthWrite depthWrite = MaterialDepthWrite::AUTO;
        // phase 2: stencil test/write and colour channels (AUTO = manifest, else none / all)
        MaterialStencil stencil;
        int colorWrite = -1;
        // resolved at load: the shader's manifest (null without one) and what it reads of
        // the frame block and custom textures
        std::shared_ptr<const ShaderManifest> shaderManifest;
        ShaderLibraryBindings libraryBindings;

        bool needUpdateBuffer = false;
        bool needReload = false;
    };
    
}

#endif //LINE_RENDERER_COMPONENT_H