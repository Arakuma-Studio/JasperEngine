// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleEffectPreview.h"

#include "Engine.h"
#include "Scene.h"
#include "component/ParticleSystemComponent.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/ParticleSystem.h"
#include "particles/AnalyticParticles.h"
#include "particles/EmitterRuntime.h"
#include "particles/ParticleCompiler.h"
#include "particles/ParticleEffectCodec.h"
#include "particles/ParticleEffectLibrary.h"
#include "particles/ParticleMeshes.h"
#include "render/ObjectRender.h"
#include "render/OffscreenDraw.h"
#include "render/RenderUtil.h"
#include "subsystem/ParticleSubsystem.h"
#include "subsystem/ParticleSystemState.h"
#include "subsystem/RenderSystem.h"
#include "texture/Framebuffer.h"
#include "util/GraphicUtils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;
using particles::EmitterRuntime;
using particles::ParticleEffectDocument;

namespace {

    using Json = qa::Json;

    constexpr float PREVIEW_FOV = 0.7853982f;   // 45 degrees, vertical
    constexpr uint32_t MIN_POP_POPULATION = 20;
    constexpr float POP_ALPHA = 0.25f;
    // the heat row's colour (sRGB): every layer adds 4 to the red, green and blue of the target
    constexpr int HEAT_STEP = 4;
    // the analytic grid's ticks at most (a longer run takes every n-th tick)
    constexpr uint64_t MAX_GRID_TICKS = 600;

    // the live particles of a system's tree: an Analytic emitter's are its twin's (S49 R19)
    uint32_t liveOf(const particles::EmitterRuntime& r){
        return r.isAnalytic() ? r.analyticAlive() + (r.totalCount() - r.count()) : r.totalCount();
    }

    // the world box a system's live particles take this tick: an Analytic one's from its twin (its
    // culling box holds everything it may ever draw); false with none
    bool liveBoundsOf(const particles::ParticleSystemState& state, float lo[3], float hi[3]){
        const particles::EmitterRuntime& r = state.runtime;
        if (!r.isAnalytic()){
            if (!state.hasBounds || r.totalCount() == 0)
                return false;
            for (int k = 0; k < 3; k++){
                lo[k] = state.boundsMin[k];
                hi[k] = state.boundsMax[k];
            }
            return true;
        }
        float sLo[3], sHi[3];
        if (!particles::AnalyticParticles::liveBounds(*r.analytic(), sLo, sHi))
            return false;
        const float* m = r.simToWorld();
        for (int k = 0; k < 3; k++){ lo[k] = 3.0e38f; hi[k] = -3.0e38f; }
        for (int corner = 0; corner < 8; corner++){
            const float p[3] = {(corner & 1) ? sHi[0] : sLo[0], (corner & 2) ? sHi[1] : sLo[1], (corner & 4) ? sHi[2] : sLo[2]};
            for (int k = 0; k < 3; k++){
                const float w = m[k] * p[0] + m[4 + k] * p[1] + m[8 + k] * p[2] + m[12 + k];
                lo[k] = std::min(lo[k], w);
                hi[k] = std::max(hi[k], w);
            }
        }
        return true;
    }

    double previewNow(){
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::vector<ParticleEffectPreview*>& liveRuns(){
        static std::vector<ParticleEffectPreview*> runs;
        return runs;
    }

    std::function<void()>& previewFrameRequester(){
        static std::function<void()> requester;
        return requester;
    }

    bool& previewInFrame(){
        static bool inFrame = false;
        return inFrame;
    }

    void matrixFloats(const Matrix4& m, float out[16]){
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) out[c * 4 + r] = m[c][r];
    }

    uint64_t mixHash(uint64_t h, uint64_t value){
        return (h * 1099511628211ull) ^ value;
    }

    std::string hashText(uint64_t h){
        char text[17];
        std::snprintf(text, sizeof(text), "%016llx", (unsigned long long)h);
        return text;
    }

    double tickOf(const ParticleEffectDocument& d){
        return d.tickRate > 0 ? 1.0 / d.tickRate : particles::kMaxVariableStep;
    }

    // the emitters of a tree, depth first, with names: "<system>" then "<system> > <child file>"
    struct TreeEmitter {
        const EmitterRuntime* runtime = nullptr;
        std::string name;
        std::string asset;
    };

    void collectTree(const EmitterRuntime& r, const std::string& name, const std::string& asset, std::vector<TreeEmitter>& out){
        out.push_back({&r, name, asset});
        const particles::CompiledEffect* effect = r.effect();
        for (size_t c = 0; c < r.childCount(); c++){
            const EmitterRuntime* child = r.child(c);
            if (!child) continue;
            const std::string file = effect && c < effect->document.subEmitters.size() ? effect->document.subEmitters[c].effect : std::to_string(c);
            collectTree(*child, name + " > " + file, file, out);
        }
    }

    // what the heat row draws: the same particles as flat additive squares adding HEAT_STEP each
    ParticleEffectDocument heatDocument(const ParticleEffectDocument& source){
        ParticleEffectDocument d = source;
        const float step = (float)HEAT_STEP / 255.0f;
        d.main.startColor = particles::MinMaxGradient(Vector4(step, step, step, 1.0f));
        d.colorOverLifetime.enabled = false;
        d.colorBySpeed.enabled = false;
        std::vector<particles::SetOverAgeEntry> kept;
        for (const particles::SetOverAgeEntry& e : d.setOverAge.entries){
            if (e.property != particles::SetProperty::Color && e.property != particles::SetProperty::Alpha) kept.push_back(e);
        }
        d.setOverAge.entries = kept;
        particles::RendererModule& r = d.renderer;
        r.shader = "library:particles";
        r.shaderFeatures.clear();
        r.shaderProperties.clear();
        r.shaderTextures.clear();
        r.texture.clear();
        r.textureGuid.clear();
        r.untexturedShape = particles::UntexturedShape::Square;
        r.blend = particles::ParticleBlend::Additive;
        r.lighting = particles::LightingMode::Unlit;
        r.emissionIntensity = 0.0f;
        r.emissionMap.clear();
        r.emissionMapGuid.clear();
        r.depthWrite = false;
        d.textureSheetAnimation.enabled = false;
        return d;
    }

    // a bare runtime tree with Max Particles lifted (its children's too): the births nothing refused
    std::shared_ptr<const particles::CompiledEffect> compileLifted(const ParticleEffectDocument& document, bool inheritsRotation){
        ParticleEffectDocument lifted = document;
        lifted.maxParticles = particles::kMaxParticlesPerEmitter;
        particles::CompileEnvironment environment;
        environment.inheritsRotation = inheritsRotation;
        environment.mesh = [](const particles::MeshSource& source, particles::MeshGeometry& out){
            particles::ParticleMeshData data;
            if (source.type == particles::MeshSourceType::Primitive){
                particles::ParticleMeshes::primitive(source.primitive, data);
            }else if (source.type != particles::MeshSourceType::Model || !particles::ParticleMeshes::model(source.model, source.submesh, data)){
                return false;
            }
            particles::ParticleMeshes::geometry(data, out);
            return true;
        };
        std::shared_ptr<particles::CompiledEffect> compiled = particles::ParticleCompiler::compile(lifted, environment);
        return compiled->valid ? compiled : nullptr;
    }

    struct CameraSetup {
        Vector3 position;
        Vector3 target;
        Vector3 up;
        Vector3 forward;
        Vector3 right;
        float nearClip = 0.01f;
        float farClip = 100.0f;
        float tanHalf = 0.41421356f;
        // the pixel view (S49): orthographic, `orthoHalf` world units above and below the centre
        bool ortho = false;
        float orthoHalf = 1.0f;
    };

