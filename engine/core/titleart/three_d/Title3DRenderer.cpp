// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The pass order and the post follow Pachislop src/engine/three/Layer3DRenderer.ts, tonemap.ts,
// hdrResolve.ts and resample.ts by SafEight, used with the author's permission.

#include "Title3DRenderer.h"

#include "pool/ShaderPool.h"
#include "render/BufferRender.h"
#include "render/CameraRender.h"
#include "render/ObjectRender.h"
#include "render/PipelineState.h"
#include "render/ShaderRender.h"
#include "render/SystemRender.h"
#include "render/TextureRender.h"
#include "titleart/render/TitleArtCaps.h"
#include "titleart/render/TitleGpu.h"
#include "titleart/render/TitleRenderServices.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    constexpr int MATERIALS_MAX = 16;
    constexpr int MATERIAL_VEC4S = 10;
    constexpr int PARTS_MAX = 64;
    constexpr int LIGHTS_MAX = 4;
    constexpr int RAMP_ROWS = 16;
    // procedural cubes kept per face size (a 1024 face is about 50 MB of half floats with mips)
    constexpr size_t CUBES_1024 = 3, CUBES_2048 = 1;

    enum PostProgram : uint32_t { TONEMAP = 1, MITCHELL_H = 2, MITCHELL_V = 3 };
    enum EnvProgram : uint32_t { PROCEDURAL = 1, PREFILTER = 2, EQUIRECT = 3, DOWNSAMPLE = 4 };

    struct alignas(16) VsBlock {
        float viewProj[16];
        float model[16];
        float normalModel[16];
        float skew[4];
        float parts[PARTS_MAX * 4][4];
    };

    struct alignas(16) FsBlock {
        float camera[4], frame[4], env[4], grade[4], grade2[4];
        float lock0[4], lock1[4], lock2[4], view0[4], view1[4];
        float ambient[4], axisX[4], axisY[4];
        float lights[LIGHTS_MAX * 2][4];
        float slots[16][4];
        float materials[MATERIALS_MAX * MATERIAL_VEC4S][4];
    };

    struct alignas(16) PostBlock {
        float p0[4], p1[4];
    };

    struct alignas(16) EnvBlock {
        float params[4], source[4], counts[4];
        float stops[TITLE_ENV_MAX_STOPS][4];
        float elements[TITLE_ENV_MAX_ELEMENTS * 3][4];
    };

    void set4(float* out, float a, float b = 0, float c = 0, float d = 0) {
        out[0] = a;
        out[1] = b;
        out[2] = c;
        out[3] = d;
    }

    std::array<float, 3> linear(const TitleColor& c) { return {srgbToLinear(c.r), srgbToLinear(c.g), srgbToLinear(c.b)}; }

    // a vertexless fullscreen program (the post and the environment passes)
    struct Fullscreen {
        std::shared_ptr<ShaderRender> shader;
        ObjectRender object;
        bool loaded = false, failed = false;
        int block = -1;
        std::pair<int, int> source{-1, -1};
    };

    // one Title Lit variant drawing one mesh
    struct LitObject {
        ObjectRender object;
        int vsBlock = -1, fsBlock = -1;
        std::pair<int, int> env{-1, -1}, ramps{-1, -1}, spans{-1, -1}, base{-1, -1}, matcap{-1, -1};
    };

    struct GpuMesh {
        uint64_t key = 0;
        std::shared_ptr<const TitleMesh> mesh;
        BufferRender vertices, indices, wrap;
        std::map<uint32_t, std::unique_ptr<LitObject>> objects;
        uint64_t lastUse = 0;

        ~GpuMesh() {
            for (auto& [bits, o] : objects) o->object.destroy();
            vertices.destroyBuffer();
            indices.destroyBuffer();
            wrap.destroyBuffer();
        }
    };

    struct Cube {
        uint64_t key = 0;
        int face = 0, levels = 0;
        TextureRender texture;
        uint64_t lastUse = 0;
        ~Cube() { texture.destroyTexture(); }
    };

    const float CLEAR[4] = {0, 0, 0, 0};

}

struct Title3DRenderer::Impl {
    std::map<uint64_t, std::unique_ptr<Fullscreen>> fullscreen;     // by (type << 32 | properties)
    std::map<uint64_t, std::unique_ptr<GpuMesh>> meshes;
    std::map<uint64_t, std::unique_ptr<Cube>> cubes;
    TextureRender placeholder2D, placeholderCube;
    bool placeholders = false;
    CameraRender pass;
    uint64_t clock = 0;

    ~Impl() {
        for (auto& [key, p] : fullscreen) {
            if (p->loaded) p->object.destroy();
        }
        meshes.clear();
        cubes.clear();
        if (placeholders) {
            placeholder2D.destroyTexture();
            placeholderCube.destroyTexture();
        }
    }

