// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef Render_h
#define Render_h

#include "Export.h"

#include <cstdint>


namespace jasperengine{

    enum class PrimitiveType{
        TRIANGLES,
        TRIANGLE_STRIP,
        POINTS,
        LINES
    };

    enum class CullingMode{
        BACK,
        FRONT
    };

    enum class WindingOrder{
        CCW,
        CW
    };

    // Colour blending of a pipeline. ALPHA is what every pipeline used before render state
    // was configurable (opaque objects included); NONE disables blending.
    enum class BlendMode{
        NONE,
        ALPHA,          // SRC_ALPHA, ONE_MINUS_SRC_ALPHA
        PREMULTIPLIED,  // ONE, ONE_MINUS_SRC_ALPHA
        ADDITIVE,       // SRC_ALPHA, ONE
        MULTIPLY        // DST_COLOR, ZERO
    };

    enum class DepthCompare{
        LESS,
        LESS_EQUAL,
        EQUAL,
        GREATER_EQUAL,
        GREATER,
        NOT_EQUAL,
        ALWAYS
    };

    enum class StencilCompare{
        NEVER,
        LESS,
        EQUAL,
        LESS_EQUAL,
        GREATER,
        NOT_EQUAL,
        GREATER_EQUAL,
        ALWAYS
    };

    enum class StencilOp{
        KEEP,
        ZERO,
        REPLACE,
        INCR_CLAMP,
        DECR_CLAMP,
        INVERT,
        INCR_WRAP,
        DECR_WRAP
    };

    // colour channels a pipeline writes (the same bits as MaterialColorWriteBits)
    enum ColorWriteBits{
        COLOR_WRITE_R = 1,
        COLOR_WRITE_G = 2,
        COLOR_WRITE_B = 4,
        COLOR_WRITE_A = 8,
        COLOR_WRITE_ALL = 15
    };

    // What ObjectRender::endLoad builds every pipeline of an object with. The defaults are
    // the state objects had before it was configurable. Each pipeline type keeps its own
    // fixed parts (colour mask, offscreen alpha, depth-only passes): blend, depth compare
    // and depth bias apply to the colour pipelines, depth bias also to the z-prepass.
    // Stencil and the colour write mask apply to the colour pipelines only (the swapchain
    // ones never write alpha); the depth-only, shadow and G-buffer pipelines ignore them.
    struct RenderState{
        bool faceCulling = false;
        CullingMode cullingMode = CullingMode::BACK;
        WindingOrder windingOrder = WindingOrder::CCW;
        // the PIP_ZPREPASS pipelines, which must cull like the color pass
        bool prepassFaceCulling = true;
        bool depthWrite = true;
        DepthCompare depthCompare = DepthCompare::LESS_EQUAL; // camera depth test off forces always
        BlendMode blend = BlendMode::ALPHA;
        float depthBiasSlope = 0.0f; // polygon offset factor
        float depthBias = 0.0f;      // polygon offset units
        // stencil test and write, the same for front and back faces (off: no test, no write)
        bool stencilEnabled = false;
        StencilCompare stencilCompare = StencilCompare::ALWAYS;
        uint8_t stencilRef = 0;
        uint8_t stencilReadMask = 0xFF;
        uint8_t stencilWriteMask = 0xFF;
        StencilOp stencilPass = StencilOp::KEEP;
        StencilOp stencilFail = StencilOp::KEEP;
        StencilOp stencilDepthFail = StencilOp::KEEP;
        uint8_t colorWriteMask = COLOR_WRITE_ALL; // ColorWriteBits, 0 = none

        RenderState() = default;
        RenderState(bool faceCulling, bool depthWrite, CullingMode cullingMode, WindingOrder windingOrder, bool prepassFaceCulling = true):
            faceCulling(faceCulling), cullingMode(cullingMode), windingOrder(windingOrder),
            prepassFaceCulling(prepassFaceCulling), depthWrite(depthWrite) {}
    };

    enum class BufferType{
        VERTEX_BUFFER,
        INDEX_BUFFER,
        STORAGE_BUFFER
    };

    enum class BufferUsage{ //see sokol_gfx.h for details
        IMMUTABLE,
        DYNAMIC,
        STREAM
    };

