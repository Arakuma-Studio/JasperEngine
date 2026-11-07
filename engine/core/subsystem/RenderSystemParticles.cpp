// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// RenderSystem's particle systems (docs/specs/S20-particles.md D2 steps 3-4, D6, D7, R11-R13,
// R17): per (system, view) the live particles are packed (interpolated, the appearance modules
// at their age, thinned, sorted when the blend needs it) into this frame's streams with
// sg_append_buffer, then drawn in one instanced draw (one per mesh in Mesh mode, one more for
// trails) with only per-instance vertex attributes: no storage buffers, no compute.

#include "RenderSystem.h"

#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "jobs/JobSystem.h"
#include "memory/Arena.h"
#include "particles/ParticleMeshes.h"
#include "particles/ParticleRender.h"
#include "pool/ShaderPool.h"
#include "profiler/Profile.h"
#include "render/SystemRender.h"
#include "subsystem/ParticleSubsystem.h"
#include "util/Color.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::particles;

namespace {

    constexpr int kMaxSpriteRects = 64;

    // std140 prefix of u_vs_particlesParams (includes/particles_vs.glsl); the lights and the
    // sprite rects follow when the variant declares them
    struct vs_particles_params_t {
        Matrix4 modelMatrix;
        Vector4 mode;
        Vector4 sheet;
        Vector4 stretch;
        Vector4 pivot;
        Vector4 screenSize;
        Vector4 cameraVelocity;
        Vector4 tiling;
    };
    static_assert(sizeof(vs_particles_params_t) == 64 + 7 * 16, "vs_particles_params_t must match u_vs_particlesParams");

    struct fs_particles_params_t {
        Vector4 blend;
        Vector4 emissionColor;
        Vector4 sixWay;
    };

    MaterialBlendMode materialBlend(ParticleBlend blend){
        switch (blend){
            case ParticleBlend::Premultiplied: return MaterialBlendMode::PREMULTIPLIED;
            case ParticleBlend::Additive:      return MaterialBlendMode::ADDITIVE;
            case ParticleBlend::Multiply:      return MaterialBlendMode::MULTIPLY;
            case ParticleBlend::Alpha:
            default:                           return MaterialBlendMode::ALPHA;
        }
    }

    Matrix4 matrixOf(const float m[16]){
        Matrix4 out;
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) out[c][r] = m[c * 4 + r];
        return out;
    }

    std::shared_ptr<const ShaderManifest> manifestOf(const std::string& shader){
        if (shader.empty())
            return nullptr;
        std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(shader);
        return (manifest && manifest->present) ? manifest : nullptr;
    }

    void applyCustomBlock(ObjectRender& render, CustomUniformBlock& block, float frameTime, const Vector2& resolution){
        if (block.slot == -1)
            return;
        block.writeTime(frameTime);
        block.writeResolution(resolution.x, resolution.y);
        render.applyUniformBlock(block.slot, (unsigned int)block.data.size(), block.data.data());
    }

    uint64_t mix(uint64_t h, uint64_t v){
        h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        return h;
    }

    uint64_t mixString(uint64_t h, const std::string& s){
        for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
        return mix(h, s.size());
    }

}

// ---------------------------------------------------------------- the frame's streams

bool RenderSystem::ensureParticleBuffers(){
    if (particleStaticBuffers)
        return true;
    JE_GPU_ALLOC_SCOPE(Particles, "Particle quads");
    static const float corners[8] = {-0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f};
    static const uint16_t quad[6] = {0, 1, 2, 0, 2, 3};
    static const unsigned char zeros[64] = {};
    bool ok = particleCornerBuffer.createBuffer(sizeof(corners), (void*)corners, BufferType::VERTEX_BUFFER, BufferUsage::IMMUTABLE);
    ok = particleQuadIndexBuffer.createBuffer(sizeof(quad), (void*)quad, BufferType::INDEX_BUFFER, BufferUsage::IMMUTABLE) && ok;
    ok = particleInstancePlaceholder.createBuffer(sizeof(zeros), (void*)zeros, BufferType::VERTEX_BUFFER, BufferUsage::IMMUTABLE) && ok;
    ok = particleRotationPlaceholder.createBuffer(sizeof(zeros), (void*)zeros, BufferType::VERTEX_BUFFER, BufferUsage::IMMUTABLE) && ok;
    ok = particleCustomPlaceholder.createBuffer(sizeof(zeros), (void*)zeros, BufferType::VERTEX_BUFFER, BufferUsage::IMMUTABLE) && ok;
    ok = particleTrailPlaceholder.createBuffer(sizeof(zeros), (void*)zeros, BufferType::VERTEX_BUFFER, BufferUsage::IMMUTABLE) && ok;
    particleStaticBuffers = ok;
    return ok;
}

void RenderSystem::destroyParticleBuffers(){
    for (ParticleStream* stream : {&particleInstanceStream, &particleRotationStream, &particleCustomStream, &particleTrailStream}){
        for (auto& page : stream->pages) page->destroyBuffer();
        for (auto& page : stream->retired) page.first->destroyBuffer();
        stream->pages.clear();
        stream->retired.clear();
        stream->capacity = 0;
        stream->used = 0;
    }
    if (particleStaticBuffers){
        particleCornerBuffer.destroyBuffer();
        particleQuadIndexBuffer.destroyBuffer();
        particleInstancePlaceholder.destroyBuffer();
        particleRotationPlaceholder.destroyBuffer();
        particleCustomPlaceholder.destroyBuffer();
        particleTrailPlaceholder.destroyBuffer();
        particleStaticBuffers = false;
    }
}

