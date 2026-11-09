// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleGpu.h"
#include "TitleElementVariant.h"
#include "diag/HangWatchdog.h"

#include "pool/ShaderPool.h"
#include "render/PipelineState.h"
#include "render/ShaderRender.h"
#include "render/SystemRender.h"

#include <algorithm>
#include <chrono>
#include <unordered_set>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::titleart;

const char* titleart::titleRenderProgramName(uint32_t program) {
    switch ((TitleRenderProgram)program) {
        case TitleRenderProgram::Copy: return "COPY";
        case TitleRenderProgram::Blend: return "BLEND";
        case TitleRenderProgram::AdjustMix: return "ADJUST_MIX";
        case TitleRenderProgram::MaskValue: return "MASK_VALUE";
        case TitleRenderProgram::MaskGradient: return "MASK_GRADIENT";
        case TitleRenderProgram::Blur: return "BLUR";
        case TitleRenderProgram::BlurRaw: return "BLUR_RAW";
        case TitleRenderProgram::Output: return "OUTPUT";
        case TitleRenderProgram::Seed: return "SEED";
        case TitleRenderProgram::Flood: return "FLOOD";
        case TitleRenderProgram::Resolve: return "RESOLVE";
        case TitleRenderProgram::Height: return "HEIGHT";
        case TitleRenderProgram::CoverInit: return "COVER_INIT";
        case TitleRenderProgram::CoverFlood: return "COVER_FLOOD";
        case TitleRenderProgram::CoverRadius: return "COVER_RADIUS";
        case TitleRenderProgram::ReduceMax: return "REDUCE_MAX";
        case TitleRenderProgram::Span: return "SPAN";
        case TitleRenderProgram::Element: return "ELEMENT";
        case TitleRenderProgram::Place: return "PLACE";
        case TitleRenderProgram::Erode: return "ERODE";
        case TitleRenderProgram::MaskKeep: return "MASK_KEEP";
        case TitleRenderProgram::Grow: return "GROW";
        case TitleRenderProgram::SeedAlpha: return "SEED_ALPHA";
        case TitleRenderProgram::ResolveAlpha: return "RESOLVE_ALPHA";
        case TitleRenderProgram::GlyphCompose: return "GLYPH_COMPOSE";
    }
    return "";
}

ShaderKey titleart::titleRenderProgramKey(uint32_t program) {
    return ShaderPool::getShaderKey(ShaderType::TITLE_ART, program);
}

uint64_t titleart::titleDrawKey(ShaderType type, uint64_t properties, const PipelineState& state) {
    // the state's FNV-1a, continued over the program
    uint64_t h = state.hash();
    for (uint64_t value : {(uint64_t)type, properties}) {
        for (int i = 0; i < 8; i++) {
            h ^= (value >> (8 * i)) & 0xFF;
            h *= 1099511628211ull;
        }
    }
    return h;
}

// ---- targets ----

SokolTitleTarget::SokolTitleTarget(const TargetKey& key) : targetKey(key) {
}

SokolTitleTarget::~SokolTitleTarget() {
    if (target.isCreated()) target.destroy();
}

bool SokolTitleTarget::create(std::string* error) {
    FramebufferDesc desc;
    desc.width = targetKey.width;
    desc.height = targetKey.height;
    desc.color = targetKey.format;
    switch (targetKey.depth) {
        case TitleDepthFormat::None: desc.depth = DepthStencilFormat::NONE; break;
        case TitleDepthFormat::Depth: desc.depth = DepthStencilFormat::DEPTH; break;
        case TitleDepthFormat::DepthStencil: desc.depth = DepthStencilFormat::DEPTH_STENCIL; break;
    }
    desc.sampleCount = std::max(1, targetKey.samples);
    desc.minFilter = desc.magFilter = targetKey.filter;
    desc.wrapU = desc.wrapV = TextureWrap::CLAMP_TO_EDGE;
    desc.label = "title art";
    if (!target.create(desc)) {
        if (error) {
            *error = "the title art target " + std::to_string(targetKey.width) + " x " + std::to_string(targetKey.height) +
                     " could not be made";
        }
        return false;
    }
    return true;
}

