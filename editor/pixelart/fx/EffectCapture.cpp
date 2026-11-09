// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "EffectCapture.h"

#include "Engine.h"
#include "Project.h"
#include "Scene.h"
#include "animation/ClipConverters.h"
#include "component/AnimationPlayerComponent.h"
#include "component/AnimatorComponent.h"
#include "component/MeshComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/Transform.h"
#include "manager/SceneManager.h"
#include "math/AABB.h"
#include "object/Camera.h"
#include "particles/ParticleEffectLibrary.h"
#include "pixelart/Palette.h"
#include "pixelart/PaletteMaterial.h"
#include "pool/AnimationClipPool.h"
#include "registry/EntityCloner.h"
#include "render/OffscreenDraw.h"
#include "render/RenderUtil.h"
#include "texture/Framebuffer.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <cmath>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::pixelart;
using namespace jasperengine::editor::pixelart::fx;

namespace {

    double captureNow() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::vector<EffectCapture*>& liveCaptures() {
        static std::vector<EffectCapture*> runs;
        return runs;
    }

    std::function<void()>& captureFrameRequester() {
        static std::function<void()> requester;
        return requester;
    }

    // Copies pin their randomness and run on the capture's one clock: a Particle System picking a
    // new seed at every play (seed 0) takes the run's seed, an explicit seed is kept for the sheet
    // (run 1) and varied for spill's other runs, and nothing reads the engine's unscaled time
    void pinCopies(Scene* scene, const std::vector<Entity>& roots, uint32_t runSeed) {
        for (Entity root : roots) {
            for (Entity e : EntityCloner::collectHierarchy(scene, root)) {
                if (ParticleSystemComponent* p = scene->findComponent<ParticleSystemComponent>(e)) {
                    if (p->seed == 0) p->seed = runSeed;
                    else if (runSeed != 1) p->seed = p->seed * 31u + runSeed;
                    p->useUnscaledTime = false;
                    p->needReload = true;
                }
            }
        }
    }

    // the scene's roots (no parent), every GameObject of it in collectHierarchy order
    std::vector<Entity> wholeScene(SceneProject* source) {
        std::vector<Entity> all;
        std::set<Entity> seen;
        for (Entity e : source->entities) {
            const Transform* t = source->scene->findComponent<Transform>(e);
            if (t && t->parent != NULL_ENTITY) continue;
            for (Entity member : EntityCloner::collectHierarchy(source->scene, e)) {
                if (seen.insert(member).second) all.push_back(member);
            }
        }
        return all;
    }

    // the first palette a copy's material draws with (S43), for off_palette
    bool findPalette(Scene* scene, const std::vector<Entity>& roots, std::vector<jasperengine::pixelart::Rgba8>& colors, std::string& name) {
        for (Entity root : roots) {
            for (Entity e : EntityCloner::collectHierarchy(scene, root)) {
                const MeshComponent* mesh = scene->findComponent<MeshComponent>(e);
                if (!mesh) continue;
                for (unsigned int i = 0; i < mesh->numSubmeshes; i++) {
                    const jasperengine::pixelart::PaletteBinding* binding = jasperengine::pixelart::findPaletteBinding(mesh->submeshes[i].material);
                    if (!binding || binding->source.empty()) continue;
                    std::shared_ptr<const jasperengine::pixelart::Palette> palette = jasperengine::pixelart::PaletteLibrary::get(binding->source.path);
                    if (!palette || palette->colors.empty()) continue;
                    colors = palette->colors;
                    name = binding->source.path;
                    return true;
                }
            }
        }
        return false;
    }

    // ------------------------------------------------------------------ targets

    class EntityTarget : public CaptureTarget {
    public:
        Project* project = nullptr;
        SceneProject* scene = nullptr;
        Entity entity = NULL_ENTITY;
        std::string clip;
        double clipLength = 0.0;
        bool clipLoops = false;

        std::string label() const override {
            const std::string name = scene->scene->getEntityName(entity);
            return (name.empty() ? "GameObject " + std::to_string(entity) : name) + (clip.empty() ? "" : " playing " + clip);
        }
        double defaultDuration() const override { return clip.empty() ? 1.0 : std::max(clipLength, 1.0 / 60.0); }
        bool defaultLoop() const override { return !clip.empty() && clipLoops; }
        SceneProject* sourceScene() const override { return scene; }