    // one private scene of the run
    struct PreviewScene {
        Scene* scene = nullptr;
        Camera* camera = nullptr;
        Light* light = nullptr;
        std::vector<std::unique_ptr<ParticleSystem>> systems;
        std::vector<double> tick;          // each system's tick
        std::vector<uint64_t> done;        // ticks run
        std::vector<std::string> names;
        std::vector<std::string> assets;
        Framebuffer target;
        bool loaded = false;
        bool warmed = false;

        PreviewScene() = default;
        PreviewScene(const PreviewScene&) = delete;
        PreviewScene& operator=(const PreviewScene&) = delete;
        ~PreviewScene(){
            systems.clear();
            delete light;
            delete camera;
            delete scene;
            target.destroy();
        }

        void build(const ParticlePreviewRequest& request, const std::vector<ParticleEffectDocument>& documents, const std::vector<uint32_t>& seeds,
                   bool drawn){
            scene = new Scene(EntityPool::System);
            if (drawn){
                camera = new Camera(scene);
                camera->setType(CameraType::CAMERA_PERSPECTIVE);
                camera->setAutoResize(false);
                light = new Light(scene);
                light->setType(LightType::DIRECTIONAL);
                light->setDirection(-0.4f, -0.5f, -0.5f);
                light->setIntensity(4.0f);
                light->setShadows(false);
            }
            for (size_t i = 0; i < request.systems.size(); i++){
                auto system = std::make_unique<ParticleSystem>(scene);
                system->setPlayOnAwake(false);
                system->setSeed(seeds[i]);
                system->setEffectDocument(documents[i]);
                system->setLocalMatrix(request.systems[i].transform);
                systems.push_back(std::move(system));
                tick.push_back(tickOf(documents[i]));
                done.push_back(0);
                names.push_back(request.systems[i].name);
                assets.push_back(request.systems[i].asset);
            }
        }

        ParticleSubsystem& particles(){
            return *scene->getSystem<ParticleSubsystem>();
        }

        particles::ParticleSystemState* state(size_t i){
            return particles().getState(systems[i]->getEntity());
        }

        // every system at `time`: the whole ticks that fit
        void advanceTo(double time){
            for (size_t i = 0; i < systems.size(); i++){
                const uint64_t want = (uint64_t)std::floor(time / tick[i] + 1e-6);
                if (want > done[i]){
                    // a one-shot that has stopped stays stopped (step would play it again)
                    particles::ParticleSystemState* s = done[i] > 0 ? state(i) : nullptr;
                    if (!s || s->runtime.state() != particles::PlayState::Stopped){
                        particles().step(systems[i]->getEntity(), (uint32_t)(want - done[i]));
                    }
                    done[i] = want;
                }
            }
        }

        uint64_t stateHash(){
            uint64_t h = 1469598103934665603ull;
            for (size_t i = 0; i < systems.size(); i++) h = mixHash(h, particles().getStateHash(systems[i]->getEntity()));
            return h;
        }

        void emitters(std::vector<TreeEmitter>& out){
            out.clear();
            for (size_t i = 0; i < systems.size(); i++){
                if (particles::ParticleSystemState* s = state(i)) collectTree(s->runtime, names[i], assets[i], out);
            }
        }
    };

    bool renderReady(const particles::ParticleRenderData& rd, const EmitterRuntime& r){
        if (!rd.loaded) return false;
        for (size_t c = 0; c < r.childCount(); c++){
            const EmitterRuntime* child = r.child(c);
            if (!child || !child->effect()) continue;
            if (c >= rd.children.size() || !rd.children[c] || !renderReady(*rd.children[c], *child)) return false;
        }
        return true;
    }

    enum class Drawn { Done, Wait };

    // draws a scene from the camera into its target and reads it back (in the editor's frame)
    Drawn drawScene(PreviewScene& s, const CameraSetup& camera, int size, const Vector4& clear, PoseImage& out, std::string& waitingFor){
        if (!Engine::isViewLoaded()){
            waitingFor = "the graphics view";
            return Drawn::Wait;
        }
        if (!s.loaded){
            s.scene->load();
            s.loaded = true;
        }
        s.scene->setBackgroundColor(clear);
        s.camera->setPosition(camera.position);
        s.camera->setTarget(camera.target);
        s.camera->setUp(camera.up);
        if (camera.ortho)
            s.camera->setOrtho(-camera.orthoHalf, camera.orthoHalf, -camera.orthoHalf, camera.orthoHalf, camera.nearClip, camera.farClip);
        else
            s.camera->setPerspective(PREVIEW_FOV, 1.0f, camera.nearClip, camera.farClip);
        RenderSystem& render = *s.scene->getSystem<RenderSystem>();
        render.update(0);

        if (!s.target.isCreated() || s.target.getWidth() != (unsigned int)size){
            s.target.destroy();
            s.target.setWidth((unsigned int)size);
            s.target.setHeight((unsigned int)size);
            s.target.create();
        }
        Engine::ViewLayer layer;
        layer.scene = s.scene;
        layer.camera = s.camera->getEntity();
        layer.viewRect = Rect(0.0f, 0.0f, (float)size, (float)size);
        layer.postProcessing = false;
        layer.disableFog = true;

        std::vector<uint8_t> rgba;
        const OffscreenDraw drawn = drawOffscreen(PARTICLE_PREVIEW_VIEW_ID, s.target, {layer}, s.warmed, rgba, waitingFor, [&](std::string& why){
            for (size_t i = 0; i < s.systems.size(); i++){
                particles::ParticleSystemState* state = s.state(i);
                if (state && !renderReady(state->render, state->runtime)){
                    why = "the textures and shaders of " + s.names[i];
                    return false;
                }
            }
            return true;
        });
        if (drawn == OffscreenDraw::Wait){
            return Drawn::Wait;
        }
        if (!out.assign(size, size, std::move(rgba))){
            waitingFor = "the read-back";
            return Drawn::Wait;
        }
        return Drawn::Done;
    }

    PoseColor backgroundAt(const std::string& background, int x, int y){
        if (background == "light") return PoseColor(217, 217, 219);
        if (background == "checker") return ((x / 12 + y / 12) & 1) ? PoseColor(96, 96, 100) : PoseColor(64, 64, 68);
        return PoseColor(31, 31, 33);
    }

}

struct ParticleEffectPreview::Impl {
    enum class Stage { Pass1, Framing, Pass2, Compose, Done };
    Stage stage = Stage::Pass1;

    std::vector<ParticleEffectDocument> documents;
    std::vector<uint32_t> seeds;
    double master = 1.0 / 30.0;      // the run's tick: the shortest of its emitters'
    uint64_t totalTicks = 0;         // 0 until known (a one-shot runs until it is over)
    uint64_t tickIndex = 0;
    bool oneShot = false;
    double durationMax = 0.0;

    // pass 1
    std::unique_ptr<PreviewScene> sim;
    std::vector<std::unique_ptr<EmitterRuntime>> shadow;
    std::vector<std::array<float, 16>> shadowTransforms;
    std::vector<uint32_t> liveCurve;
    std::vector<TreeEmitter> tree, shadowTree;
    std::vector<uint32_t> shadowPeak;  // each lifted emitter's peak live count, depth first
    size_t capacityIndex = 0;
    uint64_t runHash = 0;            // pass 1's state hash at its last tick
    float runMin[3] = {0, 0, 0}, runMax[3] = {0, 0, 0};
    double alphaSum = 0.0;
    uint64_t alphaCount = 0;

    // framing
    CameraSetup camera;
    Vector3 frameCenter;
    float frameWorldSize = 1.0f;     // the view's height at the framed centre
    int drawSize = 192;              // the offscreen target's side (the pixel view: base pixels x PIXEL_PROBE)
    int pixelSide = 0;               // the pixel view's base pixels each way
    int pixelOriginX = 0, pixelOriginY = 0;   // the emitters' origin in those base pixels
    std::vector<uint64_t> cellTicks;
    std::vector<double> cellTimes;