TextureRender& SokolTitleTarget::texture() {
    return target.getRender().getColorTexture();
}

TextureRender* SokolTitleTarget::colorTexture() const {
    // the framebuffer's accessors are not const; the texture is the target's own
    return &const_cast<Framebuffer&>(target).getRender().getColorTexture();
}

FramebufferRender& SokolTitleTarget::framebuffer() {
    return target.getRender();
}

std::unique_ptr<TitleTarget> SokolTitleTargetAllocator::create(const TargetKey& key, std::string* error) {
    auto target = std::make_unique<SokolTitleTarget>(key);
    if (!target->create(error)) return nullptr;
    return target;
}

// ---- uploads ----

TitleUpload::~TitleUpload() {
    if (made) render.destroyTexture();
}

bool TitleUpload::create(const char* label, int width, int height, ColorFormat format, const void* data, size_t bytes,
                         TextureFilter filter, TextureWrap wrapU, TextureWrap wrapV) {
    if (made) render.destroyTexture();
    made = false;
    void* faces[1] = {const_cast<void*>(data)};
    size_t sizes[1] = {bytes};
    if (!render.createTexture(label, width, height, format, TextureType::TEXTURE_2D, 1, faces, sizes, filter, filter, wrapU, wrapV)) return false;
    made = true;
    w = width;
    h = height;
    size = bytes;
    return true;
}

// ---- encodings ----

std::array<float, 4> titleart::titleEncoding(const TitleBufferFormat& format, TitleValueKind kind) {
    if (format.encoding == TitleEncoding::Fixed12_4) return {2.0f, 16.0f, 0.0f, 0.0f};
    if (format.encoding != TitleEncoding::Fixed16) return {0.0f, 1.0f, 0.0f, 0.0f};
    switch (kind) {
        case TitleValueKind::Distance: return {1.0f, 64.0f, 512.0f, 0.0f};
        case TitleValueKind::Unit: return {1.0f, 65535.0f, 0.0f, 0.0f};
        case TitleValueKind::Pixels: return {1.0f, 16.0f, 0.0f, 0.0f};
        case TitleValueKind::Index: return {1.0f, 1.0f, 0.0f, 0.0f};
        case TitleValueKind::Seeds: return {2.0f, 16.0f, 0.0f, 0.0f};
    }
    return {0.0f, 1.0f, 0.0f, 0.0f};
}

// ---- programs and passes ----

struct TitleGpu::Program {
    std::shared_ptr<ShaderRender> shader;
    bool requested = false;
    bool loaded = false;
    bool failed = false;
    ObjectRender object;
    int block = -1;
    unsigned int blockBytes = 0;
    int vertexBlock = -1;               // u_vs_titleart: S83's vertex-pulling programs
    unsigned int vertexBlockBytes = 0;
    std::pair<int, int> vertexData = {-1, -1};
    std::pair<int, int> tex[TITLE_TEXTURE_SLOTS];
    std::pair<int, int> raw[TITLE_RAW_SLOTS];

    Program() {
        for (auto& s : tex) s = {-1, -1};
        for (auto& s : raw) s = {-1, -1};
    }
};

TitleGpu::TitleGpu() = default;

TitleGpu::~TitleGpu() {
    release();
}

bool TitleGpu::ensurePlaceholder() {
    if (placeholderMade) return true;
    static unsigned char texel[4] = {0, 0, 0, 0};
    void* data[1] = {texel};
    size_t size[1] = {4};
    placeholderMade = placeholder.createTexture("title art placeholder", 1, 1, ColorFormat::RGBA, TextureType::TEXTURE_2D, 1, data, size,
                                                TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::CLAMP_TO_EDGE,
                                                TextureWrap::CLAMP_TO_EDGE);
    return placeholderMade;
}