        bool build(Scene* into, uint32_t runSeed, std::vector<Entity>& made, std::string& error) override {
            if (!scene->scene || !scene->scene->isEntityCreated(entity)) {
                error = "the GameObject is gone from its scene";
                return false;
            }
            const std::vector<Entity> copies = EntityCloner::clone(scene->scene, EntityCloner::collectHierarchy(scene->scene, entity), into);
            if (copies.empty()) {
                error = "the GameObject could not be copied";
                return false;
            }
            made = {copies.front()};
            if (!clip.empty()) {
                // the clip on an Animation Player of the copy's root, which an Animator would fight
                if (AnimatorComponent* animator = into->findComponent<AnimatorComponent>(copies.front())) animator->enabled = false;
                AnimationPlayerComponent player;
                player.clips = {ClipConverters::playerEntry(project, clip)};
                const AnimationPlayerClip& entry = player.clips.front();
                player.defaultClip = entry.name.empty() ? std::filesystem::path(entry.clip).stem().string() : entry.name;
                player.playAutomatically = true;
                if (into->findComponent<AnimationPlayerComponent>(copies.front())) into->removeComponent<AnimationPlayerComponent>(copies.front());
                into->addComponent<AnimationPlayerComponent>(copies.front(), player);
            }
            pinCopies(into, made, runSeed);
            return true;
        }
    };

    class ParticlesTarget : public CaptureTarget {
    public:
        std::string effect;
        double duration = 1.0;
        bool looping = false;

        std::string label() const override { return effect; }
        double defaultDuration() const override { return duration; }
        bool defaultLoop() const override { return looping; }

        bool build(Scene* into, uint32_t runSeed, std::vector<Entity>& made, std::string& error) override {
            (void)error;
            const Entity e = into->createEntity();
            into->setEntityName(e, "Particle System");
            into->addComponent<Transform>(e, {});
            ParticleSystemComponent particles;
            particles.effect = AssetRef(effect);
            particles.playOnAwake = true;
            particles.seed = runSeed;
            into->addComponent<ParticleSystemComponent>(e, particles);
            made = {e};
            pinCopies(into, made, runSeed);
            return true;
        }
    };

    // ------------------------------------------------------------------ passes

    // one fresh run of the target from t = 0, its frames read at `times`
    struct Pass {
        uint32_t seed = 1;
        std::vector<double> times;
        std::vector<PixelImage>* out = nullptr;
        bool cost = false;          // frame_cost: the target shown, then hidden
        bool reload = false;        // Play: the game's scene loaded again first (a fresh capture)
    };

    // the scratch scene of one pass
    struct Scratch {
        Scene* scene = nullptr;
        Camera* camera = nullptr;
        Entity cameraEntity = NULL_ENTITY;
        std::vector<Entity> made;
        double simTime = 0.0;
        double fixedAccumulator = 0.0;
        bool warmed = false;
        bool drawPending = false;   // stepped to the next time, not read back yet

        Scratch() = default;
        Scratch(const Scratch&) = delete;
        Scratch& operator=(const Scratch&) = delete;
        ~Scratch() {
            delete camera;
            delete scene;
        }
    };

    double median(std::vector<double> v) {
        if (v.empty()) return -1.0;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    }

}

struct EffectCapture::Impl {
    enum class Stage { Setup, Passes, Done };
    Stage stage = Stage::Setup;
    std::vector<Pass> passes;
    size_t pass = 0;
    size_t frame = 0;
    std::unique_ptr<Scratch> scratch;
    Framebuffer target;
    bool framed = false;
    float centerX = 0.0f, centerY = 0.0f;   // the frame's centre (world, isolated)
    std::string waiting = "starting";
    double startedAt = 0.0;
    int waitFrames = 0;
    size_t totalFrames = 0, doneFrames = 0;
    // frame_cost
    std::vector<double> shown, hidden;
    // the lints' frames (owned here; the lint input points into the output)
    std::vector<PixelImage> spill[3];
    std::vector<PixelImage> lastFrame, determinismA, determinismB, costFrames;   // costFrames: frame_cost's pass reads none
    // Play
    bool playing = false;
    bool resumeAfter = false;
    double fixedBefore = 0.0;
    uint64_t stepsDone = 0;
    bool stepPending = false;
    bool reloadIssued = false;
    std::shared_ptr<void> advance;
    bool warmedPlay = false;
};