    bool ensurePlaceholders() {
        if (placeholders) return true;
        static unsigned char texel[4] = {255, 255, 255, 255};
        void* data[1] = {texel};
        size_t size[1] = {4};
        void* faces[6] = {texel, texel, texel, texel, texel, texel};
        size_t sizes[6] = {4, 4, 4, 4, 4, 4};
        placeholders = placeholder2D.createTexture("title 3d placeholder", 1, 1, ColorFormat::RGBA, TextureType::TEXTURE_2D, 1, data, size,
                                                   TextureFilter::LINEAR, TextureFilter::LINEAR, TextureWrap::CLAMP_TO_EDGE,
                                                   TextureWrap::CLAMP_TO_EDGE) &&
                       placeholderCube.createTexture("title 3d placeholder cube", 1, 1, ColorFormat::RGBA, TextureType::TEXTURE_CUBE, 6,
                                                     faces, sizes, TextureFilter::LINEAR, TextureFilter::LINEAR, TextureWrap::CLAMP_TO_EDGE,
                                                     TextureWrap::CLAMP_TO_EDGE);
        return placeholders;
    }

    // a program's shader: null while it compiles (error untouched) or failed (error set)
    std::shared_ptr<ShaderRender> shaderOf(ShaderType type, uint64_t properties, std::string* error) {
        if (ShaderPool::isShaderBuildFailed(type, properties)) {
            if (error) *error = "the 3D title shader " + ShaderPool::getShaderStr(type, properties) + " could not be built";
            return nullptr;
        }
        std::shared_ptr<ShaderRender> shader = ShaderPool::get(type, properties);
        if (!shader || !shader->isCreated()) return nullptr;
        return shader;
    }

    Fullscreen* program(ShaderType type, uint32_t properties, const char* block, const char* source, std::string* error) {
        std::unique_ptr<Fullscreen>& slot = fullscreen[((uint64_t)type << 32) | properties];
        if (!slot) slot = std::make_unique<Fullscreen>();
        Fullscreen& p = *slot;
        if (p.loaded) return &p;
        if (p.failed) {
            if (error) *error = "the 3D title program " + ShaderPool::getShaderStr(type, properties) + " could not be loaded";
            return nullptr;
        }
        p.shader = shaderOf(type, properties, error);
        if (!p.shader || !ensurePlaceholders()) return nullptr;
        ShaderData& data = p.shader->shaderData;
        p.block = data.getUniformBlockIndexByName(block);
        if (source) p.source = data.getTextureIndexByName(source);
        p.object.beginLoad(PrimitiveType::TRIANGLES);
        p.object.setShader(p.shader.get());
        if (p.source.first >= 0) {
            const bool cubeSource = type == ShaderType::TITLE_ENV && properties != EQUIRECT;
            p.object.addTexture(p.source, ShaderStageType::FRAGMENT, cubeSource ? &placeholderCube : &placeholder2D);
        }
        p.object.setImmediatePipelines(true);
        if (p.block < 0 || !p.object.endLoadDynamic(PrimitiveType::TRIANGLES, IndexType::NONE)) {
            p.object.destroy();
            p.failed = true;
            if (error) *error = "the 3D title program " + ShaderPool::getShaderStr(type, properties) + " could not be loaded";
            return nullptr;
        }
        p.loaded = true;
        return &p;
    }

    // ---- environments ----