bool RenderSystem::appendParticleStream(ParticleStream& stream, const void* data, size_t bytes, BufferRender*& buffer, size_t& offset){
    const uint64_t frame = SystemRender::getFrameCount();
    if (stream.frame != frame){
        stream.frame = frame;
        stream.used = 0;
        // the pages that filled two frames ago are no longer read by the GPU
        for (size_t i = 0; i < stream.retired.size();){
            if (stream.retired[i].second + 2 <= frame){
                stream.retired[i].first->destroyBuffer();
                stream.retired.erase(stream.retired.begin() + (long)i);
            }else{
                i++;
            }
        }
        // a frame's appends go to the newest page only
        while (stream.pages.size() > 1){
            stream.retired.push_back({std::move(stream.pages.front()), frame});
            stream.pages.erase(stream.pages.begin());
        }
    }
    // sokol advances the append position by the size rounded up to 4
    const size_t step = (bytes + 3) / 4 * 4;
    if (stream.pages.empty() || stream.used + step > stream.capacity){
        // a page twice the last (outside the steady state: the streams keep their high water)
        size_t capacity = std::max<size_t>(stream.capacity * 2, 256 * 1024);
        while (capacity < step) capacity *= 2;
        if (!stream.pages.empty())
            stream.retired.push_back({std::move(stream.pages.back()), frame});
        stream.pages.clear();
        std::unique_ptr<BufferRender> page = std::make_unique<BufferRender>();
        JE_GPU_ALLOC_SCOPE(Particles, "Particle stream");
        if (!page->createBuffer((unsigned int)capacity, nullptr, stream.type, BufferUsage::STREAM)){
            Log::error("[Particles] could not create a %u-byte particle stream", (unsigned int)capacity);
            return false;
        }
        stream.pages.push_back(std::move(page));
        stream.capacity = capacity;
        stream.used = 0;
    }
    BufferRender* page = stream.pages.back().get();
    const int appended = page->appendBuffer((unsigned int)bytes, data);
    if (appended < 0)
        return false;
    buffer = page;
    offset = (size_t)appended;
    stream.used = (size_t)appended + step;
    return true;
}

// ---------------------------------------------------------------- loading

RenderSystem::ParticleVariantSet RenderSystem::getParticleVariants(const CompiledEffect& effect, bool fog){
    const RendererModule& r = effect.document.renderer;
    const RenderParams& rp = effect.program.render;
    ParticleVariantSet set;
    set.quads = r.renderMode != RenderMode::Mesh && r.renderMode != RenderMode::None;
    set.meshes = r.renderMode == RenderMode::Mesh;
    set.trails = effect.program.trails.enabled;
    set.reference = r.shader.empty() ? std::string() : getComponentShaderReference(r.shader, r.shaderFeatures, materialBlend(r.blend));

    ParticlesShaderFlags flags;
    flags.texture = !r.texture.empty();
    flags.stretch = r.renderMode == RenderMode::StretchedBillboard;
    flags.frameBlend = rp.frameBlending != 0;
    flags.lit = r.lighting == LightingMode::Lit;
    flags.sixWay = r.lighting == LightingMode::SixWay;
    flags.customData = rp.customData != 0;
    flags.receiveShadows = r.receiveShadows;
    flags.fog = fog;
    set.quadProperties = ShaderPool::getParticlesProperties(flags);
    ParticlesShaderFlags meshFlags = flags;
    meshFlags.mesh = true;
    meshFlags.stretch = false;
    set.meshProperties = ShaderPool::getParticlesProperties(meshFlags);
    ParticlesShaderFlags trailFlags;
    trailFlags.trail = true;
    trailFlags.texture = !r.trailTexture.empty() || !r.texture.empty();
    trailFlags.fog = fog;
    set.trailProperties = ShaderPool::getParticlesProperties(trailFlags);
    return set;
}

uint64_t RenderSystem::particleRenderKey(const ParticleSystemState& state) const{
    // what the load reads: the effect's render settings (not its simulation), the scene's fog
    // and post colour, the pipelines
    const ParticleEffectDocument& d = state.effect->document;
    const RendererModule& r = d.renderer;
    uint64_t h = 1469598103934665603ull;
    h = mix(h, (uint64_t)r.renderMode);
    h = mix(h, (uint64_t)r.blend);
    h = mix(h, (uint64_t)r.lighting);
    h = mix(h, (uint64_t)r.sixWayLayout);
    h = mix(h, (uint64_t)r.depthWrite);
    h = mix(h, (uint64_t)r.receiveShadows);
    h = mixString(h, r.shader);
    for (const std::string& f : r.shaderFeatures) h = mixString(h, f);
    for (const particles::ShaderProperty& p : r.shaderProperties){
        h = mixString(h, p.name);
        h = mix(h, (uint64_t)std::hash<float>()(p.value.x) ^ ((uint64_t)std::hash<float>()(p.value.y) << 1) ^
                       ((uint64_t)std::hash<float>()(p.value.z) << 2) ^ ((uint64_t)std::hash<float>()(p.value.w) << 3));
    }
    for (const particles::ShaderTexture& t : r.shaderTextures){
        h = mixString(h, t.name);
        h = mixString(h, t.path);
    }
    h = mixString(h, r.texture);
    h = mixString(h, r.trailTexture);
    h = mixString(h, r.sixWayMapA);
    h = mixString(h, r.sixWayMapB);
    h = mixString(h, r.emissionMap);
    for (const MeshSource& m : r.meshes){
        h = mix(h, (uint64_t)m.type);
        h = mix(h, (uint64_t)m.primitive);
        h = mixString(h, m.model);
        h = mix(h, (uint64_t)m.submesh);
    }
    const RenderParams& rp = state.effect->program.render;
    h = mix(h, (uint64_t)rp.frameBlending);
    h = mix(h, (uint64_t)rp.customData);
    h = mix(h, (uint64_t)state.effect->program.trails.enabled);
    h = mix(h, (uint64_t)hasFog);
    h = mix(h, (uint64_t)postColorProperty(ShaderType::PARTICLES));
    h = mix(h, (uint64_t)getScenePipelines());
    h = mix(h, (uint64_t)msaaPipelineSamples);
    return h;
}