    enum class ShaderType{
        POINTS,
        LINES, // Not used yet
        MESH,
        SKYBOX,
        DEPTH,
        GBUFFER,    // mesh geometry pass: MRT packed depth + view-space normal/roughness/metallic (SSR)
        UI,
        SSAO,       // fullscreen screen-space ambient occlusion pass
        SSAO_BLUR,  // fullscreen depth-aware blur of the SSAO result
        SSR,        // fullscreen screen-space reflections pass
        SSR_BLUR,   // fullscreen premultiplied blur of the SSR result (glossy)
        COMPOSITE,  // fullscreen composite of scene color + SSR reflections
        SHADOW2D,   // 1D polar shadow pass: occluder segments into a 2D-light atlas row
        BLIT,       // fullscreen upscale of the fixed-resolution scene color
        // keep appending: the ShaderKey stores this enum and validates cached .sdat
        POSTPROCESS // fullscreen user post-process pass
    };

    enum class AttributeType{
        INDEX,
        POSITION,
        TEXCOORD1,
        TEXCOORD2,
        NORMAL,
        TANGENT,
        COLOR,
        POINTSIZE,
        POINTROTATION,
        TEXTURERECT,
        BONEWEIGHTS,
        BONEIDS,
        MORPHTARGET0,
        MORPHTARGET1,
        MORPHTARGET2,
        MORPHTARGET3,
        MORPHTARGET4,
        MORPHTARGET5,
        MORPHTARGET6,
        MORPHTARGET7,
        MORPHNORMAL0,
        MORPHNORMAL1,
        MORPHNORMAL2,
        MORPHNORMAL3,
        MORPHTANGENT0,
        MORPHTANGENT1,
        INSTANCEMATRIXCOL1,
        INSTANCEMATRIXCOL2,
        INSTANCEMATRIXCOL3,
        INSTANCEMATRIXCOL4,
        INSTANCECOLOR,
        INSTANCETEXTURERECT,
        TERRAINNODEPOSITION,
        TERRAINNODESIZE,
        TERRAINNODERANGE,
        TERRAINNODERESOLUTION,
        // generated by the renderer, not stored in a buffer: the normal averaged over the
        // vertices sharing a position (a_smoothNormal, manifest @require smoothnormals)
        SMOOTHNORMAL
    };

    enum class AttributeDataType{
        BYTE, //int8_t
        UNSIGNED_BYTE, //uint8_t
        SHORT, //int16_t
        UNSIGNED_SHORT, //uint16_t
        INT, //int32_t
        UNSIGNED_INT, //uint32_t
        FLOAT
    };

    enum class UniformBlockType{
        PBR_VS_PARAMS,
        MATERIAL,
        PBR_FS_PARAMS,
        PBR_FS_TEXCOORDSETS,
        PBR_VS_FADE,
        FS_LIGHTING,
        FS_REFLECTION_PROBE,
        FS_FOG,
        FS_MIRROR,
        VS_SHADOWS,
        FS_SHADOWS,
        FS_POINT_SHADOWS,
        SKY_VS_PARAMS,
        SKY_FS_PARAMS,
        DEPTH_VS_PARAMS,
        DEPTH_FS_MATERIAL,
        GBUFFER_VS_PARAMS,
        GBUFFER_FS_MATERIAL,
        SPRITE_VS_PARAMS,
        UI_VS_PARAMS,
        UI_FS_PARAMS,
        POINTS_VS_PARAMS,
        LINES_VS_PARAMS,
        VS_SKINNING,
        DEPTH_VS_SKINNING,
        VS_MORPHTARGET,
        DEPTH_VS_MORPHTARGET,
        TERRAIN_VS_PARAMS,
        DEPTH_TERRAIN_VS_PARAMS,
        FS_TERRAIN_LAYERS,
        SSAO_FS_PARAMS,
        SSAO_BLUR_FS_PARAMS,
        SSR_FS_PARAMS,
        SSR_BLUR_FS_PARAMS,
        COMPOSITE_FS_PARAMS,
        FS_LIGHTING2D,
        SHADOW2D_VS_PARAMS,
        BLIT_FS_PARAMS
    };

    enum class StorageBufferType{
        VS_VERTEX,
        VS_SKINNING
    };

