// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pixelart/PixelFxPlayer.h"

#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "animation/KeyframeSampling.h"
#include "component/MeshComponent.h"
#include "component/SpriteComponent.h"
#include "component/Transform.h"
#include "particles/PixelParticles.h"
#include "pixelart/HitStop.h"
#include "shader/ShaderManifest.h"
#include "shader/ShaderUniforms.h"
#include "texture/TextureData.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace jasperengine::pixelart{

    struct PixelFxRun{
        Scene* scene = nullptr;
        std::weak_ptr<char> lifetime;
        Entity entity = NULL_ENTITY;
        PixelFxEffect effect = PixelFxEffect::FLASH;
        float duration = 0.0f;
        PixelFxOptions options;
        double time = 0.0;
        float progress = 0.0f;
        // its variant is drawn: time runs
        bool ready = false;
        bool done = false;
        // turned OUTLINE on itself (outline_pulse ends hidden then)
        bool enabledOutline = false;
        Vector4 outlineColor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        // embers: the frame the keys were made for, the keys of its opaque texels, the last
        // progress they burnt to and the budget carried over
        std::string emberEffect;
        std::string emberFrameKey;
        PixelFxFrameAlpha emberFrame;
        PfxDissolve emberDissolve;
        std::vector<float> emberKeys;
        float emberProgress = 0.0f;
        double emberCarry = 0.0;
        std::shared_ptr<void> scriptData;
    };

    namespace{

        const char* const kSprite = "library:sprite";

        struct EffectInfo{
            const char* name;
            const char* feature;
        };

        const EffectInfo kEffects[kPixelFxEffectCount] = {
            {"flash", "FLASH"},
            {"fade_out", "DITHER_FADE"},
            {"fade_in", "DITHER_FADE"},
            {"blink", "DITHER_FADE"},
            {"dissolve", "DISSOLVE"},
            {"undissolve", "DISSOLVE"},
            {"petrify", "RAMP_RECOLOR"},
            {"freeze", "RAMP_RECOLOR"},
            {"poison", "RAMP_RECOLOR"},
            {"outline_pulse", "OUTLINE"},
        };

        // the value each feature's effects animate (its neutral is 0)
        const char* channelMember(const std::string& feature){
            if (feature == "FLASH") return "flashAmount";
            if (feature == "DITHER_FADE") return "ditherFadeAmount";
            if (feature == "DISSOLVE") return "dissolveProgress";
            if (feature == "RAMP_RECOLOR") return "rampAmount";
            return nullptr;
        }

        struct State{
            std::vector<std::shared_ptr<PixelFxRun>> runs;
            PixelFxPlayer::EmberEmitter emitter;
            PixelFxPlayer::EmberResolver resolver;
            // materials a runtime feature was logged for
            std::set<std::string> logged;
            std::map<std::string, std::shared_ptr<const PixelFxFrameAlpha>> alphas;
        };

        State& state(){
            static State s;
            return s;
        }

        bool alive(const PixelFxRun& run){
            return !run.lifetime.expired() && run.scene && run.scene->isEntityCreated(run.entity) &&
                   run.scene->findComponent<MeshComponent>(run.entity) != nullptr;
        }

        float clamp01(float v){
            return std::min(std::max(v, 0.0f), 1.0f);
        }

        int countOf(const PixelFxOptions& options){
            return options.count > 0 ? options.count : 3;
        }

        // the library:sprite submeshes of a GameObject (an unshaded Sprite's takes library:sprite
        // when `adopt`); false when it has none
        template<typename F>
        bool forEachSpriteSubmesh(Scene* scene, Entity entity, bool adopt, F&& f){
            MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
            if (!mesh)
                return false;
            const bool sprite = scene->findComponent<SpriteComponent>(entity) != nullptr;
            bool any = false;
            for (unsigned int i = 0; i < mesh->numSubmeshes; i++){
                Material& material = mesh->submeshes[i].material;
                if (material.shader.empty() && sprite && adopt){
                    material.shader = kSprite;
                    material.shaderFeatures.clear();
                    mesh->needReload = true;
                }
                if (material.shader == kSprite){
                    f(*mesh, mesh->submeshes[i]);
                    any = true;
                }
            }
            return any;
        }

        std::string materialLabel(Scene* scene, Entity entity, const Material& material){
            if (!material.name.empty())
                return material.name.path;
            return "\"" + scene->getEntityName(entity) + "\"'s material";
        }

        // turns a feature on (with a keyword value) where it is off; logs once per material
        void ensureFeature(PixelFxRun* run, Scene* scene, Entity entity, const std::string& feature, const char* keyword,
                           const char* effectName){
            forEachSpriteSubmesh(scene, entity, true, [&](MeshComponent& mesh, Submesh& submesh){
                Material& material = submesh.material;
                if (ShaderLibraryValues::isFeatureEnabled(material.shaderFeatures, kSprite, feature))
                    return;
                ShaderLibraryValues::setFeature(material.shaderFeatures, kSprite, feature, true);
                if (keyword)
                    ShaderLibraryValues::setFeature(material.shaderFeatures, kSprite, keyword, true);
                if (run && feature == "OUTLINE")
                    run->enabledOutline = true;
                mesh.needReload = true;
                scene->markChanged<MeshComponent>(entity);
                if (Engine::isDevelopmentBuild()){
                    const std::string label = materialLabel(scene, entity, material);
                    if (state().logged.insert(label + "|" + feature).second){
                        Log::warn("PixelFX: \"%s\" turned on %s on %s at runtime; add it to the material (or record the warm-up list) to avoid a compile hitch",
                                  effectName, feature.c_str(), label.c_str());
                    }
                }
            });
        }

        // writes values into the GameObject's library:sprite materials (only what changed)
        void writeValues(Scene* scene, Entity entity, const std::vector<PixelFxValue>& values){
            forEachSpriteSubmesh(scene, entity, false, [&](MeshComponent& mesh, Submesh& submesh){
                bool changed = false;
                for (const PixelFxValue& value : values)
                    changed |= ShaderUniforms::set(submesh.material.shaderParams, value.member, value.value);
                if (changed){
                    mesh.needUpdateShaderUniforms = true;
                    scene->markChanged<MeshComponent>(entity);
                }
            });
        }

        void setRamp(Scene* scene, Entity entity, const std::string& ramp){
            if (ramp.empty())
                return;
            forEachSpriteSubmesh(scene, entity, false, [&](MeshComponent& mesh, Submesh& submesh){
                const size_t before = submesh.material.shaderTextures.size();
                if (ShaderLibraryValues::setTexture(submesh.material.shaderTextures, "u_fxRampTexture", ramp)){
                    submesh.needUpdateTexture = true;
                    if (submesh.material.shaderTextures.size() != before)
                        mesh.needReload = true;
                    scene->markChanged<MeshComponent>(entity);
                }
            });
        }

        // the variant with the features the effect needs is drawn
        bool variantReady(Scene* scene, Entity entity){
            MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
            return mesh && mesh->loaded && !mesh->needReload && !mesh->needVariantReload;
        }

        Vector4 firstParam(Scene* scene, Entity entity, const char* member){
            Vector4 value;
            bool found = false;
            forEachSpriteSubmesh(scene, entity, false, [&](MeshComponent&, Submesh& submesh){
                if (!found){
                    value = ShaderLibraryValues::getParam(submesh.material.shaderParams, kSprite, member);
                    found = true;
                }
            });
            return value;
        }

        // the effect's end: its run's values at the end, or every animated value at 0 (stop)
        std::vector<PixelFxValue> endValues(const PixelFxRun& run, bool stopped){
            if (stopped){
                if (run.effect == PixelFxEffect::OUTLINE_PULSE){
                    Vector4 color = run.outlineColor;
                    if (run.enabledOutline) color.w = 0.0f;
                    return {{"outlineColor", color}};
                }
                return {{channelMember(kEffects[(int)run.effect].feature), Vector4(0.0f, 0.0f, 0.0f, 0.0f)}};
            }
            std::vector<PixelFxValue> values = PixelFxPlayer::sample(run.effect, run.duration, run.options, run.duration);
            if (run.effect == PixelFxEffect::OUTLINE_PULSE){
                Vector4 color = run.outlineColor;
                if (run.enabledOutline) color.w = 0.0f;
                values = {{"outlineColor", color}};
            }
            return values;
        }

        // what a run writes at its time, the outline pulse's colour from the material's
        std::vector<PixelFxValue> runValues(const PixelFxRun& run){
            std::vector<PixelFxValue> values = PixelFxPlayer::sample(run.effect, run.duration, run.options, run.time);
            if (run.effect == PixelFxEffect::OUTLINE_PULSE){
                for (PixelFxValue& value : values){
                    if (std::string(value.member) == "outlineColor" && !run.options.hasColor){
                        const float on = value.value.w;
                        value.value = run.outlineColor;
                        value.value.w = on > 0.0f ? (run.outlineColor.w > 0.0f ? run.outlineColor.w : 1.0f) : 0.0f;
                    }
                }
            }
            return values;
        }

        PfxDissolve dissolveOf(Scene* scene, Entity entity){
            PfxDissolve d;
            const Vector4 dir = firstParam(scene, entity, "dissolveDirection");
            d.dirX = dir.x;
            d.dirY = dir.y;
            d.noise = firstParam(scene, entity, "dissolveNoise").x;
            d.cell = firstParam(scene, entity, "dissolveNoiseScale").x;
            d.jitter = firstParam(scene, entity, "dissolveJitter").x;
            d.edgeWidth = firstParam(scene, entity, "dissolveEdgeWidth").x;
            d.scorchWidth = firstParam(scene, entity, "dissolveScorchWidth").x;
            d.seed = (uint32_t)std::max(firstParam(scene, entity, "dissolveSeed").x, 0.0f);
            return d;
        }

        // The frame box of a Sprite in object space (the art's corners: its grown quad less the
        // growth) and its texture rect; false when it is not a sprite with a texture
        bool spriteFrame(Scene* scene, Entity entity, std::string& path, Rect& rect, Vector3& lo, Vector3& hi, int& texW, int& texH){
            SpriteComponent* sprite = scene->findComponent<SpriteComponent>(entity);
            MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
            if (!sprite || !mesh || mesh->numSubmeshes == 0)
                return false;
            const Submesh& submesh = mesh->submeshes[0];
            const Texture& texture = submesh.material.baseColorTexture;
            if (texture.empty())
                return false;
            path = texture.getPath();
            texW = (int)texture.getWidth();
            texH = (int)texture.getHeight();
            rect = submesh.hasTextureRect ? submesh.textureRect : Rect(0.0f, 0.0f, 1.0f, 1.0f);
            if (path.empty() || texW <= 0 || texH <= 0)
                return false;
            const float frameW = std::fabs(rect.getWidth()) * (float)texW, frameH = std::fabs(rect.getHeight()) * (float)texH;
            const uint32_t expand = sprite->builtFxExpand;
            const float left = (float)(expand & 0xff), bottom = (float)((expand >> 8) & 0xff);
            const float right = (float)((expand >> 16) & 0xff), top = (float)((expand >> 24) & 0xff);
            lo = mesh->aabb.getMinimum();
            hi = mesh->aabb.getMaximum();
            const float unitX = (hi.x - lo.x) / std::max(frameW + left + right, 1.0f);
            const float unitY = (hi.y - lo.y) / std::max(frameH + bottom + top, 1.0f);
            lo.x += left * unitX;
            hi.x -= right * unitX;
            lo.y += bottom * unitY;
            hi.y -= top * unitY;
            return true;
        }

        void emitEmbers(PixelFxRun& run, float progress, double dt){
            if (run.options.embers.empty() || run.options.manual)
                return;
            std::string path;
            Rect rect;
            Vector3 lo, hi;
            int texW = 0, texH = 0;
            if (!spriteFrame(run.scene, run.entity, path, rect, lo, hi, texW, texH))
                return;
            const int frameW = std::max((int)std::lround(std::fabs(rect.getWidth()) * (float)texW), 1);
            const int frameH = std::max((int)std::lround(std::fabs(rect.getHeight()) * (float)texH), 1);
            const PfxDissolve dissolve = dissolveOf(run.scene, run.entity);
            const std::string frameKey = path + "|" + std::to_string(rect.getX()) + "," + std::to_string(rect.getY()) + "," +
                                         std::to_string(rect.getWidth()) + "," + std::to_string(rect.getHeight());
            const bool sameDissolve = run.emberDissolve.dirX == dissolve.dirX && run.emberDissolve.dirY == dissolve.dirY &&
                                      run.emberDissolve.noise == dissolve.noise && run.emberDissolve.cell == dissolve.cell &&
                                      run.emberDissolve.jitter == dissolve.jitter && run.emberDissolve.seed == dissolve.seed &&
                                      run.emberDissolve.edgeWidth == dissolve.edgeWidth && run.emberDissolve.scorchWidth == dissolve.scorchWidth;
            if (frameKey != run.emberFrameKey || !sameDissolve){
                // a new frame (or values): its alpha once per texture and rect, its keys once
                run.emberFrameKey = frameKey;
                run.emberDissolve = dissolve;
                std::shared_ptr<const PixelFxFrameAlpha> alpha = PixelFxPlayer::frameAlpha(path, rect.getX(), rect.getY(),
                                                                                           std::fabs(rect.getWidth()), std::fabs(rect.getHeight()));
                if (alpha && alpha->width == frameW && alpha->height == frameH){
                    run.emberFrame = *alpha;
                }else{
                    run.emberFrame.width = frameW;
                    run.emberFrame.height = frameH;
                    run.emberFrame.opaque.assign((size_t)frameW * frameH, 1);
                }
                run.emberKeys.assign((size_t)frameW * frameH, 0.0f);
                for (int row = 0; row < frameH; row++)
                    for (int x = 0; x < frameW; x++)
                        run.emberKeys[(size_t)row * frameW + x] = pfxDissolveKey(x, frameH - 1 - row, frameW, frameH, dissolve);
            }
            const float p0 = run.emberProgress, p1 = progress;
            run.emberProgress = progress;
            if (p1 <= p0)
                return;
            const float f0 = pfxDissolveBurnFront(frameW, frameH, dissolve, p0);
            const float f1 = pfxDissolveBurnFront(frameW, frameH, dissolve, p1);
            std::vector<PixelFxTexel> crossed;
            for (int row = 0; row < frameH; row++){
                for (int x = 0; x < frameW; x++){
                    const size_t i = (size_t)row * frameW + x;
                    if (run.emberFrame.opaque[i] && run.emberKeys[i] >= f0 && run.emberKeys[i] < f1)
                        crossed.push_back({x, row});
                }
            }
            run.emberCarry += std::max(run.options.emberRate, 0.0f) * std::max(dt, 0.0);
            const size_t budget = (size_t)std::floor(run.emberCarry);
            if (crossed.empty() || budget == 0)
                return;
            const size_t take = std::min(budget, crossed.size());
            run.emberCarry -= (double)take;
            Transform* transform = run.scene->findComponent<Transform>(run.entity);
            const bool mirrorX = rect.getWidth() < 0.0f;
            std::vector<Vector3> positions;
            positions.reserve(take);
            for (size_t k = 0; k < take; k++){
                // spread over the texels that crossed, evenly (deterministic)
                const PixelFxTexel& t = crossed[(k * crossed.size()) / take];
                float fx = ((float)t.x + 0.5f) / (float)frameW;
                if (mirrorX) fx = 1.0f - fx;
                const float fy = 1.0f - ((float)t.row + 0.5f) / (float)frameH;
                Vector3 local(lo.x + fx * (hi.x - lo.x), lo.y + fy * (hi.y - lo.y), (lo.z + hi.z) * 0.5f);
                positions.push_back(transform ? transform->modelMatrix * local : local);
            }
            if (run.emberEffect.empty()){
                run.emberEffect = state().resolver ? state().resolver(run.options.embers) : std::string();
                if (run.emberEffect.empty())
                    run.emberEffect = run.options.embers;
            }
            if (state().emitter){
                state().emitter(run.scene, run.emberEffect, positions);
            }else{
                std::string error;
                if (!particles::PixelParticles::emitAt(run.scene, run.emberEffect, positions, {}, &error)){
                    if (state().logged.insert("embers|" + run.options.embers).second)
                        Log::warn("PixelFX: dissolve embers \"%s\": %s", run.options.embers.c_str(), error.c_str());
                    run.options.embers.clear();
                }
            }
        }

        void finish(PixelFxRun& run, bool byItself){
            if (run.done)
                return;
            run.done = true;
            if (alive(run))
                writeValues(run.scene, run.entity, endValues(run, !byItself));
            if (byItself){
                run.progress = 1.0f;
                if (run.options.onFinished){
                    std::function<void()> callback = std::move(run.options.onFinished);
                    run.options.onFinished = nullptr;
                    callback();
                }
            }
            run.options.onFinished = nullptr;
        }

        // the effect's start: its feature, ramp and the values of time 0
        void begin(PixelFxRun& run){
            const char* feature = kEffects[(int)run.effect].feature;
            if (run.effect == PixelFxEffect::OUTLINE_PULSE){
                bool had = false;
                forEachSpriteSubmesh(run.scene, run.entity, false, [&](MeshComponent&, Submesh& submesh){
                    had = had || ShaderLibraryValues::isFeatureEnabled(submesh.material.shaderFeatures, kSprite, "OUTLINE");
                });
                run.outlineColor = had ? firstParam(run.scene, run.entity, "outlineColor") : Vector4(0.0f, 0.0f, 0.0f, 1.0f);
                if (run.options.hasColor)
                    run.outlineColor = run.options.color;
                ensureFeature(&run, run.scene, run.entity, feature, "OUTLINE_STYLE_PIXEL4", kEffects[(int)run.effect].name);
            }else{
                ensureFeature(&run, run.scene, run.entity, feature, nullptr, kEffects[(int)run.effect].name);
            }
            if (run.effect == PixelFxEffect::DISSOLVE || run.effect == PixelFxEffect::UNDISSOLVE ||
                run.effect == PixelFxEffect::PETRIFY || run.effect == PixelFxEffect::FREEZE || run.effect == PixelFxEffect::POISON){
                setRamp(run.scene, run.entity, run.options.ramp);
            }
            std::vector<PixelFxValue> values = runValues(run);
            // the status looks' bevel and pulse, as the Petrify, Frozen and Poison presets
            if (run.effect == PixelFxEffect::PETRIFY || run.effect == PixelFxEffect::FREEZE){
                values.push_back({"rampBevel", Vector4(1.0f, 0.0f, 0.0f, 0.0f)});
                values.push_back({"rampPulse", Vector4(0.0f, 0.0f, 0.0f, 0.0f)});
            }
            if (run.effect == PixelFxEffect::POISON){
                values.push_back({"rampBevel", Vector4(0.5f, 0.0f, 0.0f, 0.0f)});
                values.push_back({"rampPulse", Vector4(2.0f, 0.0f, 0.0f, 0.0f)});
            }
            writeValues(run.scene, run.entity, values);
            run.emberProgress = run.effect == PixelFxEffect::DISSOLVE ? 0.0f : 1.0f;
        }

        void advance(PixelFxRun& run, double dt){
            if (!run.ready){
                if (!variantReady(run.scene, run.entity))
                    return;
                run.ready = true;
                // the frame it becomes ready counts from zero
                dt = 0.0;
            }
            run.time = std::min(run.time + std::max(dt, 0.0), (double)std::max(run.duration, 0.0f));
            run.progress = run.duration > 0.0f ? clamp01((float)(run.time / run.duration)) : 1.0f;
            writeValues(run.scene, run.entity, runValues(run));
            if (run.effect == PixelFxEffect::DISSOLVE){
                const std::vector<PixelFxValue> values = PixelFxPlayer::sample(run.effect, run.duration, run.options, run.time);
                emitEmbers(run, values.empty() ? run.progress : values[0].value.x, dt);
            }
            if (run.time >= run.duration)
                finish(run, true);
        }

    }

    // ---- names ----

    const char* pixelFxEffectName(PixelFxEffect effect){
        const int i = (int)effect;
        return i >= 0 && i < kPixelFxEffectCount ? kEffects[i].name : "";
    }

    bool pixelFxEffectFromName(std::string_view name, PixelFxEffect& out){
        for (int i = 0; i < kPixelFxEffectCount; i++){
            if (name == kEffects[i].name){
                out = (PixelFxEffect)i;
                return true;
            }
        }
        return false;
    }

    const char* pixelFxEffectFeature(PixelFxEffect effect){
        const int i = (int)effect;
        return i >= 0 && i < kPixelFxEffectCount ? kEffects[i].feature : "";
    }

    // ---- handle ----

    PixelFxHandle::PixelFxHandle(std::shared_ptr<PixelFxRun> run): run(std::move(run)){
    }

    bool PixelFxHandle::isValid() const{
        return run != nullptr;
    }

    bool PixelFxHandle::isDone() const{
        return !run || run->done || !alive(*run);
    }

    float PixelFxHandle::getProgress() const{
        return run ? run->progress : 0.0f;
    }

    void PixelFxHandle::stop(){
        if (run)
            finish(*run, false);
    }

    void PixelFxHandle::setOnFinished(std::function<void()> callback){
        if (run && !run->done)
            run->options.onFinished = std::move(callback);
    }

    std::shared_ptr<void>& PixelFxHandle::scriptData(){
        static std::shared_ptr<void> none;
        return run ? run->scriptData : none;
    }

    // ---- player ----

    std::vector<PixelFxValue> PixelFxPlayer::sample(PixelFxEffect effect, float duration, const PixelFxOptions& options, double seconds){
        const float u = duration > 0.0f ? clamp01((float)(seconds / duration)) : 1.0f;
        const float e = clamp01(anim::applyEase(options.ease, u));
        auto number = [](float v){ return Vector4(v, 0.0f, 0.0f, 0.0f); };
        switch (effect){
            case PixelFxEffect::FLASH:{
                std::vector<PixelFxValue> values = {{"flashAmount", number(1.0f - e)}};
                if (options.hasColor)
                    values.push_back({"flashColor", options.color});
                return values;
            }
            case PixelFxEffect::FADE_OUT:
                return {{"ditherFadeAmount", number(e)}};
            case PixelFxEffect::FADE_IN:
                return {{"ditherFadeAmount", number(1.0f - e)}};
            case PixelFxEffect::BLINK:{
                // count dithered blinks: out and back in each, through the Bayer thresholds
                const float phase = e * (float)countOf(options);
                const float f = phase - std::floor(phase);
                return {{"ditherFadeAmount", number(u >= 1.0f ? 0.0f : 1.0f - std::fabs(2.0f * f - 1.0f))}};
            }
            case PixelFxEffect::DISSOLVE:
                return {{"dissolveProgress", number(e)}};
            case PixelFxEffect::UNDISSOLVE:
                return {{"dissolveProgress", number(1.0f - e)}};
            case PixelFxEffect::PETRIFY:
            case PixelFxEffect::FREEZE:
            case PixelFxEffect::POISON:{
                // in, held, out: the ramp recolour's amount (a dithered choice per texel)
                const double total = std::max((double)duration, 0.0);
                const double hold = options.hold < 0.0f ? total * 0.5 : std::min((double)options.hold, total);
                const double ramp = (total - hold) * 0.5;
                float amount;
                if (total <= 0.0 || seconds >= total) amount = 0.0f;
                else if (seconds < ramp) amount = anim::applyEase(options.ease, (float)(seconds / ramp));
                else if (seconds < ramp + hold) amount = 1.0f;
                else amount = 1.0f - anim::applyEase(options.ease, (float)((seconds - ramp - hold) / std::max(ramp, 1e-9)));
                return {{"rampAmount", number(clamp01(amount))}};
            }
            case PixelFxEffect::OUTLINE_PULSE:{
                // count pulses, shown the first half of each: whole on or off, never partial alpha
                const float phase = u * (float)countOf(options);
                const bool on = u < 1.0f && phase - std::floor(phase) < 0.5f;
                Vector4 color = options.hasColor ? options.color : Vector4(0.0f, 0.0f, 0.0f, 1.0f);
                color.w = on ? (color.w > 0.0f ? color.w : 1.0f) : 0.0f;
                return {{"outlineColor", color}};
            }
        }
        return {};
    }

    PixelFxHandle PixelFxPlayer::play(Scene* scene, Entity entity, PixelFxEffect effect, float seconds, const PixelFxOptions& options,
                                      std::string* error){
        auto fail = [&](const std::string& text){
            if (error) *error = text;
            return PixelFxHandle();
        };
        if (!scene || !scene->isEntityCreated(entity))
            return fail("the GameObject does not exist");
        if ((int)effect < 0 || (int)effect >= kPixelFxEffectCount)
            return fail("unknown effect");
        bool any = false;
        forEachSpriteSubmesh(scene, entity, true, [&](MeshComponent&, Submesh&){ any = true; });
        if (!any)
            return fail("\"" + scene->getEntityName(entity) + "\" has no library:sprite material: give it a Sprite, or a Pixel FX material preset");

        auto run = std::make_shared<PixelFxRun>();
        run->scene = scene;
        run->lifetime = scene->getLifetime();
        run->entity = entity;
        run->effect = effect;
        run->duration = std::max(seconds, 0.0f);
        run->options = options;

        // one effect per feature on a GameObject: the running one is replaced
        const std::string feature = kEffects[(int)effect].feature;
        for (const std::shared_ptr<PixelFxRun>& other : state().runs){
            if (!other->done && other->scene == scene && other->entity == entity && feature == kEffects[(int)other->effect].feature){
                other->done = true;
                other->options.onFinished = nullptr;
            }
        }

        begin(*run);
        run->ready = variantReady(scene, entity);
        state().runs.push_back(run);
        if (run->duration <= 0.0f && run->ready && !run->options.manual)
            advance(*run, 0.0);
        return PixelFxHandle(run);
    }

    bool PixelFxPlayer::set(Scene* scene, Entity entity, const std::string& name, const Vector4& value, std::string* error){
        auto fail = [&](const std::string& text){
            if (error) *error = text;
            return false;
        };
        if (!scene || !scene->isEntityCreated(entity))
            return fail("the GameObject does not exist");
        std::string member = name;
        if (name == "flash") member = "flashAmount";
        else if (name == "fade") member = "ditherFadeAmount";
        else if (name == "dissolve") member = "dissolveProgress";
        else if (name == "ramp") member = "rampAmount";
        std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(kSprite);
        const ShaderManifestProperty* property = manifest ? manifest->findProperty(member) : nullptr;
        if (!property)
            return fail("\"" + name + "\" is not a channel (flash, fade, dissolve, ramp) or a library:sprite property");
        bool any = false;
        forEachSpriteSubmesh(scene, entity, true, [&](MeshComponent&, Submesh&){ any = true; });
        if (!any)
            return fail("\"" + scene->getEntityName(entity) + "\" has no library:sprite material: give it a Sprite, or a Pixel FX material preset");
        // its feature: the first one the property's row names
        std::string feature = property->feature;
        const size_t comma = feature.find(',');
        if (comma != std::string::npos) feature = feature.substr(0, comma);
        if (!feature.empty() && feature[0] == '!') feature.clear();
        if (!feature.empty()){
            for (const std::shared_ptr<PixelFxRun>& run : state().runs){
                if (!run->done && run->scene == scene && run->entity == entity && feature == kEffects[(int)run->effect].feature){
                    run->done = true;
                    run->options.onFinished = nullptr;
                }
            }
            const bool keyword = feature.rfind("OUTLINE_STYLE_", 0) == 0;
            if (keyword)
                ensureFeature(nullptr, scene, entity, "OUTLINE", feature.c_str(), "set");
            else
                ensureFeature(nullptr, scene, entity, feature, nullptr, "set");
        }
        writeValues(scene, entity, {{property->member.c_str(), value}});
        return true;
    }

    void PixelFxPlayer::update(double deltaTime, double unscaledDeltaTime){
        State& s = state();
        if (s.runs.empty())
            return;
        // a copy: an onFinished may play another effect
        std::vector<std::shared_ptr<PixelFxRun>> runs = s.runs;
        for (const std::shared_ptr<PixelFxRun>& run : runs){
            if (run->done || run->options.manual)
                continue;
            if (!alive(*run)){
                run->done = true;
                run->options.onFinished = nullptr;
                continue;
            }
            const double dt = run->options.unscaled ? unscaledDeltaTime : HitStop::deltaFor(run->scene, run->entity, deltaTime);
            advance(*run, dt);
        }
        s.runs.erase(std::remove_if(s.runs.begin(), s.runs.end(), [](const std::shared_ptr<PixelFxRun>& run){ return run->done; }),
                     s.runs.end());
        if (s.runs.empty())
            s.runs.shrink_to_fit();
    }

    void PixelFxPlayer::seek(Scene* scene, Entity entity, double seconds){
        for (const std::shared_ptr<PixelFxRun>& run : state().runs){
            if (run->scene != scene || run->entity != entity || !alive(*run))
                continue;
            run->done = false;
            run->ready = true;
            run->time = std::min(std::max(seconds, 0.0), (double)run->duration);
            run->progress = run->duration > 0.0f ? clamp01((float)(run->time / run->duration)) : 1.0f;
            writeValues(scene, entity, runValues(*run));
        }
    }

    size_t PixelFxPlayer::getActiveCount(){
        size_t count = 0;
        for (const std::shared_ptr<PixelFxRun>& run : state().runs)
            count += run->done ? 0 : 1;
        return count;
    }

    void PixelFxPlayer::reset(){
        State& s = state();
        for (const std::shared_ptr<PixelFxRun>& run : s.runs){
            run->done = true;
            run->options.onFinished = nullptr;
            run->scriptData.reset();
        }
        s.runs.clear();
        s.runs.shrink_to_fit();
        s.logged.clear();
        s.alphas.clear();
    }

    std::vector<PixelFxTexel> PixelFxPlayer::emberTexels(const PixelFxFrameAlpha& frame, const PfxDissolve& dissolve, float p0, float p1){
        std::vector<PixelFxTexel> out;
        if (frame.width <= 0 || frame.height <= 0 || p1 <= p0)
            return out;
        const float f0 = pfxDissolveBurnFront(frame.width, frame.height, dissolve, p0);
        const float f1 = pfxDissolveBurnFront(frame.width, frame.height, dissolve, p1);
        const bool mask = frame.opaque.size() == (size_t)frame.width * frame.height;
        for (int row = 0; row < frame.height; row++){
            for (int x = 0; x < frame.width; x++){
                if (mask && !frame.opaque[(size_t)row * frame.width + x])
                    continue;
                const float key = pfxDissolveKey(x, frame.height - 1 - row, frame.width, frame.height, dissolve);
                if (key >= f0 && key < f1)
                    out.push_back({x, row});
            }
        }
        return out;
    }

    void PixelFxPlayer::setEmberEmitter(EmberEmitter emitter){
        state().emitter = std::move(emitter);
    }

    void PixelFxPlayer::setEmberResolver(EmberResolver resolver){
        state().resolver = std::move(resolver);
    }

    std::shared_ptr<const PixelFxFrameAlpha> PixelFxPlayer::frameAlpha(const std::string& texturePath, float x, float y, float w, float h){
        State& s = state();
        const std::string key = texturePath + "|" + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(w) + "," + std::to_string(h);
        auto found = s.alphas.find(key);
        if (found != s.alphas.end())
            return found->second;
        if (s.alphas.size() >= 64)
            s.alphas.clear();
        std::shared_ptr<const PixelFxFrameAlpha> result;
        TextureData data;
        if (!texturePath.empty() && data.loadTextureFromFile(texturePath.c_str()) && !data.isCompressed() && data.getData() &&
            data.getWidth() > 0 && data.getHeight() > 0 && (data.getChannels() == 4 || data.getChannels() == 2)){
            auto alpha = std::make_shared<PixelFxFrameAlpha>();
            const int texW = data.getWidth(), texH = data.getHeight(), channels = data.getChannels();
            const int x0 = (int)std::lround(x * (float)texW), y0 = (int)std::lround(y * (float)texH);
            alpha->width = std::max((int)std::lround(w * (float)texW), 1);
            alpha->height = std::max((int)std::lround(h * (float)texH), 1);
            alpha->opaque.assign((size_t)alpha->width * alpha->height, 0);
            const uint8_t* pixels = (const uint8_t*)data.getData();
            for (int row = 0; row < alpha->height; row++){
                const int ty = y0 + row;
                for (int col = 0; col < alpha->width; col++){
                    const int tx = x0 + col;
                    if (tx < 0 || ty < 0 || tx >= texW || ty >= texH)
                        continue;
                    const uint8_t a = pixels[((size_t)ty * texW + tx) * channels + (channels - 1)];
                    alpha->opaque[(size_t)row * alpha->width + col] = a >= 128 ? 1 : 0;
                }
            }
            result = alpha;
        }
        s.alphas[key] = result;
        return result;
    }

}