void TitleGpu::release() {
    for (auto& [properties, p] : programs) {
        if (!p) continue;
        if (p->loaded) p->object.destroy();
        if (p->requested) {
            p->shader.reset();
            ShaderPool::remove(ShaderType::TITLE_ART, properties);
        }
    }
    programs.clear();
    if (placeholderMade) placeholder.destroyTexture();
    placeholderMade = false;
}

bool TitleGpu::ensureProgram(uint32_t number, std::string* error) {
    if ((number & 0x7Fu) == 0 || (number & ~(0xFFFu | TITLE_ELEMENT_VARIANT_BITS)) != 0) {
        if (error) *error = "title art program " + std::to_string(number) + " does not exist";
        return false;
    }
    std::unique_ptr<Program>& slot = programs[number];
    if (!slot) slot = std::make_unique<Program>();
    Program& p = *slot;
    if (p.loaded) return true;
    if (p.failed || ShaderPool::isShaderBuildFailed(ShaderType::TITLE_ART, number)) {
        p.failed = true;
        if (error) *error = "the title art shader " + ShaderPool::getShaderStr(ShaderType::TITLE_ART, number) + " could not be built";
        return false;
    }
    // asked again until it is built: the runtime compiler takes the requests a frame brings
    p.shader = ShaderPool::get(ShaderType::TITLE_ART, number);
    p.requested = true;
    if (!p.shader || !p.shader->isCreated()) return false;
    if (!ensurePlaceholder()) {
        if (error) *error = "the title art placeholder texture could not be made";
        return false;
    }
    ShaderData& data = p.shader->shaderData;
    p.block = data.getUniformBlockIndexByName("u_fs_titleart");
    // the renderer's programs read TITLE_PARAMS vec4s, the effects' fewer
    if (data.getUniformBlockMembers("u_fs_titleart", p.blockBytes) == nullptr) p.blockBytes = 0;
    p.blockBytes = std::min<unsigned int>(p.blockBytes, sizeof(TitleParams::v));
    for (int i = 0; i < TITLE_TEXTURE_SLOTS; i++) p.tex[i] = data.getTextureIndexByName("u_taTex" + std::to_string(i));
    for (int i = 0; i < TITLE_RAW_SLOTS; i++) p.raw[i] = data.getTextureIndexByName("u_taRaw" + std::to_string(i));
    p.vertexBlock = data.getUniformBlockIndexByName("u_vs_titleart");
    if (p.vertexBlock >= 0 && data.getUniformBlockMembers("u_vs_titleart", p.vertexBlockBytes) == nullptr) p.vertexBlockBytes = 0;
    p.vertexBlockBytes = std::min<unsigned int>(p.vertexBlockBytes, sizeof(TitleParams::v));
    p.vertexData = data.getTextureIndexByName("u_taVertexData");
    p.object.beginLoad(PrimitiveType::TRIANGLES);
    p.object.setShader(p.shader.get());
    for (const auto& s : p.tex) {
        if (s.first >= 0) p.object.addTexture(s, ShaderStageType::FRAGMENT, &placeholder);
    }
    for (const auto& s : p.raw) {
        if (s.first >= 0) p.object.addTexture(s, ShaderStageType::FRAGMENT, &placeholder);
    }
    if (p.vertexData.first >= 0) p.object.addTexture(p.vertexData, ShaderStageType::VERTEX, &placeholder);
    // a pass of a render: created at once, never skipped (a skipped pass leaves its target undefined)
    p.object.setImmediatePipelines(true);
    if (p.block < 0 || !p.object.endLoadDynamic(PrimitiveType::TRIANGLES, IndexType::NONE)) {
        p.object.destroy();
        p.failed = true;
        if (error) *error = "the title art program " + ShaderPool::getShaderStr(ShaderType::TITLE_ART, number) + " could not be loaded";
        return false;
    }
    p.loaded = true;
    return true;
}