EffectCapture::EffectCapture(CaptureSettings s, std::unique_ptr<CaptureTarget> t) : settings(std::move(s)), target(std::move(t)), impl(std::make_unique<Impl>()) {}

EffectCapture::~EffectCapture() {
    auto& runs = liveCaptures();
    runs.erase(std::remove(runs.begin(), runs.end(), this), runs.end());
    if (!finished) finish("The capture was dropped.");
    impl->target.destroy();
}

void EffectCapture::start() {
    if (started) return;
    started = true;
    impl->startedAt = captureNow();
    liveCaptures().push_back(this);
    if (captureFrameRequester()) captureFrameRequester()();
}

void EffectCapture::cancel() {
    if (!finished) finish("Cancelled.");
}

double EffectCapture::getProgress() const {
    if (finished) return 1.0;
    return impl->totalFrames ? (double)impl->doneFrames / (double)impl->totalFrames : 0.0;
}

std::string EffectCapture::getStage() const {
    if (finished) return error.empty() ? "done" : "failed";
    return "waiting for " + impl->waiting;
}

void EffectCapture::finish(const std::string& failure) {
    if (finished) return;
    Impl& d = *impl;
    d.scratch.reset();
    // Play: the game as it was (its clock, its pause)
    if (d.playing) {
        Engine::setFixedDeltatime(d.fixedBefore);
        Project* project = settings.project;
        if (d.resumeAfter && project && project->getPlaySessionSceneId() == settings.sceneId) project->resume(settings.sceneId);
        d.advance.reset();
        d.playing = false;
    }
    error = failure;
    finished = true;
    d.stage = Impl::Stage::Done;
    if (error.empty()) {
        // the lint input points into the output, which no longer moves
        lint::EffectLintInput& in = output.lintInput;
        in.frames = lint::frameView(output.frames);
        in.times = output.times;
        in.duration = settings.duration;
        in.loop = settings.loop;
        in.bounds = output.bounds;
        in.margin = settings.margin;
        in.spillApplies = !output.opaque;
        in.spillSkipReason = "scene mode draws the whole view";
        in.spillSeeds = {1};
        in.spillRuns.clear();
        output.spillRuns.clear();
        for (int k = 0; k < 3; k++) {
            if (!d.spill[k].empty()) output.spillRuns.push_back(std::move(d.spill[k]));
        }
        static const uint32_t extraSeeds[3] = {2, 3, 7};
        for (size_t k = 0; k < output.spillRuns.size(); k++) {
            in.spillRuns.push_back(lint::frameView(output.spillRuns[k]));
            in.spillSeeds.push_back(extraSeeds[k]);
        }
        if (!d.lastFrame.empty()) output.last = std::move(d.lastFrame.front());
        in.last = output.last.empty() ? nullptr : &output.last;
        if (!d.determinismA.empty() && !d.determinismB.empty()) {
            output.determinism[0] = std::move(d.determinismA.front());
            output.determinism[1] = std::move(d.determinismB.front());
            in.determinismFirst = &output.determinism[0];
            in.determinismSecond = &output.determinism[1];
        } else {
            in.determinismSkipReason = settings.lints ? "two fresh captures were not made" : "lints were not asked for";
        }
        in.determinismTime = 0.37 * settings.duration;
        if (d.shown.empty()) {
#ifdef NDEBUG
            in.cost.skipReason = output.opaque ? "the frame cost is measured in isolated mode" : "the frame cost was not measured";
#else
            in.cost.skipReason = "the frame cost is measured in release builds";
#endif
        } else {
            in.cost.shownMs = median(d.shown);
            in.cost.hiddenMs = median(d.hidden);
        }
        in.hitStopSkipReason = settings.mode == CaptureMode::Scene && settings.sceneId != 0
            ? "this build records no hit-stop requests"
            : "hit-stops are measured in a scene-mode capture of the playing game";
        target->describeLints(in);
    }
    if (onFinished) {
        std::function<void()> done = onFinished;
        done();
    }
}

