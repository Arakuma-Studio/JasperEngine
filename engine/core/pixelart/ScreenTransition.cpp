// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The transitions' structure follows pixelfx's screen transitions (src/fx/transitions/, MIT,
// Copyright (c) 2026 Patch): cover in about 45 % of the time, hold, uncover so the last frame
// is empty; rim bands quantised to ramp steps; CRT Off's squeeze to a line and a dot; Pixel
// Dissolve's blocks of 1, 2 and 4 cells winking in by a clumpy rank; Burn Away's ragged front
// from the edges and its reveal from a hole near the middle. Their particles, glints and motes
// (Sprite Fusion's) are not ported: the options name what S49 emits instead.

#include "pixelart/ScreenTransition.h"

#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "System.h"
#include "component/CameraComponent.h"
#include "component/Transform.h"
#include "manager/SceneManager.h"
#include "pixelart/Palette.h"
#include "pixelart/PaletteRampTexture.h"
#include "pixelart/PixelColor.h"
#include "pixelart/PixelFxKey.h"
#include "pool/ShaderPool.h"
#include "render/CameraRender.h"
#include "render/ObjectRender.h"
#include "render/PipelineState.h"
#include "render/ShaderRender.h"
#include "texture/Framebuffer.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace jasperengine::pixelart {

    namespace {

        const char* const kKindNames[kTransitionKindCount] = {"iris", "blinds", "slash", "pixel_dissolve", "crt_off", "burn_away", "fade"};
        const char* const kEaseNames[4] = {"linear", "in", "out", "in_out"};
        const char* const kShapeNames[3] = {"circle", "diamond", "square"};

        enum class RunMode { PLAY, COVER, REVEAL, LOAD_SCENE };
        enum class LoadStage { NONE, COVERING, WAIT, SHOW_LOADING, RECOVER, LOADED };

        struct Run {
            uint32_t id = 0;
            RunMode mode = RunMode::PLAY;
            TransitionKind kind = TransitionKind::IRIS;
            TransitionOptions options;
            std::vector<Rgba8> ramp;
            TransitionPhase phase = TransitionPhase::NONE;
            float coverSeconds = 0.0f, holdSeconds = 0.0f, revealSeconds = 0.0f;
            float phaseTime = 0.0f, phaseLength = 0.0f;   // seconds; a hold of length < 0 never ends
            float coverage = 0.0f;
            bool finished = true;
            std::function<void()> onCovered;
            // loadScene
            std::string scene;
            LoadStage stage = LoadStage::NONE;
            bool holding = false;
            int framesAfterLoad = 0;
        };

        Run run;
        uint32_t nextId = 1;
        TransitionDefaults defaults;
        bool engineDraw = true;

        struct Preview {
            bool active = false;
            TransitionKind kind = TransitionKind::IRIS;
            TransitionOptions options;
            std::vector<Rgba8> ramp;
            float coverage = 0.0f;
            TransitionPhase phase = TransitionPhase::COVER;
        };
        Preview preview;

        struct Gpu {
            std::shared_ptr<ShaderRender> shader;
            ObjectRender render;
            CameraRender pass;
            int slot = -1;
            bool loaded = false;
        };
        Gpu* gpu = nullptr;

        float clamp01(float v){
            return std::min(std::max(v, 0.0f), 1.0f);
        }

        // the u where the ease reaches c (it rises monotonically)
        float inverseEase(TransitionEase ease, float c){
            if (c <= 0.0f) return 0.0f;
            if (c >= 1.0f) return 1.0f;
            float lo = 0.0f, hi = 1.0f;
            for (int i = 0; i < 24; i++){
                const float mid = 0.5f * (lo + hi);
                if (applyTransitionEase(ease, mid) < c) lo = mid;
                else hi = mid;
            }
            return 0.5f * (lo + hi);
        }

        float phaseIndex(TransitionPhase phase){
            return phase == TransitionPhase::HOLD ? 1.0f : (phase == TransitionPhase::REVEAL ? 2.0f : 0.0f);
        }

        void startCover(float seconds){
            run.phase = TransitionPhase::COVER;
            run.phaseLength = std::max(seconds, 0.0f);
            run.phaseTime = inverseEase(run.options.ease, run.coverage) * run.phaseLength;
        }

        void startReveal(float seconds){
            run.phase = TransitionPhase::REVEAL;
            run.phaseLength = std::max(seconds, 0.0f);
            // the reveal mirrors the cover: coverage = ease(1 - u)
            run.phaseTime = (1.0f - inverseEase(run.options.ease, run.coverage)) * run.phaseLength;
        }

        void startHold(float seconds){
            run.phase = TransitionPhase::HOLD;
            run.phaseLength = seconds;
            run.phaseTime = 0.0f;
            run.coverage = 1.0f;
        }

        uint32_t beginRun(RunMode mode, TransitionKind kind, const TransitionOptions& options, std::function<void()> onCovered){
            // a new transition replaces the running one from its coverage (the screen never pops);
            // a load it was waiting on is let go
            if (run.holding){
                SceneManager::releaseLoading();
                run.holding = false;
            }
            const float coverage = run.finished && run.phase == TransitionPhase::NONE ? 0.0f : run.coverage;
            run = Run();
            run.id = nextId++;
            run.mode = mode;
            run.kind = kind;
            run.options = options;
            run.ramp = ScreenTransition::resolveRamp(options);
            run.coverage = coverage;
            run.finished = false;
            run.onCovered = std::move(onCovered);
            preview.active = false;
            return run.id;
        }

        void fireCovered(){
            if (!run.onCovered) return;
            std::function<void()> callback = std::move(run.onCovered);
            run.onCovered = nullptr;
            callback();
        }

        // ---- the CPU mirror of transition.frag (same names, same order of operations)

        struct Shader {
            const transition_fs_t& u;
            int kind() const { return (int)std::lround(u.state.z); }
            int steps() const { return std::clamp((int)std::lround(u.rim.z), 1, kTransitionMaxRampSteps); }
            uint32_t seed() const { return (uint32_t)std::lround(u.rim.w); }
            bool reveal() const { return (int)std::lround(u.state.y) == 2; }
            Vector4 hot(int i) const { const int n = steps(); return u.ramp[std::clamp(n - 1 - i, 0, n - 1)]; }

            static float sign(float v){ return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f); }

            float burn(float px, float py) const{
                const float sizeX = u.art.x, sizeY = u.art.y, scale = u.params0.x;
                const uint32_t s = seed();
                const float ragged = 0.36f * scale * (pfxValueNoise(px, py, std::max(scale * 0.5f, 2.0f), s + 21u) - 0.5f)
                                   + 2.4f * (pfxValueNoise(px, py, 7.0f, s + 33u) - 0.5f);
                if (reveal()){
                    const float dx = (px - u.params1.x) / (0.5f * sizeX), dy = (py - u.params1.y) / (0.5f * sizeY);
                    return -std::sqrt(dx * dx + dy * dy) * scale - ragged;
                }
                const float ux = (px - 0.5f * sizeX) / (0.5f * sizeX), uy = (py - 0.5f * sizeY) / (0.5f * sizeY);
                const float u2x = ux * ux, u2y = uy * uy;
                const float m = std::sqrt(std::sqrt(u2x * u2x + u2y * u2y));
                return (1.0f - m) * scale + ragged + 0.07f * scale * (0.5f - py / sizeY);
            }

            float field(float px, float py) const{
                switch (kind()){
                    case 0: {
                        const float dx = std::fabs(px - u.params0.x), dy = std::fabs(py - u.params0.y);
                        const int shape = (int)std::lround(u.state.w);
                        const float f = shape == 1 ? dx + dy : (shape == 2 ? std::max(dx, dy) : std::sqrt(dx * dx + dy * dy));
                        return u.params0.z - f;
                    }
                    case 1: {
                        const bool vertical = u.state.w > 0.5f;
                        const float a = vertical ? px : py;
                        const float band = (vertical ? u.art.x : u.art.y) / std::max(u.params0.x, 1.0f);
                        return a - std::floor(a / band) * band;
                    }
                    case 2:
                        return px * u.params0.x + py * u.params0.y - u.params0.z;
                    default:
                        return burn(px, py);
                }
            }

            float level() const{
                const float c = u.state.x;
                switch (kind()){
                    case 0: return c * (u.params0.z + 1.0f);
                    case 1: return c * ((u.state.w > 0.5f ? u.art.x : u.art.y) / std::max(u.params0.x, 1.0f) + 1.0f);
                    case 2: return c * (u.params0.w + 1.0f);
                    default: {
                        const float lo = reveal() ? u.params1.z : u.params0.y;
                        const float hi = reveal() ? u.params1.w : u.params0.z;
                        return lo - 1.0f + c * (hi - lo + 2.0f);
                    }
                }
            }

            void gradient(float px, float py, float v, float& gx, float& gy) const{
                if (kind() == 1){
                    const bool vertical = u.state.w > 0.5f;
                    gx = vertical ? 1.0f : 0.0f;
                    gy = vertical ? 0.0f : 1.0f;
                    return;
                }
                if (kind() == 2){
                    gx = u.params0.x;
                    gy = u.params0.y;
                    return;
                }
                const float sx = px + 1.0f < u.art.x ? 1.0f : -1.0f;
                const float sy = py + 1.0f < u.art.y ? 1.0f : -1.0f;
                gx = (field(px + sx, py) - v) * sx;
                gy = (field(px, py + sy) - v) * sy;
            }

            bool inside(float px, float py) const{
                return px >= 0.0f && py >= 0.0f && px < u.art.x && py < u.art.y;
            }

            // returns the cover (a 0 or 1) and its colour, the light, the cell and the rim depth
            Vector4 band(float px, float py, Vector4& light, uint8_t& cell, uint8_t& depth) const{
                light = Vector4(0, 0, 0, 0);
                const float lv = level();
                const float v = field(px, py) - lv;
                float gx, gy;
                gradient(px, py, v + lv, gx, gy);
                const float dominant = std::max(std::fabs(gx), std::fabs(gy));
                const float scale = 1.0f / std::clamp(dominant, 0.45f, 3.0f);
                const int rimWidth = (int)std::lround(u.rim.x);
                if (v >= 0.0f){
                    const float d = v * scale;
                    const float reach = 2.0f * (float)std::max(rimWidth, 1) + 2.0f;
                    cell = TransitionImage::UNCOVERED;
                    if (u.rim.y > 0.0f && d < reach && u.state.x > 0.0f){
                        const float q = std::floor(d / reach * 4.0f);
                        const Vector4 h = hot((int)q);
                        const float k = u.rim.y * (1.0f - q * 0.25f);
                        light = Vector4(h.x * k, h.y * k, h.z * k, 0.0f);
                        cell = TransitionImage::LIGHT;
                    }
                    return Vector4(0, 0, 0, 0);
                }
                if (rimWidth > 0 && -v * scale <= (float)rimWidth + 1.5f){
                    const float stepX = std::fabs(gx) >= std::fabs(gy) ? sign(gx) : 0.0f;
                    const float stepY = std::fabs(gx) >= std::fabs(gy) ? 0.0f : sign(gy);
                    for (int s = 1; s <= kTransitionMaxRim; s++){
                        if (s > rimWidth) break;
                        const float qx = px + stepX * (float)s, qy = py + stepY * (float)s;
                        if (!inside(qx, qy)) break;
                        if (field(qx, qy) >= lv){
                            const int n = steps();
                            const Vector4 h = hot((int)std::floor((float)(s - 1) * (float)n / (float)rimWidth));
                            cell = TransitionImage::RIM;
                            depth = (uint8_t)s;
                            return Vector4(h.x, h.y, h.z, 1.0f);
                        }
                    }
                }
                cell = TransitionImage::INK;
                return Vector4(u.ink.x, u.ink.y, u.ink.z, 1.0f);
            }

            Vector4 pixelDissolve(float px, float py, Vector4& light, uint8_t& cell, uint8_t& depth) const{
                light = Vector4(0, 0, 0, 0);
                const float cellSize = std::max(u.params0.x, 1.0f);
                const uint32_t s = seed();
                const float bx = std::floor(px / cellSize), by = std::floor(py / cellSize);
                const float h = pfxHash01((uint32_t)bx, (uint32_t)by, s);
                const float n = h < 0.32f ? 1.0f : (h < 0.74f ? 2.0f : 4.0f);
                const float size = cellSize / n;
                const float ox = bx * cellSize + std::floor((px - bx * cellSize) / size) * size;
                const float oy = by * cellSize + std::floor((py - by * cellSize) / size) * size;
                const float cx = ox + 0.5f * size, cy = oy + 0.5f * size;
                const float key = 0.65f * pfxValueNoise(cx, cy, 48.0f, s + 11u) + 0.35f * pfxValueNoise(cx, cy, 16.0f, s + 13u)
                                + 0.22f * pfxHash01((uint32_t)ox, (uint32_t)oy, s + 17u) + (n == 1.0f ? 0.12f : (n == 4.0f ? -0.08f : 0.0f));
                const float rank = std::clamp((key + 0.08f) / 1.42f, 0.0f, 1.0f);
                const float age = (u.state.x - rank * 0.83f) / 0.17f;
                if (age < 0.0f){
                    cell = TransitionImage::UNCOVERED;
                    return Vector4(0, 0, 0, 0);
                }
                cell = TransitionImage::INK;
                if (age >= 1.0f)
                    return Vector4(u.ink.x, u.ink.y, u.ink.z, 1.0f);
                const float lx = std::floor(px - ox), ly = std::floor(py - oy);
                const float side = std::floor(size + 0.5f);
                const float edge = std::min(std::min(lx, ly), std::min(side - 1.0f - lx, side - 1.0f - ly));
                if (edge < u.rim.x){
                    const Vector4 c = hot((int)std::floor(age * (float)steps()));
                    cell = TransitionImage::RIM;
                    depth = (uint8_t)std::min(edge + 1.0f, 255.0f);
                    return Vector4(c.x, c.y, c.z, 1.0f);
                }
                if (age > 0.2f)
                    return Vector4(u.ink.x, u.ink.y, u.ink.z, 1.0f);
                const Vector4 c = hot(0);
                const float k = u.rim.y * (1.0f - age / 0.2f);
                light = Vector4(c.x * k, c.y * k, c.z * k, 0.0f);
                cell = k > 0.0f ? TransitionImage::LIGHT : TransitionImage::UNCOVERED;
                return Vector4(0, 0, 0, 0);
            }

            Vector4 crtOff(float px, float py, Vector4& light, uint8_t& cell, uint8_t& depthOut) const{
                light = Vector4(0, 0, 0, 0);
                const float c = u.state.x;
                const float sizeX = u.art.x, sizeY = u.art.y;
                const float dX = std::fabs(std::floor(px) - std::floor(0.5f * sizeX));
                const float dY = std::fabs(std::floor(py) - std::floor(0.5f * sizeY));
                float halfRows = 0.0f, halfCols = std::floor(0.5f * sizeX), bright = 1.0f;
                if (c < 0.55f){
                    const float t = c / 0.55f;
                    halfRows = std::floor(0.5f * sizeY * (1.0f - t * t * t));
                }else if (c < 0.85f){
                    const float t = (c - 0.55f) / 0.3f;
                    halfCols = std::floor(0.5f * sizeX * (1.0f - t * t));
                }else{
                    halfCols = 0.0f;
                    bright = 1.0f - (c - 0.85f) / 0.15f;
                }
                const int rimWidth = (int)std::lround(u.rim.x);
                if (halfRows >= 1.0f){
                    if (dY < halfRows){
                        const float squeeze = 1.0f - halfRows / (0.5f * sizeY);
                        float k = 0.55f * squeeze * squeeze + (reveal() ? 0.6f * squeeze : 0.0f) +
                                  (std::fmod(std::floor(py), 2.0f) > 0.5f ? 0.08f * squeeze : 0.0f);
                        k = std::floor(clamp01(k) * 4.0f) * 0.25f;
                        const Vector4 h = hot(1);
                        const float g = u.rim.y * k;
                        light = Vector4(h.x * g, h.y * g, h.z * g, 0.0f);
                        cell = g > 0.0f ? TransitionImage::LIGHT : TransitionImage::UNCOVERED;
                        return Vector4(0, 0, 0, 0);
                    }
                    const float depth = dY - halfRows + 1.0f;
                    if (depth <= (float)rimWidth){
                        const Vector4 h = hot((int)std::floor((depth - 1.0f) * (float)steps() / (float)std::max(rimWidth, 1)));
                        cell = TransitionImage::RIM;
                        depthOut = (uint8_t)depth;
                        return Vector4(h.x, h.y, h.z, 1.0f);
                    }
                    cell = TransitionImage::INK;
                    return Vector4(u.ink.x, u.ink.y, u.ink.z, 1.0f);
                }
                const float depth = std::max(dY, std::max(dX - halfCols, 0.0f));
                if (depth <= (float)rimWidth){
                    const int n = steps();
                    const int step = (int)std::floor(depth * (float)n / (float)(rimWidth + 1)) + (int)std::floor((1.0f - bright) * (float)n);
                    if (step < n){
                        const Vector4 h = hot(step);
                        cell = TransitionImage::RIM;
                        depthOut = (uint8_t)(depth + 1.0f);
                        return Vector4(h.x, h.y, h.z, 1.0f);
                    }
                }
                cell = TransitionImage::INK;
                return Vector4(u.ink.x, u.ink.y, u.ink.z, 1.0f);
            }

            Vector4 fade(float px, float py, uint8_t& cell) const{
                const float c = u.state.x;
                if (bayerThreshold(8, (int)std::floor(px), (int)std::floor(py)) >= c){
                    cell = TransitionImage::UNCOVERED;
                    return Vector4(0, 0, 0, 0);
                }
                cell = TransitionImage::INK;
                if (u.state.w > 0.5f){
                    const int n = steps();
                    const int k = (int)std::floor(c * (float)(n + 1));
                    if (k < n){
                        const Vector4 h = hot(k);
                        return Vector4(h.x, h.y, h.z, 1.0f);
                    }
                }
                return Vector4(u.ink.x, u.ink.y, u.ink.z, 1.0f);
            }
        };

        TransitionGrid gridFor(Framebuffer* target, int pixelSize){
            TransitionGrid grid;
            grid.targetWidth = target ? (float)target->getWidth() : (float)System::instance().getScreenWidth();
            grid.targetHeight = target ? (float)target->getHeight() : (float)System::instance().getScreenHeight();
            grid.flipY = !target && Engine::isOpenGL();
            const float w = std::max(grid.targetWidth, 1.0f), h = std::max(grid.targetHeight, 1.0f);
            if (pixelSize > 0){
                grid.scale = (float)pixelSize;
                grid.artWidth = std::ceil(w / grid.scale);
                grid.artHeight = std::ceil(h / grid.scale);
                return grid;
            }
            const pixelart::PixelPerfectLayout& layout = Engine::getPixelPerfectLayout();
            if (Engine::getPixelPerfectView().active && layout.baseWidth > 0 && layout.baseHeight > 0){
                // the Pixel Perfect Camera's base pixels where its image lies (viewports count from the bottom)
                grid.scale = (float)std::max(layout.scale, 1);
                grid.artWidth = (float)layout.baseWidth;
                grid.artHeight = (float)layout.baseHeight;
                grid.originX = layout.viewport.getX();
                grid.originY = h - (layout.viewport.getY() + layout.viewport.getHeight());
                return grid;
            }
            Scene* main = Engine::getMainScene();
            if (main && main->isFixedResolutionEnabled() && main->getFixedResolutionWidth() > 0 && main->getFixedResolutionHeight() > 0){
                const Rect view = Engine::getViewRect();
                grid.artWidth = (float)main->getFixedResolutionWidth();
                grid.artHeight = (float)main->getFixedResolutionHeight();
                grid.scale = std::max(view.getWidth() / grid.artWidth, 1e-3f);
                grid.originX = view.getX();
                grid.originY = h - (view.getY() + view.getHeight());
                return grid;
            }
            grid.artWidth = w;
            grid.artHeight = h;
            return grid;
        }

        // the iris's centre in art pixels: a GameObject's screen position, else the normalized centre
        void irisCentre(const TransitionOptions& options, const TransitionGrid& grid, float& x, float& y){
            x = options.center.x * grid.artWidth;
            y = options.center.y * grid.artHeight;
            if (!options.centerScene || options.centerObject == NULL_ENTITY)
                return;
            Scene* scene = options.centerScene;
            const Transform* transform = scene->findComponent<Transform>(options.centerObject);
            const Entity cameraEntity = scene->getCamera();
            const CameraComponent* camera = cameraEntity != NULL_ENTITY ? scene->findComponent<CameraComponent>(cameraEntity) : nullptr;
            if (!transform || !camera)
                return;
            const Vector4 clip = camera->viewProjectionMatrix * Vector4(transform->worldPosition.x, transform->worldPosition.y, transform->worldPosition.z, 1.0f);
            if (std::fabs(clip.w) < 1e-6f)
                return;
            x = (clip.x / clip.w * 0.5f + 0.5f) * grid.artWidth;
            y = (0.5f - clip.y / clip.w * 0.5f) * grid.artHeight;
        }

        bool ensureProgram(){
            if (!gpu) gpu = new Gpu();
            if (gpu->loaded) return true;
            // asked again until it is created: a background build finishes in a later get()
            gpu->shader = ShaderPool::get(ShaderType::TRANSITION, 0);
            if (!gpu->shader || !gpu->shader->isCreated())
                return false;
            gpu->render.beginLoad(PrimitiveType::TRIANGLES);
            gpu->render.setShader(gpu->shader.get());
            // a pass of the frame: created at once, never skipped (spec S32 §6.4)
            gpu->render.setImmediatePipelines(true);
            if (!gpu->render.endLoadDynamic(PrimitiveType::TRIANGLES, IndexType::NONE)){
                Log::error("Screen transition: could not load its program");
                return false;
            }
            gpu->slot = gpu->shader->shaderData.getUniformBlockIndexByName("u_fs_transition");
            gpu->loaded = true;
            return true;
        }

        void drawPass(Framebuffer* target, const transition_fs_t& uniforms){
            PipelineState state;
            state.blend = BlendMode::PREMULTIPLIED;
            state.depthCompare = CompareFunc::ALWAYS;
            state.depthWrite = false;
            gpu->pass.setColorActions(LoadAction::LOAD, StoreAction::STORE);
            gpu->pass.setLabel("transition");
            if (target){
                gpu->pass.startRenderPass(&target->getRender());
                state.target = targetFormatFor(PipelineType::PIP_RTT);
            }else{
                gpu->pass.startRenderPass();
                state.target = targetFormatFor(PipelineType::PIP_DEFAULT);
            }
            gpu->pass.applyViewport(Rect(0.0f, 0.0f, uniforms.frame.z, uniforms.frame.w));
            if (gpu->render.beginDraw(state)){
                if (gpu->slot >= 0)
                    gpu->render.applyUniformBlock(gpu->slot, sizeof(transition_fs_t), const_cast<transition_fs_t*>(&uniforms));
                gpu->render.draw(0, 3, 1);
            }
            gpu->pass.endRenderPass();
        }

    }

    const char* transitionKindName(TransitionKind kind){
        const int index = (int)kind;
        return index >= 0 && index < kTransitionKindCount ? kKindNames[index] : "iris";
    }

    bool transitionKindFromName(const std::string& name, TransitionKind& kind){
        for (int i = 0; i < kTransitionKindCount; i++){
            if (name == kKindNames[i]){
                kind = (TransitionKind)i;
                return true;
            }
        }
        return false;
    }

    const char* transitionEaseName(TransitionEase ease){
        const int index = (int)ease;
        return index >= 0 && index < 4 ? kEaseNames[index] : "in_out";
    }

    bool transitionEaseFromName(const std::string& name, TransitionEase& ease){
        for (int i = 0; i < 4; i++){
            if (name == kEaseNames[i]){
                ease = (TransitionEase)i;
                return true;
            }
        }
        return false;
    }

    const char* irisShapeName(IrisShape shape){
        const int index = (int)shape;
        return index >= 0 && index < 3 ? kShapeNames[index] : "circle";
    }

    bool irisShapeFromName(const std::string& name, IrisShape& shape){
        for (int i = 0; i < 3; i++){
            if (name == kShapeNames[i]){
                shape = (IrisShape)i;
                return true;
            }
        }
        return false;
    }

    float applyTransitionEase(TransitionEase ease, float t){
        t = clamp01(t);
        switch (ease){
            case TransitionEase::LINEAR: return t;
            case TransitionEase::IN: return t * t * t;
            case TransitionEase::OUT: { const float r = 1.0f - t; return 1.0f - r * r * r; }
            case TransitionEase::IN_OUT:
            default:
                if (t < 0.5f) return 4.0f * t * t * t;
                { const float r = -2.0f * t + 2.0f; return 1.0f - r * r * r * 0.5f; }
        }
    }

    bool TransitionDefaults::operator==(const TransitionDefaults& other) const{
        return kind == other.kind && duration == other.duration && color == other.color && ramp == other.ramp && rim == other.rim &&
               pixelSize == other.pixelSize && coverUI == other.coverUI;
    }

    TransitionOptions TransitionOptions::fromDefaults(){
        TransitionOptions options;
        options.ink = defaults.color;
        options.ramp = defaults.ramp;
        options.rim = defaults.rim;
        options.pixelSize = defaults.pixelSize;
        return options;
    }

    bool TransitionHandle::isDone() const{
        if (id == 0 || id != run.id) return true;
        return run.finished;
    }

    float TransitionHandle::getProgress() const{
        if (isDone()) return 1.0f;
        // the run's share: cover, then the hold (or the load), then the reveal
        const float total = std::max(run.coverSeconds + std::max(run.holdSeconds, 0.0f) + run.revealSeconds, 1e-6f);
        const float u = run.phaseLength > 0.0f ? clamp01(run.phaseTime / run.phaseLength) : 1.0f;
        switch (run.phase){
            case TransitionPhase::COVER: return run.mode == RunMode::LOAD_SCENE && run.stage == LoadStage::RECOVER ? 0.5f : u * run.coverSeconds / total;
            case TransitionPhase::HOLD:
                if (run.mode == RunMode::COVER) return 1.0f;
                return (run.coverSeconds + (run.holdSeconds > 0.0f ? u * run.holdSeconds : 0.0f)) / total;
            case TransitionPhase::REVEAL:
                if (run.mode == RunMode::LOAD_SCENE && run.stage == LoadStage::SHOW_LOADING) return 0.5f;
                return (run.coverSeconds + std::max(run.holdSeconds, 0.0f) + u * run.revealSeconds) / total;
            case TransitionPhase::NONE: return 0.5f;
        }
        return 0.0f;
    }

    void TransitionHandle::stop() const{
        if (!isDone()) ScreenTransition::stop();
    }

    TransitionHandle ScreenTransition::play(TransitionKind kind, float seconds, const TransitionOptions& options, std::function<void()> onCovered){
        const uint32_t id = beginRun(RunMode::PLAY, kind, options, std::move(onCovered));
        const float total = std::max(seconds, 0.0f);
        run.holdSeconds = options.hold >= 0.0f ? std::min(options.hold, total) : 0.1f * total;
        run.coverSeconds = run.revealSeconds = 0.5f * (total - run.holdSeconds);
        startCover(run.coverSeconds);
        return TransitionHandle{id};
    }

    TransitionHandle ScreenTransition::cover(TransitionKind kind, float seconds, const TransitionOptions& options, std::function<void()> onCovered){
        const uint32_t id = beginRun(RunMode::COVER, kind, options, std::move(onCovered));
        run.coverSeconds = std::max(seconds, 0.0f);
        startCover(run.coverSeconds);
        return TransitionHandle{id};
    }

    TransitionHandle ScreenTransition::reveal(TransitionKind kind, float seconds, const TransitionOptions& options){
        const bool idle = !isActive();
        const uint32_t id = beginRun(RunMode::REVEAL, kind, options, nullptr);
        if (idle) run.coverage = 1.0f;
        run.revealSeconds = std::max(seconds, 0.0f);
        startReveal(run.revealSeconds);
        return TransitionHandle{id};
    }

    TransitionHandle ScreenTransition::loadScene(const std::string& scene, TransitionKind kind, float seconds, const TransitionOptions& options,
                                                 std::function<void()> onCovered){
        if (SceneManager::getSceneId(scene) == 0){
            Log::error("Transition.loadScene: no scene '%s' (SceneManager has %d)", scene.c_str(), SceneManager::getSceneCount());
            return TransitionHandle{};
        }
        const uint32_t id = beginRun(RunMode::LOAD_SCENE, kind, options, std::move(onCovered));
        const float total = std::max(seconds, 0.0f);
        run.coverSeconds = run.revealSeconds = 0.45f * total;
        run.holdSeconds = -1.0f;
        run.scene = scene;
        run.stage = LoadStage::COVERING;
        startCover(run.coverSeconds);
        return TransitionHandle{id};
    }

    bool ScreenTransition::isActive(){
        return !run.finished || run.phase != TransitionPhase::NONE;
    }

    bool ScreenTransition::isCovered(){
        return isActive() && run.coverage >= 1.0f;
    }

    float ScreenTransition::getCoverage(){
        return preview.active ? preview.coverage : (isActive() ? run.coverage : 0.0f);
    }

    TransitionPhase ScreenTransition::getPhase(){
        return run.phase;
    }

    TransitionKind ScreenTransition::getKind(){
        return run.kind;
    }

    void ScreenTransition::stop(){
        if (run.holding)
            SceneManager::releaseLoading();
        const uint32_t id = run.id;
        run = Run();
        run.id = id;
    }

    void ScreenTransition::update(double unscaledDt){
        if (!isActive())
            return;
        const float dt = (float)std::max(unscaledDt, 0.0);
        const uint32_t id = run.id;
        run.phaseTime += dt;
        const float u = run.phaseLength > 0.0f ? clamp01(run.phaseTime / run.phaseLength) : 1.0f;

        switch (run.phase){
            case TransitionPhase::COVER:
                run.coverage = applyTransitionEase(run.options.ease, u);
                if (u < 1.0f) break;
                run.coverage = 1.0f;
                if (run.mode == RunMode::LOAD_SCENE){
                    if (run.stage == LoadStage::RECOVER){
                        // covered again over the loading scene: the load may finish now
                        startHold(-1.0f);
                        if (run.holding){
                            SceneManager::releaseLoading();
                            run.holding = false;
                        }
                        run.stage = LoadStage::LOADED;
                        run.framesAfterLoad = 0;
                        break;
                    }
                    startHold(-1.0f);
                    fireCovered();
                    if (run.id != id) return;
                    if (!SceneManager::loadScene(run.scene)){
                        startReveal(run.revealSeconds);
                        run.stage = LoadStage::NONE;
                        break;
                    }
                    run.holding = SceneManager::holdLoading();
                    run.stage = LoadStage::WAIT;
                    break;
                }
                if (run.mode == RunMode::PLAY){
                    startHold(run.holdSeconds);
                    fireCovered();
                    if (run.id != id) return;
                    break;
                }
                // cover(): done, and held until reveal() or stop()
                startHold(-1.0f);
                run.finished = true;
                fireCovered();
                return;
            case TransitionPhase::HOLD:
                run.coverage = 1.0f;
                if (run.mode == RunMode::PLAY && run.phaseLength >= 0.0f && run.phaseTime >= run.phaseLength)
                    startReveal(run.revealSeconds);
                break;
            case TransitionPhase::REVEAL:
                run.coverage = applyTransitionEase(run.options.ease, 1.0f - u);
                if (u < 1.0f) break;
                run.coverage = 0.0f;
                run.phase = TransitionPhase::NONE;
                if (!(run.mode == RunMode::LOAD_SCENE && run.stage == LoadStage::SHOW_LOADING))
                    run.finished = true;
                break;
            case TransitionPhase::NONE:
                break;
        }

        if (run.mode != RunMode::LOAD_SCENE)
            return;
        switch (run.stage){
            case LoadStage::WAIT:
                if (SceneManager::isLoadComplete()){
                    // loaded under the cover: let the loading scene go, then reveal on the new scenes
                    if (run.holding){
                        SceneManager::releaseLoading();
                        run.holding = false;
                    }
                    run.stage = LoadStage::LOADED;
                    run.framesAfterLoad = 0;
                }else if (SceneManager::isLoadingSceneShown()){
                    // the loading scene: shown at half the reveal, covered again at half the cover
                    run.stage = LoadStage::SHOW_LOADING;
                    startReveal(0.5f * run.revealSeconds);
                }
                break;
            case LoadStage::SHOW_LOADING:
                if (SceneManager::isLoadComplete()){
                    run.stage = LoadStage::RECOVER;
                    startCover(0.5f * run.coverSeconds);
                }
                break;
            case LoadStage::LOADED:
                // the new scenes have drawn a frame without the loading scene
                if (!SceneManager::isLoading() && ++run.framesAfterLoad >= 2){
                    run.stage = LoadStage::NONE;
                    startReveal(run.revealSeconds);
                }
                break;
            default:
                break;
        }
    }

    bool ScreenTransition::needsDraw(){
        if (preview.active)
            return preview.coverage > 0.0f;
        return isActive() && (run.coverage > 0.0f || run.phase == TransitionPhase::HOLD);
    }

    void ScreenTransition::draw(Framebuffer* target){
        if (!needsDraw() || !ensureProgram())
            return;
        if (target && !target->isCreated())
            return;
        const bool fromPreview = preview.active;
        const TransitionKind kind = fromPreview ? preview.kind : run.kind;
        const TransitionOptions& options = fromPreview ? preview.options : run.options;
        TransitionOptions resolved = options;
        resolved.rampColors = fromPreview ? preview.ramp : run.ramp;
        const TransitionGrid grid = gridFor(target, options.pixelSize);
        const transition_fs_t uniforms = makeUniforms(kind, resolved, fromPreview ? preview.coverage : run.coverage,
                                                      fromPreview ? preview.phase : run.phase, grid);
        drawPass(target, uniforms);
    }

    void ScreenTransition::setEngineDrawEnabled(bool enabled){
        engineDraw = enabled;
    }

    bool ScreenTransition::isEngineDrawEnabled(){
        return engineDraw;
    }

    void ScreenTransition::setPreview(TransitionKind kind, const TransitionOptions& options, float coverage, TransitionPhase phase){
        if (coverage < 0.0f){
            preview.active = false;
            return;
        }
        preview.active = true;
        preview.kind = kind;
        preview.options = options;
        preview.ramp = resolveRamp(options);
        preview.coverage = clamp01(coverage);
        preview.phase = phase == TransitionPhase::NONE ? TransitionPhase::COVER : phase;
    }

    void ScreenTransition::setPreviewProgress(float coverage){
        if (coverage < 0.0f){
            preview.active = false;
            return;
        }
        if (!preview.active){
            setPreview(defaults.kind, TransitionOptions::fromDefaults(), coverage);
            return;
        }
        preview.coverage = clamp01(coverage);
    }

    bool ScreenTransition::renderInto(Framebuffer* target, TransitionKind kind, const TransitionOptions& options, float coverage, TransitionPhase phase){
        if (!target || !target->isCreated() || !ensureProgram())
            return false;
        const float c = clamp01(coverage);
        if (c <= 0.0f)
            return true;
        TransitionGrid grid;
        grid.targetWidth = (float)target->getWidth();
        grid.targetHeight = (float)target->getHeight();
        grid.scale = (float)std::max(options.pixelSize, 1);
        grid.artWidth = std::ceil(grid.targetWidth / grid.scale);
        grid.artHeight = std::ceil(grid.targetHeight / grid.scale);
        TransitionOptions resolved = options;
        resolved.rampColors = resolveRamp(options);
        drawPass(target, makeUniforms(kind, resolved, c, phase, grid));
        return true;
    }

    transition_fs_t ScreenTransition::makeUniforms(TransitionKind kind, const TransitionOptions& options, float coverage, TransitionPhase phase,
                                                   const TransitionGrid& grid){
        transition_fs_t u{};
        const float w = std::max(grid.artWidth, 1.0f), h = std::max(grid.artHeight, 1.0f);
        const int rim = std::clamp(options.rim, 0, kTransitionMaxRim);
        u.art = Vector4(w, h, std::max(grid.scale, 1e-3f), grid.flipY ? 1.0f : 0.0f);
        u.frame = Vector4(grid.originX, grid.originY, std::max(grid.targetWidth, 1.0f), std::max(grid.targetHeight, 1.0f));
        float kindSwitch = 0.0f;
        switch (kind){
            case TransitionKind::IRIS: {
                kindSwitch = (float)options.shape;
                float cx, cy;
                irisCentre(options, grid, cx, cy);
                float radius = 0.0f;
                const float corners[4][2] = {{0.0f, 0.0f}, {w, 0.0f}, {0.0f, h}, {w, h}};
                for (const auto& corner : corners){
                    const float dx = std::fabs(corner[0] - cx), dy = std::fabs(corner[1] - cy);
                    const float f = options.shape == IrisShape::DIAMOND ? dx + dy :
                                    (options.shape == IrisShape::SQUARE ? std::max(dx, dy) : std::sqrt(dx * dx + dy * dy));
                    radius = std::max(radius, f);
                }
                u.params0 = Vector4(cx, cy, radius + (float)rim + 2.0f, 0.0f);
                break;
            }
            case TransitionKind::BLINDS:
                kindSwitch = options.vertical ? 1.0f : 0.0f;
                u.params0 = Vector4((float)std::max(options.count, 1), 0.0f, 0.0f, 0.0f);
                break;
            case TransitionKind::SLASH: {
                const float angle = options.angle * 0.0174532925f;
                const float nx = std::cos(angle), ny = std::sin(angle);
                float lo = 1e30f, hi = -1e30f;
                const float centres[4][2] = {{0.5f, 0.5f}, {w - 0.5f, 0.5f}, {0.5f, h - 0.5f}, {w - 0.5f, h - 0.5f}};
                for (const auto& corner : centres){
                    const float d = corner[0] * nx + corner[1] * ny;
                    lo = std::min(lo, d);
                    hi = std::max(hi, d);
                }
                // a little before the first centre, so coverage 0 covers nothing
                lo -= 0.5f;
                u.params0 = Vector4(nx, ny, lo, hi - lo + (float)rim + 1.0f);
                break;
            }
            case TransitionKind::PIXEL_DISSOLVE:
                u.params0 = Vector4((float)std::max(options.cell, 1), 0.0f, 0.0f, 0.0f);
                break;
            case TransitionKind::BURN_AWAY: {
                const float scale = 0.5f * std::min(w, h);
                const float ragged = 0.18f * scale + 1.2f;
                const float lo = (1.0f - 1.18921f) * scale - ragged - 0.035f * scale;
                const float hi = scale + ragged + 0.035f * scale;
                u.params0 = Vector4(scale, lo, hi, 0.0f);
                const uint32_t seed = options.seed;
                const float ox = 0.5f * w + (pfxHash01(seed, 1u, 0x5eedu) - 0.5f) * 0.16f * w;
                const float oy = 0.5f * h + (pfxHash01(seed, 2u, 0x5eedu) - 0.6f) * 0.1f * h;
                float far = 0.0f;
                const float corners[4][2] = {{0.0f, 0.0f}, {w, 0.0f}, {0.0f, h}, {w, h}};
                for (const auto& corner : corners){
                    const float dx = (corner[0] - ox) / (0.5f * w), dy = (corner[1] - oy) / (0.5f * h);
                    far = std::max(far, std::sqrt(dx * dx + dy * dy));
                }
                u.params1 = Vector4(ox, oy, -far * scale - ragged, ragged);
                break;
            }
            case TransitionKind::FADE:
                kindSwitch = options.throughRamp ? 1.0f : 0.0f;
                break;
            case TransitionKind::CRT_OFF:
                break;
        }
        u.state = Vector4(clamp01(coverage), phaseIndex(phase), (float)(int)kind, kindSwitch);

        std::vector<Rgba8> ramp = options.rampColors.empty() ? resolveRamp(options) : options.rampColors;
        if ((int)ramp.size() > kTransitionMaxRampSteps)
            ramp = fitRamp(ramp, kTransitionMaxRampSteps);
        if (ramp.empty())
            ramp = emberRamp();
        u.rim = Vector4((float)rim, clamp01(options.glow), (float)ramp.size(), (float)std::min(options.seed, 16777215u));
        u.ink = Vector4(clamp01(options.ink.x), clamp01(options.ink.y), clamp01(options.ink.z), 1.0f);
        for (size_t i = 0; i < ramp.size(); i++)
            u.ramp[i] = Vector4(ramp[i].r / 255.0f, ramp[i].g / 255.0f, ramp[i].b / 255.0f, 1.0f);
        return u;
    }

    void ScreenTransition::evaluate(TransitionKind kind, const TransitionOptions& options, float coverage, TransitionPhase phase,
                                    int width, int height, TransitionImage& out){
        TransitionGrid grid;
        grid.artWidth = (float)std::max(width, 1);
        grid.artHeight = (float)std::max(height, 1);
        grid.targetWidth = grid.artWidth;
        grid.targetHeight = grid.artHeight;
        const transition_fs_t uniforms = makeUniforms(kind, options, coverage, phase, grid);
        const Shader shader{uniforms};
        out.width = std::max(width, 1);
        out.height = std::max(height, 1);
        const size_t count = (size_t)out.width * (size_t)out.height;
        out.cells.assign(count, TransitionImage::UNCOVERED);
        out.rimDepth.assign(count, 0);
        out.colors.assign(count, Rgba8{0, 0, 0, 0});
        // coverage 0 draws nothing at all (the pass is absent)
        if (uniforms.state.x <= 0.0f)
            return;
        for (int y = 0; y < out.height; y++){
            for (int x = 0; x < out.width; x++){
                const float px = (float)x + 0.5f, py = (float)y + 0.5f;
                Vector4 light(0, 0, 0, 0), cover(0, 0, 0, 0);
                uint8_t cell = TransitionImage::UNCOVERED, depth = 0;
                if (uniforms.state.x >= 1.0f){
                    cover = Vector4(uniforms.ink.x, uniforms.ink.y, uniforms.ink.z, 1.0f);
                    cell = TransitionImage::INK;
                }else switch (kind){
                    case TransitionKind::PIXEL_DISSOLVE: cover = shader.pixelDissolve(px, py, light, cell, depth); break;
                    case TransitionKind::CRT_OFF: cover = shader.crtOff(px, py, light, cell, depth); break;
                    case TransitionKind::FADE: cover = shader.fade(px, py, cell); break;
                    default: cover = shader.band(px, py, light, cell, depth); break;
                }
                const size_t i = (size_t)y * (size_t)out.width + (size_t)x;
                out.cells[i] = cell;
                out.rimDepth[i] = depth;
                auto byte = [](float v){ return (uint8_t)std::lround(clamp01(v) * 255.0f); };
                const float a = cover.w;
                out.colors[i] = Rgba8{byte(light.x * (1.0f - a) + cover.x * a), byte(light.y * (1.0f - a) + cover.y * a),
                                      byte(light.z * (1.0f - a) + cover.z * a), byte(a)};
            }
        }
    }

    TransitionRimMeasure ScreenTransition::measureRim(TransitionKind kind, const TransitionImage& image){
        TransitionRimMeasure measure;
        if (kind == TransitionKind::PIXEL_DISSOLVE || kind == TransitionKind::FADE)
            return measure;
        auto at = [&](int x, int y) -> uint8_t {
            return image.cells[(size_t)y * (size_t)image.width + (size_t)x];
        };
        auto depthAt = [&](int x, int y) -> int {
            return image.rimDepth[(size_t)y * (size_t)image.width + (size_t)x];
        };
        auto open = [](uint8_t cell){ return cell == TransitionImage::UNCOVERED || cell == TransitionImage::LIGHT; };
        const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (int y = 0; y < image.height; y++){
            for (int x = 0; x < image.width; x++){
                if (at(x, y) != TransitionImage::RIM || depthAt(x, y) != 1)
                    continue;
                // the longest run of depths 1, 2, 3... walking away from an uncovered neighbour,
                // complete when it ends on ink; runs cut by the screen or a thin cover are not edges
                int best = -1;
                for (const auto& dir : dirs){
                    const int nx = x + dir[0], ny = y + dir[1];
                    if (nx < 0 || ny < 0 || nx >= image.width || ny >= image.height || !open(at(nx, ny)))
                        continue;
                    int run = 1;
                    int qx = x - dir[0], qy = y - dir[1];
                    bool complete = false;
                    while (true){
                        if (qx < 0 || qy < 0 || qx >= image.width || qy >= image.height)
                            break;
                        const uint8_t cell = at(qx, qy);
                        if (cell == TransitionImage::RIM && depthAt(qx, qy) == run + 1){
                            run++;
                            qx -= dir[0];
                            qy -= dir[1];
                            continue;
                        }
                        // a rim cell of another depth was measured along another axis: not this edge
                        complete = cell == TransitionImage::INK;
                        break;
                    }
                    if (complete)
                        best = std::max(best, run);
                }
                if (best < 0)
                    continue;
                if (measure.crossings == 0){
                    measure.minWidth = measure.maxWidth = best;
                }else{
                    if (best < measure.minWidth || best > measure.maxWidth){
                        measure.atX = x;
                        measure.atY = y;
                    }
                    measure.minWidth = std::min(measure.minWidth, best);
                    measure.maxWidth = std::max(measure.maxWidth, best);
                }
                measure.crossings++;
            }
        }
        return measure;
    }

    std::vector<Rgba8> ScreenTransition::resolveRamp(const TransitionOptions& options){
        if (!options.rampColors.empty())
            return options.rampColors;
        std::string palette = options.palette;
        std::string ramp = options.ramp;
        std::string fromPath, nameFromPath;
        if (parsePaletteRampPath(ramp, &fromPath, &nameFromPath)){
            palette = fromPath;
            ramp = nameFromPath;
        }
        if (palette.empty() || ramp.empty())
            return emberRamp();
        std::shared_ptr<const Palette> source = PaletteLibrary::get(palette);
        std::vector<Rgba8> colors = source ? source->rampColors(ramp) : std::vector<Rgba8>();
        if (colors.empty()){
            Log::warn("Screen transition: no ramp '%s' in '%s' (palette_ramp_missing): the built-in ember ramp instead", ramp.c_str(), palette.c_str());
            return emberRamp();
        }
        return colors;
    }

    const std::vector<Rgba8>& ScreenTransition::emberRamp(){
        // pixelfx's 'ember' ramp (src/core/color.js PAL; MIT, Copyright (c) 2026 Patch)
        static const std::vector<Rgba8> ramp = {
            {0x1a, 0x0a, 0x0e, 255}, {0x3d, 0x10, 0x12, 255}, {0x70, 0x1a, 0x14, 255}, {0xa8, 0x30, 0x1a, 255},
            {0xe0, 0x56, 0x1e, 255}, {0xff, 0x8a, 0x2a, 255}, {0xff, 0xc4, 0x6a, 255}};
        return ramp;
    }

    void ScreenTransition::setDefaults(const TransitionDefaults& value){
        defaults = value;
        defaults.rim = std::clamp(defaults.rim, 0, kTransitionMaxRim);
        defaults.pixelSize = std::max(defaults.pixelSize, 0);
        defaults.duration = std::max(defaults.duration, 0.0f);
    }

    const TransitionDefaults& ScreenTransition::getDefaults(){
        return defaults;
    }

    bool ScreenTransition::isCoverUI(){
        return defaults.coverUI;
    }

    void ScreenTransition::reset(){
        stop();
        run.onCovered = nullptr;
        preview = Preview();
    }

    void ScreenTransition::releaseGpu(){
        if (!gpu) return;
        if (gpu->loaded)
            gpu->render.destroy();
        if (gpu->shader){
            gpu->shader.reset();
            ShaderPool::remove(ShaderType::TRANSITION, 0);
        }
        delete gpu;
        gpu = nullptr;
    }

    bool ScreenTransition::isProgramCreated(){
        return gpu && gpu->loaded;
    }

}