    // pass 2
    std::unique_ptr<PreviewScene> look;
    std::unique_ptr<PreviewScene> heat;
    size_t nextCell = 0;
    int drawPhase = 0;               // 0 black, 1 white, 2 heat
    PoseImage onBlack, onWhite, heatImage;
    double drawStarted = 0.0;
    std::string waitingFor;
    std::vector<uint32_t> cellLive;
    std::vector<PoseImage> heatCells;
    uint64_t gridStride = 1;
    double coveragePrev = -1.0;
    double overdrawAvgSum = 0.0;
    uint64_t overdrawAvgCount = 0;
    std::map<std::string, double> emitterArea;
    double cellsAnalyticPeak = 0.0, cellsAnalyticAvgSum = 0.0;
    int cellsAnalyticCount = 0;
    std::vector<double> gridLayers, gridWeighted;

    // the analytic grid at the current tick of a scene: layers and alpha-weighted cover per cell
    void projectGrid(PreviewScene& scene, double& peak, double& avg, double& coverage, double& areaFraction){
        gridLayers.assign((size_t)GRID * GRID, 0.0);
        gridWeighted.assign((size_t)GRID * GRID, 0.0);
        areaFraction = 0.0;
        std::vector<TreeEmitter> emitters;
        scene.emitters(emitters);
        for (const TreeEmitter& e : emitters){
            const EmitterRuntime& r = *e.runtime;
            if (!r.program()) continue;
            const float* m = r.simToWorld();
            const bool additive = r.effect()->document.renderer.blend == particles::ParticleBlend::Additive;
            const float minShare = r.effect()->document.renderer.minScreenSize;
            const float maxShare = r.effect()->document.renderer.maxScreenSize;
            double area = 0.0;
            // one particle of the emitter onto the grid (simulation space, its size and colour)
            auto addParticle = [&](float sx, float sy, float sz, float size, const float color[4]){
                const Vector3 world(m[0] * sx + m[4] * sy + m[8] * sz + m[12],
                                    m[1] * sx + m[5] * sy + m[9] * sz + m[13],
                                    m[2] * sx + m[6] * sy + m[10] * sz + m[14]);
                const Vector3 rel = world - camera.position;
                const float depth = rel.dotProduct(camera.forward);
                if (depth <= camera.nearClip || size <= 0.0f) return;
                // the view's half height at the particle: perspective, or the orthographic pixel view
                const float halfHeight = camera.ortho ? camera.orthoHalf : depth * camera.tanHalf;
                // the shader's screen-size clamp: a share of the view height
                float share = size / (2.0f * halfHeight);
                share = std::clamp(share, minShare, std::max(minShare, maxShare));
                const float half = share * 0.5f * (float)GRID;
                const float cx = (rel.dotProduct(camera.right) / halfHeight * 0.5f + 0.5f) * (float)GRID;
                const float cy = (0.5f - rel.dotProduct(camera.up) / halfHeight * 0.5f) * (float)GRID;
                const float weight = additive ? color[3] * std::max({color[0], color[1], color[2]}) : color[3];
                area += (double)share * share;
                const int x0 = std::max(0, (int)std::floor(cx - half)), x1 = std::min(GRID - 1, (int)std::floor(cx + half));
                const int y0 = std::max(0, (int)std::floor(cy - half)), y1 = std::min(GRID - 1, (int)std::floor(cy + half));
                for (int y = y0; y <= y1; y++){
                    const float oy = std::min((float)y + 1.0f, cy + half) - std::max((float)y, cy - half);
                    if (oy <= 0.0f) continue;
                    for (int x = x0; x <= x1; x++){
                        const float ox = std::min((float)x + 1.0f, cx + half) - std::max((float)x, cx - half);
                        if (ox <= 0.0f) continue;
                        gridLayers[(size_t)y * GRID + x] += ox * oy;
                        gridWeighted[(size_t)y * GRID + x] += ox * oy * weight;
                    }
                }
            };
            if (r.isAnalytic()){
                // an Analytic emitter stores nothing: its CPU twin places the particles (S49 R19)
                const particles::AnalyticProgram& program = *r.analytic();
                for (uint32_t slot = 0; slot < program.slotCount; slot++){
                    const particles::AnalyticParticle a = particles::AnalyticParticles::evaluate(program, slot);
                    if (!a.alive) continue;
                    const float color[4] = {(float)(a.color & 255u) / 255.0f, (float)((a.color >> 8) & 255u) / 255.0f,
                                            (float)((a.color >> 16) & 255u) / 255.0f, (float)(a.color >> 24) / 255.0f};
                    addParticle(a.position.x, a.position.y, a.position.z, std::max(a.size.x, a.size.y), color);
                }
            }else{
                const particles::ParticleChannels& ch = r.particleChannels();
                const float* px = ch.get(particles::ChPosX);
                const float* py = ch.get(particles::ChPosY);
                const float* pz = ch.get(particles::ChPosZ);
                for (uint32_t i = 0; i < r.count(); i++){
                    float color[4], size, rotation;
                    r.appearance(i, color, size, rotation);
                    addParticle(px[i], py[i], pz[i], size, color);
                }
            }
            emitterArea[e.name] += area;
            areaFraction += area;
        }
        peak = 0.0;
        double sum = 0.0;
        int covered = 0, visible = 0;
        for (size_t k = 0; k < gridLayers.size(); k++){
            peak = std::max(peak, gridLayers[k]);
            if (gridLayers[k] > 0.01){
                sum += gridLayers[k];
                covered++;
            }
            if (gridWeighted[k] >= 0.05) visible++;
        }
        avg = covered ? sum / covered : 0.0;
        coverage = 100.0 * visible / (double)gridLayers.size();
    }
};

// ---------------------------------------------------------------- the run

ParticleEffectPreview::ParticleEffectPreview(ParticlePreviewRequest r): impl(std::make_unique<Impl>()), request(std::move(r)){
    liveRuns().push_back(this);
}

ParticleEffectPreview::~ParticleEffectPreview(){
    auto& runs = liveRuns();
    runs.erase(std::remove(runs.begin(), runs.end(), this), runs.end());
}

std::string ParticleEffectPreview::validate(const ParticlePreviewRequest& request){
    if (request.systems.empty()) return "nothing to preview: no Particle System in the subtree.";
    if (request.frames < 0 || request.frames > MAX_FRAMES) return "frames is 0 to " + std::to_string(MAX_FRAMES) + ".";
    if (request.seconds < 0.0 || request.seconds > MAX_SECONDS) return "seconds is 0 (one play) to " + std::to_string((int)MAX_SECONDS) + ".";
    if (request.times.size() > (size_t)MAX_FRAMES) return "times holds at most " + std::to_string(MAX_FRAMES) + " values.";
    if (request.pixelGrid && (request.pixelScale < 1 || request.pixelScale > 8)) return "scale is a whole number from 1 to 8.";
    if (request.pixelGrid && request.overdrawMap) return "overdraw_map draws the perspective view: leave it out with pixel: true.";
    for (double t : request.times){
        if (!(t >= 0.0) || t > MAX_SECONDS) return "times are seconds from 0 to " + std::to_string((int)MAX_SECONDS) + ".";
    }
    for (const ParticlePreviewSystem& system : request.systems){
        std::shared_ptr<particles::CompiledEffect> compiled = particles::ParticleCompiler::compile(system.document);
        if (!compiled->valid){
            return system.name + " does not compile: " + particles::ParticleEffectCodec::format(compiled->diagnostics);
        }
    }
    return std::string();
}

bool ParticleEffectPreview::needsGpu() const{
    return request.frames > 0 || !request.times.empty() || request.overdrawMap;
}

double ParticleEffectPreview::getProgress() const{
    switch (impl->stage){
        case Impl::Stage::Pass1: return impl->totalTicks ? 0.3 * (double)impl->tickIndex / (double)impl->totalTicks : 0.1;
        case Impl::Stage::Framing: return 0.3;
        case Impl::Stage::Pass2: return 0.3 + 0.65 * (impl->cellTicks.empty() ? 1.0 : (double)impl->nextCell / (double)impl->cellTicks.size());
        default: return 1.0;
    }
}

