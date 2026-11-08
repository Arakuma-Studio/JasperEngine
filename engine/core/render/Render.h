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

    // Colour blending of a pipeline: of the fixed pipeline types (RenderState::blend, the
    // shader library's material blends) and of the dynamic path (PipelineState::blend, the
    // HTML UI renderer and its effects). ALPHA is what every pipeline used before render
    // state was configurable (opaque objects included); NONE disables blending. Factors are
    // the colour's; the alpha channel of offscreen targets keeps ONE, ONE_MINUS_SRC_ALPHA
    // (fixed types, see SokolObject::applyBlend), the dynamic path blends alpha as the
    // premultiplied modes say.
    //   NONE                    blending off
    //   ALPHA                   SRC_ALPHA, ONE_MINUS_SRC_ALPHA
    //   PREMULTIPLIED           ONE, ONE_MINUS_SRC_ALPHA
    //   ADDITIVE                SRC_ALPHA, ONE (straight colour)
    //   MULTIPLY                DST_COLOR, ZERO
    // and the HTML UI effects' blends of premultiplied colour (rgb and alpha alike):
    //   ADDITIVE_PREMULTIPLIED  ONE, ONE
    //   SCREEN_PREMULTIPLIED    ONE, ONE_MINUS_SRC_COLOR
    //   MULTIPLY_PREMULTIPLIED  DST_COLOR, ONE_MINUS_SRC_ALPHA
    // and Title Art's light blends (docs/specs/S80-title-art-core.md §6.4), dynamic path only:
    //   LIGHT_PREMULTIPLIED     colour ONE, ONE; alpha ONE, ONE_MINUS_SRC_ALPHA (light adds
    //                           colour and unions coverage, where ADDITIVE_PREMULTIPLIED keeps
    //                           adding alpha)
    //   MAX_PREMULTIPLIED       MAX of source and destination, colour and alpha (all factors ONE)
    //   ERASE_PREMULTIPLIED     ZERO, ONE_MINUS_SRC_ALPHA (dst x (1 - src alpha): knockout)
    enum class BlendMode : uint8_t {
        NONE,
        ALPHA,
        PREMULTIPLIED,
        ADDITIVE,
        MULTIPLY,
        ADDITIVE_PREMULTIPLIED,
        SCREEN_PREMULTIPLIED,
        MULTIPLY_PREMULTIPLIED,
        // dst - src (premultiplied colour taken away; alpha kept): the atmosphere's
        // volumetric composite removes the analytic fog it replaces with it
        SUBTRACT_PREMULTIPLIED,
        LIGHT_PREMULTIPLIED,
        MAX_PREMULTIPLIED,
        ERASE_PREMULTIPLIED
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

    // stencil operations of RenderState and of PipelineState's StencilState
    enum class StencilOp : uint8_t {
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
        POSTPROCESS, // fullscreen user post-process pass
        BLOOM,       // the bloom and blur chain's passes (shader library phase 4: prefilter, dual-filter down and up)
        HTMLUI,     // HTML/CSS UI geometry and composite: premultiplied alpha, no color conversion
        HTMLUI_GRADIENT, // HTML UI linear/radial/conic gradients (RmlUi CompileShader)
        HTMLUI_POST,     // HTML UI fullscreen passes: layer composite, filters, masks (exclusive variants)
        UI_EFFECT,       // HTML UI effects (phase 5): one per effect and form, the effect in the custom-shader id
        PROBE_FILTER,    // reflection probe capture prefiltered into roughness mip levels (probe_filter.frag)
        FLUID,           // FluidSimulator's passes (the solver and the display), the pass in the property bits
        SHADOW_CACHE,    // the shadow cache's depth copy and slot clear; 'Vis' the atlas debug view (RenderSystem)
        // the atmosphere's passes (docs/specs/S12-atmosphere.md, RenderSystem's AtmosphereRenderer)
        ATMOSPHERE_DEPTH,     // reduced-resolution linear depth of the scene (Atmosphere/DepthDownsample)
        VOLUMETRIC_FOG,       // the volumetric fog march (Atmosphere/VolumetricFog)
        ATMOSPHERE_DENOISE,   // depth-aware 3x3 blur of a reduced (S, T) target (Atmosphere/Denoise)
        VOLUMETRIC_CLOUDS,    // the clouds march, Screen and Sky Map (Atmosphere/CloudsScreen, CloudsSkyMap)
        SKY_COMPOSITE,        // clouds and sky fill over the sky pixels (Atmosphere/SkyComposite)
        VOLUMETRIC_COMPOSITE, // volumetric fog and Screen clouds over the scene (Atmosphere/VolumetricComposite)
        PASS_PREVIEW,         // the Frame Debugger's pass output preview (S15, SokolPassPreview; editor only)
        PARTICLES,            // particle systems (docs/specs/S20-particles.md): instanced quads, meshes and trails
        HTMLUI_PRIM,          // HTML UI instanced primitives (S16): quads, SDF boxes, borders and box shadows
        // S57's fullscreen passes: the contact shadows' mask (contact_shadows.frag), 'Oir' the OIT
        // resolve (oit_resolve.frag)
        LIGHTING_PASS,
        TITLE_ART,            // Title Art's passes (S80): the program is the low property bits (titleart/render/TitlePasses.h)
        // 3D titles (S86, titleart/three_d/TitleTierFeatures.h): the Title Lit mesh shader (the TL_*
        // feature bits), the 3D post (the pass in the bits), the environment builder's passes, the
        // Low tier's sprite glints and the glint chain's passes (the pass in the bits)
        TITLE_LIT,
        TITLE_3D_POST,
        TITLE_ENV,
        TITLE_GLINT_SPRITE,
        TITLE_GLINT
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
        SMOOTHNORMAL,
        // particle systems (shader type PARTICLES): the quad corner (per vertex) and the
        // per-instance record (particles/ParticleRender.h); trails read the same names per vertex
        PARTICLECORNER,
        PARTICLEPOSITION,
        PARTICLECOLOR,
        PARTICLESIZE,
        PARTICLEROTATIONFRAME,
        PARTICLEVELOCITY,
        PARTICLEROTATION,
        PARTICLECUSTOM,
        // Vertex Animation (includes/vat.glsl): the vertex's row in the payload (glTF
        // _VAT_INDEX, a_vatIndex) and an instance's playback state (i_vat0, i_vat1: the
        // vat::vatPackInstance vectors, in their own per-instance buffer)
        VAT_INDEX,
        INSTANCEVAT0,
        INSTANCEVAT1,
        // the ribbon of thick lines and trails (includes/lines_vs.glsl): the directions of the
        // line in and out of the vertex's point (a_tangentIn, a_tangentOut), its side, half
        // width, role and fan parameter (a_lineParams)
        LINETANGENTIN,
        LINETANGENTOUT,
        LINEPARAMS,
        // an automatically instanced batch's per-instance object block (spec S55,
        // includes/object_params.glsl, object_depth_params.glsl): the model matrix's rows, the
        // normal matrix's columns, the model-view-projection's columns, the depth layout's extra
        // vector (i_objectModel0..2, i_objectNormal0..2, i_objectMvp0..3, i_objectExtra)
        OBJECTMODEL0,
        OBJECTMODEL1,
        OBJECTMODEL2,
        OBJECTNORMAL0,
        OBJECTNORMAL1,
        OBJECTNORMAL2,
        OBJECTMVP0,
        OBJECTMVP1,
        OBJECTMVP2,
        OBJECTMVP3,
        OBJECTEXTRA
    };

    enum class AttributeDataType{
        BYTE, //int8_t
        UNSIGNED_BYTE, //uint8_t
        SHORT, //int16_t
        UNSIGNED_SHORT, //uint16_t
        INT, //int32_t
        UNSIGNED_INT, //uint32_t
        FLOAT,
        HALF //IEEE 754 binary16 (2 or 4 elements)
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
        UI_FS_TEXT_PARAMS,
        POINTS_VS_PARAMS,
        LINES_VS_PARAMS,
        VS_SKINNING,
        DEPTH_VS_SKINNING,
        VS_MORPHTARGET,
        DEPTH_VS_MORPHTARGET,
        // u_vs_lattice (includes/lattice.glsl) of the colour, depth and G-buffer vertex stages
        VS_LATTICE,
        DEPTH_VS_LATTICE,
        GBUFFER_VS_LATTICE,
        // u_vs_vat (includes/vat.glsl, Vertex Animation) of the colour, depth and G-buffer vertex stages
        VS_VAT,
        DEPTH_VS_VAT,
        GBUFFER_VS_VAT,
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
        BLIT_FS_PARAMS,
        PROBE_FILTER_FS_PARAMS,
        PARTICLES_VS_PARAMS,
        PARTICLES_FS_PARAMS
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
        GBUFFERALBEDOTEXTURE, // linear base color (rgb) + SSR reflection source code (a)
        SHADOW2DATLAS,       // 1D polar shadow rows (one per 2D light)
        SPOTMASKATLAS,       // projected spot masks, one horizontal tile per 3D light slot
        BONES,
        LATTICE,             // the lattice records of a mesh (RGBA32F, includes/lattice.glsl)
        PROBESOURCE,         // reflection probe capture (or its box mip chain) read by the prefilter
        SHADOWATLASRAW,      // the projective shadow atlas's raw depths (a second view: PCSS)
        PARTICLES,           // the particle texture (sprite sheet)
        PARTICLESIXWAYA,     // six-way lighting: the two response maps
        PARTICLESIXWAYB,
        PARTICLEEMISSION,    // the emission map
        VAT                  // a Vertex Animation payload (RGBA16F/32F, includes/vat.glsl)
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
        RED16, // single channel, 16-bit unsigned normalized (2 bytes/texel)
        // RGBA8 stored sRGB-encoded: the hardware decodes on sampling (before filtering) and
        // encodes on write, so a chain of them filters in linear light (the bloom chain).
        // Colour-renderable and filterable in core GLES 3.0, GL 3.0, D3D11, Metal and Vulkan
        SRGB8A8,
        // float colour attachments (phase 6, the UI effects' simulation state): renderable on
        // GL 3.0, D3D11, Metal and Vulkan; GLES3/WebGL2 need EXT_color_buffer_float (or
        // _half_float for RGBA16F). RGBA16F is filterable everywhere; the 32-bit ones are not on
        // GLES3/WebGL2 without OES_texture_float_linear (sample them with NEAREST). Ask
        // SystemRender::getFeatures() before creating one
        RGBA16F,
        RGBA32F,
        R32F,
        // half-float two- and one-channel targets (the fluid simulation's velocity and its
        // pressure, divergence and curl): renderable and filterable where RGBA16F is (same GLES3
        // extension rules), half the memory of RGBA16F per channel dropped
        RG16F,
        R16F
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
        PIP_GBUFFER     = 1 << 4, // geometry pass with 4 color attachments
        PIP_SHADOW_DEPTH = 1 << 5, // depth-only projective shadow atlas
        PIP_DEFAULT_NODEPTH = 1 << 6, // no depth test or write, submission order
        PIP_RTT_NODEPTH = 1 << 7,
        PIP_ZPREPASS    = 1 << 8, // depth only, into the swapchain color pass ahead of its opaque draws
        PIP_ZPREPASS_RTT = 1 << 9, // same into an offscreen color pass
        // a fullscreen effect pass into an offscreen target with a depth-only attachment it
        // never tests (FramebufferDepth::DEPTH: SSAO, SSR, their blurs, post-process buffer 1)
        PIP_EFFECT      = 1 << 10,
        // a fullscreen pass into an SRGB8A8 target without depth (FramebufferDepth::NONE,
        // ColorFormat::SRGB8A8): the bloom and blur chain (phase 4), no blending
        PIP_POST        = 1 << 11,
        // PIP_RTT, PIP_RTT_NODEPTH and PIP_ZPREPASS_RTT into the multisampled scene target of
        // the post-processed camera (PostProcessingSettings::msaaSamples); their sample count
        // is the one the object's endLoad was given (its RenderSystem's)
        PIP_MSAA        = 1 << 12,
        PIP_MSAA_NODEPTH = 1 << 13,
        PIP_ZPREPASS_MSAA = 1 << 14,
        // a fullscreen pass into one face and mip level of an RGBA8 colour texture, no depth
        // attachment, no blending (CameraRender::startRenderPass(TextureRender*, face, mip):
        // the reflection probe prefilter)
        PIP_RTT_COLORONLY = 1 << 15
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

    enum class BlendFactor {
        ZERO, ONE,
        SRC_COLOR, ONE_MINUS_SRC_COLOR, SRC_ALPHA, ONE_MINUS_SRC_ALPHA,
        DST_COLOR, ONE_MINUS_DST_COLOR, DST_ALPHA, ONE_MINUS_DST_ALPHA
    };

    // MAX takes ONE for every factor (sokol validates it)
    enum class BlendOperation { ADD, SUBTRACT, REVERSE_SUBTRACT, MAX };

    // Color blending of a render object's color pipelines (PIP_DEFAULT, PIP_DEFAULT_NODEPTH,
    // PIP_RTT, PIP_RTT_NODEPTH, PIP_RTT_INVERT). Without one, those pipelines keep their
    // built-in straight-alpha blending (SokolObject::makePipelineDesc).
    struct BlendState {
        bool enabled = true;
        BlendFactor srcFactorRGB = BlendFactor::SRC_ALPHA;
        BlendFactor dstFactorRGB = BlendFactor::ONE_MINUS_SRC_ALPHA;
        BlendOperation opRGB = BlendOperation::ADD;
        BlendFactor srcFactorAlpha = BlendFactor::ONE;
        BlendFactor dstFactorAlpha = BlendFactor::ONE_MINUS_SRC_ALPHA;
        BlendOperation opAlpha = BlendOperation::ADD;

        // ONE, ONE_MINUS_SRC_ALPHA on color and alpha: colors already multiplied by alpha
        static BlendState premultipliedAlpha() {
            BlendState s; s.srcFactorRGB = BlendFactor::ONE; return s;
        }
        // no blending: the source replaces the destination
        static BlendState opaque() { BlendState s; s.enabled = false; return s; }
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