void RenderSystem::destroyParticles(ParticleSystemComponent& particles){
    if (!particles.state)
        return;
    ParticleRenderData& rd = particles.state->render;
    if (rd.loaded){
        rd.quads.destroy();
        for (ObjectRender& m : rd.meshes) m.destroy();
        rd.trails.destroy();
    }
    for (ParticleMeshBuffers& m : rd.meshBuffers){
        if (m.created){
            m.vertices.destroyBuffer();
            m.indices.destroyBuffer();
            m.created = false;
        }
    }
    if (rd.trailIndexCapacity > 0){
        rd.trailIndices.destroyBuffer();
        rd.trailIndexCapacity = 0;
        rd.trailIndexPoints = 0;
    }
    rd.loaded = false;
    rd.needReload = true;
}

bool RenderSystem::loadParticles(Entity entity, ParticleSystemComponent& particles, uint16_t pipelines){
    if (!Engine::isViewLoaded() || !particles.state || !particles.state->effect)
        return false;
    if (!ensureParticleBuffers())
        return false;
    ParticleSystemState& state = *particles.state;
    ParticleRenderData& rd = state.render;
    const ParticleEffectDocument& d = state.effect->document;
    const RendererModule& r = d.renderer;
    const ParticleProgram& prog = state.effect->program;
    const RenderParams& rp = prog.render;

    // the textures: none still loading (an empty path is no texture, not a file to load)
    auto assign = [](Texture& texture, const std::string& path){
        if (path.empty())
            texture = Texture();
        else
            texture.setPath(path);
    };
    assign(rd.texture, r.texture);
    assign(rd.trailTexture, r.trailTexture.empty() ? r.texture : r.trailTexture);
    assign(rd.sixWayA, r.sixWayMapA);
    assign(rd.sixWayB, r.sixWayMapB);
    assign(rd.emission, r.emissionMap);
    for (Texture* t : {&rd.texture, &rd.trailTexture, &rd.sixWayA, &rd.sixWayB, &rd.emission}){
        if (t->empty())
            continue;
        TextureRender* tr = t->getRender(&emptyWhite);
        if (!tr || !tr->isCreated())
            return false;
    }
    rd.shaderTextures.clear();
    for (const particles::ShaderTexture& t : r.shaderTextures)
        rd.shaderTextures.push_back({t.name, Texture(t.path)});
    rd.shaderUniforms.clear();
    for (const particles::ShaderProperty& p : r.shaderProperties)
        rd.shaderUniforms.push_back({p.name, p.value});

    // the shader and its variants: the Renderer's (library:particles by default), with the
    // variant bits of the effect's modules and the BLEND_* define of its blend
    const std::string base = r.shader;
    const MaterialBlendMode blend = materialBlend(r.blend);
    const ParticleVariantSet variants = getParticleVariants(*state.effect, hasFog);
    const std::string& reference = variants.reference;
    const uint16_t customId = reference.empty() ? 0 : ShaderPool::registerCustomShader(reference);
    std::shared_ptr<const ShaderManifest> manifest = manifestOf(base);
    const uint64_t postColor = postColorProperty(ShaderType::PARTICLES);
    const bool drawsQuads = variants.quads;
    const bool drawsMeshes = variants.meshes;
    const bool drawsTrails = variants.trails;
    const bool customData = rp.customData != 0;
    const bool sixWay = r.lighting == LightingMode::SixWay;
    const bool receiveShadows = r.receiveShadows;

    auto shaderFor = [&](uint64_t properties, uint16_t id) -> std::shared_ptr<ShaderRender> {
        std::shared_ptr<ShaderRender> shader = ShaderPool::get(ShaderType::PARTICLES, properties, id);
        if (!shader->isCreated() && id != 0 && ShaderPool::isShaderBuildFailed(ShaderType::PARTICLES, properties, id)){
            // a fork that failed to compile: the built-in shader keeps the system visible
            Log::error("Particles shader '%s' failed to compile; using the built-in shader", reference.c_str());
            shader = ShaderPool::get(ShaderType::PARTICLES, properties, 0);
        }
        return shader;
    };
    rd.properties = variants.quadProperties | postColor;
    rd.meshProperties = variants.meshProperties | postColor;
    rd.trailProperties = variants.trailProperties | postColor;
    rd.customShaderId = customId;
    rd.shader = drawsQuads ? shaderFor(rd.properties, customId) : nullptr;
    rd.meshShader = drawsMeshes ? shaderFor(rd.meshProperties, customId) : nullptr;
    rd.trailShader = drawsTrails ? shaderFor(rd.trailProperties, customId) : nullptr;
    for (const std::shared_ptr<ShaderRender>& shader : {rd.shader, rd.meshShader, rd.trailShader}){
        if (shader && !shader->isCreated())
            return false;
    }
    rd.manifest = manifest;

    const RenderState renderState = resolveComponentRenderState(blend, r.depthWrite ? MaterialDepthWrite::ENABLED : MaterialDepthWrite::DISABLED,
                                                                manifest.get(), r.shaderFeatures, false);
    rd.state = renderState;
    rd.sortPriority = r.sortPriority + (manifest ? manifest->resolveState(ShaderReference::getEnabledFeatures(base, r.shaderFeatures)).priority : 0);
    rd.sceneInputs = manifest ? (manifest->resolveRequire(ShaderReference::getEnabledFeatures(base, r.shaderFeatures)) & ShaderRequire_SceneInputs) : 0u;

    // what every program binds: its blocks, the particle textures, the library's
    auto bindCommon = [&](ObjectRender& render, ShaderData& shaderData, ParticleRenderData::Slots& slots, bool trail) -> bool {
        slots.vsParams = shaderData.getUniformBlockIndex(UniformBlockType::PARTICLES_VS_PARAMS);
        slots.fsParams = shaderData.getUniformBlockIndex(UniformBlockType::PARTICLES_FS_PARAMS);
        slots.fsShadowData = receiveShadows && !trail ? shaderData.getUniformBlockIndexByName("u_fs_shadow_data") : -1;
        slots.customVS.resolve(shaderData, "u_vs_customParams");
        slots.customFS.resolve(shaderData, "u_fs_customParams");
        slots.customVS.writeValues(rd.shaderUniforms, nullptr, manifest.get());
        slots.customFS.writeValues(rd.shaderUniforms, nullptr, manifest.get());
        Texture& base = trail ? rd.trailTexture : rd.texture;
        if (!base.empty())
            render.addTexture(shaderData.getTextureIndex(TextureShaderType::PARTICLES), ShaderStageType::FRAGMENT, base.getRender(&emptyWhite));
        render.addTexture(shaderData.getTextureIndex(TextureShaderType::PARTICLEEMISSION), ShaderStageType::FRAGMENT,
                          rd.emission.empty() ? &emptyWhite : rd.emission.getRender(&emptyWhite));
        if (sixWay && !trail){
            render.addTexture(shaderData.getTextureIndex(TextureShaderType::PARTICLESIXWAYA), ShaderStageType::FRAGMENT,
                              rd.sixWayA.empty() ? &emptyWhite : rd.sixWayA.getRender(&emptyWhite));
            render.addTexture(shaderData.getTextureIndex(TextureShaderType::PARTICLESIXWAYB), ShaderStageType::FRAGMENT,
                              rd.sixWayB.empty() ? &emptyWhite : rd.sixWayB.getRender(&emptyWhite));
        }
        slots.shadowAtlasSlot = slots.fsShadowData != -1 ? shaderData.getTextureIndex(TextureShaderType::SHADOWATLAS) : std::pair<int, int>(-1, -1);
        if (!loadShaderLibraryBindings(slots.bindings, shaderData, render, rd.shaderTextures, manifest.get()))
            return false;
        // the built-in shader shapes its colour for the blend itself (particlesFsParams)
        slots.bindings.postColorBlend = 0;
        return true;
    };

    // per-instance attributes of the 32-byte record, from the stream's placeholder
    auto addInstanceAttributes = [&](ObjectRender& render, ShaderData& shaderData, bool perInstance){
        const unsigned int stride = sizeof(ParticleInstance);
        BufferRender* source = &particleInstancePlaceholder;
        render.addAttribute(shaderData.getAttrIndex(AttributeType::PARTICLEPOSITION), source, 3, AttributeDataType::FLOAT, stride, 0, false, perInstance);
        render.addAttribute(shaderData.getAttrIndex(AttributeType::PARTICLECOLOR), source, 4, AttributeDataType::UNSIGNED_BYTE, stride, 12, true, perInstance);
        render.addAttribute(shaderData.getAttrIndex(AttributeType::PARTICLESIZE), source, 2, AttributeDataType::HALF, stride, 16, false, perInstance);
        render.addAttribute(shaderData.getAttrIndex(AttributeType::PARTICLEROTATIONFRAME), source, 2, AttributeDataType::UNSIGNED_SHORT, stride, 20, true, perInstance);
        render.addAttribute(shaderData.getAttrIndex(AttributeType::PARTICLEVELOCITY), source, 4, AttributeDataType::HALF, stride, 24, false, perInstance);
        if (customData)
            render.addAttribute(shaderData.getAttrIndex(AttributeType::PARTICLECUSTOM), &particleCustomPlaceholder, 4, AttributeDataType::HALF,
                                sizeof(ParticleCustomInstance), 0, false, perInstance);
    };

    if (rd.loaded){
        rd.quads.destroy();
        for (ObjectRender& m : rd.meshes) m.destroy();
        rd.trails.destroy();
        rd.loaded = false;
    }

    if (drawsQuads){
        ObjectRender& render = rd.quads;
        ShaderData& shaderData = rd.shader->shaderData;
        render.beginLoad(PrimitiveType::TRIANGLES);
        render.setShader(rd.shader.get());
        render.setIndex(&particleQuadIndexBuffer, AttributeDataType::UNSIGNED_SHORT, 0);
        render.addAttribute(shaderData.getAttrIndex(AttributeType::PARTICLECORNER), &particleCornerBuffer, 2, AttributeDataType::FLOAT, 8, 0, false, false);
        addInstanceAttributes(render, shaderData, true);
        if (!bindCommon(render, shaderData, rd.quadSlots, false))
            return false;
        if (!render.endLoad(pipelines, renderState, msaaPipelineSamples))
            return false;
    }

    if (drawsMeshes){
        rd.meshCount = 0;
        for (size_t m = 0; m < r.meshes.size() && m < kMaxRendererMeshes; m++){
            const MeshSource& source = r.meshes[m];
            ParticleMeshBuffers& gpu = rd.meshBuffers[m];
            if (!gpu.created){
                ParticleMeshData data;
                bool ok = true;
                switch (source.type){
                    case MeshSourceType::Primitive: ParticleMeshes::primitive(source.primitive, data); break;
                    case MeshSourceType::Model: ok = ParticleMeshes::model(source.model, source.submesh, data); break;
                    case MeshSourceType::GameObject: {
                        MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
                        ok = mesh && ParticleMeshes::meshComponent(*mesh, source.submesh, data);
                        break;
                    }
                }
                if (!ok){
                    // a mesh that cannot be read draws as a cube (and says so once)
                    Log::warn("[Particles] Renderer mesh %u cannot be read: drawn as a cube", (unsigned int)m);
                    ParticleMeshes::primitive(PrimitiveMesh::Cube, data);
                }
                JE_GPU_ALLOC_SCOPE(Particles, "Particle meshes");
                if (!gpu.vertices.createBuffer((unsigned int)(data.vertices.size() * sizeof(float)), data.vertices.data(), BufferType::VERTEX_BUFFER, BufferUsage::IMMUTABLE) ||
                    !gpu.indices.createBuffer((unsigned int)(data.indices.size() * sizeof(uint32_t)), data.indices.data(), BufferType::INDEX_BUFFER, BufferUsage::IMMUTABLE)){
                    return false;
                }
                gpu.indexCount = (uint32_t)data.indices.size();
                gpu.created = true;
            }
            ObjectRender& render = rd.meshes[m];
            ShaderData& shaderData = rd.meshShader->shaderData;
            const unsigned int stride = ParticleMeshData::kFloatsPerVertex * sizeof(float);
            render.beginLoad(PrimitiveType::TRIANGLES);
            render.setShader(rd.meshShader.get());
            render.setIndex(&gpu.indices, AttributeDataType::UNSIGNED_INT, 0);
            render.addAttribute(shaderData.getAttrIndex(AttributeType::POSITION), &gpu.vertices, 3, AttributeDataType::FLOAT, stride, 0, false, false);
            render.addAttribute(shaderData.getAttrIndex(AttributeType::NORMAL), &gpu.vertices, 3, AttributeDataType::FLOAT, stride, 12, false, false);
            render.addAttribute(shaderData.getAttrIndex(AttributeType::TEXCOORD1), &gpu.vertices, 2, AttributeDataType::FLOAT, stride, 24, false, false);
            addInstanceAttributes(render, shaderData, true);
            render.addAttribute(shaderData.getAttrIndex(AttributeType::PARTICLEROTATION), &particleRotationPlaceholder, 4, AttributeDataType::SHORT,
                                sizeof(ParticleMeshInstance), 0, true, true);
            if (!bindCommon(render, shaderData, rd.meshSlots, false))
                return false;
            // a mesh hides its back faces (particles draw without depth writes by default);
            // a flipped one turns some inside out, so with Flip it draws both sides
            RenderState meshState = renderState;
            meshState.faceCulling = r.flip.x == 0.0f && r.flip.y == 0.0f && r.flip.z == 0.0f;
            if (!render.endLoad(pipelines, meshState, msaaPipelineSamples))
                return false;
            rd.meshCount++;
        }
    }

    if (drawsTrails){
        ObjectRender& render = rd.trails;
        ShaderData& shaderData = rd.trailShader->shaderData;
        const unsigned int stride = sizeof(TrailVertex);
        BufferRender* source = &particleTrailPlaceholder;
        // the strips' triangles: every pair of points a quad (sized for the rings, or the ribbons
        // at their fixed stride)
        const bool ribbons = prog.trails.mode == TrailMode::Ribbon;
        const uint32_t points = ribbons ? ParticleRenderPrep::ribbonStripPoints(prog) : prog.trails.maxPoints + 1;
        const uint32_t strips = ribbons ? std::max(prog.trails.ribbonCount, 1u) : std::max<uint32_t>(state.runtime.trails().ringCount(), 1u);
        if (rd.trailIndexCapacity < strips || rd.trailIndexPoints != points){
            if (rd.trailIndexCapacity > 0)
                rd.trailIndices.destroyBuffer();
            std::vector<uint32_t> indices;
            indices.reserve((size_t)strips * (points - 1) * 6);
            for (uint32_t s = 0; s < strips; s++){
                const uint32_t base = s * points * 2;
                for (uint32_t k = 0; k + 1 < points; k++){
                    const uint32_t a = base + k * 2;
                    indices.insert(indices.end(), {a, a + 1, a + 3, a, a + 3, a + 2});
                }
            }
            JE_GPU_ALLOC_SCOPE(Particles, "Trail strips");
            if (!rd.trailIndices.createBuffer((unsigned int)(indices.size() * sizeof(uint32_t)), indices.data(), BufferType::INDEX_BUFFER, BufferUsage::IMMUTABLE))
                return false;
            rd.trailIndexCapacity = strips;
            rd.trailIndexPoints = points;
        }
        render.beginLoad(PrimitiveType::TRIANGLES);
        render.setShader(rd.trailShader.get());
        render.setIndex(&rd.trailIndices, AttributeDataType::UNSIGNED_INT, 0);
        render.addAttribute(shaderData.getAttrIndex(AttributeType::PARTICLEPOSITION), source, 3, AttributeDataType::FLOAT, stride, 0, false, false);
        render.addAttribute(shaderData.getAttrIndex(AttributeType::PARTICLEVELOCITY), source, 4, AttributeDataType::HALF, stride, 12, false, false);
        render.addAttribute(shaderData.getAttrIndex(AttributeType::PARTICLECOLOR), source, 4, AttributeDataType::UNSIGNED_BYTE, stride, 20, true, false);
        render.addAttribute(shaderData.getAttrIndex(AttributeType::PARTICLEROTATIONFRAME), source, 2, AttributeDataType::UNSIGNED_SHORT, stride, 24, true, false);
        if (!bindCommon(render, shaderData, rd.trailSlots, true))
            return false;
        // trails never write depth: they cross themselves
        RenderState trailState = renderState;
        trailState.depthWrite = false;
        trailState.faceCulling = false;
        if (!render.endLoad(pipelines, trailState, msaaPipelineSamples))
            return false;
    }

    rd.loaded = true;
    rd.needReload = false;
    rd.loadedKey = particleRenderKey(state);
    setParticleSceneInputUser(entity, rd.sceneInputs);
    return true;
}