bool TitleGpu::clear(TitleTarget* target, const float color[4], std::string& error) {
    auto* t = static_cast<SokolTitleTarget*>(target);
    if (!t) {
        error = "device_lost: a title art target is gone";
        return false;
    }
    pass.setColorActions(LoadAction::CLEAR, StoreAction::STORE, Vector4(color[0], color[1], color[2], color[3]));
    pass.setDepthStencilActions(LoadAction::DONTCARE, StoreAction::DONTCARE);
    pass.setLabel("title art clear");
    pass.startRenderPass(&t->framebuffer());
    pass.endRenderPass();
    counters.clears++;
    return true;
}

bool TitleGpu::run(const TitleGpuPass& desc, std::string& error) {
    deferredRun = false;
    auto* t = static_cast<SokolTitleTarget*>(desc.target);
    if (!t) {
        error = "device_lost: a title art target is gone";
        return false;
    }
    const uint32_t number = (uint32_t)desc.program | desc.variant;
    if (!ensureProgram(number, &error)) {
        if (error.empty()) error = std::string("the title art program ") + titleRenderProgramName(number) + " is not ready";
        return false;
    }
    Program& p = *programs[number];
    const TargetKey& key = t->key();
    PipelineState state;
    state.target.color = key.format;
    state.target.depth = key.depth == TitleDepthFormat::None ? DepthStencilFormat::NONE
                       : key.depth == TitleDepthFormat::Depth ? DepthStencilFormat::DEPTH
                                                              : DepthStencilFormat::DEPTH_STENCIL;
    state.target.sampleCount = std::max(1, key.samples);
    state.blend = desc.blend;
    // the first draw in this state compiles the driver's variant: one a frame
    const uint64_t drawKey = titleDrawKey(ShaderType::TITLE_ART, number, state);
    const bool firstDraw = !TitleFirstDraws::drew(drawKey);
    if (!TitleFirstDraws::allow(drawKey)) {
        deferredRun = true;
        return true;
    }
    const TitleRectI whole{0, 0, key.width, key.height};
    TitleRectI scissor = desc.scissor.empty() ? whole : desc.scissor;
    const int x0 = std::max(0, scissor.x), y0 = std::max(0, scissor.y);
    const int x1 = std::min(key.width, scissor.x + scissor.width), y1 = std::min(key.height, scissor.y + scissor.height);
    if (x1 <= x0 || y1 <= y0) return true;
    scissor = {x0, y0, x1 - x0, y1 - y0};
    const bool full = scissor == whole;

    TitleParams params = desc.params;
    if (!desc.keepParam0) params.set(0, (float)key.width, (float)key.height, 1.0f / (float)key.width, 1.0f / (float)key.height);

    if (desc.clear) {
        pass.setColorActions(LoadAction::CLEAR, StoreAction::STORE,
                             Vector4(desc.clearColor[0], desc.clearColor[1], desc.clearColor[2], desc.clearColor[3]));
    } else if (full && desc.blend == BlendMode::NONE) {
        // every pixel is written: the previous contents need not be loaded
        pass.setColorActions(LoadAction::DONTCARE, StoreAction::STORE);
    } else {
        pass.setColorActions(LoadAction::LOAD, StoreAction::STORE);
    }
    pass.setDepthStencilActions(LoadAction::DONTCARE, StoreAction::DONTCARE);
    pass.setLabel("title art");
    pass.startRenderPass(&t->framebuffer());
    pass.applyScissorTopLeft(scissor.x, scissor.y, scissor.width, scissor.height);
    bool drawn = false;
    const auto drawStart = std::chrono::steady_clock::now();
    if (p.object.beginDraw(state)) {
        for (int i = 0; i < TITLE_TEXTURE_SLOTS; i++) {
            if (p.tex[i].first >= 0) p.object.addTexture(p.tex[i], ShaderStageType::FRAGMENT, desc.tex[i] ? desc.tex[i] : &placeholder);
        }
        for (int i = 0; i < TITLE_RAW_SLOTS; i++) {
            if (p.raw[i].first >= 0) p.object.addTexture(p.raw[i], ShaderStageType::FRAGMENT, desc.raw[i] ? desc.raw[i] : &placeholder);
        }
        if (p.vertexData.first >= 0) p.object.addTexture(p.vertexData, ShaderStageType::VERTEX, desc.vertexData ? desc.vertexData : &placeholder);
        p.object.applyUniformBlock(p.block, p.blockBytes, params.v);
        // the vertex stage's block mirrors the first vec4s of the fragment's (generators/TitleGen.h)
        if (p.vertexBlock >= 0 && p.vertexBlockBytes > 0) p.object.applyUniformBlock(p.vertexBlock, p.vertexBlockBytes, params.v);
        p.object.draw(0, (unsigned int)std::max(3, desc.vertices), 1);
        drawn = true;
    }
    pass.endRenderPass();
    counters.passes++;
    const double drawMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - drawStart).count();
    if (drawMs > counters.slowestPassMs) {
        counters.slowestPassMs = drawMs;
        counters.slowestProgram = number;
        counters.slowestWasFirstDraw = firstDraw;
    }
    if (!drawn) {
        error = std::string("the title art program ") + titleRenderProgramName(number) + " could not draw";
        return false;
    }
    return true;
}