float ParticleEffectPreview::getFrameWorldSize() const{
    return impl->frameWorldSize;
}

Vector3 ParticleEffectPreview::getFrameCenter() const{
    return impl->frameCenter;
}

std::string ParticleEffectPreview::getStage() const{
    switch (impl->stage){
        case Impl::Stage::Pass1: return "simulating";
        case Impl::Stage::Framing: return "framing";
        case Impl::Stage::Pass2: return impl->waitingFor.empty() ? "drawing cells" : "waiting for " + impl->waitingFor;
        case Impl::Stage::Compose: return "composing";
        default: return "done";
    }
}

void ParticleEffectPreview::cancel(){
    if (!finished) finish("Cancelled.");
}

void ParticleEffectPreview::start(){
    if (started) return;
    started = true;
    if (previewFrameRequester()) previewFrameRequester()();
}

void ParticleEffectPreview::finish(const std::string& failure){
    error = failure;
    finished = true;
    impl->stage = Impl::Stage::Done;
    impl->sim.reset();
    impl->look.reset();
    impl->heat.reset();
    impl->shadow.clear();
    if (onFinished){
        std::function<void()> done = std::move(onFinished);
        onFinished = nullptr;
        done();
    }
}

bool ParticleEffectPreview::advance(double budgetMs, bool drawing){
    if (finished) return true;
    Impl& d = *impl;
    const double sliceStart = previewNow();
    auto overBudget = [&](){ return (previewNow() - sliceStart) * 1000.0 > budgetMs; };
    const size_t n = request.systems.size();

    if (d.stage == Impl::Stage::Pass1 && !d.sim){
        // ---- setup: seeds, the run's tick and length
        for (size_t i = 0; i < n; i++){
            const ParticlePreviewSystem& s = request.systems[i];
            d.documents.push_back(s.document);
            uint32_t seed = request.seed != 0 ? request.seed + (uint32_t)i : s.seed != 0 ? s.seed : s.document.seed != 0 ? s.document.seed : 1u + (uint32_t)i;
            d.seeds.push_back(seed);
            d.master = i == 0 ? tickOf(s.document) : std::min(d.master, tickOf(s.document));
            d.durationMax = std::max(d.durationMax, (double)s.document.duration);
        }
        bool anyLooping = false;
        for (const ParticleEffectDocument& doc : d.documents) anyLooping = anyLooping || doc.looping;
        measured.looping = anyLooping;
        d.oneShot = !anyLooping;
        double seconds = request.seconds;
        if (seconds <= 0.0) seconds = anyLooping ? std::max(d.durationMax, d.master) : d.durationMax + ONE_SHOT_LIMIT;
        for (double t : request.times) seconds = std::max(seconds, t);
        d.totalTicks = (uint64_t)std::ceil(seconds / d.master - 1e-6);
        measured.tickSeconds = d.master;
        // a variable step (tick rate 0) steps once a frame: 60 a second at 60 fps
        measured.ticksPerSecond = 1.0 / d.master;
        for (const ParticleEffectDocument& doc : d.documents){
            if (doc.tickRate <= 0) measured.ticksPerSecond = std::max(measured.ticksPerSecond, 60.0);
        }
        measured.runCut = request.seconds > 0.0 || !request.times.empty();

        d.sim = std::make_unique<PreviewScene>();
        d.sim->build(request, d.documents, d.seeds, false);
        for (size_t i = 0; i < n; i++){
            const EmitterRuntime* real = d.sim->state(i) ? &d.sim->state(i)->runtime : nullptr;
            auto runtime = std::make_unique<EmitterRuntime>();
            std::shared_ptr<const particles::CompiledEffect> lifted = compileLifted(d.documents[i], false);
            if (lifted && real){
                runtime->bind(lifted);
                runtime->bindSubEmitters([](const particles::SubEmitterEntry& entry) -> std::shared_ptr<const particles::CompiledEffect> {
                    const particles::EffectAsset& asset = particles::ParticleEffectLibrary::get(entry.effect);
                    return asset.document ? compileLifted(*asset.document, entry.inheritRotation) : nullptr;
                });
                runtime->reset(d.seeds[i]);
                runtime->play();
            }
            std::array<float, 16> transform;
            matrixFloats(request.systems[i].transform, transform.data());
            d.shadowTransforms.push_back(transform);
            d.shadow.push_back(std::move(runtime));
        }
        measured.noise = false;
        for (const ParticleEffectDocument& doc : d.documents) measured.noise = measured.noise || (doc.noise.present && doc.noise.enabled);
        d.liveCurve.push_back(0);
    }

    if (d.stage == Impl::Stage::Pass1){
        // ---- pass 1: count, births, bounds, pops
        const Vector3 gravity = d.sim->scene->getGravity3D();
        while (d.tickIndex < d.totalTicks){
            if (overBudget()) return false;
            d.tickIndex++;
            const double t = (double)d.tickIndex * d.master;
            // the counts before the tick, for the pops
            d.sim->emitters(d.tree);
            std::vector<uint32_t> before;
            std::vector<double> dyingVisible;
            for (const TreeEmitter& e : d.tree){
                const EmitterRuntime& r = *e.runtime;
                before.push_back(r.count());
                double dying = 0.0;
                const uint32_t count = r.count();
                if (count >= MIN_POP_POPULATION && r.program()){
                    const particles::ParticleChannels& ch = r.particleChannels();
                    const float* age = ch.get(particles::ChAge);
                    const float* inv = ch.get(particles::ChInvLife);
                    const float tick = (float)tickOf(r.effect()->document);
                    for (uint32_t i = 0; i < count; i++){
                        if ((age[i] + tick) * inv[i] < 1.0f) continue;
                        float color[4], size, rotation;
                        r.appearance(i, color, size, rotation);
                        if (color[3] >= POP_ALPHA && size > 0.0f) dying += 1.0;
                    }
                }
                dyingVisible.push_back(dying);
            }
            d.sim->advanceTo(t);
            for (size_t i = 0; i < n; i++){
                if (!d.shadow[i]->effect()) continue;
                const uint64_t want = (uint64_t)std::floor(t / tickOf(d.documents[i]) + 1e-6);
                particles::EmitterInput input;
                input.transform = d.shadowTransforms[i].data();
                input.seed = d.seeds[i];
                input.gravity[0] = gravity.x;
                input.gravity[1] = gravity.y;
                input.gravity[2] = gravity.z;
                // (a stopped one-shot runs no more ticks)
                while (d.shadow[i]->tickCount() < want && d.shadow[i]->state() != particles::PlayState::Stopped) d.shadow[i]->step(input);
            }
            for (size_t k = 0; k < d.tree.size() && k < before.size(); k++){
                if (before[k] < MIN_POP_POPULATION) continue;
                const double percent = 100.0 * dyingVisible[k] / (double)before[k];
                if (percent > measured.popPercent){
                    measured.popPercent = percent;
                    measured.popAt = t;
                    measured.popEmitter = d.tree[k].name;
                }
            }

            // live, births refused, bounds
            uint32_t live = 0;
            bool alive = false, emitting = false;
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            bool bounded = false;
            for (size_t i = 0; i < n; i++){
                particles::ParticleSystemState* state = d.sim->state(i);
                if (!state) continue;
                live += liveOf(state->runtime);
                alive = alive || state->runtime.isAlive();
                emitting = emitting || state->runtime.isEmitting();
                float sLo[3], sHi[3];
                if (liveBoundsOf(*state, sLo, sHi)){
                    bounded = true;
                    for (int a = 0; a < 3; a++){
                        lo[a] = std::min(lo[a], sLo[a]);
                        hi[a] = std::max(hi[a], sHi[a]);
                    }
                }
            }
            d.liveCurve.push_back(live);
            if (live > measured.peak){
                measured.peak = live;
                measured.peakAt = t;
            }
            if (bounded){
                if (!measured.hasBounds){
                    for (int a = 0; a < 3; a++){ d.runMin[a] = lo[a]; d.runMax[a] = hi[a]; }
                }
                for (int a = 0; a < 3; a++){
                    d.runMin[a] = std::min(d.runMin[a], lo[a]);
                    d.runMax[a] = std::max(d.runMax[a], hi[a]);
                }
                measured.hasBounds = true;
                const float size = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]});
                if (size > measured.largestSize){
                    measured.largestSize = size;
                    measured.largestAt = t;
                }
            }
            // the lifted run: each emitter's peak (what Max Particles would have to hold), and the first
            // emitter that refused a birth
            d.sim->emitters(d.tree);
            d.shadowTree.clear();
            for (size_t i = 0; i < n; i++){
                if (d.shadow[i]->effect()) collectTree(*d.shadow[i], d.sim->names[i], d.sim->assets[i], d.shadowTree);
            }
            if (d.shadowPeak.size() < d.shadowTree.size()) d.shadowPeak.resize(d.shadowTree.size(), 0);
            for (size_t k = 0; k < d.shadowTree.size(); k++) d.shadowPeak[k] = std::max(d.shadowPeak[k], d.shadowTree[k].runtime->count());
            for (size_t k = 0; measured.capacityAt < 0.0 && k < d.tree.size() && k < d.shadowTree.size(); k++){
                if (d.shadowTree[k].runtime->spawnedCount() > d.tree[k].runtime->spawnedCount()){
                    measured.capacityAt = t;
                    measured.capacityEmitter = d.tree[k].name;
                    measured.capacityMax = d.tree[k].runtime->program()->maxParticles;
                    d.capacityIndex = k;
                }
            }
            for (const TreeEmitter& e : d.tree){
                if (!e.runtime->effect()) continue;
                const particles::RendererModule& renderer = e.runtime->effect()->document.renderer;
                if (renderer.blend == particles::ParticleBlend::Additive) measured.additive = true;
                if (std::find(renderer.shaderFeatures.begin(), renderer.shaderFeatures.end(), "DISTORTION") != renderer.shaderFeatures.end() &&
                        std::find(measured.distorting.begin(), measured.distorting.end(), e.name) == measured.distorting.end()){
                    measured.distorting.push_back(e.name);
                }
            }
            if (live > 0){
                // the run's mean alpha, sampled on a few particles a tick
                d.sim->emitters(d.tree);
                for (const TreeEmitter& e : d.tree){
                    const uint32_t count = e.runtime->count();
                    const uint32_t stride = std::max(1u, count / 64u);
                    for (uint32_t i = 0; i < count; i += stride){
                        float color[4], size, rotation;
                        e.runtime->appearance(i, color, size, rotation);
                        d.alphaSum += color[3];
                        d.alphaCount++;
                    }
                }
            }
            // a one-shot is over when nothing is left (its start delay passed)
            if (d.oneShot && request.seconds <= 0.0 && request.times.empty() && !alive && !emitting && t >= d.master){
                bool started = true;
                for (size_t i = 0; i < n; i++){
                    particles::ParticleSystemState* state = d.sim->state(i);
                    if (state && state->runtime.time() <= 0.0) started = false;
                }
                if (started){
                    measured.ended = true;
                    measured.endedAt = t;
                    d.totalTicks = d.tickIndex;
                    break;
                }
            }
        }
        // the end of pass 1
        measured.ticks = d.totalTicks;
        measured.seconds = (double)d.totalTicks * d.master;
        for (int a = 0; a < 3; a++){
            measured.boundsMin[a] = d.runMin[a];
            measured.boundsMax[a] = d.runMax[a];
        }
        measured.meanAlpha = d.alphaCount ? d.alphaSum / (double)d.alphaCount : 0.0;
        if (d.oneShot && !measured.ended){
            bool alive = false;
            for (size_t i = 0; i < n; i++){
                particles::ParticleSystemState* state = d.sim->state(i);
                if (state) alive = alive || state->runtime.isAlive() || state->runtime.isEmitting();
            }
            measured.stillAlive = alive;
        }
        d.sim->emitters(d.tree);
        measured.emitters = (uint32_t)d.tree.size();
        uint64_t spawned = 0, lifted = 0;
        for (const TreeEmitter& e : d.tree) spawned += e.runtime->spawnedCount();
        d.shadowTree.clear();
        for (size_t i = 0; i < n; i++){
            if (d.shadow[i]->effect()) collectTree(*d.shadow[i], d.sim->names[i], d.sim->assets[i], d.shadowTree);
        }
        for (const TreeEmitter& e : d.shadowTree) lifted += e.runtime->spawnedCount();
        measured.emitted = spawned;
        measured.neverBorn = lifted > spawned ? lifted - spawned : 0;
        if (measured.neverBorn > 0 && d.capacityIndex < d.shadowPeak.size()) measured.capacityNeeded = d.shadowPeak[d.capacityIndex];
        if (measured.neverBorn == 0) measured.capacityAt = -1.0;
        // the count curve: about 32 samples
        const size_t samples = std::min<size_t>(32, d.liveCurve.size() - 1);
        measured.curve.clear();
        for (size_t s = 0; s <= samples; s++){
            const size_t k = samples ? (size_t)std::llround((double)s * (double)(d.liveCurve.size() - 1) / (double)samples) : 0;
            measured.curve.push_back({(double)k * d.master, d.liveCurve[k]});
        }

        // the cells' ticks
        std::vector<double> times = request.times;
        std::sort(times.begin(), times.end());
        const int frames = request.times.empty() ? request.frames : (int)request.times.size();
        if (times.empty()){
            for (int k = 0; k < frames; k++){
                times.push_back(measured.seconds * ((double)k + 0.5) / (double)frames);
            }
        }
        for (double t : times){
            const uint64_t tick = std::min<uint64_t>((uint64_t)std::llround(t / d.master), d.totalTicks);
            d.cellTicks.push_back(tick);
            d.cellTimes.push_back((double)tick * d.master);
        }
        d.stage = Impl::Stage::Framing;
        // pass 2 replays from scratch: its state at the last tick must be this one
        d.runHash = d.sim->stateHash();
        d.sim.reset();
        d.shadow.clear();
    }

    if (d.stage == Impl::Stage::Framing){
        // ---- one framing for the whole run
        Vector3 lo(-0.5f, -0.5f, -0.5f), hi(0.5f, 0.5f, 0.5f);
        if (measured.hasBounds){
            lo = Vector3(d.runMin[0], d.runMin[1], d.runMin[2]);
            hi = Vector3(d.runMax[0], d.runMax[1], d.runMax[2]);
        }
        const Vector3 center = (lo + hi) * 0.5f;
        const float radius = std::max(0.25f, (hi - lo).length() * 0.5f);
        Vector3 offset(0.5f, 0.3f, 0.8f);
        Vector3 up(0.0f, 1.0f, 0.0f);
        if (request.view == "front") offset = Vector3(0.0f, 0.0f, 1.0f);
        else if (request.view == "side") offset = Vector3(1.0f, 0.0f, 0.0f);
        else if (request.view == "top"){
            offset = Vector3(0.0f, 1.0f, 0.0f);
            up = Vector3(0.0f, 0.0f, -1.0f);
        }
        offset.normalize();
        CameraSetup& c = d.camera;
        c.tanHalf = std::tan(PREVIEW_FOV * 0.5f);
        const float distance = radius / std::sin(PREVIEW_FOV * 0.5f) * 1.05f;
        c.position = center + offset * distance;
        c.target = center;
        c.forward = (center - c.position);
        c.forward.normalize();
        c.right = c.forward.crossProduct(up);
        c.right.normalize();
        c.up = c.right.crossProduct(c.forward);
        c.up.normalize();
        d.frameCenter = center;
        d.frameWorldSize = 2.0f * distance * c.tanHalf;
        c.nearClip = std::max(0.01f, distance - radius * 1.5f);
        c.farClip = distance + radius * 1.5f + 1.0f;
        d.drawSize = request.cellSize;
        if (request.pixelGrid){
            // the pixel view: from the front, on whole base pixels around the run's box (an even
            // count, so the centre is a grid corner and every base pixel covers whole screen pixels)
            const float ppu = (float)std::max(request.pixelsPerUnit, 1);
            const int margin = 4;
            const int minX = (int)std::floor(lo.x * ppu) - margin, maxX = (int)std::ceil(hi.x * ppu) + margin;
            const int minY = (int)std::floor(lo.y * ppu) - margin, maxY = (int)std::ceil(hi.y * ppu) + margin;
            int side = std::max(maxX - minX, maxY - minY);
            side = std::clamp(side + (side & 1), 2, MAX_PIXEL_SIDE);
            const int cx = (int)std::floor((minX + maxX) * 0.5f), cy = (int)std::floor((minY + maxY) * 0.5f);
            const Vector3 grid((float)cx / ppu, (float)cy / ppu, center.z);
            c.ortho = true;
            c.orthoHalf = (float)side * 0.5f / ppu;
            c.position = grid + Vector3(0.0f, 0.0f, radius * 2.0f + 10.0f);
            c.target = grid;
            c.forward = Vector3(0.0f, 0.0f, -1.0f);
            c.right = Vector3(1.0f, 0.0f, 0.0f);
            c.up = Vector3(0.0f, 1.0f, 0.0f);
            c.nearClip = 0.01f;
            c.farClip = radius * 4.0f + 20.0f;
            d.frameCenter = grid;
            d.frameWorldSize = (float)side / ppu;
            d.pixelSide = side;
            // where the emitters' origin (world 0, 0) falls among the base pixels, rows from the top
            d.pixelOriginX = side / 2 - cx;
            d.pixelOriginY = side / 2 + cy;
            d.drawSize = side * PIXEL_PROBE;
            // the cells the filmstrip lays out: the base pixels at the asked scale
            request.cellSize = side * request.pixelScale;
        }

        d.look = std::make_unique<PreviewScene>();
        d.look->build(request, d.documents, d.seeds, needsGpu());
        // a pixel effect: an emitter of the tree draws on the grid (off_grid's subject, S49)
        {
            std::vector<TreeEmitter> emitters;
            d.look->emitters(emitters);
            for (const TreeEmitter& e : emitters)
                measured.pixelEffect = measured.pixelEffect || (e.runtime->program() && e.runtime->program()->render.pixel.active);
        }
        if (request.overdrawMap){
            std::vector<ParticleEffectDocument> heatDocs;
            for (const ParticleEffectDocument& doc : d.documents) heatDocs.push_back(heatDocument(doc));
            d.heat = std::make_unique<PreviewScene>();
            d.heat->build(request, heatDocs, d.seeds, true);
        }
        d.tickIndex = 0;
        d.gridStride = std::max<uint64_t>(1, (d.totalTicks + MAX_GRID_TICKS - 1) / MAX_GRID_TICKS);
        d.stage = Impl::Stage::Pass2;
    }

    if (d.stage == Impl::Stage::Pass2){
        // ---- pass 2: the analytic grid every (stride) tick, the cells drawn at their ticks
        while (true){
            const bool atCell = d.nextCell < d.cellTicks.size() && d.cellTicks[d.nextCell] == d.tickIndex;
            if (atCell){
                if (!drawing) return false;   // the editor's frame draws it
                if (d.drawStarted == 0.0) d.drawStarted = previewNow();
                PreviewScene& scene = d.drawPhase == 2 ? *d.heat : *d.look;
                PoseImage& target = d.drawPhase == 0 ? d.onBlack : d.drawPhase == 1 ? d.onWhite : d.heatImage;
                const Vector4 clear = d.drawPhase == 1 ? Vector4(1.0f, 1.0f, 1.0f, 1.0f) : Vector4(0.0f, 0.0f, 0.0f, 1.0f);
                if (drawScene(scene, d.camera, d.drawSize, clear, target, d.waitingFor) == Drawn::Wait){
                    if (previewNow() - d.drawStarted > RENDER_TIMEOUT_SECONDS){
                        finish("A cell could not be drawn within " + std::to_string((int)RENDER_TIMEOUT_SECONDS) + " s: still waiting for " + d.waitingFor +
                               ". Nothing is returned rather than a stand-in.");
                        return true;
                    }
                    return false;   // the next frame
                }
                d.waitingFor.clear();
                d.drawStarted = 0.0;
                d.drawPhase++;
                if (d.drawPhase == 2 && !d.heat) d.drawPhase = 3;
                if (d.drawPhase < 3) return false;   // one draw a frame
                d.drawPhase = 0;
                // the cell: premultiplied colour on black, coverage from black against white
                const int size = d.drawSize;
                PoseImage cell(size, size);
                double covered = 0.0, clipped = 0.0;
                for (int y = 0; y < size; y++){
                    for (int x = 0; x < size; x++){
                        const PoseColor b = d.onBlack.getPixel(x, y);
                        const PoseColor w = d.onWhite.getPixel(x, y);
                        const int alpha = std::clamp(255 - ((int)w.r - b.r + (int)w.g - b.g + (int)w.b - b.b) / 3, 0, 255);
                        const int lit = std::max({(int)b.r, (int)b.g, (int)b.b});
                        if (alpha > 2 || lit > 2){
                            covered += 1.0;
                            if (b.r >= 250 && b.g >= 250 && b.b >= 250) clipped += 1.0;
                        }
                        if (request.transparentCells){
                            // straight alpha; light without coverage (additive) keeps its brightest channel as alpha
                            const int a = std::max(alpha, lit);
                            if (a == 0){
                                cell.setPixel(x, y, PoseColor(0, 0, 0, 0));
                            }else{
                                auto un = [a](int v){ return (uint8_t)std::clamp((v * 255 + a / 2) / a, 0, 255); };
                                cell.setPixel(x, y, PoseColor(un(b.r), un(b.g), un(b.b), (uint8_t)a));
                            }
                        }else{
                            const PoseColor bg = backgroundAt(request.background, x, y);
                            auto over = [alpha](int premultiplied, int back){ return (uint8_t)std::clamp(premultiplied + (back * (255 - alpha) + 127) / 255, 0, 255); };
                            cell.setPixel(x, y, PoseColor(over(b.r, bg.r), over(b.g, bg.g), over(b.b, bg.b), 255));
                        }
                    }
                }
                const double percent = 100.0 * covered / (double)(size * size);
                const double clippedShare = covered > 0.0 ? 100.0 * clipped / covered : 0.0;
                measured.visibilityFromImages = true;
                measured.coverage.push_back({d.cellTimes[d.nextCell], percent});
                measured.maxCoverage = std::max(measured.maxCoverage, percent);
                if (clippedShare > measured.clippedPercent){
                    measured.clippedPercent = clippedShare;
                    measured.clippedAt = d.cellTimes[d.nextCell];
                }
                if (request.pixelGrid){
                    // the pixel view (S49): a base pixel whose screen pixels differ (on black or on
                    // white) was drawn between grid lines; the cell is the base pixels at pixelScale
                    const int side = d.pixelSide, scale = request.pixelScale;
                    auto same = [](const PoseColor& a, const PoseColor& b){ return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; };
                    PoseImage pixels(side * scale, side * scale);
                    // S48's effect lints read the base pixels premultiplied on transparent black:
                    // the draw on black is the colour, white minus black the transparency
                    jasperengine::editor::pixelart::PixelImage base(side, side);
                    for (int by = 0; by < side; by++){
                        for (int bx = 0; bx < side; bx++){
                            const int x0 = bx * PIXEL_PROBE, y0 = by * PIXEL_PROBE;
                            const PoseColor b0 = d.onBlack.getPixel(x0, y0), w0 = d.onWhite.getPixel(x0, y0);
                            bool even = true;
                            for (int sy = 0; sy < PIXEL_PROBE && even; sy++){
                                for (int sx = 0; sx < PIXEL_PROBE && even; sx++)
                                    even = same(d.onBlack.getPixel(x0 + sx, y0 + sy), b0) && same(d.onWhite.getPixel(x0 + sx, y0 + sy), w0);
                            }
                            const PoseColor c = cell.getPixel(x0, y0);
                            const bool drawn = !same(b0, PoseColor(0, 0, 0, 255)) || !same(w0, PoseColor(255, 255, 255, 255));
                            measured.pixelsDrawn += drawn ? 1 : 0;
                            const int through = std::max({(int)w0.r - (int)b0.r, (int)w0.g - (int)b0.g, (int)w0.b - (int)b0.b, 0});
                            base.set(bx, by, jasperengine::editor::pixelart::PixelRgba{b0.r, b0.g, b0.b, (uint8_t)std::clamp(255 - through, 0, 255)});
                            if (!even){
                                measured.offGrid++;
                                if (measured.offGridAt.size() < (size_t)qa::MAX_AT) measured.offGridAt.push_back({d.cellTimes[d.nextCell], {bx, by}});
                            }
                            for (int y = 0; y < scale; y++)
                                for (int x = 0; x < scale; x++) pixels.setPixel(bx * scale + x, by * scale + y, c);
                        }
                    }
                    measured.pixelGrid = true;
                    measured.pixelFrames.push_back(std::move(base));
                    measured.pixelTimes.push_back(d.cellTimes[d.nextCell]);
                    measured.pixelOriginX = d.pixelOriginX;
                    measured.pixelOriginY = d.pixelOriginY;
                    measured.pixelSeed = d.seeds.empty() ? 1u : d.seeds.front();
                    cell = std::move(pixels);
                }
                cells.push_back(std::move(cell));
                uint32_t live = 0;
                for (size_t i = 0; i < n; i++){
                    if (particles::ParticleSystemState* state = d.look->state(i)) live += liveOf(state->runtime);
                }
                d.cellLive.push_back(live);
                if (d.heat){
                    // layers per pixel, averaged over the analytic grid's cells
                    const int per = std::max(1, size / GRID);
                    std::vector<double> grid((size_t)GRID * GRID, 0.0);
                    for (int y = 0; y < GRID * per && y < size; y++){
                        for (int x = 0; x < GRID * per && x < size; x++){
                            grid[(size_t)(y / per) * GRID + (x / per)] += (double)d.heatImage.getPixel(x, y).r / HEAT_STEP;
                        }
                    }
                    double peak = 0.0, sum = 0.0;
                    int coveredCells = 0;
                    for (double& v : grid){
                        v /= (double)(per * per);
                        peak = std::max(peak, v);
                        if (v > 0.01){
                            sum += v;
                            coveredCells++;
                        }
                    }
                    measured.heat = true;
                    measured.heatPeakLayers = std::max(measured.heatPeakLayers, peak);
                    measured.heatAvgLayers += coveredCells ? sum / coveredCells : 0.0;
                    // the heat row: layers as a ramp
                    PoseImage heatCell(size, size);
                    for (int y = 0; y < size; y++){
                        for (int x = 0; x < size; x++){
                            const float layers = (float)d.heatImage.getPixel(x, y).r / HEAT_STEP;
                            const float k = std::clamp(layers / 16.0f, 0.0f, 1.0f);
                            heatCell.setPixel(x, y, PoseColor::fromFloat(k, std::clamp(k * 2.0f - 1.0f, 0.0f, 1.0f) * 0.9f, layers > 0.0f ? 0.25f * (1.0f - k) : 0.0f));
                        }
                    }
                    d.heatCells.push_back(std::move(heatCell));
                }
                // the analytic grid at this cell, for the heat comparison
                double peak = 0.0, avg = 0.0, coverage = 0.0, area = 0.0;
                d.projectGrid(*d.look, peak, avg, coverage, area);
                d.cellsAnalyticPeak = std::max(d.cellsAnalyticPeak, peak);
                measured.cellCoverage.push_back({percent, coverage});
                measured.analyticMaxCoverage = std::max(measured.analyticMaxCoverage, coverage);
                if (avg > 0.0){
                    d.cellsAnalyticAvgSum += avg;
                    d.cellsAnalyticCount++;
                }
                d.nextCell++;
                continue;
            }
            if (d.tickIndex >= d.totalTicks) break;
            if (overBudget()) return false;
            d.tickIndex++;
            const double t = (double)d.tickIndex * d.master;
            d.look->advanceTo(t);
            if (d.heat) d.heat->advanceTo(t);
            if (d.tickIndex % d.gridStride == 0 || d.tickIndex == d.totalTicks){
                double peak = 0.0, avg = 0.0, coverage = 0.0, area = 0.0;
                d.projectGrid(*d.look, peak, avg, coverage, area);
                if (peak > measured.peakLayers){
                    measured.peakLayers = peak;
                    measured.overdrawAt = t;
                }
                if (avg > 0.0){
                    d.overdrawAvgSum += avg;
                    d.overdrawAvgCount++;
                }
                // at 1080p with the frame's height 25% of the screen's
                measured.fillMpix = std::max(measured.fillMpix, area * 270.0 * 270.0 / 1.0e6);
                measured.analyticMaxCoverage = std::max(measured.analyticMaxCoverage, coverage);
                if (d.cellTicks.empty()){
                    measured.coverage.push_back({t, coverage});
                    measured.maxCoverage = std::max(measured.maxCoverage, coverage);
                }
                if (d.coveragePrev >= 0.0 && std::fabs(coverage - d.coveragePrev) > measured.coverageJump){
                    measured.coverageJump = std::fabs(coverage - d.coveragePrev);
                    measured.coverageJumpAt = t;
                }
                // the first cycle wrap of a looping effect
                if (measured.looping && measured.loopSeam < 0.0 && d.coveragePrev >= 0.0 && d.durationMax > 0.0 &&
                    std::fmod(t, d.durationMax) < d.master * (double)d.gridStride && t > d.durationMax * 0.5){
                    measured.loopSeam = std::fabs(coverage - d.coveragePrev);
                }
                d.coveragePrev = coverage;
            }
        }
        // the replay must have run as pass 1 did
        const uint64_t replayed = d.look->stateHash();
        if (replayed != d.runHash){
            finish("The effect did not repeat: the second simulation's state_hash " + hashText(replayed) + " differs from the first's " +
                   hashText(d.runHash) + ". Nothing is returned rather than images of another run.");
            return true;
        }
        measured.avgLayers = d.overdrawAvgCount ? d.overdrawAvgSum / (double)d.overdrawAvgCount : 0.0;
        double best = 0.0;
        for (const auto& [name, area] : d.emitterArea){
            if (area > best){
                best = area;
                measured.overdrawEmitter = name;
            }
        }
        if (measured.overdrawEmitter.empty() && !request.systems.empty()) measured.overdrawEmitter = request.systems.front().name;
        if (measured.heat && !cells.empty()) measured.heatAvgLayers /= (double)cells.size();
        d.stage = Impl::Stage::Compose;
    }

    if (d.stage == Impl::Stage::Compose){
        // the state the run ended in (the hash of pass 2's last tick, pass 1's too)
        composeFilmstrip();
        buildReport();
        d.look.reset();
        d.heat.reset();
        finish(std::string());
    }
    return true;
}

