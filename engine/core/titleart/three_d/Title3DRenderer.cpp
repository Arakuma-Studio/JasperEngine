// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The pass order and the post follow Pachislop src/engine/three/Layer3DRenderer.ts, ao.ts,
// tonemap.ts, hdrResolve.ts and resample.ts by SafEight, used with the author's permission.

#include "Title3DRenderer.h"
#include "Glint3D.h"

#include "pool/ShaderPool.h"
#include "render/BufferRender.h"
#include "render/CameraRender.h"
#include "render/ObjectRender.h"
#include "render/PipelineState.h"
#include "render/ShaderRender.h"
#include "render/SystemRender.h"
#include "render/TextureRender.h"
#include "texture/TextureData.h"
#include "titleart/render/TitleArtCaps.h"
#include "titleart/render/TitleGpu.h"
#include "titleart/effects/TitleFx.h"
#include "titleart/render/TitleRenderServices.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <set>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    constexpr int MATERIALS_MAX = 16;
    constexpr int MATERIAL_VEC4S = 14;
    constexpr int PARTS_MAX = 64;
    constexpr int LIGHTS_MAX = 4;
    constexpr int RAMP_ROWS = 16;
    // procedural cubes kept per face size (a 1024 face is about 50 MB of half floats with mips)
    constexpr size_t CUBES_1024 = 3, CUBES_2048 = 1;
    // an environment build's tile passes: at most this many px a side
    constexpr int ENV_TILE = 128;
    // a ready() call shorter than this grows the build share, a longer one than the second shrinks it
    constexpr double ENV_FAST_MS = 4.0, ENV_SLOW_MS = 16.0;
    // the gem's refraction thickness, a fraction of the source size (Pachislop GEM_THICKNESS)
    constexpr double GEM_THICKNESS = 0.3;

    enum PostProgram : uint32_t { TONEMAP = 1, MITCHELL_H = 2, MITCHELL_V = 3, AO = 4, AO_BLUR = 5, BEHIND = 6, BOX_DOWN = 7 };
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
        float view2[4], aoDepth[4], behind[4];
        float viewProj[16];
        float viewProjInv[16];
        float layerInv[16];
        float viewport[4];
        float faceBox[4], faceMix[4];
        float lights[LIGHTS_MAX * 2][4];
        float slots[16][4];
        float materials[MATERIALS_MAX * MATERIAL_VEC4S][4];
    };

    struct alignas(16) PostBlock {
        float p0[4], p1[4], p2[4], p3[4];
        float projInv[16];
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
        std::pair<int, int> source{-1, -1}, aux{-1, -1}, aux2{-1, -1};
    };

    // one Title Lit variant drawing one mesh (triangles, or the wireframe's edges)
    struct LitObject {
        ObjectRender object;
        int vsBlock = -1, fsBlock = -1;
        std::pair<int, int> env{-1, -1}, ramps{-1, -1}, spans{-1, -1}, base{-1, -1}, matcap{-1, -1}, behind{-1, -1}, pattern{-1, -1};
        std::pair<int, int> face{-1, -1}, faceEdge{-1, -1};
    };

    // a layer picture's face and edge textures (R1a)
    struct PictureGpu {
        std::shared_ptr<TitlePicture3D> picture;
        TitleUpload face, edge;
        uint64_t lastUse = 0;
    };

    struct GpuMesh {
        uint64_t key = 0;
        std::shared_ptr<const TitleMesh> mesh;
        BufferRender vertices, indices, wrap, edges;
        uint32_t edgeIndexCount = 0;
        std::map<uint32_t, std::unique_ptr<LitObject>> objects;
        std::map<uint32_t, std::unique_ptr<LitObject>> lines;
        uint64_t lastUse = 0;

        ~GpuMesh() {
            for (auto& [bits, o] : objects) o->object.destroy();
            for (auto& [bits, o] : lines) o->object.destroy();
            vertices.destroyBuffer();
            indices.destroyBuffer();
            wrap.destroyBuffer();
            edges.destroyBuffer();
        }
    };

    struct Cube {
        uint64_t key = 0;
        int face = 0, levels = 0;
        TextureRender texture;
        uint64_t lastUse = 0;
        ~Cube() { texture.destroyTexture(); }
    };

    // one tile pass of an environment build
    enum class EnvTexture : uint8_t { None, Panorama, Source, Reduce, Made };
    struct EnvItem {
        EnvProgram program = PROCEDURAL;
        EnvTexture target = EnvTexture::Source, from = EnvTexture::None;
        int face = 0, level = 0, size = 0;
        int x = 0, y = 0, w = 0, h = 0;
        float roughness = 0, downsampleMode = 0;
        int sourceLevel = 0;
        uint64_t units = 0;
    };

    // an environment cube being built, a share of its passes per ready() call
    struct EnvBuild {
        uint64_t key = 0;
        std::unique_ptr<Cube> made;
        TextureRender source, reduce;
        TitleUpload panorama;
        bool hasReduce = false;
        EnvBlock block;
        int stops = 0, elements = 0;
        float yaw = 0;
        std::vector<EnvItem> items;
        size_t next = 0;

        ~EnvBuild() {
            source.destroyTexture();
            if (hasReduce) reduce.destroyTexture();
        }
    };

    const float CLEAR[4] = {0, 0, 0, 0};

    double millisSince(std::chrono::steady_clock::time_point start) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

}

struct Title3DRenderer::Impl {
    std::map<uint64_t, std::unique_ptr<Fullscreen>> fullscreen;     // by (type << 32 | properties)
    std::map<uint64_t, std::unique_ptr<GpuMesh>> meshes;
    std::map<uint64_t, std::unique_ptr<Cube>> cubes;
    std::map<uint64_t, std::unique_ptr<EnvBuild>> builds;
    std::map<const TitlePicture3D*, std::unique_ptr<PictureGpu>> pictures;
    uint64_t envShare = TITLE_ENV_STEP_UNITS;
    // placeholders: filtering samplers, and a nearest one for the slots that declare nonfiltering
    TextureRender placeholder2D, placeholderCube, placeholderNearest;
    bool placeholders = false;
    // the gem behind copy and its scratch twin (box mips through two textures), each with a
    // nearest view for the passes that read them texel by texel
    TextureRender behindCopy, behindScratch, behindCopyRaw, behindScratchRaw;
    std::unique_ptr<Glint3D> glint;
    int behindWidth = 0, behindHeight = 0, behindLevels = 0;
    CameraRender pass;
    uint64_t clock = 0;

    ~Impl() {
        for (auto& [key, p] : fullscreen) {
            if (p->loaded) p->object.destroy();
        }
        meshes.clear();
        builds.clear();
        cubes.clear();
        releaseBehind();
        if (placeholders) {
            placeholder2D.destroyTexture();
            placeholderCube.destroyTexture();
            placeholderNearest.destroyTexture();
        }
    }