// ---- first draws ----

namespace {

    struct FirstDraws {
        std::unordered_set<uint64_t> drawn;
        uint64_t beats = UINT64_MAX, commits = UINT64_MAX;
        uint64_t grants = 0;
        bool unbounded = false;
        TitleFirstDrawStats stats;

        // the grants of the frame now: a frame ends at a main-loop heartbeat or at a commit (the
        // editor commits only when a view redraws, so commits alone could hold a render forever)
        uint64_t& frameGrants() {
            const uint64_t nowBeats = diag::heartbeats(), nowCommits = SystemRender::getFrameCount();
            if (nowBeats != beats || nowCommits != commits) {
                beats = nowBeats;
                commits = nowCommits;
                grants = 0;
            }
            return grants;
        }
    };

    FirstDraws& firstDraws() {
        static FirstDraws f;
        return f;
    }

}

bool TitleFirstDraws::drew(uint64_t key) {
    return firstDraws().drawn.count(key) > 0;
}

bool TitleFirstDraws::allow(uint64_t key) {
    FirstDraws& f = firstDraws();
    if (f.drawn.count(key)) return true;
    uint64_t& grants = f.frameGrants();
    if (grants > 0 && !f.unbounded) {
        f.stats.deferred++;
        return false;
    }
    f.drawn.insert(key);
    grants++;
    f.stats.granted++;
    f.stats.maxPerFrame = std::max(f.stats.maxPerFrame, grants);
    return true;
}

bool TitleFirstDraws::available() {
    FirstDraws& f = firstDraws();
    return f.unbounded || f.frameGrants() == 0;
}

const TitleFirstDrawStats& TitleFirstDraws::stats() {
    return firstDraws().stats;
}

TitleDrawGate::Result TitleDrawGate::check(ShaderType type, uint64_t properties, const PipelineState& state) {
    const uint64_t key = titleDrawKey(type, properties, state);
    if (warming && TitleFirstDraws::drew(key)) {
        skipped = true;
        return Result::Skip;
    }
    if (TitleFirstDraws::allow(key)) return Result::Draw;
    stop = true;
    return Result::Stop;
}

void TitleDrawGate::begin() {
    stop = false;
    skipped = false;
}

bool TitleDrawGate::end() {
    if (stop) {
        warming = true;
        return true;
    }
    warming = false;
    return skipped;
}

TitleFirstDraws::SynchronousScope::SynchronousScope() : previous(firstDraws().unbounded) {
    firstDraws().unbounded = true;
}

TitleFirstDraws::SynchronousScope::~SynchronousScope() {
    firstDraws().unbounded = previous;
}

void TitleFirstDraws::resetForTesting(bool unbounded) {
    FirstDraws& f = firstDraws();
    f.drawn.clear();
    f.grants = 0;
    f.beats = f.commits = UINT64_MAX;
    f.unbounded = unbounded;
    f.stats = TitleFirstDrawStats();
}