    Cube* cube(const Title3DPlan& plan, std::string& error, Title3DStats& stats) {
        auto it = cubes.find(plan.environmentKey);
        if (it != cubes.end()) {
            it->second->lastUse = ++clock;
            return it->second.get();
        }
        const bool panorama = plan.hdri != nullptr;
        Fullscreen* procedural = panorama ? nullptr : program(ShaderType::TITLE_ENV, PROCEDURAL, "u_fs_titleEnv", nullptr, &error);
        Fullscreen* equirect = panorama ? program(ShaderType::TITLE_ENV, EQUIRECT, "u_fs_titleEnv", "u_teEquirect", &error) : nullptr;
        Fullscreen* downsample = panorama ? program(ShaderType::TITLE_ENV, DOWNSAMPLE, "u_fs_titleEnv", "u_teSource", &error) : nullptr;
        Fullscreen* prefilter = program(ShaderType::TITLE_ENV, PREFILTER, "u_fs_titleEnv", "u_teSource", &error);
        if ((!procedural && !panorama) || (panorama && (!equirect || !downsample)) || !prefilter) {
            if (error.empty()) error = "the title environment programs are not ready";
            return nullptr;
        }
        const int face = plan.environmentFace;
        const int levels = titleEnvironmentLevels(face);
        // the source chain: the scene box-filtered at every level (each level evaluated on its own
        // 4 x 4 grid, so no level reads another)
        TextureRender source;
        auto createCube = [&](TextureRender& t) {
            return t.createFramebufferTexture(TextureType::TEXTURE_CUBE, false, false, face, face, TextureFilter::LINEAR_MIPMAP_LINEAR,
                                              TextureFilter::LINEAR, TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE, ColorFormat::RGBA16F,
                                              1, false, false, levels);
        };
        auto made = std::make_unique<Cube>();
        if (!createCube(source) || !createCube(made->texture)) {
            source.destroyTexture();
            error = "the title environment cube (" + std::to_string(face) + " px) could not be made";
            return nullptr;
        }
        EnvBlock block;
        std::memset(&block, 0, sizeof(block));
        const TitleEnvScene& scene = *plan.scene;
        const size_t stops = std::min<size_t>(scene.sky.size(), TITLE_ENV_MAX_STOPS);
        for (size_t i = 0; i < stops; i++) std::memcpy(block.stops[i], scene.sky[i].data(), sizeof(float) * 4);
        const size_t elements = std::min<size_t>(scene.elements.size(), TITLE_ENV_MAX_ELEMENTS);
        for (size_t i = 0; i < elements; i++) {
            const TitleEnvElement& e = scene.elements[i];
            const float extra = e.kind == TitleEnvElementKind::Disc || e.kind == TitleEnvElementKind::Tube ? e.radius : 0.0f;
            set4(block.elements[i * 3], (float)e.kind, e.a[0], e.a[1], e.a[2]);
            if (e.kind == TitleEnvElementKind::Tube) {
                set4(block.elements[i * 3 + 1], e.b[0], e.b[1], e.b[2], extra);
            } else {
                set4(block.elements[i * 3 + 1], e.width, e.height, 0, extra);
            }
            set4(block.elements[i * 3 + 2], e.radiance[0], e.radiance[1], e.radiance[2], e.edge);
        }
        auto run = [&](Fullscreen& p, TextureRender& target, TextureRender* from, int level, int faceIndex) {
            pass.setColorActions(LoadAction::DONTCARE, StoreAction::STORE);
            pass.setDepthStencilActions(LoadAction::DONTCARE, StoreAction::DONTCARE);
            pass.setLabel("title environment");
            pass.startRenderPass(&target, (size_t)faceIndex, level);
            PipelineState state;
            state.target.color = ColorFormat::RGBA16F;
            state.target.depth = DepthStencilFormat::NONE;
            state.blend = BlendMode::NONE;
            if (p.object.beginDraw(state)) {
                if (from && p.source.first >= 0) p.object.addTexture(p.source, ShaderStageType::FRAGMENT, from);
                p.object.applyUniformBlock(p.block, sizeof(EnvBlock), &block);
                p.object.draw(0, 3, 1);
            }
            pass.endRenderPass();
        };
        if (panorama) {
            // the panorama into the top level, then each level a box of the one above through a
            // second cube (a pass never samples the texture it renders to)
            TitleUpload pano;
            TextureRender reduce;
            if (!pano.create("title environment panorama", plan.hdri->width, plan.hdri->height, ColorFormat::RGBA16F, plan.hdri->rgba.data(),
                             plan.hdri->rgba.size() * 2, TextureFilter::LINEAR, TextureWrap::REPEAT, TextureWrap::CLAMP_TO_EDGE) ||
                !createCube(reduce)) {
                source.destroyTexture();
                reduce.destroyTexture();
                error = "the title environment's panorama could not be uploaded";
                return nullptr;
            }
            for (int f = 0; f < 6; f++) {
                set4(block.params, (float)f, 0, 0, (float)face);
                set4(block.source, (float)face, (float)levels, 0, (float)stops);
                set4(block.counts, (float)elements, plan.hdriYaw);
                run(*equirect, source, &pano.texture(), 0, f);
            }
            for (int level = 1; level < levels; level++) {
                const int size = std::max(1, face >> level);
                for (int f = 0; f < 6; f++) {
                    set4(block.params, (float)f, 0, 2, (float)size);
                    set4(block.source, (float)face, (float)levels, (float)(level - 1), (float)stops);
                    run(*downsample, reduce, &source, level, f);
                }
                for (int f = 0; f < 6; f++) {
                    set4(block.params, (float)f, 0, 1, (float)size);
                    set4(block.source, (float)face, (float)levels, (float)level, (float)stops);
                    run(*downsample, source, &reduce, level, f);
                }
            }
            reduce.destroyTexture();
        } else {
            for (int level = 0; level < levels; level++) {
                const int size = std::max(1, face >> level);
                for (int f = 0; f < 6; f++) {
                    set4(block.params, (float)f, 0, 0, (float)size);
                    set4(block.source, (float)face, (float)levels, 0, (float)stops);
                    set4(block.counts, (float)elements);
                    run(*procedural, source, nullptr, level, f);
                }
            }
        }
        for (int level = 0; level < levels; level++) {
            const int size = std::max(1, face >> level);
            const float roughness = titleEnvironmentRoughness(level, face);
            for (int f = 0; f < 6; f++) {
                set4(block.params, (float)f, roughness, 128, (float)size);
                set4(block.source, (float)face, (float)levels, 0, (float)stops);
                run(*prefilter, made->texture, &source, level, f);
            }
        }
        source.destroyTexture();
        stats.environmentBuilds++;
        made->key = plan.environmentKey;
        made->face = face;
        made->levels = levels;
        made->lastUse = ++clock;
        Cube* out = made.get();
        cubes[plan.environmentKey] = std::move(made);
        // keep 3 cubes at 1024 and 1 at 2048 (least recently used go first)
        for (int faceSize : {1024, 2048}) {
            const size_t keep = faceSize == 2048 ? CUBES_2048 : CUBES_1024;
            while (true) {
                size_t count = 0;
                uint64_t oldest = UINT64_MAX;
                uint64_t victim = 0;
                for (auto& [key, c] : cubes) {
                    if (c->face != faceSize) continue;
                    count++;
                    if (c->lastUse < oldest && c.get() != out) {
                        oldest = c->lastUse;
                        victim = key;
                    }
                }
                if (count <= keep || oldest == UINT64_MAX) break;
                cubes.erase(victim);
            }
        }
        return out;
    }