bool EffectCapture::processFrame() {
    bool running = false;
    const std::vector<EffectCapture*> runs = liveCaptures();
    const double frameStart = captureNow();
    for (EffectCapture* run : runs) {
        auto& live = liveCaptures();
        if (std::find(live.begin(), live.end(), run) == live.end() || !run->started || run->finished) continue;
        const double left = kFrameBudgetMs - (captureNow() - frameStart) * 1000.0;
        run->advance(std::max(left, 1.0));
        auto& after = liveCaptures();
        if (std::find(after.begin(), after.end(), run) != after.end() && !run->finished) running = true;
    }
    if (running && captureFrameRequester()) captureFrameRequester()();
    return running;
}

void EffectCapture::setFrameRequester(std::function<void()> requester) {
    captureFrameRequester() = std::move(requester);
}

void EffectCapture::releaseGpu() {
    const std::vector<EffectCapture*> runs = liveCaptures();
    for (EffectCapture* run : runs) {
        if (!run->finished) run->finish("The editor is closing.");
        run->impl->target.destroy();
    }
}

namespace {

    // the frame of a scene-mode capture: the canvas divided by the smallest whole number that
    // brings it within maxWidth (the camera still covers the whole canvas)
    void sceneFrameSize(int canvasW, int canvasH, int maxWidth, int& w, int& h) {
        canvasW = std::max(canvasW, 1);
        canvasH = std::max(canvasH, 1);
        int k = 1;
        while (canvasW / k > maxWidth) k++;
        w = std::max(1, canvasW / k);
        h = std::max(1, canvasH / k);
    }

    bool createTarget(Framebuffer& target, int w, int h) {
        if (target.isCreated() && (int)target.getWidth() == w && (int)target.getHeight() == h) return true;
        target.destroy();
        target.setWidth((unsigned int)w);
        target.setHeight((unsigned int)h);
        target.create();
        return target.isCreated();
    }

    PixelImage toImage(int w, int h, const std::vector<uint8_t>& rgba) {
        PixelImage image(w, h);
        std::span<uint8_t> pixels = image.editPixels();
        if (rgba.size() >= pixels.size()) std::copy(rgba.begin(), rgba.begin() + (std::ptrdiff_t)pixels.size(), pixels.begin());
        return image;
    }

}