void ParticleEffectPreview::composeFilmstrip(){
    Impl& d = *impl;
    filmstrip.clear();
    if (cells.empty()) return;
    const bool reference = !request.reference.isEmpty();
    PoseSheetLayout layout;
    layout.columns = (int)cells.size() + (reference ? 1 : 0);
    layout.rows = d.heatCells.empty() ? 1 : 2;
    layout.cellWidth = request.cellSize;
    layout.cellHeight = request.cellSize;
    layout.labelHeight = request.labels ? PoseSheetLayout::LABEL_HEIGHT : 0;
    PoseContactSheet sheet(layout);
    for (size_t k = 0; k < cells.size(); k++){
        sheet.setCell((int)k, 0, cells[k]);
        if (request.labels){
            char label[64];
            std::snprintf(label, sizeof(label), "t=%.2f %u", d.cellTimes[k], d.cellLive[k]);
            sheet.setLabel((int)k, 0, label);
        }
        if (k < d.heatCells.size()){
            sheet.setCell((int)k, 1, d.heatCells[k]);
            if (request.labels) sheet.setLabel((int)k, 1, "overdraw");
        }
    }
    if (reference){
        PoseImage fitted(request.cellSize, request.cellSize, PoseColor(0, 0, 0, 255));
        const PoseImage& source = request.reference;
        // nearest-neighbour fit, aspect kept
        const float scale = std::min((float)request.cellSize / (float)source.getWidth(), (float)request.cellSize / (float)source.getHeight());
        const int w = std::max(1, (int)(source.getWidth() * scale)), h = std::max(1, (int)(source.getHeight() * scale));
        const int ox = (request.cellSize - w) / 2, oy = (request.cellSize - h) / 2;
        for (int y = 0; y < h; y++){
            for (int x = 0; x < w; x++){
                fitted.setPixel(ox + x, oy + y, source.getPixel(std::min(source.getWidth() - 1, (int)(x / scale)), std::min(source.getHeight() - 1, (int)(y / scale))));
            }
        }
        sheet.setCell((int)cells.size(), 0, fitted);
        if (request.labels) sheet.setLabel((int)cells.size(), 0, "reference");
    }
    filmstrip = sheet.getImage();
}