    // ---- meshes ----

    GpuMesh* mesh(const Title3DPlan& plan, std::string& error, Title3DStats& stats) {
        const TitleMesh& m = *plan.mesh;
        auto it = meshes.find(m.sourceHash);
        if (it != meshes.end() && it->second->mesh == plan.mesh) {
            it->second->lastUse = ++clock;
            return it->second.get();
        }
        auto g = std::make_unique<GpuMesh>();
        g->key = m.sourceHash;
        g->mesh = plan.mesh;
        const bool ok =
            !m.vertices.empty() &&
            g->vertices.createBuffer((unsigned)(m.vertices.size() * sizeof(TitleMeshVertex)), (void*)m.vertices.data(), BufferType::VERTEX_BUFFER,
                                     BufferUsage::IMMUTABLE) &&
            (m.wideIndices ? g->indices.createBuffer((unsigned)(m.indices32.size() * 4), (void*)m.indices32.data(), BufferType::INDEX_BUFFER,
                                                     BufferUsage::IMMUTABLE)
                           : g->indices.createBuffer((unsigned)(m.indices16.size() * 2), (void*)m.indices16.data(), BufferType::INDEX_BUFFER,
                                                     BufferUsage::IMMUTABLE)) &&
            (m.wrap.empty() || g->wrap.createBuffer((unsigned)(m.wrap.size() * 2), (void*)m.wrap.data(), BufferType::VERTEX_BUFFER,
                                                    BufferUsage::IMMUTABLE));
        if (!ok) {
            error = "the 3D title mesh could not be uploaded";
            return nullptr;
        }
        stats.meshUploads++;
        g->lastUse = ++clock;
        GpuMesh* out = g.get();
        meshes[m.sourceHash] = std::move(g);
        // a renderer keeps the meshes of its last few layers
        while (meshes.size() > 8) {
            auto oldest = std::min_element(meshes.begin(), meshes.end(), [](const auto& a, const auto& b) { return a.second->lastUse < b.second->lastUse; });
            if (oldest->second.get() == out) break;
            meshes.erase(oldest);
        }
        return out;
    }

    LitObject* lit(GpuMesh& g, uint32_t bits, std::string* error) {
        auto it = g.objects.find(bits);
        if (it != g.objects.end()) return it->second.get();
        std::shared_ptr<ShaderRender> shader = shaderOf(ShaderType::TITLE_LIT, bits, error);
        if (!shader || !ensurePlaceholders()) return nullptr;
        auto o = std::make_unique<LitObject>();
        ShaderData& data = shader->shaderData;
        o->vsBlock = data.getUniformBlockIndexByName("u_vs_titleLit");
        o->fsBlock = data.getUniformBlockIndexByName("u_fs_titleLit");
        o->env = data.getTextureIndexByName("u_tlEnv");
        o->matcap = data.getTextureIndexByName("u_tlMatcap");
        o->ramps = data.getTextureIndexByName("u_tlRamps");
        o->spans = data.getTextureIndexByName("u_tlSpans");
        o->base = data.getTextureIndexByName("u_tlBase");
        ObjectRender& r = o->object;
        r.beginLoad(PrimitiveType::TRIANGLES);
        r.setShader(shader.get());
        const unsigned stride = sizeof(TitleMeshVertex);
        r.addAttribute(data.getAttrIndexByName("a_position"), &g.vertices, 3, AttributeDataType::FLOAT, stride, 0, false, false);
        r.addAttribute(data.getAttrIndexByName("a_normal"), &g.vertices, 2, AttributeDataType::SHORT, stride, 12, true, false);
        r.addAttribute(data.getAttrIndexByName("a_ids"), &g.vertices, 4, AttributeDataType::UNSIGNED_SHORT, stride, 16, true, false);
        if (bits & TitleLitBits::WRAP_UV) {
            r.addAttribute(data.getAttrIndexByName("a_wrap"), &g.wrap, 2, AttributeDataType::HALF, 4, 0, false, false);
        }
        r.setIndex(&g.indices, g.mesh->wideIndices ? AttributeDataType::UNSIGNED_INT : AttributeDataType::UNSIGNED_SHORT, 0);
        for (auto slot : {o->env, o->matcap}) {
            if (slot.first >= 0) r.addTexture(slot, ShaderStageType::FRAGMENT, slot == o->env ? &placeholderCube : &placeholder2D);
        }
        for (auto slot : {o->ramps, o->spans, o->base}) {
            if (slot.first >= 0) r.addTexture(slot, ShaderStageType::FRAGMENT, &placeholder2D);
        }
        r.setImmediatePipelines(true);
        if (o->vsBlock < 0 || o->fsBlock < 0 ||
            !r.endLoadDynamic(PrimitiveType::TRIANGLES, g.mesh->wideIndices ? IndexType::UINT32 : IndexType::UINT16)) {
            r.destroy();
            if (error) *error = "the Title Lit variant " + ShaderPool::getShaderStr(ShaderType::TITLE_LIT, bits) + " could not be loaded";
            return nullptr;
        }
        LitObject* out = o.get();
        g.objects[bits] = std::move(o);
        return out;
    }