    void releaseBehind() {
        if (behindWidth > 0) {
            behindCopyRaw.destroyTexture();
            behindScratchRaw.destroyTexture();
            behindCopy.destroyTexture();
            behindScratch.destroyTexture();
        }
        behindWidth = behindHeight = behindLevels = 0;
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
                                                     TextureWrap::CLAMP_TO_EDGE) &&
                       placeholderNearest.createTexture("title 3d placeholder nearest", 1, 1, ColorFormat::RGBA, TextureType::TEXTURE_2D, 1, data,
                                                        size, TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::CLAMP_TO_EDGE,
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
        if (type == ShaderType::TITLE_3D_POST) {
            p.aux = data.getTextureIndexByName("u_tpAux");
            p.aux2 = data.getTextureIndexByName("u_tpAux2");
        }
        p.object.beginLoad(PrimitiveType::TRIANGLES);
        p.object.setShader(p.shader.get());
        const bool post = type == ShaderType::TITLE_3D_POST;
        if (p.source.first >= 0) {
            const bool cubeSource = type == ShaderType::TITLE_ENV && properties != EQUIRECT;
            p.object.addTexture(p.source, ShaderStageType::FRAGMENT, cubeSource ? &placeholderCube : post ? &placeholderNearest : &placeholder2D);
        }
        for (auto slot2 : {p.aux, p.aux2}) {
            if (slot2.first >= 0) p.object.addTexture(slot2, ShaderStageType::FRAGMENT, &placeholderNearest);
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

    // the cube texture of a build's stage
    TextureRender* envTexture(EnvBuild& b, EnvTexture t) {
        switch (t) {
            case EnvTexture::Panorama: return &b.panorama.texture();
            case EnvTexture::Source: return &b.source;
            case EnvTexture::Reduce: return &b.reduce;
            case EnvTexture::Made: return &b.made->texture;
            default: return nullptr;
        }
    }

    // the tiles of one face of one level, appended as items
    static void addTiles(std::vector<EnvItem>& items, EnvItem item, uint64_t unitsPerTexel) {
        const int size = item.size;
        for (int y = 0; y < size; y += ENV_TILE) {
            for (int x = 0; x < size; x += ENV_TILE) {
                EnvItem t = item;
                t.x = x;
                t.y = y;
                t.w = std::min(ENV_TILE, size - x);
                t.h = std::min(ENV_TILE, size - y);
                t.units = (uint64_t)t.w * (uint64_t)t.h * unitsPerTexel;
                items.push_back(t);
            }
        }
    }

    // a build's textures and its pass list (the programs are ready)
    std::unique_ptr<EnvBuild> startBuild(const Title3DEnvPlan& env, std::string& error) {
        auto b = std::make_unique<EnvBuild>();
        b->key = env.key;
        const int face = env.face;
        const int levels = titleEnvironmentLevels(face);
        const bool panorama = env.hdri != nullptr;
        auto createCube = [&](TextureRender& t) {
            return t.createFramebufferTexture(TextureType::TEXTURE_CUBE, false, false, face, face, TextureFilter::LINEAR_MIPMAP_LINEAR,
                                              TextureFilter::LINEAR, TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE, ColorFormat::RGBA16F,
                                              1, false, false, levels);
        };
        b->made = std::make_unique<Cube>();
        b->made->key = env.key;
        b->made->face = face;
        b->made->levels = levels;
        if (!createCube(b->source) || !createCube(b->made->texture)) {
            error = "the title environment cube (" + std::to_string(face) + " px) could not be made";
            return nullptr;
        }
        if (panorama) {
            b->hasReduce = createCube(b->reduce);
            if (!b->hasReduce ||
                !b->panorama.create("title environment panorama", env.hdri->width, env.hdri->height, ColorFormat::RGBA16F, env.hdri->rgba.data(),
                                    env.hdri->rgba.size() * 2, TextureFilter::LINEAR, TextureWrap::REPEAT, TextureWrap::CLAMP_TO_EDGE)) {
                error = "the title environment's panorama could not be uploaded";
                return nullptr;
            }
        }
        std::memset(&b->block, 0, sizeof(b->block));
        const TitleEnvScene& scene = *env.scene;
        b->stops = (int)std::min<size_t>(scene.sky.size(), TITLE_ENV_MAX_STOPS);
        for (int i = 0; i < b->stops; i++) std::memcpy(b->block.stops[i], scene.sky[(size_t)i].data(), sizeof(float) * 4);
        b->elements = (int)std::min<size_t>(scene.elements.size(), TITLE_ENV_MAX_ELEMENTS);
        for (int i = 0; i < b->elements; i++) {
            const TitleEnvElement& e = scene.elements[(size_t)i];
            const float extra = e.kind == TitleEnvElementKind::Disc || e.kind == TitleEnvElementKind::Tube ? e.radius : 0.0f;
            set4(b->block.elements[i * 3], (float)e.kind, e.a[0], e.a[1], e.a[2]);
            if (e.kind == TitleEnvElementKind::Tube) {
                set4(b->block.elements[i * 3 + 1], e.b[0], e.b[1], e.b[2], extra);
            } else {
                set4(b->block.elements[i * 3 + 1], e.width, e.height, 0, extra);
            }
            set4(b->block.elements[i * 3 + 2], e.radiance[0], e.radiance[1], e.radiance[2], e.edge);
        }
        b->yaw = env.hdriYaw;
        // the source chain: a procedural scene box-filtered at every level (each level evaluated on
        // its own 4 x 4 grid, so no level reads another); a panorama into the top level, then each
        // level a box of the one above through the reduce cube (a pass never samples the texture it
        // renders to); then the GGX prefilter of every level
        std::vector<EnvItem>& items = b->items;
        if (panorama) {
            for (int f = 0; f < 6; f++) {
                EnvItem item;
                item.program = EQUIRECT;
                item.target = EnvTexture::Source;
                item.from = EnvTexture::Panorama;
                item.face = f;
                item.size = face;
                addTiles(items, item, 4);
            }
            for (int level = 1; level < levels; level++) {
                const int size = std::max(1, face >> level);
                for (int f = 0; f < 6; f++) {
                    EnvItem item;
                    item.program = DOWNSAMPLE;
                    item.target = EnvTexture::Reduce;
                    item.from = EnvTexture::Source;
                    item.face = f;
                    item.level = level;
                    item.size = size;
                    item.downsampleMode = 2;
                    item.sourceLevel = level - 1;
                    addTiles(items, item, 4);
                }
                for (int f = 0; f < 6; f++) {
                    EnvItem item;
                    item.program = DOWNSAMPLE;
                    item.target = EnvTexture::Source;
                    item.from = EnvTexture::Reduce;
                    item.face = f;
                    item.level = level;
                    item.size = size;
                    item.downsampleMode = 1;
                    item.sourceLevel = level;
                    addTiles(items, item, 1);
                }
            }
        } else {
            for (int level = 0; level < levels; level++) {
                for (int f = 0; f < 6; f++) {
                    EnvItem item;
                    item.program = PROCEDURAL;
                    item.target = EnvTexture::Source;
                    item.face = f;
                    item.level = level;
                    item.size = std::max(1, face >> level);
                    addTiles(items, item, 16u * (uint64_t)(1 + b->elements));
                }
            }
        }
        for (int level = 0; level < levels; level++) {
            const float roughness = titleEnvironmentRoughness(level, face);
            for (int f = 0; f < 6; f++) {
                EnvItem item;
                item.program = PREFILTER;
                item.target = EnvTexture::Made;
                item.from = EnvTexture::Source;
                item.face = f;
                item.level = level;
                item.size = std::max(1, face >> level);
                item.roughness = roughness;
                addTiles(items, item, roughness <= 0 ? 1 : 128);
            }
        }
        return b;
    }

    // one tile pass
    bool runItem(EnvBuild& b, const EnvItem& item, int face, int levels, std::string& error) {
        Fullscreen* p = nullptr;
        switch (item.program) {
            case PROCEDURAL: p = program(ShaderType::TITLE_ENV, PROCEDURAL, "u_fs_titleEnv", nullptr, &error); break;
            case EQUIRECT: p = program(ShaderType::TITLE_ENV, EQUIRECT, "u_fs_titleEnv", "u_teEquirect", &error); break;
            case DOWNSAMPLE: p = program(ShaderType::TITLE_ENV, DOWNSAMPLE, "u_fs_titleEnv", "u_teSource", &error); break;
            case PREFILTER: p = program(ShaderType::TITLE_ENV, PREFILTER, "u_fs_titleEnv", "u_teSource", &error); break;
        }
        if (!p) {
            if (error.empty()) error = "the title environment programs are not ready";
            return false;
        }
        EnvBlock& block = b.block;
        switch (item.program) {
            case PROCEDURAL:
                set4(block.params, (float)item.face, 0, 0, (float)item.size);
                set4(block.source, (float)face, (float)levels, 0, (float)b.stops);
                set4(block.counts, (float)b.elements);
                break;
            case EQUIRECT:
                set4(block.params, (float)item.face, 0, 0, (float)face);
                set4(block.source, (float)face, (float)levels, 0, (float)b.stops);
                set4(block.counts, (float)b.elements, b.yaw);
                break;
            case DOWNSAMPLE:
                set4(block.params, (float)item.face, 0, item.downsampleMode, (float)item.size);
                set4(block.source, (float)face, (float)levels, (float)item.sourceLevel, (float)b.stops);
                break;
            case PREFILTER:
                set4(block.params, (float)item.face, item.roughness, 128, (float)item.size);
                set4(block.source, (float)face, (float)levels, 0, (float)b.stops);
                break;
        }
        const bool whole = item.w == item.size && item.h == item.size;
        pass.setColorActions(whole ? LoadAction::DONTCARE : LoadAction::LOAD, StoreAction::STORE);
        pass.setDepthStencilActions(LoadAction::DONTCARE, StoreAction::DONTCARE);
        pass.setLabel("title environment");
        pass.startRenderPass(envTexture(b, item.target), (size_t)item.face, item.level);
        if (!whole) pass.applyScissorTopLeft(item.x, item.y, item.w, item.h);
        PipelineState state;
        state.target.color = ColorFormat::RGBA16F;
        state.target.depth = DepthStencilFormat::NONE;
        state.blend = BlendMode::NONE;
        if (p->object.beginDraw(state)) {
            TextureRender* from = envTexture(b, item.from);
            if (from && p->source.first >= 0) p->object.addTexture(p->source, ShaderStageType::FRAGMENT, from);
            p->object.applyUniformBlock(p->block, sizeof(EnvBlock), &block);
            p->object.draw(0, 3, 1);
        }
        pass.endRenderPass();
        return true;
    }

    // runs a build's passes until `budget` units are spent (at least one); true when complete
    bool advance(EnvBuild& b, uint64_t budget, uint64_t& used, std::string& error) {
        const int face = b.made->face, levels = b.made->levels;
        used = 0;
        while (b.next < b.items.size()) {
            const EnvItem& item = b.items[b.next];
            if (used > 0 && used + item.units > budget) break;
            if (!runItem(b, item, face, levels, error)) return false;
            used += item.units;
            b.next++;
        }
        return b.next >= b.items.size();
    }

    // a completed build's cube into the cache (keeps 3 cubes at 1024 and 1 at 2048, least
    // recently used go first; never one of `pinned`, the render's own, which would build again
    // forever or be drawn after it went)
    Cube* finish(std::unique_ptr<EnvBuild> b, Title3DStats& stats, const std::vector<Title3DEnvPlan>& pinned) {
        std::unique_ptr<Cube> made = std::move(b->made);
        made->lastUse = ++clock;
        Cube* out = made.get();
        const uint64_t key = made->key;
        b.reset();
        cubes[key] = std::move(made);
        stats.environmentBuilds++;
        for (int faceSize : {1024, 2048}) {
            const size_t keep = faceSize == 2048 ? CUBES_2048 : CUBES_1024;
            while (true) {
                size_t count = 0;
                uint64_t oldest = UINT64_MAX;
                uint64_t victim = 0;
                for (auto& [k, c] : cubes) {
                    if (c->face != faceSize) continue;
                    count++;
                    const bool held = std::any_of(pinned.begin(), pinned.end(), [&](const Title3DEnvPlan& e) { return e.key == k; });
                    if (c->lastUse < oldest && c.get() != out && !held) {
                        oldest = c->lastUse;
                        victim = k;
                    }
                }
                if (count <= keep || oldest == UINT64_MAX) break;
                cubes.erase(victim);
            }
        }
        return out;
    }

    bool envProgramsReady(const Title3DEnvPlan& env, std::string& error) {
        const std::vector<uint32_t> programs = env.hdri ? std::vector<uint32_t>{EQUIRECT, DOWNSAMPLE, PREFILTER}
                                                        : std::vector<uint32_t>{PROCEDURAL, PREFILTER};
        for (uint32_t p : programs) {
            if (!shaderOf(ShaderType::TITLE_ENV, p, &error)) return false;
        }
        return true;
    }

    // a cooked KTX2 cube uploaded (in the device's format, or its fallback)
    Cube* uploadCooked(const Title3DEnvPlan& env, std::string& error, Title3DStats& stats) {
        TextureData data;
        const TextureFormatSupport support = SystemRender::getTextureFormatSupport();
        std::string loadError;
        if (!data.loadKtx2(env.cooked->data(), (unsigned)env.cooked->size(), 0, &support, false, &loadError)) {
            error = "the cooked title environment could not be read: " + loadError;
            return nullptr;
        }
        auto made = std::make_unique<Cube>();
        made->key = env.key;
        made->face = data.getWidth();
        made->levels = (int)data.getLevels().size();
        if (!made->texture.createTextureLevels("title environment (cooked)", TextureType::TEXTURE_CUBE, data.getGpuFormat(), data.getWidth(),
                                               data.getHeight(), 6, data.getLevels(), TextureFilter::LINEAR_MIPMAP_LINEAR, TextureFilter::LINEAR,
                                               TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE)) {
            error = "the cooked title environment could not be uploaded";
            return nullptr;
        }
        made->lastUse = ++clock;
        Cube* out = made.get();
        cubes[env.key] = std::move(made);
        stats.cookedEnvironments++;
        return out;
    }

    // the cube of an environment: cached, else uploaded (cooked) or built to the end now (a render
    // that did not wait for ready())
    Cube* cube(const Title3DEnvPlan& env, std::string& error, Title3DStats& stats, const std::vector<Title3DEnvPlan>& pinned) {
        auto it = cubes.find(env.key);
        if (it != cubes.end()) {
            it->second->lastUse = ++clock;
            return it->second.get();
        }
        if (env.cooked) return uploadCooked(env, error, stats);
        std::unique_ptr<EnvBuild>& slot = builds[env.key];
        if (!slot) slot = startBuild(env, error);
        if (!slot) {
            builds.erase(env.key);
            return nullptr;
        }
        uint64_t used = 0;
        if (!advance(*slot, UINT64_MAX, used, error)) {
            if (error.empty()) error = "the title environment could not be built";
            builds.erase(env.key);
            return nullptr;
        }
        stats.environmentMaxStepUnits = std::max(stats.environmentMaxStepUnits, used);
        std::unique_ptr<EnvBuild> done = std::move(slot);
        builds.erase(env.key);
        return finish(std::move(done), stats, pinned);
    }

    // ---- pictures ----

    // the textures of a layer's picture (uploaded once, the last few kept)
    PictureGpu* picture(const std::shared_ptr<TitlePicture3D>& p, std::string& error) {
        auto it = pictures.find(p.get());
        if (it != pictures.end()) {
            it->second->lastUse = ++clock;
            return it->second.get();
        }
        auto g = std::make_unique<PictureGpu>();
        g->picture = p;
        if (!g->face.create("title 3d picture", p->width, p->height, ColorFormat::RGBA, p->face.data(), p->face.size(), TextureFilter::LINEAR) ||
            !g->edge.create("title 3d picture edges", p->edgeWidth, p->edgeHeight, ColorFormat::RGBA, p->edge.data(), p->edge.size(),
                            TextureFilter::LINEAR)) {
            error = "the 3D layer's picture could not be uploaded";
            return nullptr;
        }
        g->lastUse = ++clock;
        PictureGpu* out = g.get();
        pictures[p.get()] = std::move(g);
        while (pictures.size() > 4) {
            auto oldest = std::min_element(pictures.begin(), pictures.end(), [](const auto& a, const auto& b) { return a.second->lastUse < b.second->lastUse; });
            if (oldest->second.get() == out) break;
            pictures.erase(oldest);
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

    // the mesh's unique edges (the wireframe debug view), made on first use
    bool ensureEdges(GpuMesh& g) {
        if (g.edgeIndexCount > 0) return true;
        const TitleMesh& m = *g.mesh;
        std::set<std::pair<uint32_t, uint32_t>> unique;
        const size_t count = m.indexCount();
        for (size_t t = 0; t + 2 < count; t += 3) {
            for (int e = 0; e < 3; e++) {
                const uint32_t a = m.index(t + (size_t)e), b = m.index(t + (size_t)((e + 1) % 3));
                if (a != b) unique.insert({std::min(a, b), std::max(a, b)});
            }
        }
        std::vector<uint32_t> lines;
        lines.reserve(unique.size() * 2);
        for (const auto& [a, b] : unique) {
            lines.push_back(a);
            lines.push_back(b);
        }
        if (lines.empty() ||
            !g.edges.createBuffer((unsigned)(lines.size() * 4), lines.data(), BufferType::INDEX_BUFFER, BufferUsage::IMMUTABLE)) {
            return false;
        }
        g.edgeIndexCount = (uint32_t)lines.size();
        return true;
    }

    LitObject* lit(GpuMesh& g, uint32_t bits, bool edges, std::string* error) {
        auto& objects = edges ? g.lines : g.objects;
        auto it = objects.find(bits);
        if (it != objects.end()) return it->second.get();
        if (edges && !ensureEdges(g)) {
            if (error) *error = "the 3D title's wireframe could not be made";
            return nullptr;
        }
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
        o->behind = data.getTextureIndexByName("u_tlBehind");
        o->pattern = data.getTextureIndexByName("u_tlPattern");
        o->face = data.getTextureIndexByName("u_tlFace");
        o->faceEdge = data.getTextureIndexByName("u_tlFaceEdge");
        const PrimitiveType primitive = edges ? PrimitiveType::LINES : PrimitiveType::TRIANGLES;
        ObjectRender& r = o->object;
        r.beginLoad(primitive);
        r.setShader(shader.get());
        const unsigned stride = sizeof(TitleMeshVertex);
        r.addAttribute(data.getAttrIndexByName("a_position"), &g.vertices, 3, AttributeDataType::FLOAT, stride, 0, false, false);
        r.addAttribute(data.getAttrIndexByName("a_normal"), &g.vertices, 2, AttributeDataType::SHORT, stride, 12, true, false);
        r.addAttribute(data.getAttrIndexByName("a_ids"), &g.vertices, 4, AttributeDataType::UNSIGNED_SHORT, stride, 16, true, false);
        if (bits & TitleLitBits::WRAP_UV) {
            r.addAttribute(data.getAttrIndexByName("a_wrap"), &g.wrap, 2, AttributeDataType::HALF, 4, 0, false, false);
        }
        const bool wide = edges || g.mesh->wideIndices;
        r.setIndex(edges ? &g.edges : &g.indices, wide ? AttributeDataType::UNSIGNED_INT : AttributeDataType::UNSIGNED_SHORT, 0);
        for (auto slot : {o->env, o->matcap}) {
            if (slot.first >= 0) r.addTexture(slot, ShaderStageType::FRAGMENT, slot == o->env ? &placeholderCube : &placeholder2D);
        }
        for (auto slot : {o->ramps, o->base, o->behind, o->pattern, o->face, o->faceEdge}) {
            if (slot.first >= 0) r.addTexture(slot, ShaderStageType::FRAGMENT, &placeholder2D);
        }
        if (o->spans.first >= 0) r.addTexture(o->spans, ShaderStageType::FRAGMENT, &placeholderNearest);
        r.setImmediatePipelines(true);
        if (o->vsBlock < 0 || o->fsBlock < 0 || !r.endLoadDynamic(primitive, wide ? IndexType::UINT32 : IndexType::UINT16)) {
            r.destroy();
            if (error) *error = "the Title Lit variant " + ShaderPool::getShaderStr(ShaderType::TITLE_LIT, bits) + " could not be loaded";
            return nullptr;
        }
        LitObject* out = o.get();
        objects[bits] = std::move(o);
        return out;
    }

    // a fullscreen post pass into a texture level (or a pool target's framebuffer)
    bool post(Fullscreen& p, TitleTarget* target, TextureRender* level, int mip, ColorFormat format, TextureRender* source,
              TextureRender* aux, TextureRender* aux2, const PostBlock& block, std::string& error) {
        pass.setColorActions(LoadAction::DONTCARE, StoreAction::STORE);
        pass.setDepthStencilActions(LoadAction::DONTCARE, StoreAction::DONTCARE);
        pass.setLabel("title 3d post");
        if (target) {
            auto* t = static_cast<SokolTitleTarget*>(target);
            pass.startRenderPass(&t->framebuffer());
            format = t->key().format;
        } else {
            pass.startRenderPass(level, 0, mip);
        }
        PipelineState state;
        state.target.color = format;
        state.target.depth = DepthStencilFormat::NONE;
        state.target.sampleCount = 1;
        state.blend = BlendMode::NONE;
        bool drawn = false;
        if (p.object.beginDraw(state)) {
            if (p.source.first >= 0) p.object.addTexture(p.source, ShaderStageType::FRAGMENT, source ? source : &placeholderNearest);
            if (p.aux.first >= 0) p.object.addTexture(p.aux, ShaderStageType::FRAGMENT, aux ? aux : &placeholderNearest);
            if (p.aux2.first >= 0) p.object.addTexture(p.aux2, ShaderStageType::FRAGMENT, aux2 ? aux2 : &placeholderNearest);
            p.object.applyUniformBlock(p.block, sizeof(PostBlock), (void*)&block);
            p.object.draw(0, 3, 1);
            drawn = true;
        }
        pass.endRenderPass();
        if (!drawn) error = "a 3D title post pass could not draw";
        return drawn;
    }

    bool fullscreenPass(Fullscreen& p, TitleTarget* target, TextureRender* source, const PostBlock& block, std::string& error,
                        TextureRender* aux = nullptr, TextureRender* aux2 = nullptr) {
        if (!target) {
            error = "device_lost: a 3D title target is gone";
            return false;
        }
        return post(p, target, nullptr, 0, ColorFormat::RGBA, source, aux, aux2, block, error);
    }

    // the gem behind copy at `width` x `height` with every level (kept between renders of that size)
    bool ensureBehind(int width, int height) {
        if (behindWidth == width && behindHeight == height) return true;
        releaseBehind();
        int levels = 1;
        while ((std::max(width, height) >> levels) > 0) levels++;
        auto make = [&](TextureRender& t) {
            return t.createFramebufferTexture(TextureType::TEXTURE_2D, false, false, width, height, TextureFilter::LINEAR_MIPMAP_LINEAR,
                                              TextureFilter::LINEAR, TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE, ColorFormat::RGBA16F,
                                              1, false, false, levels);
        };
        if (!make(behindCopy)) return false;
        if (!make(behindScratch) || !behindCopyRaw.createAliasView(behindCopy, "title 3d behind (texels)") ||
            !behindScratchRaw.createAliasView(behindScratch, "title 3d behind scratch (texels)")) {
            behindCopyRaw.destroyTexture();
            behindScratchRaw.destroyTexture();
            behindScratch.destroyTexture();
            behindCopy.destroyTexture();
            return false;
        }
        behindWidth = width;
        behindHeight = height;
        behindLevels = levels;
        return true;
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

namespace {

    bool wantsAO(const Title3DPlan& plan) {
        return plan.ao || plan.debugView == Title3DDebugView::AO;
    }

    bool floatPasses() {
        const RenderFeatures& f = SystemRender::getFeatures();
        return f.renderRGBA16F && f.filterRGBA16F;
    }

    // the glint chain of the render (the debug view shows the full chain when the layer has none);
    // it runs with or without float targets (glintFloat)
    GlintMode glintMode(const Title3DPlan& plan) {
        if (plan.glintParams.intensity <= 0) return GlintMode::Off;
        if (plan.glint == GlintMode::Off && plan.debugView == Title3DDebugView::Glint) return GlintMode::Full;
        return plan.glint;
    }

    // whether the glint's targets are RGBA16F (rendered, filtered and blended), else SRGB8A8 (R30)
    bool glintFloat(uint32_t testFlags) {
        const RenderFeatures& f = SystemRender::getFeatures();
        if (testFlags & (TITLE_ART_TEST_NO_FLOAT_TARGETS | TITLE_ART_TEST_NO_FLOAT_FILTER | TITLE_ART_TEST_NO_FLOAT_BLEND)) return false;
        return f.renderRGBA16F && f.filterRGBA16F && f.blendRGBA16F;
    }

    bool debugGeometry(const Title3DPlan& plan) {
        return plan.debugView == Title3DDebugView::Normals || plan.debugView == Title3DDebugView::Environment ||
               plan.debugView == Title3DDebugView::Wireframe;
    }

    // whether the render makes the gem behind copy (float passes, not a geometry view)
    bool behindActive(const Title3DPlan& plan, uint32_t testFlags) {
        return plan.behind && floatPasses() && !debugGeometry(plan) && !(testFlags & TITLE_ART_TEST_NO_BEHIND);
    }

    // a slot's Title Lit variant in this render
    uint32_t slotBits(const Title3DSlotMaterial& slot, bool compressed, bool behind) {
        uint32_t bits = slot.bits | (compressed ? TitleLitBits::HDR_COMPRESS : 0u);
        if (!behind) bits &= ~TitleLitBits::GEM_BEHIND;
        return bits;
    }

    // the Title Lit bits of the AO prepass and the wireframe (geometry only)
    uint32_t geometryBits(const Title3DPlan& plan, uint32_t output) {
        return (plan.mesh && !plan.mesh->rigs.empty() ? TitleLitBits::RIGS : 0u) | (output << TitleLitBits::OUTPUT_SHIFT);
    }

}

uint32_t Title3DRenderer::passCount(const Title3DPlan& plan) {
    // main, tone map, and the two Mitchell passes when supersampled; the AO prepass, SAO and its
    // two blurs; the behind copy, its levels and the main pass's second half
    uint32_t passes = 2u + (plan.k > 1 ? 2u : 0u);
    if (wantsAO(plan)) passes += 4u;
    if (plan.behind) passes += 2u + 2u * 8u;
    // the glint chain: about 30 passes in full, 20 reduced, 4 and a draw as sprites
    switch (glintMode(plan)) {
        case GlintMode::Full: passes += 30u; break;
        case GlintMode::Mobile: passes += 20u; break;
        case GlintMode::Sprites: passes += 5u; break;
        default: break;
    }
    return passes;
}

bool Title3DRenderer::ready(const Title3DPlan& plan, uint32_t testFlags) {
    const bool compressed = compressedLight(plan, testFlags);
    const bool behind = behindActive(plan, testFlags);
    std::string error;
    // the layer's picture is made on a job
    if (plan.picture && !plan.picture->isReady()) return false;
    // the programs the render draws (their first draws go together: TitleFirstDraws)
    std::vector<uint64_t> drawKeys;
    auto need = [&](ShaderType type, uint64_t properties) {
        drawKeys.push_back(((uint64_t)type << 48) ^ properties);
        return impl->shaderOf(type, properties, &error) != nullptr || !error.empty();
    };
    for (const Title3DSlotMaterial& s : plan.slots) {
        if (!need(ShaderType::TITLE_LIT, slotBits(s, compressed, behind))) return false;
    }
    std::vector<uint32_t> posts = {TONEMAP, MITCHELL_H, MITCHELL_V};
    if (wantsAO(plan)) {
        posts.insert(posts.end(), {AO, AO_BLUR});
        if (!need(ShaderType::TITLE_LIT, geometryBits(plan, 3))) return false;
    }
    if (behind) posts.insert(posts.end(), {BEHIND, BOX_DOWN});
    if (plan.debugView == Title3DDebugView::Wireframe) {
        if (!need(ShaderType::TITLE_LIT, geometryBits(plan, 1))) return false;
    }
    for (uint32_t p : posts) {
        if (!need(ShaderType::TITLE_3D_POST, p)) return false;
    }
    const GlintMode glint = glintMode(plan);
    if (glint != GlintMode::Off) {
        if (!impl->glint) impl->glint = std::make_unique<Glint3D>();
        if (!impl->glint->ready(glint, &error) && error.empty()) return false;
        drawKeys.push_back(((uint64_t)ShaderType::TITLE_GLINT << 48) ^ (uint64_t)glint);
    }
    // the environments: a share of a build per call, so a cube never stalls a frame
    for (const Title3DEnvPlan& env : plan.environments) {
        if (env.matcap || env.cooked || impl->cubes.count(env.key)) continue;
        if (!impl->envProgramsReady(env, error)) return !error.empty();
        std::unique_ptr<EnvBuild>& slot = impl->builds[env.key];
        if (!slot) {
            // the builder's programs' first draws
            const uint64_t t = (uint64_t)ShaderType::TITLE_ENV << 48;
            const std::vector<uint64_t> envKeys = env.hdri ? std::vector<uint64_t>{t ^ EQUIRECT, t ^ DOWNSAMPLE, t ^ PREFILTER}
                                                           : std::vector<uint64_t>{t ^ PROCEDURAL, t ^ PREFILTER};
            if (!TitleFirstDraws::allowGroup(envKeys)) return false;
            slot = impl->startBuild(env, error);
        }
        if (!slot) {
            impl->builds.erase(env.key);
            return true;    // the render reports the error
        }
        const auto start = std::chrono::steady_clock::now();
        const uint64_t share = (testFlags & TITLE_ART_TEST_ENV_ONE_SHOT) ? UINT64_MAX : impl->envShare;
        uint64_t used = 0;
        const bool complete = impl->advance(*slot, share, used, error);
        const double ms = millisSince(start);
        counters.environmentSteps++;
        counters.environmentMaxStepUnits = std::max(counters.environmentMaxStepUnits, used);
        if (!(testFlags & TITLE_ART_TEST_ENV_ONE_SHOT)) {
            if (ms < ENV_FAST_MS) impl->envShare = std::min(TITLE_ENV_STEP_UNITS_MAX, impl->envShare * 2);
            else if (ms > ENV_SLOW_MS) impl->envShare = std::max(TITLE_ENV_STEP_UNITS / 4, impl->envShare / 2);
        }
        if (!error.empty()) {
            impl->builds.erase(env.key);
            return true;
        }
        if (!complete) return false;
        std::unique_ptr<EnvBuild> done = std::move(slot);
        impl->builds.erase(env.key);
        impl->finish(std::move(done), counters, plan.environments);
        return false;   // the next call goes on with the next build, or renders
    }
    return TitleFirstDraws::allowGroup(drawKeys);
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

    // the environments of the layer and its parts
    std::vector<Cube*> envCubes(plan.environments.size(), nullptr);
    for (size_t i = 0; i < plan.environments.size(); i++) {
        if (plan.environments[i].matcap) continue;
        envCubes[i] = im.cube(plan.environments[i], error, counters, plan.environments);
        if (!envCubes[i]) return false;
    }
    GpuMesh* gpuMesh = im.mesh(plan, error, counters);
    if (!gpuMesh) return false;
    const TitleMesh& mesh = *plan.mesh;
    // the layer's picture (R1a)
    PictureGpu* pictureGpu = nullptr;
    if (plan.picture) {
        const std::shared_ptr<TitlePicture3D> p = plan.picture->get();
        if (p) {
            pictureGpu = im.picture(p, error);
            if (!pictureGpu) return false;
        }
    }

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
    set4(fs.camera, camera.position[0], camera.position[1], camera.position[2], (float)exposure);
    const bool shoulder = !compressed && samples > 1 && k == 1 && plan.debugView == Title3DDebugView::None;
    // the fallback's encoding (hdr_compress.glsl hd_decode): supersampled, where the float path's
    // resolve averages light, linear light so the fallback's resolve averages light too; at k = 1,
    // like the float path's shoulder, the highlight-compressing one
    const bool linearLight = compressed && k > 1;
    const float encoding = linearLight ? 2.0f : compressed ? 1.0f : 0.0f;
    // an environment's lookup uniforms (the layer's, or a part's own before its draws)
    auto setEnvironment = [&](size_t index) {
        const Title3DEnvPlan& envPlan = plan.environments[std::min(index, plan.environments.size() - 1)];
        const Environment3D& e = envPlan.environment;
        const Cube* c = index < envCubes.size() ? envCubes[index] : nullptr;
        set4(fs.frame, (float)e.intensity, (float)(e.rotation * 3.14159265358979323846 / 180.0), shoulder || linearLight ? 1.0f : 0.0f,
             (float)(plan.size / 100.0));
        const int face = c ? c->face : 512;
        set4(fs.env, (float)std::log2(face / 512.0), (float)((c ? c->levels : 1) - 1), (float)e.blur,
             (e.repeatX != 1 || e.repeatY != 1) ? 1.0f : 0.0f);
        const auto tint = linear(e.tint);
        set4(fs.grade, tint[0], tint[1], tint[2], (float)e.saturation);
        fs.grade2[0] = (float)e.contrast;
        fs.grade2[1] = (float)e.repeatX;
        fs.grade2[2] = (float)e.repeatY;
    };
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
    auto setView = [&](const TitleCamera3D& c) {
        set4(fs.view0, c.view.at(0, 0), c.view.at(0, 1), c.view.at(0, 2), c.view.at(0, 3));
        set4(fs.view1, c.view.at(1, 0), c.view.at(1, 1), c.view.at(1, 2), c.view.at(1, 3));
        set4(fs.view2, c.view.at(2, 0), c.view.at(2, 1), c.view.at(2, 2), c.view.at(2, 3));
        const Mat4 vp = c.projection * c.view;
        std::memcpy(fs.viewProj, vp.m.data(), sizeof(fs.viewProj));
        const Mat4 vpInv = inverse(vp);
        std::memcpy(fs.viewProjInv, vpInv.m.data(), sizeof(fs.viewProjInv));
    };
    {
        const Mat4 layerInv = inverse(plan.model);
        std::memcpy(fs.layerInv, layerInv.m.data(), sizeof(fs.layerInv));
    }
    setView(camera);
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
    fs.grade2[3] = (float)lights;
    setEnvironment(0);
    for (size_t s = 0; s < plan.slots.size() && s < 64; s++) fs.slots[s / 4][s % 4] = (float)plan.slots[s].material;
    for (size_t i = 0; i < plan.materials.size() && i < (size_t)MATERIALS_MAX; i++) {
        const TitleMaterial3D& m = plan.materials[i];
        float(*v)[4] = &fs.materials[i * MATERIAL_VEC4S];
        // a gradient overrides the colour (Pachislop: the material's colour white under a ramp)
        const auto base = m.gradient ? std::array<float, 3>{1, 1, 1} : linear(m.color);
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
        // the pattern: layer units -> motif units (Pachislop patternMatrix), its colours linear
        if (m.hasPattern()) {
            const Json& p = m.pattern;
            const double cell = std::max(1e-3, p.value("scale", 12.0) / 100.0 * plan.size);
            const double a = p.value("angle", 0.0) * 3.14159265358979323846 / 180.0;
            double aspect = 1;
            const std::shared_ptr<TitleCacheEntry>& image = i < plan.patternTextures.size() ? plan.patternTextures[i] : nullptr;
            if (image && image->upload && image->upload->height() > 0) aspect = (double)image->upload->width() / image->upload->height();
            const double c = std::cos(a) / cell, s = std::sin(a) / cell;
            TitleColor ink{0, 0, 0}, ink2{1, 1, 1};
            if (p.contains("color")) parseTitleColor(p["color"], ink);
            if (p.contains("color2")) parseTitleColor(p["color2"], ink2);
            const auto color = linear(ink);
            const auto color2 = linear(ink2);
            set4(v[10], (float)fx::patternKindId(p.value("kind", std::string("none"))), (float)p.value("seed", 1), (float)p.value("opacity", 1.0),
                 (float)p.value("shade", 0.6));
            set4(v[11], color[0], color[1], color[2], (float)p.value("relief", 0.0));
            set4(v[12], color2[0], color2[1], color2[2], (float)cell);
            set4(v[13], (float)c, (float)-s, (float)(s * aspect), (float)(c * aspect));
        }
    }
    set4(fs.viewport, (float)(region.width * k), (float)(region.height * k), (float)plan.style.skew.x, (float)plan.style.skew.y);
    if (pictureGpu) {
        const TitlePicture3D& p = *pictureGpu->picture;
        set4(fs.faceBox, (float)p.x0, (float)p.y0, (float)p.x1, (float)p.y1);
        set4(fs.faceMix, (float)plan.faceImage, (float)p.edgeAlpha, (float)plan.edgeImage);
    } else {
        set4(fs.faceBox, -1, -1, 1, 1);
        set4(fs.faceMix, 0, 0.5f, 0);
    }
    // the title's bounding sphere (world): the AO prepass's depth encoding
    std::array<float, 3> centreWorld{0, 0, 0};
    float radiusWorld = 1;
    {
        const std::array<float, 4> c = plan.model.apply({(mesh.boundsMin[0] + mesh.boundsMax[0]) * 0.5f, (mesh.boundsMin[1] + mesh.boundsMax[1]) * 0.5f,
                                                         (mesh.boundsMin[2] + mesh.boundsMax[2]) * 0.5f, 1});
        centreWorld = {c[0], c[1], c[2]};
        float d2 = 0;
        for (int i = 0; i < 3; i++) d2 += (mesh.boundsMax[i] - mesh.boundsMin[i]) * (mesh.boundsMax[i] - mesh.boundsMin[i]);
        radiusWorld = std::max(1e-3f, 0.5f * std::sqrt(d2) * (float)plan.layerScale);
    }
    auto viewDepth = [&](const TitleCamera3D& c) {
        return -(c.view.at(2, 0) * centreWorld[0] + c.view.at(2, 1) * centreWorld[1] + c.view.at(2, 2) * centreWorld[2] + c.view.at(2, 3));
    };

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

    // ---- the AO prepass, SAO and its blurs at output resolution (R31) ----
    std::string poolError;
    TargetRef aoTarget;
    if (wantsAO(plan) && floatPasses()) {
        const TitleCamera3D aoCamera = title3DCamera(plan.camera, plan.canvasWidth, plan.canvasHeight, pxScale, region, !features.originTopLeft);
        TargetKey ndKey;
        ndKey.width = region.width;
        ndKey.height = region.height;
        ndKey.format = ColorFormat::RGBA16F;
        ndKey.filter = TextureFilter::NEAREST;
        ndKey.depth = TitleDepthFormat::Depth;
        TargetRef nd = services.pool.acquire(ndKey, &poolError);
        TargetKey aoKey = ndKey;
        aoKey.depth = TitleDepthFormat::None;
        TargetRef aoA = nd ? services.pool.acquire(aoKey, &poolError) : TargetRef();
        TargetRef aoB = aoA ? services.pool.acquire(aoKey, &poolError) : TargetRef();
        LitObject* prepass = im.lit(*gpuMesh, geometryBits(plan, 3), false, &error);
        Fullscreen* sao = im.program(ShaderType::TITLE_3D_POST, AO, "u_fs_title3dPost", "u_tpSrc", &error);
        Fullscreen* blur = im.program(ShaderType::TITLE_3D_POST, AO_BLUR, "u_fs_title3dPost", "u_tpSrc", &error);
        if (!nd || !aoA || !aoB || !prepass || !sao || !blur) {
            if (error.empty()) error = poolError.empty() ? "the 3D title's AO passes could not be set up" : poolError;
            return false;
        }
        VsBlock aoVs = vs;
        const Mat4 aoViewProj = aoCamera.projection * aoCamera.view;
        std::memcpy(aoVs.viewProj, aoViewProj.m.data(), sizeof(aoVs.viewProj));
        FsBlock aoFs = fs;
        {
            FsBlock saved = fs;
            setView(aoCamera);
            aoFs = fs;
            fs = saved;
        }
        const float zCentre = viewDepth(aoCamera);
        set4(aoFs.aoDepth, zCentre, radiusWorld);
        auto* ndt = nd.as<SokolTitleTarget>();
        im.pass.setColorActions(LoadAction::CLEAR, StoreAction::STORE, Vector4(0, 0, 0, 0));
        im.pass.setDepthStencilActions(LoadAction::CLEAR, StoreAction::DONTCARE, 1.0f);
        im.pass.setLabel("title 3d ao prepass");
        im.pass.startRenderPass(&ndt->framebuffer());
        PipelineState state;
        state.target.color = ColorFormat::RGBA16F;
        state.target.depth = DepthStencilFormat::DEPTH;
        state.target.sampleCount = 1;
        state.blend = BlendMode::NONE;
        state.depthCompare = CompareFunc::LESS_EQUAL;
        state.depthWrite = true;
        if (prepass->object.beginDraw(state)) {
            if (prepass->env.first >= 0) prepass->object.addTexture(prepass->env, ShaderStageType::FRAGMENT, &im.placeholderCube);
            for (auto slot : {prepass->ramps, prepass->base, prepass->matcap, prepass->behind}) {
                if (slot.first >= 0) prepass->object.addTexture(slot, ShaderStageType::FRAGMENT, &im.placeholder2D);
            }
            if (prepass->spans.first >= 0) prepass->object.addTexture(prepass->spans, ShaderStageType::FRAGMENT, &im.placeholderNearest);
            prepass->object.applyUniformBlock(prepass->vsBlock, sizeof(VsBlock), &aoVs);
            prepass->object.applyUniformBlock(prepass->fsBlock, sizeof(FsBlock), &aoFs);
            prepass->object.draw(0, (unsigned)mesh.indexCount(), 1);
        }
        im.pass.endRenderPass();
        PostBlock block;
        std::memset(&block, 0, sizeof(block));
        const double radiusView = std::max(1e-3, plan.aoParams.radius / 100.0 * plan.size * plan.layerScale);
        const double projScale = region.height * 0.5 * std::abs(aoCamera.projection.at(1, 1));
        set4(block.p1, (float)radiusView, (float)projScale, (float)std::max(0.0, plan.aoParams.intensity), features.originTopLeft ? 1.0f : 0.0f);
        set4(block.p2, (float)region.x, (float)region.y, zCentre, radiusWorld);
        const Mat4 projInv = inverse(aoCamera.projection);
        std::memcpy(block.projInv, projInv.m.data(), sizeof(block.projInv));
        if (!im.fullscreenPass(*sao, aoA.get(), &ndt->texture(), block, error)) return false;
        set4(block.p1, 1, 0);
        if (!im.fullscreenPass(*blur, aoB.get(), &ndt->texture(), block, error, &aoA.as<SokolTitleTarget>()->texture())) return false;
        set4(block.p1, 0, 1);
        if (!im.fullscreenPass(*blur, aoA.get(), &ndt->texture(), block, error, &aoB.as<SokolTitleTarget>()->texture())) return false;
        counters.aoPasses += 4;
        aoTarget = std::move(aoA);
    }

    // ---- the main pass ----
    TargetKey mainKey;
    mainKey.width = region.width * k;
    mainKey.height = region.height * k;
    mainKey.format = compressed ? ColorFormat::SRGB8A8 : ColorFormat::RGBA16F;
    mainKey.filter = TextureFilter::NEAREST;
    mainKey.depth = TitleDepthFormat::Depth;
    mainKey.samples = samples;
    TargetRef main = services.pool.acquire(mainKey, &poolError);
    if (!main) {
        error = poolError.empty() ? "out_of_target_memory: the 3D main pass" : poolError;
        return false;
    }
    auto* mt = main.as<SokolTitleTarget>();
    // the gem behind copy: the opaque parts resolved, box-mipped (needs float passes)
    const bool behind = behindActive(plan, testFlags);
    set4(fs.behind, 0, (float)(GEM_THICKNESS * plan.size * plan.layerScale), 0, features.originTopLeft ? 1.0f : 0.0f);
    auto startMain = [&](bool clearColor) {
        im.pass.setColorActions(clearColor ? LoadAction::CLEAR : LoadAction::LOAD, StoreAction::STORE, Vector4(0, 0, 0, 0));
        im.pass.setDepthStencilActions(LoadAction::CLEAR, StoreAction::DONTCARE, 1.0f);
        im.pass.setLabel("title 3d main");
        im.pass.startRenderPass(&mt->framebuffer());
    };
    startMain(true);
    auto drawSlots = [&](bool transparent, bool depthOnly, bool everyPart) {
        for (size_t s = 0; s < mesh.slots.size() && s < plan.slots.size(); s++) {
            const TitleMeshSlot& slot = mesh.slots[s];
            if (!slot.indexCount) continue;
            const Title3DSlotMaterial& sm = plan.slots[s];
            const bool isTransparent = (sm.bits & TitleLitBits::TRANSPARENT) != 0;
            if (isTransparent != transparent) continue;
            const bool listed = everyPart || parts.empty() || std::find(parts.begin(), parts.end(), partOf(slot)) != parts.end();
            if (!depthOnly && !listed) continue;
            LitObject* o = im.lit(*gpuMesh, slotBits(sm, compressed, behind), false, &error);
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
            const size_t envIndex = std::min<size_t>(sm.environment, plan.environments.size() - 1);
            setEnvironment(envIndex);
            const Title3DEnvPlan& envPlan = plan.environments[envIndex];
            if (o->env.first >= 0) {
                Cube* c = envCubes[envIndex];
                o->object.addTexture(o->env, ShaderStageType::FRAGMENT, c ? &c->texture : &im.placeholderCube);
            }
            if (o->matcap.first >= 0) {
                TextureRender* t = envPlan.matcap ? envPlan.matcap->texture() : nullptr;
                o->object.addTexture(o->matcap, ShaderStageType::FRAGMENT, t ? t : &im.placeholder2D);
            }
            if (o->ramps.first >= 0) o->object.addTexture(o->ramps, ShaderStageType::FRAGMENT, &rampTex.texture());
            if (o->spans.first >= 0) o->object.addTexture(o->spans, ShaderStageType::FRAGMENT, &spanTex.texture());
            if (o->base.first >= 0) {
                TextureRender* t = sm.material < plan.baseTextures.size() && plan.baseTextures[sm.material] ? plan.baseTextures[sm.material]->texture()
                                                                                                         : nullptr;
                o->object.addTexture(o->base, ShaderStageType::FRAGMENT, t ? t : &im.placeholder2D);
            }
            if (o->behind.first >= 0) o->object.addTexture(o->behind, ShaderStageType::FRAGMENT, behind ? &im.behindCopy : &im.placeholder2D);
            if (o->face.first >= 0) o->object.addTexture(o->face, ShaderStageType::FRAGMENT, pictureGpu ? &pictureGpu->face.texture() : &im.placeholder2D);
            if (o->faceEdge.first >= 0) {
                o->object.addTexture(o->faceEdge, ShaderStageType::FRAGMENT, pictureGpu ? &pictureGpu->edge.texture() : &im.placeholder2D);
            }
            if (o->pattern.first >= 0) {
                TextureRender* t = sm.material < plan.patternTextures.size() && plan.patternTextures[sm.material]
                                       ? plan.patternTextures[sm.material]->texture()
                                       : nullptr;
                o->object.addTexture(o->pattern, ShaderStageType::FRAGMENT, t ? t : &im.placeholder2D);
            }
            o->object.applyUniformBlock(o->vsBlock, sizeof(VsBlock), &vs);
            o->object.applyUniformBlock(o->fsBlock, sizeof(FsBlock), &fs);
            o->object.draw(slot.firstIndex, slot.indexCount, 1);
        }
    };
    if (plan.debugView == Title3DDebugView::Wireframe) {
        // the mesh's edges coloured by their normals, depth tested against the solid title
        drawSlots(false, true, true);
        drawSlots(true, true, true);
        LitObject* wire = im.lit(*gpuMesh, geometryBits(plan, 1) | (compressed ? TitleLitBits::HDR_COMPRESS : 0u), true, &error);
        if (wire) {
            PipelineState state;
            state.target.color = mainKey.format;
            state.target.depth = DepthStencilFormat::DEPTH;
            state.target.sampleCount = samples;
            state.blend = BlendMode::NONE;
            state.depthCompare = CompareFunc::LESS_EQUAL;
            state.depthWrite = false;
            if (wire->object.beginDraw(state)) {
                if (wire->env.first >= 0) wire->object.addTexture(wire->env, ShaderStageType::FRAGMENT, &im.placeholderCube);
                for (auto slot : {wire->ramps, wire->base, wire->matcap, wire->behind}) {
                    if (slot.first >= 0) wire->object.addTexture(slot, ShaderStageType::FRAGMENT, &im.placeholder2D);
                }
                if (wire->spans.first >= 0) wire->object.addTexture(wire->spans, ShaderStageType::FRAGMENT, &im.placeholderNearest);
                wire->object.applyUniformBlock(wire->vsBlock, sizeof(VsBlock), &vs);
                wire->object.applyUniformBlock(wire->fsBlock, sizeof(FsBlock), &fs);
                wire->object.draw(0, gpuMesh->edgeIndexCount, 1);
            }
        }
    } else {
        if (!parts.empty()) drawSlots(false, true, false);     // the whole title's depth first
        drawSlots(false, false, false);
        if (behind) {
            // the opaque parts resolved into the behind copy and its levels, then the pass again
            // for the gems (the opaque depth drawn anew)
            im.pass.endRenderPass();
            counters.mainPasses++;
            Fullscreen* copy = im.program(ShaderType::TITLE_3D_POST, BEHIND, "u_fs_title3dPost", "u_tpSrc", &error);
            Fullscreen* down = im.program(ShaderType::TITLE_3D_POST, BOX_DOWN, "u_fs_title3dPost", "u_tpSrc", &error);
            if (!copy || !down || !im.ensureBehind(mainKey.width, mainKey.height)) {
                if (error.empty()) error = "the gem behind copy could not be set up";
                return false;
            }
            PostBlock block;
            std::memset(&block, 0, sizeof(block));
            const bool behindShoulder = !compressed && samples > 1 && k == 1;
            set4(block.p0, 0, (float)exposure, behindShoulder ? 1.0f : 0.0f, encoding);
            if (!im.post(*copy, nullptr, &im.behindCopy, 0, ColorFormat::RGBA16F, &mt->texture(), nullptr, nullptr, block, error)) return false;
            for (int level = 1; level < im.behindLevels; level++) {
                set4(block.p1, (float)(level - 1), 0);
                if (!im.post(*down, nullptr, &im.behindScratch, level, ColorFormat::RGBA16F, nullptr, &im.behindCopyRaw, nullptr, block, error)) return false;
                set4(block.p1, (float)level, 1);
                if (!im.post(*down, nullptr, &im.behindCopy, level, ColorFormat::RGBA16F, nullptr, &im.behindScratchRaw, nullptr, block, error)) return false;
            }
            counters.behindCopies++;
            // the whole render's width: a region's preview mips as the whole render would
            const double frameSpan = (double)plan.canvasWidth * pxScale / std::max(1, region.width);
            set4(fs.behind, (float)std::log2(std::max(1.0, mainKey.width * frameSpan)), (float)(GEM_THICKNESS * plan.size * plan.layerScale), 1,
                 features.originTopLeft ? 1.0f : 0.0f);
            startMain(false);
            drawSlots(false, true, true);
        }
        drawSlots(true, false, false);
    }
    im.pass.endRenderPass();
    counters.mainPasses++;
    if (!error.empty()) return false;

    // ---- the glint on the pre-tone-map image (R32) ----
    Glint3DOutput glintOut;
    const GlintMode glint = glintMode(plan);
    if (glint != GlintMode::Off) {
        if (!im.glint) im.glint = std::make_unique<Glint3D>();
        Glint3DInput gi;
        gi.source = &mt->texture();
        gi.sourceWidth = mainKey.width;
        gi.sourceHeight = mainKey.height;
        gi.compressed = compressed;
        gi.linearLight = linearLight;
        gi.shoulder = shoulder;
        gi.exposure = exposure;
        gi.k = k;
        gi.region = region;
        gi.frameWidth = plan.canvasWidth * pxScale;
        gi.frameHeight = plan.canvasHeight * pxScale;
        gi.pxScale = pxScale;
        gi.unit = plan.size / 100.0 * plan.layerScale * pxScale;
        gi.params = plan.glintParams;
        gi.mode = glint;
        gi.originTopLeft = features.originTopLeft;
        gi.floatTargets = glintFloat(testFlags);
        if (!im.glint->render(gi, services.pool, glintOut, error)) return false;
        counters.glintPasses += glintOut.passes;
    }

    // ---- tone map (decode, expand, AO, glint), then Mitchell down ----
    Fullscreen* tonemap = im.program(ShaderType::TITLE_3D_POST, TONEMAP, "u_fs_title3dPost", "u_tpSrc", &error);
    Fullscreen* mitchellH = k > 1 ? im.program(ShaderType::TITLE_3D_POST, MITCHELL_H, "u_fs_title3dPost", "u_tpSrc", &error) : nullptr;
    Fullscreen* mitchellV = k > 1 ? im.program(ShaderType::TITLE_3D_POST, MITCHELL_V, "u_fs_title3dPost", "u_tpSrc", &error) : nullptr;
    if (!tonemap || (k > 1 && (!mitchellH || !mitchellV))) {
        if (error.empty()) error = "the 3D title post programs are not ready";
        return false;
    }
    PostBlock post;
    std::memset(&post, 0, sizeof(post));
    set4(post.p0, (float)plan.lighting.toneMapping, (float)exposure, shoulder ? 1.0f : 0.0f, encoding);
    float debug = 0;
    if (plan.debugView == Title3DDebugView::AO) debug = 1;
    else if (plan.debugView == Title3DDebugView::Glint) debug = 2;
    else if (plan.debugView == Title3DDebugView::Ink) debug = 3;
    set4(post.p2, aoTarget ? 1.0f : 0.0f, glintOut.light ? 1.0f : 0.0f, debug, glintOut.lightScale);
    set4(post.p3, glintOut.uv[0], glintOut.uv[1], glintOut.uv[2], glintOut.uv[3]);
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
    TextureRender* aoTexture = aoTarget ? &aoTarget.as<SokolTitleTarget>()->texture() : nullptr;
    TextureRender* glintTexture = glintOut.light ? &glintOut.light.as<SokolTitleTarget>()->texture() : nullptr;
    if (!im.fullscreenPass(*tonemap, tonedTarget, &mt->texture(), post, error, aoTexture, glintTexture)) return false;
    counters.postPasses++;
    main.reset();
    aoTarget.reset();
    glintOut.light.reset();
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