    enum class TextureShaderType{
        BASECOLOR,
        EMISSIVE,
        METALLICROUGHNESS,
        OCCULSION,
        NORMAL,
        SHADOWATLAS,
        SHADOWPOINTATLAS,
        SKYCUBE,
        IRRADIANCEMAP,
        PREFILTEREDMAP,
        REFLECTIONPROBE,
        UI,
        POINTS,
        HEIGHTMAP,
        BLENDMAP,   // BLENDMAP + m addresses blend map m, so these stay contiguous
        BLENDMAP1,
        BLENDMAP2,
        TERRAINDETAIL,
        DEPTHTEXTURE,
        SSAOTEXTURE,
        NOISETEXTURE,
        SCENECOLORTEXTURE,
        SSRTEXTURE,
        GBUFFERTEXTURE,      // view-space normal (rg, octahedral) + roughness (b) + metallic (a)
        GBUFFERALBEDOTEXTURE, // linear base color (rgb) + hasIBL flag (a)
        SHADOW2DATLAS,       // 1D polar shadow rows (one per 2D light)
        SPOTMASKATLAS,       // projected spot masks, one horizontal tile per 3D light slot
        BONES
    };

    enum class TextureType {
        TEXTURE_2D,
        TEXTURE_3D,
        TEXTURE_CUBE,
        TEXTURE_ARRAY
    };

    enum class TextureSamplerType {
        SINT,
        UINT,
        FLOAT,
        DEPTH,
        UNFILTERABLE_FLOAT
    };

    enum class SamplerType {
        COMPARISON,
        FILTERING,
        NONFILTERING
    };

    enum class ColorFormat{
        RED,
        RGBA,
        RED16 // single channel, 16-bit unsigned normalized (2 bytes/texel)
    };

    enum class TextureFilter{
        NEAREST,
        LINEAR,
        NEAREST_MIPMAP_NEAREST,
        NEAREST_MIPMAP_LINEAR,
        LINEAR_MIPMAP_NEAREST,
        LINEAR_MIPMAP_LINEAR
    };

    enum class TextureWrap{
        REPEAT,
        MIRRORED_REPEAT,
        CLAMP_TO_EDGE,
        CLAMP_TO_BORDER
    };

    enum class TextureAlphaBorder{
        AUTO, // fixed for sprites, tilemaps, polygons, UI and particles, kept otherwise
        FIX,
        KEEP
    };

    enum PipelineType {
        PIP_DEFAULT     = 1 << 0,
        PIP_RTT         = 1 << 1,
        PIP_DEPTH       = 1 << 2,
        PIP_RTT_INVERT  = 1 << 3, // render-to-texture with reversed winding (planar reflection)
        PIP_GBUFFER     = 1 << 4, // geometry pass with 3 color attachments
        PIP_SHADOW_DEPTH = 1 << 5, // depth-only projective shadow atlas
        PIP_DEFAULT_NODEPTH = 1 << 6, // no depth test or write, submission order
        PIP_RTT_NODEPTH = 1 << 7,
        PIP_ZPREPASS    = 1 << 8, // depth only, into the swapchain color pass ahead of its opaque draws
        PIP_ZPREPASS_RTT = 1 << 9, // same into an offscreen color pass
        // a fullscreen effect pass into an offscreen target with a depth-only attachment it
        // never tests (FramebufferDepth::DEPTH: SSAO, SSR, their blurs, post-process buffer 1)
        PIP_EFFECT      = 1 << 10
    };

    // the depth attachment of an offscreen framebuffer
    enum class FramebufferDepth {
        DEPTH_STENCIL, // colour passes that draw meshes (PIP_RTT pipelines): stencil masks work
        // depth only: shadow maps, the SSAO depth pre-pass, the G-buffer, and the fullscreen
        // effect targets (PIP_EFFECT), which never test it. No stencil plane: D32 instead of
        // D32S8 on Metal and Vulkan; the same 4 bytes as D24S8 on GL and D3D11. (NONE there
        // changed llvmpipe's output by 1 on a few pixels, so effects keep a depth plane.)
        DEPTH,
        NONE           // no depth attachment (only pipelines whose depth format is NONE)
    };

    //-------Start shader definition--------
    enum class ShaderLang{
        GLSL,
        MSL,
        HLSL,
        SPIRV
    };

    enum class ShaderVertexType{
        FLOAT,
        FLOAT2,
        FLOAT3,
        FLOAT4,
        INT,
        INT2,
        INT3,
        INT4
    };

    enum class ShaderStorageBufferType{
        STRUCT
    };

    enum class ShaderUniformType{
        FLOAT,
        FLOAT2,
        FLOAT3,
        FLOAT4,
        INT,
        INT2,
        INT3,
        INT4,
        MAT3,
        MAT4
    };

    enum class ShaderStageType{
        VERTEX,
        FRAGMENT
    };
    //-------End shader definition--------
}

#endif //Render_h