    bool fullscreenPass(Fullscreen& p, TitleTarget* target, TextureRender* source, const PostBlock& block, std::string& error) {
        auto* t = static_cast<SokolTitleTarget*>(target);
        if (!t) {
            error = "device_lost: a 3D title target is gone";
            return false;
        }
        pass.setColorActions(LoadAction::DONTCARE, StoreAction::STORE);
        pass.setDepthStencilActions(LoadAction::DONTCARE, StoreAction::DONTCARE);
        pass.setLabel("title 3d post");
        pass.startRenderPass(&t->framebuffer());
        PipelineState state;
        state.target.color = t->key().format;
        state.target.depth = DepthStencilFormat::NONE;
        state.target.sampleCount = 1;
        state.blend = BlendMode::NONE;
        bool drawn = false;
        if (p.object.beginDraw(state)) {
            if (p.source.first >= 0) p.object.addTexture(p.source, ShaderStageType::FRAGMENT, source);
            p.object.applyUniformBlock(p.block, sizeof(PostBlock), (void*)&block);
            p.object.draw(0, 3, 1);
            drawn = true;
        }
        pass.endRenderPass();
        if (!drawn) error = "a 3D title post pass could not draw";
        return drawn;
    }
};

Title3DRenderer::Title3DRenderer() : impl(std::make_unique<Impl>()) {}

Title3DRenderer::~Title3DRenderer() = default;

bool Title3DRenderer::compressedLight(const Title3DPlan& plan, uint32_t testFlags) {
    const RenderFeatures& f = SystemRender::getFeatures();
    if (testFlags & TITLE_ART_TEST_NO_FLOAT_TARGETS) return true;
    if (titleTierLimits(plan.tier).compressedLight) return true;
    const bool msaa = (testFlags & TITLE_ART_TEST_NO_FLOAT_MSAA) == 0 && f.msaaRGBA16F;
    return !(f.renderRGBA16F && f.blendRGBA16F && f.filterRGBA16F && msaa);
}

uint32_t Title3DRenderer::passCount(const Title3DPlan& plan) {
    // main, tone map, and the two Mitchell passes when supersampled
    return 2u + (plan.k > 1 ? 2u : 0u);
}

bool Title3DRenderer::ready(const Title3DPlan& plan, uint32_t testFlags) {
    const bool compressed = compressedLight(plan, testFlags);
    std::string error;
    for (const Title3DSlotMaterial& s : plan.slots) {
        const uint32_t bits = s.bits | (compressed ? TitleLitBits::HDR_COMPRESS : 0u);
        if (!impl->shaderOf(ShaderType::TITLE_LIT, bits, &error) && error.empty()) return false;
    }
    for (uint32_t p : {TONEMAP, MITCHELL_H, MITCHELL_V}) {
        if (!impl->shaderOf(ShaderType::TITLE_3D_POST, p, &error) && error.empty()) return false;
    }
    if (!impl->cubes.count(plan.environmentKey)) {
        const std::vector<uint32_t> programs = plan.hdri ? std::vector<uint32_t>{EQUIRECT, DOWNSAMPLE, PREFILTER}
                                                         : std::vector<uint32_t>{PROCEDURAL, PREFILTER};
        for (uint32_t p : programs) {
            if (!impl->shaderOf(ShaderType::TITLE_ENV, p, &error) && error.empty()) return false;
        }
    }
    return true;
}