bool EffectCapture::advance(double budgetMs) {
    if (finished) return true;
    Impl& d = *impl;
    const double sliceStart = captureNow();
    auto overBudget = [&]() { return (captureNow() - sliceStart) * 1000.0 > budgetMs; };
    if (captureNow() - d.startedAt > kTimeoutSeconds) {
        finish("The capture did not finish within " + std::to_string((int)kTimeoutSeconds) + " s (it waited for " + d.waiting + ").");
        return true;
    }
    Project* project = settings.project;
    const std::vector<std::string>& ids = settings.lintIds;
    auto wanted = [&](const char* id) { return settings.lints && (ids.empty() || std::find(ids.begin(), ids.end(), id) != ids.end()); };
    const bool inPlay = settings.mode == CaptureMode::Scene && settings.sceneId != 0;

    if (d.stage == Impl::Stage::Setup) {
        // the passes: the sheet's frames, then the lints' fresh runs
        Pass main;
        main.times = settings.times;
        main.out = &output.frames;
        d.passes.push_back(main);
        if (settings.lints && !settings.loop && wanted("one_shot_end") && !inPlay &&
            (settings.times.empty() || settings.times.back() < settings.duration - 1e-9)) {
            d.passes.push_back(Pass{1, {settings.duration}, &d.lastFrame});
        }
        if (wanted("spill") && settings.mode == CaptureMode::Isolated) {
            const uint32_t seeds[3] = {2, 3, 7};
            for (int k = 0; k < 3; k++) d.passes.push_back(Pass{seeds[k], settings.times, &d.spill[k]});
        }
        if (wanted("determinism")) {
            const double t = 0.37 * settings.duration;
            d.passes.push_back(Pass{1, {t}, &d.determinismA, false, inPlay});
            d.passes.push_back(Pass{1, {t}, &d.determinismB, false, inPlay});
        }
#ifdef NDEBUG
        if (wanted("frame_cost") && settings.mode == CaptureMode::Isolated) {
            d.passes.push_back(Pass{1, {0.37 * settings.duration}, &d.costFrames, true});
        }
#endif
        for (const Pass& p : d.passes) d.totalFrames += p.times.size() + (p.cost ? 18 : 0);
        output.times = settings.times;
        if (inPlay) {
            SceneProject* sp = project ? project->getScene(settings.sceneId) : nullptr;
            if (!sp || project->getPlaySessionSceneId() != settings.sceneId) {
                finish("Nothing is playing any more.");
                return true;
            }
            d.playing = true;
            d.fixedBefore = Engine::getFixedDeltatime();
            Engine::setFixedDeltatime(kStep);
            if (settings.acquireAdvance) d.advance = settings.acquireAdvance();
            if (sp->playState == ScenePlayState::PLAYING) {
                project->pause(settings.sceneId);
                d.resumeAfter = true;
            }
            sceneFrameSize(Engine::getCanvasWidth(), Engine::getCanvasHeight(), settings.sceneMaxWidth, output.width, output.height);
            output.opaque = true;
            output.bounds = {0, 0, output.width, output.height};
            output.sceneColor = jasperengine::pixelart::Rgba8{21, 19, 29, 255};
            if (!createTarget(d.target, output.width, output.height)) {
                finish("The capture's framebuffer could not be made.");
                return true;
            }
        }
        d.stage = Impl::Stage::Passes;
    }

    while (d.stage == Impl::Stage::Passes && !finished) {
        if (d.pass >= d.passes.size()) {
            finish("");
            return true;
        }
        Pass& pass = d.passes[d.pass];

        // ---------------------------------------------------------------- Play: the game itself
        if (inPlay) {
            SceneProject* sp = project ? project->getScene(settings.sceneId) : nullptr;
            if (!sp || project->getPlaySessionSceneId() != settings.sceneId) {
                finish("Play stopped during the capture.");
                return true;
            }
            if (d.stepPending) {
                if (project->isStepPending(settings.sceneId)) {
                    d.waiting = "the game's step";
                    return false;
                }
                d.stepPending = false;
                // a fresh capture counts its steps from the first frame after the reload
                if (pass.reload && SceneManager::isLoading()) d.stepsDone = 0;
                else d.stepsDone++;
            }
            if (pass.reload && !d.reloadIssued) {
                // a fresh capture: the game's scene loaded again, counted from its first frame
                SceneManager::loadScene(SceneManager::getCurrentSceneId());
                d.reloadIssued = true;
                d.stepsDone = 0;
                project->step(settings.sceneId);
                d.stepPending = true;
                d.waiting = "the scene's reload";
                return false;
            }
            if (pass.reload && SceneManager::isLoading()) {
                project->step(settings.sceneId);
                d.stepPending = true;
                d.waiting = "the scene's reload";
                return false;
            }
            const double t = pass.times[d.frame];
            const uint64_t want = (uint64_t)std::llround(t / kStep);
            if (d.stepsDone < want) {
                project->step(settings.sceneId);
                d.stepPending = true;
                d.waiting = "the game's step";
                return false;
            }
            std::vector<Engine::ViewLayer> layers;
            for (Scene* scene : Engine::getScenesSnapshot()) {
                if (!scene || scene->getCamera() == NULL_ENTITY) continue;
                Engine::ViewLayer layer;
                layer.scene = scene;
                layer.camera = scene->getCamera();
                layer.viewRect = Rect(0.0f, 0.0f, (float)output.width, (float)output.height);
                layer.canvasWidth = (float)Engine::getCanvasWidth();
                layer.canvasHeight = (float)Engine::getCanvasHeight();
                layers.push_back(layer);
            }
            if (layers.empty()) {
                finish("The playing game has no scene with a camera to capture.");
                return true;
            }
            std::vector<uint8_t> rgba;
            if (drawOffscreen(CONTACT_SHEET_VIEW_ID, d.target, layers, d.warmedPlay, rgba, d.waiting) == OffscreenDraw::Wait) return false;
            PixelImage image = toImage(output.width, output.height, rgba);
            // the view is opaque: its clear colour is the frame's background
            for (uint8_t* p = image.editPixels().data(), *end = p + image.pixels().size(); p < end; p += 4) p[3] = 255;
            pass.out->push_back(std::move(image));
            d.doneFrames++;
            if (++d.frame >= pass.times.size()) {
                d.frame = 0;
                d.pass++;
                d.reloadIssued = false;
            }
            if (overBudget()) return false;
            continue;
        }

        // ---------------------------------------------------------------- a scratch scene
        if (!d.scratch) {
            auto s = std::make_unique<Scratch>();
            s->scene = new Scene(EntityPool::System);
            std::string failure;
            if (settings.mode == CaptureMode::Isolated) {
                s->scene->setBackgroundColor(0.0f, 0.0f, 0.0f, 0.0f);
                if (!target->build(s->scene, pass.seed, s->made, failure)) {
                    finish("The target could not be captured: " + failure + ".");
                    return true;
                }
                s->camera = new Camera(s->scene);
                s->camera->setType(CameraType::CAMERA_ORTHO);
                s->camera->setAutoResize(false);
                s->scene->setCamera(s->camera);
                s->cameraEntity = s->camera->getEntity();
            } else {
                SceneProject* source = target->sourceScene();
                if (!source || !source->scene) {
                    finish("The target's scene is not open.");
                    return true;
                }
                std::unordered_map<Entity, Entity> map;
                const std::vector<Entity> all = wholeScene(source);
                const std::vector<Entity> copies = EntityCloner::clone(source->scene, all, s->scene, NULL_ENTITY, &map);
                for (size_t i = 0; i < all.size() && i < copies.size(); i++) {
                    const Transform* t = source->scene->findComponent<Transform>(all[i]);
                    if (!t || t->parent == NULL_ENTITY) s->made.push_back(copies[i]);
                }
                pinCopies(s->scene, s->made, pass.seed);
                const Vector4 clear = source->scene->getBackgroundColor();
                s->scene->setBackgroundColor(clear.x, clear.y, clear.z, 1.0f);
                Entity camera = source->mainCamera != NULL_ENTITY ? source->mainCamera : source->scene->getCamera();
                auto found = map.find(camera);
                if (found == map.end()) {
                    finish("The target's scene has no camera to capture through.");
                    return true;
                }
                s->cameraEntity = found->second;
                s->scene->setCamera(s->cameraEntity);
                output.sceneColor = jasperengine::pixelart::Rgba8{(uint8_t)std::lround(std::clamp(clear.x, 0.0f, 1.0f) * 255.0f),
                                                                  (uint8_t)std::lround(std::clamp(clear.y, 0.0f, 1.0f) * 255.0f),
                                                                  (uint8_t)std::lround(std::clamp(clear.z, 0.0f, 1.0f) * 255.0f), 255};
            }
            s->scene->load();
            // the geometry (sprites, meshes) and their bounds exist after an update
            s->scene->update(0.0);
            s->scene->update(0.0);
            if (!d.framed) {
                if (settings.mode == CaptureMode::Isolated) {
                    // the target's bounds at t = 0 (meshes; a GameObject without one is a point)
                    AABB box;
                    for (Entity root : s->made) {
                        for (Entity e : EntityCloner::collectHierarchy(s->scene, root)) {
                            const MeshComponent* mesh = s->scene->findComponent<MeshComponent>(e);
                            if (mesh && !mesh->worldAABB.isNull() && mesh->worldAABB.isFinite()) {
                                box.merge(mesh->worldAABB);
                            } else if (const Transform* t = s->scene->findComponent<Transform>(e)) {
                                box.merge(t->worldPosition);
                            }
                        }
                    }
                    const float ppu = (float)std::max(settings.pixelsPerUnit, 1);
                    const Vector3 lo = box.isNull() ? Vector3(0, 0, 0) : box.getMinimum();
                    const Vector3 hi = box.isNull() ? Vector3(0, 0, 0) : box.getMaximum();
                    const int minX = (int)std::floor(lo.x * ppu + 1e-3f), maxX = std::max(minX + 1, (int)std::ceil(hi.x * ppu - 1e-3f));
                    const int minY = (int)std::floor(lo.y * ppu + 1e-3f), maxY = std::max(minY + 1, (int)std::ceil(hi.y * ppu - 1e-3f));
                    // twice the margin, so a spill past it shows; an even count each way puts the
                    // camera's centre on a pixel corner
                    const int pad = 2 * std::max(settings.margin, 0);
                    int fx0 = minX - pad, fx1 = maxX + pad, fy0 = minY - pad, fy1 = maxY + pad;
                    auto fit = [&](int& a, int& b) {
                        if ((b - a) % 2) b++;
                        if (b - a > settings.maxSide) {
                            const int c = (a + b) / 2;
                            a = c - settings.maxSide / 2;
                            b = a + settings.maxSide;
                        }
                    };
                    fit(fx0, fx1);
                    fit(fy0, fy1);
                    output.width = fx1 - fx0;
                    output.height = fy1 - fy0;
                    output.bounds = {minX - fx0, fy1 - maxY, maxX - minX, maxY - minY};
                    output.groundY = std::min(output.height - 1, output.bounds.y + output.bounds.h);
                    d.centerX = (float)(fx0 + fx1) * 0.5f / ppu;
                    d.centerY = (float)(fy0 + fy1) * 0.5f / ppu;
                } else {
                    sceneFrameSize((int)project->getCanvasWidth(), (int)project->getCanvasHeight(), settings.sceneMaxWidth, output.width, output.height);
                    output.opaque = true;
                    output.bounds = {0, 0, output.width, output.height};
                }
                std::vector<jasperengine::pixelart::Rgba8> colors;
                std::string paletteName;
                if (findPalette(s->scene, s->made, colors, paletteName)) {
                    output.lintInput.palette = std::move(colors);
                    output.lintInput.paletteName = paletteName;
                }
                d.framed = true;
                if (!createTarget(d.target, output.width, output.height)) {
                    finish("The capture's framebuffer could not be made.");
                    return true;
                }
            }
            if (s->camera) {
                const float ppu = (float)std::max(settings.pixelsPerUnit, 1);
                const float hw = (float)output.width * 0.5f / ppu, hh = (float)output.height * 0.5f / ppu;
                s->camera->setOrtho(-hw, hw, -hh, hh, 0.1f, 1000.0f);
                s->camera->setPosition(d.centerX, d.centerY, 100.0f);
                s->camera->setTarget(d.centerX, d.centerY, 0.0f);
                s->scene->update(0.0);
            }
            d.scratch = std::move(s);
            d.frame = 0;
            if (overBudget()) return false;
        }

        Scratch& s = *d.scratch;
        Engine::ViewLayer layer;
        layer.scene = s.scene;
        layer.camera = s.cameraEntity;
        layer.viewRect = Rect(0.0f, 0.0f, (float)output.width, (float)output.height);
        layer.postProcessing = output.opaque;
        layer.disableFog = !output.opaque;
        if (output.opaque) {
            layer.canvasWidth = (float)project->getCanvasWidth();
            layer.canvasHeight = (float)project->getCanvasHeight();
        }

        if (pass.cost) {
            // the target shown, then hidden: a frame's update and draw, 9 times each
            if (!s.warmed || !s.drawPending) {
                const double t = pass.times.front();
                while (s.simTime + 1e-9 < t) {
                    const double step = std::min(kStep, t - s.simTime);
                    s.scene->update(step);
                    s.simTime += step;
                }
                s.drawPending = true;
            }
            std::vector<uint8_t> rgba;
            if (!s.warmed) {
                if (drawOffscreen(CONTACT_SHEET_VIEW_ID, d.target, {layer}, s.warmed, rgba, d.waiting) == OffscreenDraw::Wait) return false;
            }
            const double previousPin = Engine::getPinnedShaderTime();
            Engine::setPinnedShaderTime(pass.times.front());
            for (int round = 0; round < 2; round++) {
                for (int k = 0; k < 9; k++) {
                    const double t0 = captureNow();
                    s.scene->update(0.0);
                    Engine::drawView(CONTACT_SHEET_VIEW_ID, &d.target, {layer});
                    (round == 0 ? d.shown : d.hidden).push_back((captureNow() - t0) * 1000.0);
                    d.doneFrames++;
                }
                for (Entity root : s.made) s.scene->setEntityActive(root, false);
            }
            Engine::setPinnedShaderTime(previousPin);
            d.scratch.reset();
            d.pass++;
            continue;
        }

        // step to the frame's time on the capture's clock (whole 1/60 s steps, the last one short)
        if (!s.drawPending) {
            const double t = pass.times[d.frame];
            while (s.simTime + 1e-9 < t) {
                const double step = std::min(kStep, t - s.simTime);
                s.fixedAccumulator += step;
                while (s.fixedAccumulator + 1e-9 >= kStep) {
                    s.scene->fixedUpdate(kStep);
                    s.fixedAccumulator -= kStep;
                }
                s.scene->advanceSceneTime(step);
                s.scene->update(step);
                s.simTime += step;
            }
            target->seek(s.scene, s.made, t);
            s.drawPending = true;
        }
        const double previousPin = Engine::getPinnedShaderTime();
        Engine::setPinnedShaderTime(pass.times[d.frame]);
        std::vector<uint8_t> rgba;
        const OffscreenDraw drawn = drawOffscreen(CONTACT_SHEET_VIEW_ID, d.target, {layer}, s.warmed, rgba, d.waiting);
        Engine::setPinnedShaderTime(previousPin);
        if (drawn == OffscreenDraw::Wait) {
            if (++d.waitFrames > 1200) {
                finish("The target's shaders and textures did not load within 1200 frames (waiting for " + d.waiting + ").");
                return true;
            }
            return false;
        }
        PixelImage image = toImage(output.width, output.height, rgba);
        if (output.opaque) {
            for (uint8_t* p = image.editPixels().data(), *end = p + image.pixels().size(); p < end; p += 4) p[3] = 255;
        }
        pass.out->push_back(std::move(image));
        s.drawPending = false;
        d.doneFrames++;
        if (++d.frame >= pass.times.size()) {
            d.scratch.reset();
            d.frame = 0;
            d.pass++;
        }
        if (overBudget()) return false;
    }
    return finished;
}