void RenderSystem::setParticleSceneInputUser(Entity entity, uint32_t inputs){
    inputs &= ShaderRequire_SceneInputs;
    if (inputs){
        auto found = sceneInputParticles.find(entity);
        if (found != sceneInputParticles.end() && found->second == inputs)
            return;
        sceneInputParticles[entity] = inputs;
    }else if (sceneInputParticles.empty() || sceneInputParticles.erase(entity) == 0){
        return;
    }
    // the usage of every reader (setSceneInputUser adds the meshes' and the points')
    uint32_t usage = 0;
    for (const auto& user : sceneInputMeshes) usage |= user.second;
    for (const auto& user : sceneInputPoints) usage |= user.second;
    for (const auto& user : sceneInputParticles) usage |= user.second;
    sceneInputUsage = usage;
}

void RenderSystem::updateParticleRenders(uint16_t pipelines){
    auto systems = scene->getComponentArray<ParticleSystemComponent>();
    if (systems->size() == 0){
        if (!sceneInputParticles.empty()){
            sceneInputParticles.clear();
            setParticleSceneInputUser(NULL_ENTITY, 0);
        }
        return;
    }
    for (size_t i = 0; i < systems->size(); i++){
        ParticleSystemComponent& particles = systems->getComponentFromIndex(i);
        const Entity entity = systems->getEntity(i);
        if (!particles.state || !particles.state->effect)
            continue;
        ParticleRenderData& rd = particles.state->render;
        if (rd.loaded && !rd.needReload && rd.loadedKey == particleRenderKey(*particles.state))
            continue;
        loadParticles(entity, particles, pipelines);
    }
}