bool Title3DRenderer::render(const Title3DPlan& plan, const TitleRectI& region, double pxScale, const std::vector<std::string>& parts,
                             TitleRenderServices& services, TitleTarget* out, uint32_t testFlags, std::string& error) {
    if (!plan.valid || !plan.mesh || region.empty()) return services.gpu.clear(out, CLEAR, error);
    Impl& im = *impl;
    const RenderFeatures& features = SystemRender::getFeatures();
    const bool compressed = compressedLight(plan, testFlags);
    const int k = std::max(1, plan.k);
    const TitleTierLimits limits = titleTierLimits(plan.tier);
    int samples = k > 1 ? 2 : std::max(1, limits.msaa);
    if (plan.tier == TitleTier::High) samples = 2;
    if (compressed ? !features.msaaRGBA8 : (testFlags & TITLE_ART_TEST_NO_FLOAT_MSAA) != 0) samples = 1;

    Cube* env = nullptr;
    if (!plan.matcap) {
        env = im.cube(plan, error, counters);
        if (!env) return false;
    }
    GpuMesh* gpuMesh = im.mesh(plan, error, counters);
    if (!gpuMesh) return false;
    const TitleMesh& mesh = *plan.mesh;

    // ---- the uploads of this render: ramps and spans ----
    std::vector<uint8_t> ramps((size_t)256 * RAMP_ROWS * 4, 255);
    std::vector<int> rampRow(plan.materials.size(), -1);
    int rows = 0;
    for (size_t i = 0; i < plan.materials.size() && rows < RAMP_ROWS; i++) {
        if (!plan.materials[i].gradient) continue;
        bakeGradient(*plan.materials[i].gradient, &ramps[(size_t)rows * 256 * 4]);
        rampRow[i] = rows++;
    }
    TitleUpload rampTex, spanTex;
    std::vector<float> spans;
    spans.reserve(mesh.spans.size() * 8);
    for (const TitleMeshSpan& s : mesh.spans) {
        spans.insert(spans.end(), s.line, s.line + 4);
        spans.insert(spans.end(), s.chr, s.chr + 4);
    }
    if (spans.empty()) spans.assign(8, 0.0f);
    if (!rampTex.create("title 3d ramps", 256, RAMP_ROWS, ColorFormat::RGBA, ramps.data(), ramps.size(), TextureFilter::LINEAR) ||
        !spanTex.create("title 3d spans", (int)(spans.size() / 4), 1, ColorFormat::RGBA32F, spans.data(), spans.size() * 4, TextureFilter::NEAREST)) {
        error = "the 3D title's ramps or span table could not be uploaded";
        return false;
    }

    // ---- uniforms ----
    const TitleCamera3D camera = title3DCamera(plan.camera, plan.canvasWidth, plan.canvasHeight, pxScale * k,
                                               {region.x * k, region.y * k, region.width * k, region.height * k}, !features.originTopLeft);
    VsBlock vs;
    std::memset(&vs, 0, sizeof(vs));
    const Mat4 viewProj = camera.projection * camera.view;
    std::memcpy(vs.viewProj, viewProj.m.data(), sizeof(vs.viewProj));
    std::memcpy(vs.model, plan.model.m.data(), sizeof(vs.model));
    const Mat4 normalModel = normalMatrix(plan.model);
    std::memcpy(vs.normalModel, normalModel.m.data(), sizeof(vs.normalModel));
    set4(vs.skew, (float)(plan.style.skew.x), (float)(plan.style.skew.y));
    for (size_t p = 0; p < (size_t)PARTS_MAX; p++) {
        const Mat4 rig = p < plan.partRigs.size() ? plan.partRigs[p] : Mat4();
        for (int r = 0; r < 3; r++) set4(vs.parts[p * 4 + r], rig.at(r, 0), rig.at(r, 1), rig.at(r, 2), rig.at(r, 3));
        const TitleMeshPart part = p < mesh.parts.size() ? mesh.parts[p] : TitleMeshPart();
        set4(vs.parts[p * 4 + 3], (part.flags & TITLE_PART_NO_SHEAR) ? 1.0f : 0.0f, part.zFront, part.zBack);
    }

    FsBlock fs;
    std::memset(&fs, 0, sizeof(fs));
    const double exposure = plan.lighting.exposure;
    const Environment3D& e = plan.lighting.environment;
    set4(fs.camera, camera.position[0], camera.position[1], camera.position[2], (float)exposure);
    const bool shoulder = !compressed && samples > 1 && k == 1;
    set4(fs.frame, (float)e.intensity, (float)(e.rotation * 3.14159265358979323846 / 180.0), shoulder ? 1.0f : 0.0f, (float)(plan.size / 100.0));
    const int face = env ? env->face : 512;
    set4(fs.env, (float)std::log2(face / 512.0), (float)((env ? env->levels : 1) - 1), (float)e.blur,
         (e.repeatX != 1 || e.repeatY != 1) ? 1.0f : 0.0f);
    const auto tint = linear(e.tint);
    set4(fs.grade, tint[0], tint[1], tint[2], (float)e.saturation);
    // env lock: lookups with the layer's own rotation removed
    Mat4 lock;
    if (plan.style.envLock) {
        Mat4 rotation = normalMatrix(plan.model);
        for (int r = 0; r < 3; r++) {
            float l = std::sqrt(rotation.at(r, 0) * rotation.at(r, 0) + rotation.at(r, 1) * rotation.at(r, 1) + rotation.at(r, 2) * rotation.at(r, 2));
            if (l <= 0) l = 1;
            for (int c = 0; c < 3; c++) rotation.at(r, c) /= l;
        }
        // rows of the transpose: world -> head-on
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) lock.at(r, c) = rotation.at(c, r);
        }
    }
    set4(fs.lock0, lock.at(0, 0), lock.at(0, 1), lock.at(0, 2));
    set4(fs.lock1, lock.at(1, 0), lock.at(1, 1), lock.at(1, 2));
    set4(fs.lock2, lock.at(2, 0), lock.at(2, 1), lock.at(2, 2));
    set4(fs.view0, camera.view.at(0, 0), camera.view.at(0, 1), camera.view.at(0, 2));
    set4(fs.view1, camera.view.at(1, 0), camera.view.at(1, 1), camera.view.at(1, 2));
    // the layer's axes in world space (surface relief and flakes tilt along them)
    {
        float ax[3] = {plan.model.at(0, 0), plan.model.at(1, 0), plan.model.at(2, 0)};
        float ay[3] = {plan.model.at(0, 1), plan.model.at(1, 1), plan.model.at(2, 1)};
        const float lx = std::max(1e-6f, std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]));
        const float ly = std::max(1e-6f, std::sqrt(ay[0] * ay[0] + ay[1] * ay[1] + ay[2] * ay[2]));
        set4(fs.axisX, ax[0] / lx, ax[1] / lx, ax[2] / lx);
        set4(fs.axisY, ay[0] / ly, ay[1] / ly, ay[2] / ly);
    }
    int lights = 0;
    float ambient[3] = {0, 0, 0};
    for (const Light3D& l : plan.lighting.lights) {
        const auto c = linear(l.color);
        if (l.type == LightType3D::Ambient) {
            for (int i = 0; i < 3; i++) ambient[i] += c[i] * (float)l.intensity;
            continue;
        }
        if (lights >= LIGHTS_MAX) continue;
        set4(fs.lights[lights * 2], (float)l.position[0], (float)l.position[1], (float)l.position[2], l.type == LightType3D::Point ? 1.0f : 0.0f);
        set4(fs.lights[lights * 2 + 1], c[0] * (float)l.intensity, c[1] * (float)l.intensity, c[2] * (float)l.intensity);
        lights++;
    }
    set4(fs.ambient, ambient[0], ambient[1], ambient[2]);
    set4(fs.grade2, (float)e.contrast, (float)e.repeatX, (float)e.repeatY, (float)lights);
    for (size_t s = 0; s < plan.slots.size() && s < 64; s++) fs.slots[s / 4][s % 4] = (float)plan.slots[s].material;
    for (size_t i = 0; i < plan.materials.size() && i < (size_t)MATERIALS_MAX; i++) {
        const TitleMaterial3D& m = plan.materials[i];
        float(*v)[4] = &fs.materials[i * MATERIAL_VEC4S];
        const auto base = linear(m.color);
        set4(v[0], base[0], base[1], base[2], (float)m.opacity);
        set4(v[1], (float)m.metallic, (float)m.roughness, (float)m.envMultiplier, (float)m.specularMultiplier);
        set4(v[2], (float)m.clearcoat, (float)m.clearcoatRoughness, (float)m.iridescence, (float)m.iridescenceIor);
        set4(v[3], (float)m.iridescenceThicknessNm[1], (float)m.transmission, (float)m.ior, (float)m.dispersion);
        set4(v[4], (float)m.glassDepth, (float)m.flakes, (float)(m.flakeScale / 100.0 * plan.size), (float)m.gradientBend);
        const auto em = linear(m.emission);
        set4(v[5], em[0] * (float)m.emissionIntensity, em[1] * (float)m.emissionIntensity, em[2] * (float)m.emissionIntensity, (float)rampRow[i]);
        set4(v[6], (float)m.gradientKind, (float)(m.gradientAngle * 3.14159265358979323846 / 180.0), (float)m.gradientSpan,
             m.gradientDepth ? 1.0f : 0.0f);
        set4(v[7], (float)m.gradientScale, (float)m.gradientOffset);
        set4(v[8], (float)m.surfaceAmount, (float)(m.surfaceScale / 100.0 * plan.size), (float)m.surfaceSeed, m.baseWrap ? 0.0f : 1.0f);
        set4(v[9], (float)m.baseTiling[0], (float)m.baseTiling[1], (float)m.baseOffset[0], (float)m.baseOffset[1]);
    }

    // ---- the main pass ----
    TargetKey mainKey;
    mainKey.width = region.width * k;
    mainKey.height = region.height * k;
    mainKey.format = compressed ? ColorFormat::SRGB8A8 : ColorFormat::RGBA16F;
    mainKey.filter = TextureFilter::NEAREST;
    mainKey.depth = TitleDepthFormat::Depth;
    mainKey.samples = samples;
    std::string poolError;
    TargetRef main = services.pool.acquire(mainKey, &poolError);
    if (!main) {
        error = poolError.empty() ? "out_of_target_memory: the 3D main pass" : poolError;
        return false;
    }
    auto* mt = main.as<SokolTitleTarget>();
    im.pass.setColorActions(LoadAction::CLEAR, StoreAction::STORE, Vector4(0, 0, 0, 0));
    im.pass.setDepthStencilActions(LoadAction::CLEAR, StoreAction::DONTCARE, 1.0f);
    im.pass.setLabel("title 3d main");
    im.pass.startRenderPass(&mt->framebuffer());
    // the parts to colour (per-part effects render their slots alone over the whole title's depth)
    auto partOf = [&](const TitleMeshSlot& slot) -> std::string {
        switch (slot.role) {
            case TitleSlotRole::Face: return "face";
            case TitleSlotRole::Bevel: return "bevel";
            case TitleSlotRole::Side: return "side";
            default: {
                const size_t b = (size_t)std::max<int16_t>(0, slot.band);
                return b < plan.style.bands.size() ? "band:" + plan.style.bands[b].id : std::string();
            }
        }
    };
    auto drawSlots = [&](bool transparent, bool depthOnly) {
        for (size_t s = 0; s < mesh.slots.size() && s < plan.slots.size(); s++) {
            const TitleMeshSlot& slot = mesh.slots[s];
            if (!slot.indexCount) continue;
            const Title3DSlotMaterial& sm = plan.slots[s];
            const bool isTransparent = (sm.bits & TitleLitBits::TRANSPARENT) != 0;
            if (isTransparent != transparent) continue;
            const bool listed = parts.empty() || std::find(parts.begin(), parts.end(), partOf(slot)) != parts.end();
            if (!depthOnly && !listed) continue;
            const uint32_t bits = sm.bits | (compressed ? TitleLitBits::HDR_COMPRESS : 0u);
            LitObject* o = im.lit(*gpuMesh, bits, &error);
            if (!o) continue;
            PipelineState state;
            state.target.color = mainKey.format;
            state.target.depth = DepthStencilFormat::DEPTH;
            state.target.sampleCount = samples;
            state.blend = transparent ? BlendMode::PREMULTIPLIED : BlendMode::NONE;
            state.depthCompare = CompareFunc::LESS_EQUAL;
            state.depthWrite = !transparent;
            state.writeMask = depthOnly ? ColorWriteMask::NONE : ColorWriteMask::RGBA;
            if (!o->object.beginDraw(state)) continue;
            if (o->env.first >= 0) o->object.addTexture(o->env, ShaderStageType::FRAGMENT, env ? &env->texture : &im.placeholderCube);
            if (o->matcap.first >= 0) {
                TextureRender* t = plan.matcap ? plan.matcap->texture() : nullptr;
                o->object.addTexture(o->matcap, ShaderStageType::FRAGMENT, t ? t : &im.placeholder2D);
            }
            if (o->ramps.first >= 0) o->object.addTexture(o->ramps, ShaderStageType::FRAGMENT, &rampTex.texture());
            if (o->spans.first >= 0) o->object.addTexture(o->spans, ShaderStageType::FRAGMENT, &spanTex.texture());
            if (o->base.first >= 0) {
                TextureRender* t = sm.material < plan.baseTextures.size() && plan.baseTextures[sm.material] ? plan.baseTextures[sm.material]->texture()
                                                                                                         : nullptr;
                o->object.addTexture(o->base, ShaderStageType::FRAGMENT, t ? t : &im.placeholder2D);
            }
            o->object.applyUniformBlock(o->vsBlock, sizeof(VsBlock), &vs);
            o->object.applyUniformBlock(o->fsBlock, sizeof(FsBlock), &fs);
            o->object.draw(slot.firstIndex, slot.indexCount, 1);
        }
    };
    if (!parts.empty()) drawSlots(false, true);     // the whole title's depth first
    drawSlots(false, false);
    drawSlots(true, false);
    im.pass.endRenderPass();
    counters.mainPasses++;
    if (!error.empty()) return false;

    // ---- tone map (decode, expand), then Mitchell down ----
    Fullscreen* tonemap = im.program(ShaderType::TITLE_3D_POST, TONEMAP, "u_fs_title3dPost", "u_tpSrc", &error);
    Fullscreen* mitchellH = k > 1 ? im.program(ShaderType::TITLE_3D_POST, MITCHELL_H, "u_fs_title3dPost", "u_tpSrc", &error) : nullptr;
    Fullscreen* mitchellV = k > 1 ? im.program(ShaderType::TITLE_3D_POST, MITCHELL_V, "u_fs_title3dPost", "u_tpSrc", &error) : nullptr;
    if (!tonemap || (k > 1 && (!mitchellH || !mitchellV))) {
        if (error.empty()) error = "the 3D title post programs are not ready";
        return false;
    }
    PostBlock post;
    std::memset(&post, 0, sizeof(post));
    set4(post.p0, (float)plan.lighting.toneMapping, (float)exposure, shoulder ? 1.0f : 0.0f, compressed ? 1.0f : 0.0f);
    const TargetKey outKey = static_cast<SokolTitleTarget*>(out)->key();
    TargetRef toned;
    TitleTarget* tonedTarget = out;
    if (k > 1) {
        TargetKey key;
        key.width = mainKey.width;
        key.height = mainKey.height;
        key.format = outKey.format;
        key.filter = TextureFilter::NEAREST;
        toned = services.pool.acquire(key, &poolError);
        if (!toned) {
            error = poolError;
            return false;
        }
        tonedTarget = toned.get();
    }
    if (!im.fullscreenPass(*tonemap, tonedTarget, &mt->texture(), post, error)) return false;
    counters.postPasses++;
    main.reset();
    if (k > 1) {
        TargetKey key;
        key.width = region.width;
        key.height = mainKey.height;
        key.format = outKey.format;
        key.filter = TextureFilter::NEAREST;
        TargetRef half = services.pool.acquire(key, &poolError);
        if (!half) {
            error = poolError;
            return false;
        }
        set4(post.p1, (float)k, (float)(mainKey.width - 1));
        if (!im.fullscreenPass(*mitchellH, half.get(), &toned.as<SokolTitleTarget>()->texture(), post, error)) return false;
        toned.reset();
        set4(post.p1, (float)k, (float)(mainKey.height - 1));
        if (!im.fullscreenPass(*mitchellV, out, &half.as<SokolTitleTarget>()->texture(), post, error)) return false;
        counters.postPasses += 2;
    }
    counters.renders++;
    return true;
}

Title3DRenderer& jasperengine::titleart::title3DRenderer(TitleRenderServices& services) {
    if (!services.title3D) services.title3D = std::make_shared<Title3DRenderer>();
    return *static_cast<Title3DRenderer*>(services.title3D.get());
}