void ParticleEffectPreview::buildReport(){
    Impl& d = *impl;
    for (const ParticlePreviewSystem& system : request.systems){
        const particles::ParticleEffectDocument& doc = system.document;
        if ((doc.pixelArt.present && doc.pixelArt.enabled) || doc.renderer.renderMode == particles::RenderMode::Pixel) measured.pixelEffect = true;
    }
    ParticleCheckRequest checkRequest;
    checkRequest.platform = request.platform;
    checkRequest.qa = request.qa;
    checkRequest.hasImages = !cells.empty();
    checkRequest.sceneColor = !request.transparentCells;
    Json rows = ParticleEffectChecks::rows(measured, checkRequest);
    const bool stop = qa::countRounds(rows, request.rounds, request.qa.fixRounds);
    const std::string verdict = qa::verdict(rows);
    Json metrics = ParticleEffectChecks::metrics(measured, request.platform);
    if (measured.heat){
        // the analytic numbers at the same cells, beside the heat row's
        metrics["overdraw"]["heat"]["analytic_peak_layers"] = std::round(d.cellsAnalyticPeak * 100.0) / 100.0;
        metrics["overdraw"]["heat"]["analytic_avg_layers"] = d.cellsAnalyticCount ? std::round(d.cellsAnalyticAvgSum / d.cellsAnalyticCount * 100.0) / 100.0 : 0.0;
    }
    Json seeds = Json::array();
    for (size_t i = 0; i < d.seeds.size(); i++) seeds.push_back({{"system", request.systems[i].name}, {"seed", d.seeds[i]}});
    Json cellList = Json::array();
    for (size_t k = 0; k < cells.size(); k++) cellList.push_back({{"t", std::round(d.cellTimes[k] * 1000.0) / 1000.0}, {"live", d.cellLive[k]}});
    // a single-edit fix the window's Fix button can apply: Max Particles of the emitter that refused births
    Json fixes = Json::array();
    if (measured.neverBorn > 0){
        std::string asset;
        int system = -1;
        for (size_t i = 0; i < request.systems.size(); i++){
            if (measured.capacityEmitter == request.systems[i].name){
                system = (int)i;
                asset = request.systems[i].asset;
            }
        }
        const size_t marker = measured.capacityEmitter.rfind(" > ");
        if (system < 0 && marker != std::string::npos) asset = measured.capacityEmitter.substr(marker + 3);
        fixes.push_back({{"check", "capacity_hit"}, {"system", system}, {"path", asset},
                         {"patch", {{"maxParticles", std::min<uint64_t>(std::max(measured.capacityNeeded, measured.capacityMax + 1), particles::kMaxParticlesPerEmitter)}}}});
    }
    report = {{"format", "jasperengine.particle-check"}, {"version", 1}, {"verdict", verdict}, {"seeds", seeds},
              {"tick_seconds", std::round(d.master * 1e6) / 1e6}, {"ticks", measured.ticks}, {"seconds", std::round(measured.seconds * 1000.0) / 1000.0},
              {"state_hash", hashText(d.runHash)}, {"platform", request.platform},
              {"metrics", metrics}, {"checks", rows}, {"fixes", fixes}, {"thresholds", ParticleEffectChecks::thresholds(request.qa)},
              {"guidance", qa::guidance(stop, verdict == "FAIL", request.qa.fixRounds, "preview again", ParticleEffectChecks::CLOSING)},
              {"cells", cellList},
              {"frame", {{"view", request.view}, {"center", {d.frameCenter.x, d.frameCenter.y, d.frameCenter.z}},
                         {"world_size", std::round(d.frameWorldSize * 1000.0) / 1000.0}}}};
    if (request.pixelGrid && d.pixelSide > 0){
        // the pixel view: base pixels each way, and a cell's pixels a base pixel
        report["frame"]["view"] = "pixel";
        report["frame"]["base_pixels"] = d.pixelSide;
        report["frame"]["scale"] = request.pixelScale;
    }
}