std::unique_ptr<CaptureTarget> editor::pixelart::fx::makeEntityTarget(Project* project, SceneProject* scene, Entity entity, const std::string& clip,
                                                                       std::string& error) {
    if (!scene || !scene->scene || !scene->scene->isEntityCreated(entity)) {
        error = "no such GameObject";
        return nullptr;
    }
    auto target = std::make_unique<EntityTarget>();
    target->project = project;
    target->scene = scene;
    target->entity = entity;
    target->clip = clip;
    if (!clip.empty()) {
        std::string why;
        const std::string assetRelative = project->normalizeToAssetsRelative(project->getProjectPath() / std::filesystem::path(clip)).generic_string();
        std::shared_ptr<const AnimationClip> loaded = AnimationClipPool::getOrLoad(assetRelative, &why);
        if (!loaded) {
            error = "the clip " + clip + " does not load" + (why.empty() ? std::string() : ": " + why);
            return nullptr;
        }
        target->clipLength = loaded->getLength();
        target->clipLoops = loaded->getWrap() == ClipWrapMode::Loop || loaded->getWrap() == ClipWrapMode::PingPong;
    }
    return target;
}

std::unique_ptr<CaptureTarget> editor::pixelart::fx::makeParticlesTarget(Project* project, const std::string& effect, std::string& error) {
    (void)project;
    const particles::EffectAsset& asset = particles::ParticleEffectLibrary::get(effect);
    if (!asset.document) {
        error = "the effect " + effect + " does not read";
        for (const particles::Diagnostic& d : asset.diagnostics) {
            error += ": " + d.message;
            break;
        }
        return nullptr;
    }
    auto target = std::make_unique<ParticlesTarget>();
    target->effect = effect;
    target->looping = asset.document->looping;
    target->duration = std::max(0.05, (double)asset.document->duration);
    return target;
}

PixelImage editor::pixelart::fx::composeCapture(const CaptureOutput& output, SheetSettings settings, SheetLayout* layout) {
    settings.opaqueFrames = output.opaque;
    settings.groundY = output.groundY;
    settings.sceneColor = output.sceneColor;
    std::vector<const PixelImage*> frames;
    for (const PixelImage& f : output.frames) frames.push_back(&f);
    return composeSheet(frames, output.times, settings, layout);
}

lint::LintReport editor::pixelart::fx::lintCapture(const CaptureOutput& output, const CaptureSettings& settings) {
    if (!settings.lints) return lint::LintReport();
    return lint::runEffectLints(output.lintInput, settings.lintIds);
}