// ---------------------------------------------------------------- drawing

bool RenderSystem::isParticleRenderActive(Entity entity, uint32_t cullingMask) const{
    if (!scene->isEntityActiveInHierarchy(entity))
        return false;
#ifdef JASPERENGINE_EDITOR
    if (!passDrawsOverlays && isEditorOverlay(entity))
        return false;
    if (entity == passExcludedEntity)
        return false;
#endif
    return isLayerVisible(entity, cullingMask);
}

bool RenderSystem::particleBoundsVisible(const ParticleSystemState& state, CameraComponent& camera){
    if (!state.hasBounds)
        return false;
    const AABB box(Vector3(state.boundsMin[0], state.boundsMin[1], state.boundsMin[2]), Vector3(state.boundsMax[0], state.boundsMax[1], state.boundsMax[2]));
    return isInsideCamera(camera, box);
}

void RenderSystem::drawParticles(Entity entity, ParticleSystemComponent& particles, Transform& transform, CameraComponent& camera, Transform& camTransform, PipelineType pipType){
    if (!particles.state || !particles.state->effect)
        return;
    ParticleSystemState& state = *particles.state;
    ParticleRenderData& rd = state.render;
    if (!rd.loaded || state.runtime.isPrewarming())
        return;
#if JASPERENGINE_PROFILER
    // (the editor's and the profiler's statistic: a release game reads no clock here)
    const auto start = std::chrono::steady_clock::now();
#endif
    const ParticleEffectDocument& d = state.effect->document;
    const ParticleProgram& prog = state.effect->program;
    const RenderParams& rp = prog.render;
    const RendererModule& r = d.renderer;
    const bool hasParticles = state.runtime.count() > 0 && r.renderMode != RenderMode::None;
    const bool hasTrails = prog.trails.enabled;
    if (!hasParticles && !hasTrails)
        return;
    // culling by last frame's bounds (a system the cameras miss pauses, ParticleSubsystem)
    if (!particleBoundsVisible(state, camera) && !hasTrails)
        return;
    scene->getSystem<ParticleSubsystem>()->markVisible(entity);

    memory::Arena& arena = memory::frameArena().current();
    const Matrix4 simToWorld = matrixOf(state.runtime.simToWorld());
    const Matrix4 worldToSim = simToWorld.inverse();

    // the view in simulation space
    particles::RenderView view;
    const Vector3 eye = worldToSim * camTransform.worldPosition;
    view.position[0] = eye.x; view.position[1] = eye.y; view.position[2] = eye.z;
    Vector3 forward = -camera.worldDirection;
    forward = Vector3(worldToSim[0][0] * forward.x + worldToSim[1][0] * forward.y + worldToSim[2][0] * forward.z,
                      worldToSim[0][1] * forward.x + worldToSim[1][1] * forward.y + worldToSim[2][1] * forward.z,
                      worldToSim[0][2] * forward.x + worldToSim[1][2] * forward.y + worldToSim[2][2] * forward.z);
    if (forward.length() > 0.0f) forward.normalize();
    view.forward[0] = forward.x; view.forward[1] = forward.y; view.forward[2] = forward.z;
    if (rp.mesh && rp.meshAlignment != MeshAlignment::Simulation && rp.meshAlignment != MeshAlignment::Velocity){
        // the simulation space's rotation in the world: the emitter's (Local), or none (World)
        const Quaternion simRotation = prog.space == SimulationSpace::Local ? transform.worldRotation : Quaternion();
        const Quaternion toSim = simRotation.inverse();
        const Vector3 up = toSim * (camTransform.worldRotation * Vector3(0.0f, 1.0f, 0.0f));
        view.up[0] = up.x; view.up[1] = up.y; view.up[2] = up.z;
        Quaternion align;
        switch (rp.meshAlignment){
            case MeshAlignment::View: align = toSim * camTransform.worldRotation; break;
            case MeshAlignment::World: align = toSim; break;
            case MeshAlignment::Local: align = toSim * transform.worldRotation; break;
            default: break;
        }
        align.normalize();
        view.alignQuat[0] = align.x; view.alignQuat[1] = align.y; view.alignQuat[2] = align.z; view.alignQuat[3] = align.w;
    }
    view.sort = rp.sort != SortMode::None && (rp.blend == ParticleBlend::Alpha || rp.blend == ParticleBlend::Premultiplied);
    if (d.lod.enabled && d.lod.maxDistance > 0.0f){
        const Vector3 center((state.boundsMin[0] + state.boundsMax[0]) * 0.5f, (state.boundsMin[1] + state.boundsMax[1]) * 0.5f,
                             (state.boundsMin[2] + state.boundsMax[2]) * 0.5f);
        const float t = std::min((center - camTransform.worldPosition).length() / d.lod.maxDistance, 1.0f);
        view.keep = std::clamp(d.lod.renderThinning.evaluate(t, 0.5f), 0.0f, 1.0f);
    }

    // the uniform blocks
    alignas(16) unsigned char vsBlock[sizeof(vs_particles_params_t) + 2 * MAX_PARTICLE_LIGHTS * 16 + kMaxSpriteRects * 16] = {};
    vs_particles_params_t& vs = *reinterpret_cast<vs_particles_params_t*>(vsBlock);
    vs.modelMatrix = simToWorld;
    const float sheetKind = rp.sheet == SheetKind::Sprites ? 2.0f : (rp.sheet == SheetKind::Grid ? 1.0f : 0.0f);
    vs.mode = Vector4((float)(int)r.renderMode, rp.verticalEmitter ? 1.0f : 0.0f, (float)std::max<uint32_t>(rp.frameCount, 1u), sheetKind);
    vs.sheet = Vector4((float)std::max<uint32_t>(rp.tilesX, 1u), (float)std::max<uint32_t>(rp.tilesY, 1u), rp.meshAlignment == MeshAlignment::Velocity ? 1.0f : 0.0f,
                       prog.trails.textureMode == TrailTextureMode::Tile ? kTrailTileRange : 1.0f);
    vs.stretch = Vector4(rp.cameraScale, rp.speedScale, rp.lengthScale, rp.freezeInView ? 1.0f : 0.0f);
    const bool perspective = camera.type == CameraType::CAMERA_PERSPECTIVE;
    const float viewHeight = perspective ? 2.0f * std::tan(camera.yfov * 0.5f) : std::fabs(camera.topClip - camera.bottomClip);
    vs.screenSize = Vector4(rp.minScreenSize, rp.maxScreenSize, perspective ? 1.0f : 0.0f, viewHeight);
    vs.cameraVelocity = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    vs.tiling = r.textureTiling;
    vs.pivot = Vector4(rp.pivot[0], rp.pivot[1], rp.pivot[2], 0.0f);
    auto vsLights = [&](size_t offset){
        // the point and spot lights nearest the system, in range
        Vector4* positions = reinterpret_cast<Vector4*>(vsBlock + offset);
        Vector4* colors = positions + MAX_PARTICLE_LIGHTS;
        const Vector3 center((state.boundsMin[0] + state.boundsMax[0]) * 0.5f, (state.boundsMin[1] + state.boundsMax[1]) * 0.5f,
                             (state.boundsMin[2] + state.boundsMax[2]) * 0.5f);
        struct Near { float distance; Vector4 position; Vector4 color; };
        Near nearest[MAX_PARTICLE_LIGHTS];
        int found = 0;
        auto lights = scene->getComponentArray<LightComponent>();
        for (size_t i = 0; i < lights->size(); i++){
            const LightComponent& light = lights->getComponentFromIndex(i);
            const Entity lightEntity = lights->getEntity(i);
            if (light.type == LightType::DIRECTIONAL || light.resolvedIntensity <= 0.0f || !isLightActive(lightEntity, light))
                continue;
            const Transform* lt = scene->findComponent<Transform>(lightEntity);
            if (!lt)
                continue;
            const float range = light.range > 0.0f ? light.range : 1.0e4f;
            const float distance = (lt->worldPosition - center).length();
            if (distance > range + 0.5f * Vector3(state.boundsMax[0] - state.boundsMin[0], state.boundsMax[1] - state.boundsMin[1], state.boundsMax[2] - state.boundsMin[2]).length())
                continue;
            Near n{distance, Vector4(lt->worldPosition, range), Vector4(light.resolvedColor * light.resolvedIntensity, 1.0f)};
            int slot = found < MAX_PARTICLE_LIGHTS ? found++ : -1;
            if (slot < 0){
                int far = 0;
                for (int k = 1; k < MAX_PARTICLE_LIGHTS; k++){
                    if (nearest[k].distance > nearest[far].distance) far = k;
                }
                if (nearest[far].distance <= distance)
                    continue;
                slot = far;
            }
            nearest[slot] = n;
        }
        for (int k = 0; k < found; k++){
            positions[k] = nearest[k].position;
            colors[k] = nearest[k].color;
        }
    };

    fs_particles_params_t fs;
    // a billboard without a texture (nor a sheet over it) draws a soft disc unless it asks for squares
    const bool sheet = d.textureSheetAnimation.present && d.textureSheetAnimation.enabled;
    const bool disc = r.texture.empty() && r.untexturedShape == UntexturedShape::Disc && !sheet;
    fs.blend = Vector4((float)(int)r.blend, disc ? 1.0f : 0.0f, 0.0f, 0.0f);
    const Vector3 emissionLinear = Color::sRGBToLinear(Vector3(r.emissionColor.x, r.emissionColor.y, r.emissionColor.z));
    fs.emissionColor = Vector4(emissionLinear, rd.emission.empty() ? 0.0f : 1.0f);
    fs.sixWay = Vector4((float)(int)r.sixWayLayout, r.sixWayContrast, 0.0f, 0.0f);

    auto applyBlocks = [&](ObjectRender& render, ParticleRenderData::Slots& slots, uint64_t properties){
        size_t size = sizeof(vs_particles_params_t);
        if (properties & ((1u << 5) | (1u << 6))){        // 'Lit', 'Sxw'
            vsLights(size);
            size += 2 * MAX_PARTICLE_LIGHTS * 16;
        }
        if (properties & (1u << 0)){                      // 'Tex': the sprite rects
            Vector4* rects = reinterpret_cast<Vector4*>(vsBlock + size);
            if (rp.sheet == SheetKind::Sprites && rp.spriteCount > 0){
                const float* source = reinterpret_cast<const float*>(reinterpret_cast<const unsigned char*>(prog.pool.data()) + rp.sprites);
                for (uint32_t k = 0; k < std::min<uint32_t>(rp.spriteCount, kMaxSpriteRects); k++)
                    rects[k] = Vector4(source[k * 4], source[k * 4 + 1], source[k * 4 + 2], source[k * 4 + 3]);
            }
            size += kMaxSpriteRects * 16;
        }
        render.applyUniformBlock(slots.vsParams, (unsigned int)size, vsBlock);
        render.applyUniformBlock(slots.fsParams, sizeof(fs), &fs);
        if (slots.fsShadowData != -1){
            render.applyUniformBlock(slots.fsShadowData, sizeof(fs_shadow_data_t), &ps().fs_shadow_data);
            const CameraShadows& shadows = ps();
            TextureRender* atlas = shadows.projectiveAtlas ? &shadows.projectiveAtlas->getDepthTexture()
                                                           : (hasShadowAtlas ? &shadowAtlasFramebuffer.getDepthTexture() : &emptyShadowDepth);
            render.addTexture(slots.shadowAtlasSlot, ShaderStageType::FRAGMENT, atlas);
        }
        applyCustomBlock(render, slots.customVS, frameTime, passResolution);
        applyCustomBlock(render, slots.customFS, frameTime, passResolution);
        applyFrameBlock(render, slots.bindings);
        applySceneInputs(render, slots.bindings);
    };

    uint32_t drawn = 0;
    if (hasParticles && particleBoundsVisible(state, camera)){
        const PackResult pack = ParticleRenderPrep::pack(state.runtime, view, arena, &jobs::system());
        if (pack.count > 0){
            BufferRender* instanceBuffer = nullptr;
            size_t instanceOffset = 0;
            BufferRender* customBuffer = nullptr;
            size_t customOffset = 0;
            bool streamed = appendParticleStream(particleInstanceStream, pack.instances, sizeof(ParticleInstance) * pack.count, instanceBuffer, instanceOffset);
            if (streamed && rp.customData)
                streamed = appendParticleStream(particleCustomStream, pack.custom, sizeof(ParticleCustomInstance) * pack.count, customBuffer, customOffset);
            if (streamed && r.renderMode == RenderMode::Mesh){
                BufferRender* rotationBuffer = nullptr;
                size_t rotationOffset = 0;
                if (appendParticleStream(particleRotationStream, pack.mesh, sizeof(ParticleMeshInstance) * pack.count, rotationBuffer, rotationOffset)){
                    for (uint32_t m = 0; m < rd.meshCount; m++){
                        if (pack.meshCount[m] == 0)
                            continue;
                        ObjectRender& render = rd.meshes[m];
                        if (!render.beginDraw(pipType))
                            continue;
                        render.replaceVertexBuffer(&particleInstancePlaceholder, instanceBuffer, instanceOffset + (size_t)pack.meshFirst[m] * sizeof(ParticleInstance));
                        render.replaceVertexBuffer(&particleRotationPlaceholder, rotationBuffer, rotationOffset + (size_t)pack.meshFirst[m] * sizeof(ParticleMeshInstance));
                        if (customBuffer)
                            render.replaceVertexBuffer(&particleCustomPlaceholder, customBuffer, customOffset + (size_t)pack.meshFirst[m] * sizeof(ParticleCustomInstance));
                        applyBlocks(render, rd.meshSlots, rd.meshProperties);
                        render.draw(0, rd.meshBuffers[m].indexCount, pack.meshCount[m]);
                        drawn++;
                    }
                }
            }else if (streamed){
                ObjectRender& render = rd.quads;
                if (render.beginDraw(pipType)){
                    render.replaceVertexBuffer(&particleInstancePlaceholder, instanceBuffer, instanceOffset);
                    if (customBuffer)
                        render.replaceVertexBuffer(&particleCustomPlaceholder, customBuffer, customOffset);
                    applyBlocks(render, rd.quadSlots, rd.properties);
                    render.draw(0, 6, pack.count);
                    drawn++;
                }
            }
        }
    }

    if (hasTrails){
        const TrailPack trails = ParticleRenderPrep::packTrails(state.runtime, arena);
        // the triangles cover one strip of up to their points, or strips of exactly that many
        const bool fits = trails.strips <= rd.trailIndexCapacity &&
                          (trails.strips == 1 ? trails.pointsPerStrip <= rd.trailIndexPoints : trails.pointsPerStrip == rd.trailIndexPoints);
        if (trails.strips > 0 && fits){
            BufferRender* trailBuffer = nullptr;
            size_t trailOffset = 0;
            const size_t bytes = sizeof(TrailVertex) * (size_t)trails.strips * trails.pointsPerStrip * 2;
            ObjectRender& render = rd.trails;
            if (appendParticleStream(particleTrailStream, trails.vertices, bytes, trailBuffer, trailOffset) && render.beginDraw(pipType)){
                render.replaceVertexBuffer(&particleTrailPlaceholder, trailBuffer, trailOffset);
                vs.pivot = Vector4(rp.pivot[0], rp.pivot[1], rp.pivot[2], trails.worldSpace ? 1.0f : 0.0f);
                applyBlocks(render, rd.trailSlots, rd.trailProperties);
                render.draw(0, trails.strips * (trails.pointsPerStrip - 1) * 6, 1);
                drawn++;
            }
        }else if (trails.strips > 0){
            // more rings than the index buffer was made for: it grows on the next load
            rd.needReload = true;
        }
    }

    state.drawnViews += drawn;
    if (view.sort && drawn > 0)
        state.sortedViews++;
#if JASPERENGINE_PROFILER
    state.renderMicros += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
#endif
}