// ---------------------------------------------------------------- driving

bool ParticleEffectPreview::runCpu(const ParticlePreviewRequest& request, qa::Json& report, std::string& error){
    ParticlePreviewRequest metricsOnly = request;
    metricsOnly.frames = 0;
    metricsOnly.times.clear();
    metricsOnly.overdrawMap = false;
    error = validate(metricsOnly);
    if (!error.empty()) return false;
    ParticleEffectPreview run(metricsOnly);
    while (!run.advance(1.0e9, false)){}
    if (run.isFailed()){
        error = run.getError();
        return false;
    }
    report = run.getReport();
    return true;
}

bool ParticleEffectPreview::processFrame(){
    bool running = false;
    // a copy: a finished run's callback may destroy runs
    const std::vector<ParticleEffectPreview*> runs = liveRuns();
    const double frameStart = previewNow();
    for (ParticleEffectPreview* run : runs){
        auto& live = liveRuns();
        if (std::find(live.begin(), live.end(), run) == live.end() || !run->started || run->finished) continue;
        const double left = FRAME_BUDGET_MS - (previewNow() - frameStart) * 1000.0;
        run->advance(std::max(left, 1.0), previewInFrame());
        if (std::find(liveRuns().begin(), liveRuns().end(), run) != liveRuns().end() && !run->finished) running = true;
    }
    if (running && previewFrameRequester()) previewFrameRequester()();
    return running;
}

void ParticleEffectPreview::setFrameRequester(std::function<void()> requester){
    previewFrameRequester() = std::move(requester);
}

void ParticleEffectPreview::setInEditorFrame(bool inFrame){
    previewInFrame() = inFrame;
}

void ParticleEffectPreview::releaseGpu(){
    const std::vector<ParticleEffectPreview*> runs = liveRuns();
    for (ParticleEffectPreview* run : runs){
        auto& live = liveRuns();
        if (std::find(live.begin(), live.end(), run) != live.end() && !run->finished) run->finish("The editor is closing.");
    }
}
