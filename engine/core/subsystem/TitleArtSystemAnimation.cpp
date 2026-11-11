// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// TitleArtSystem's half for S85's animations (docs/specs/S85-title-animation.md R13, R16,
// R24-R31): the library (the module compiles the document; a Baked player reads
// <file>.anim.json), the player's frame (Play Automatically, the driven time of a clip, seeks),
// the companion's draw values and flipbook page, and the markers (C++ and Lua subscribers, the
// script method of the marker's name, sounds) and ends. Always built: a Baked title plays with
// no module. Main thread.

#include "TitleArtSystem.h"

#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "asset/AssetMap.h"
#include "component/AudioSourceComponent.h"
#include "component/RectTransformComponent.h"
#include "component/SpriteComponent.h"
#include "component/Transform.h"
#include "object/sound/Audio.h"
#include "object/sound/SoundHandle.h"
#include "script/LuaBinding.h"
#include "subsystem/AudioSystem.h"
#include "titleanim/TitleAnimDataIO.h"
#include "titleanim/TitleAnimPlayer.h"
#if defined(JASPERENGINE_SOUND_LAYER) && JASPERENGINE_SOUND_LAYER
#include "audio/SoundEventLibrary.h"
#include "audio/TimelineAudio.h"
#endif

#include <chrono>
#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::titleart;
namespace ta = jasperengine::titleanim;

namespace {

    TitleArtRendererComponent* titleComponent(EntityRegistry* registry, Entity entity){
        if (!registry || !registry->isComponentRegistered<TitleArtRendererComponent>() || !registry->isEntityCreated(entity)) return nullptr;
        return registry->findComponent<TitleArtRendererComponent>(entity);
    }

    std::string joined(const std::vector<std::string>& names){
        std::string out;
        for (const std::string& n : names) out += (out.empty() ? "" : ", ") + n;
        return out.empty() ? std::string("none") : out;
    }

    // the still's frame as the companion shows it (S81's sidecar)
    ta::TitleFrameGeometry frameOf(const TitleArtInstance& instance){
        ta::TitleFrameGeometry g;
        const TitleArtBakeInfo& bake = instance.lendingRealtime ? instance.liveStatus.frame : instance.bake;
        g.frameX = bake.frame.x;
        g.frameY = bake.frame.y;
        g.frameWidth = bake.frame.width > 0 ? bake.frame.width : 1;
        g.frameHeight = bake.frame.height > 0 ? bake.frame.height : 1;
        g.originX = bake.originX;
        g.originY = bake.originY;
        g.inkX = bake.alphaBounds.x;
        g.inkY = bake.alphaBounds.y;
        g.inkWidth = bake.alphaBounds.width;
        g.inkHeight = bake.alphaBounds.height;
        return g;
    }

    void liveRestOf(const TitleArtRendererComponent& title, double* rest){
        for (size_t i = 0; i < (size_t)ta::TitleField::Count; i++) rest[i] = 0.0;
        rest[(size_t)ta::TitleField::SweepPosition] = title.sweepPosition;
        rest[(size_t)ta::TitleField::SweepAngle] = title.sweepAngle;
        rest[(size_t)ta::TitleField::SweepWidth] = title.sweepWidth;
        rest[(size_t)ta::TitleField::GlowPulse] = title.glowPulse;
        rest[(size_t)ta::TitleField::TintAmount] = title.tintAmount;
        rest[(size_t)ta::TitleField::GlintAmount] = title.glintAmount;
        rest[(size_t)ta::TitleField::TintColor] = 1.0;
    }

    std::string soundPath(const AssetRef& ref){
        std::string path;
        if (!ref.guid.isNull() && AssetMap::findPath(ref.guid, path)) return path;
        return ref.path;
    }

}

// ==================================================================== the library

void TitleArtSystem::loadAnimations(EntityRegistry* scene, Entity entity, TitleArtRendererComponent& title, TitleArtInstance& instance){
    if (instance.animationLoaded) return;
    instance.animationLoaded = true;
    instance.flipbookPages.clear();
    instance.animationPage = nullptr;
    if (title.title.path.empty()){
        instance.animation.setLibrary(nullptr);
        return;
    }
    const std::string name = scene ? scene->getEntityName(entity) : std::string();
    // Realtime and Glyph Atlas read the shipped document (every track); a Baked title, or a build
    // without the module, reads what its bake wrote (R60)
    if (title.renderMode != TitleArtRenderMode::BAKED && TitleArtRuntime::isBuilt()){
        std::string error;
        std::shared_ptr<ta::TitleAnimLibrary> library = TitleArtRuntime::compileAnimations(title.title.path, &error);
        if (library){
            instance.animationSource = "document";
            instance.animation.setLibrary(std::move(library));
            return;
        }
    }
    const std::string path = ta::titleAnimDataPath(title.title.path);
    ta::TitleAnimData data;
    std::string error;
    std::vector<std::string> warnings;
    auto library = std::make_shared<ta::TitleAnimLibrary>();
    instance.animationSource = "data";
    if (!ta::loadAnimData(path, data, &warnings, &error)){
        // no file: a title without animations (nothing to say); a newer or broken one is said once
        if (error.rfind("cannot read", 0) != 0 && instance.sayOnce("anim_data|" + error))
            Log::warn("Title Art '%s': %s %s", name.c_str(), path.c_str(), error.c_str());
        instance.animation.setLibrary(std::move(library));
        return;
    }
    // R60: a stale one plays and is reported
    if (instance.hasBake && !instance.bake.sourceHash.empty() && data.sourceHash != instance.bake.sourceHash && instance.sayOnce("anim_stale"))
        Log::warn("Title Art '%s': %s is older than the bake (bake_fresh): its animations play as they were baked. Bake the title again.",
                  name.c_str(), path.c_str());
    library->defaultName = data.defaultName;
    for (ta::TitleAnimDataEntry& e : data.animations){
        // a flipbook's pages are ordinary textures beside the document
        if (!e.compiled.flipbook.empty()){
            std::vector<Texture>& pages = instance.flipbookPages[e.compiled.id];
            pages.clear();
            for (const ta::FlipbookPage& page : e.compiled.flipbook.pages){
                Texture t;
                t.setPath(titleArtSiblingPath(title.title.path, page.file));
                pages.push_back(std::move(t));
            }
        }
        library->animations.push_back(std::move(e.compiled));
    }
    instance.animation.setLibrary(std::move(library));
}

titleanim::TitleAnimPlayer* TitleArtSystem::animationPlayer(EntityRegistry* scene, Entity entity){
    TitleArtRendererComponent* title = titleComponent(scene, entity);
    if (!title) return nullptr;
    TitleArtInstance& instance = instanceOf(*title);
    if (!instance.resolved || instance.resolvedTitle != title->title.path || instance.resolvedMode != title->renderMode){
        // a script asks before the system's first frame: what the document and bake hold now
        instance.animationLoaded = false;
    }
    loadAnimations(scene, entity, *title, instance);
    return &instance.animation;
}

std::vector<std::string> TitleArtSystem::animationNames(EntityRegistry* scene, Entity entity){
    ta::TitleAnimPlayer* player = animationPlayer(scene, entity);
    return player && player->library() ? player->library()->names() : std::vector<std::string>();
}

double TitleArtSystem::animationDuration(EntityRegistry* scene, Entity entity, const std::string& name){
    ta::TitleAnimPlayer* player = animationPlayer(scene, entity);
    return player ? player->duration(name) : -1.0;
}

// ==================================================================== the API

bool TitleArtSystem::playAnimation(EntityRegistry* scene, Entity entity, const std::string& name, const ta::TitleAnimPlayOptions& options, bool overlay){
    ta::TitleAnimPlayer* player = animationPlayer(scene, entity);
    if (!player) return false;
    TitleArtInstance& instance = instanceOf(*titleComponent(scene, entity));
    const bool ok = overlay || (player->find(name) && player->find(name)->overlay) ? player->playOverlay(name, options) : player->play(name, options);
    if (!ok){
        // R30: once per name per component
        if (instance.sayOnce("anim_unknown|" + name))
            Log::warn("Title Art '%s' has no animation '%s' (it has: %s)", scene->getEntityName(entity).c_str(), name.c_str(),
                      joined(animationNames(scene, entity)).c_str());
        return false;
    }
    // R24: the start pose shows before the frame draws
    if (TitleArtRendererComponent* title = titleComponent(scene, entity)) title->animationTime = (float)player->baseTime();
    return true;
}

bool TitleArtSystem::stopAnimation(EntityRegistry* scene, Entity entity, const std::string& name){
    ta::TitleAnimPlayer* player = animationPlayer(scene, entity);
    if (!player) return false;
    if (name.empty()){
        player->stop();
        return true;
    }
    if (!player->find(name)){
        TitleArtInstance& instance = instanceOf(*titleComponent(scene, entity));
        if (instance.sayOnce("anim_unknown|" + name))
            Log::warn("Title Art '%s' has no animation '%s' (it has: %s)", scene->getEntityName(entity).c_str(), name.c_str(),
                      joined(animationNames(scene, entity)).c_str());
        return false;
    }
    return player->stop(name);
}

void TitleArtSystem::resetAnimation(EntityRegistry* scene, Entity entity){
    if (ta::TitleAnimPlayer* player = animationPlayer(scene, entity)) player->reset();
}

bool TitleArtSystem::startAnimationEditPreview(EntityRegistry* scene, Entity entity){
    TitleArtRendererComponent* title = titleComponent(scene, entity);
    ta::TitleAnimPlayer* player = animationPlayer(scene, entity);
    if (!title || !player || !player->library() || player->library()->animations.empty()) return false;
    const std::string name = !title->defaultAnimation.empty() ? title->defaultAnimation : player->library()->defaultName;
    if (name.empty()) return false;
    ta::TitleAnimPlayOptions options;
    options.loop = 1;       // the preview repeats until Stop, whatever the animation's wrap
    if (!playAnimation(scene, entity, name, options)) return false;
    instanceOf(*title).animationEditPreview = true;
    instanceOf(*title).animationEditPreviewLast = std::chrono::steady_clock::time_point();
    return true;
}

void TitleArtSystem::stopAnimationEditPreview(EntityRegistry* scene, Entity entity){
    TitleArtRendererComponent* title = titleComponent(scene, entity);
    if (!title) return;
    instanceOf(*title).animationEditPreview = false;
    resetAnimation(scene, entity);
}

bool TitleArtSystem::isAnimationEditPreviewing(EntityRegistry* scene, Entity entity){
    TitleArtRendererComponent* title = titleComponent(scene, entity);
    return title && instanceOf(*title).animationEditPreview;
}

bool TitleArtSystem::animationHasMarkerSounds(EntityRegistry* scene, Entity entity){
    ta::TitleAnimPlayer* player = animationPlayer(scene, entity);
    if (!player || !player->library()) return false;
    for (const ta::CompiledTitleAnimation& a : player->library()->animations)
        for (const ta::CompiledMarker& m : a.markers) if (!m.sound.empty()) return true;
    return false;
}

bool TitleArtSystem::sampleAnimation(EntityRegistry* scene, Entity entity, const std::string& name, double time){
    ta::TitleAnimPlayer* player = animationPlayer(scene, entity);
    if (!player) return false;
    if (!player->sample(name, time)){
        TitleArtInstance& instance = instanceOf(*titleComponent(scene, entity));
        if (instance.sayOnce("anim_unknown|" + name))
            Log::warn("Title Art '%s' has no animation '%s' (it has: %s)", scene->getEntityName(entity).c_str(), name.c_str(),
                      joined(animationNames(scene, entity)).c_str());
        return false;
    }
    if (TitleArtRendererComponent* title = titleComponent(scene, entity)) title->animationTime = (float)time;
    return true;
}

bool TitleArtSystem::isAnimationReady(EntityRegistry* scene, Entity entity, const std::string& name){
    ta::TitleAnimPlayer* player = animationPlayer(scene, entity);
    if (!player) return false;
    const ta::CompiledTitleAnimation* animation = player->find(name);
    if (!animation) return false;
    TitleArtInstance& instance = instanceOf(*titleComponent(scene, entity));
    if (animation->rerender == ta::RerenderPolicy::Prerender && instance.live && TitleArtRuntime::isPrerendering(*instance.live)) return false;
    // a Baked flipbook is ready when its pages are on the GPU; everything else plays at once
    auto pages = instance.flipbookPages.find(animation->id);
    if (pages == instance.flipbookPages.end()) return true;
    for (Texture& page : pages->second){
        TextureRender* render = page.getRender();
        if (!render || !render->isCreated()) return false;
    }
    return true;
}

bool TitleArtSystem::prepareAnimation(EntityRegistry* scene, Entity entity, const std::string& name){
    ta::TitleAnimPlayer* player = animationPlayer(scene, entity);
    if (!player) return false;
    if (!player->find(name)){
        TitleArtInstance& instance = instanceOf(*titleComponent(scene, entity));
        if (instance.sayOnce("anim_unknown|" + name))
            Log::warn("Title Art '%s' has no animation '%s' (it has: %s)", scene->getEntityName(entity).c_str(), name.c_str(),
                      joined(animationNames(scene, entity)).c_str());
        return false;
    }
    // R18: a prerender animation's frames render ahead; the pages' loads start on first use
    TitleArtInstance& instance = instanceOf(*titleComponent(scene, entity));
    if (instance.live) TitleArtRuntime::prepareAnimation(*instance.live, *player, name);
    return isAnimationReady(scene, entity, name);
}

// ==================================================================== the frame

void TitleArtSystem::updateAnimation(Entity entity, TitleArtRendererComponent& title, TitleArtInstance& instance, double dt){
    loadAnimations(scene, entity, title, instance);
    ta::TitleAnimPlayer& player = instance.animation;
    instance.firedMarkers.clear();
    instance.finishedAnimations.clear();
    instance.animationDrawn = false;
    instance.animationPage = nullptr;
    const std::shared_ptr<const ta::TitleAnimLibrary>& library = player.library();
    if (!library || library->animations.empty()){
        title.animationTimeKeyed = false;
        return;
    }
    // R26: Play Automatically, in Play (the editor's Edit mode previews through the Animation panel)
    if (!instance.animationAutoStarted && !paused){
        instance.animationAutoStarted = true;
        if (title.playAutomatically && title.driveAnimation.empty()){
            const std::string name = !title.defaultAnimation.empty() ? title.defaultAnimation : library->defaultName;
            if (!name.empty()) playAnimation(scene, entity, name, ta::TitleAnimPlayOptions());
        }
    }
    player.setSpeedMultiplier(std::max(0.0f, std::min(10.0f, title.animationSpeed)));
    if (!title.driveAnimation.empty()){
        // R27: a clip or an Animator sets animationTime; the title shows that animation there
        if (!player.drive(title.driveAnimation, title.animationTime, &instance.firedMarkers) && instance.sayOnce("anim_unknown|" + title.driveAnimation)){
            Log::warn("Title Art '%s': Drive Animation '%s' is not one of its animations (it has: %s)", scene->getEntityName(entity).c_str(),
                      title.driveAnimation.c_str(), joined(library->names()).c_str());
        }
    }else{
        if (player.isDriven()) player.reset();
        // a written animationTime seeks (no markers)
        if (title.animationTimeKeyed) player.seek(title.animationTime);
        if (!paused || instance.animationEditPreview){
            // the component's Animation Update, or a play's own `unscaled`; Edit mode has no scaled
            // time, so its preview runs on the unscaled clock
            double unscaled = (double)Engine::getUnscaledDeltatime();
            if (paused){
                // no frame time in Edit mode: the preview runs on the wall clock
                const auto now = std::chrono::steady_clock::now();
                unscaled = instance.animationEditPreviewLast.time_since_epoch().count() ? std::chrono::duration<double>(now - instance.animationEditPreviewLast).count() : 0.0;
                unscaled = std::min(unscaled, 0.25);
                instance.animationEditPreviewLast = now;
            }
            const double step = title.animationUnscaledTime || paused ? unscaled : dt;
            player.update(step, unscaled, instance.firedMarkers, instance.finishedAnimations);
            // R49: silent, and no script is running in Edit mode to hear it
            if (paused){
                instance.firedMarkers.clear();
                instance.finishedAnimations.clear();
            }
        }
        title.animationTime = (float)player.baseTime();
    }
    title.animationTimeKeyed = false;

    // the companion's draw values (R13) and the flipbook frame (R16)
    liveRestOf(title, instance.animationLiveRest);
    player.summary(instance.animationLiveRest, instance.animationSummary);
    int next = -1;
    float blend = 0.0f;
    const ta::PlayingAnimation* base = player.baseAnimation();
    int frame = -1;
    const ta::CompiledFlipbook* flipbook = nullptr;
    // flipbooks are the Baked path's (Realtime and Glyph Atlas render their own poses)
    if (base && title.renderMode == TitleArtRenderMode::BAKED && !instance.lendingRealtime){
        frame = player.flipbookFrame(&next, &blend);
        if (frame >= 0){
            flipbook = &base->animation->flipbook;
            auto pages = instance.flipbookPages.find(base->animation->id);
            const int page = flipbook->frames[(size_t)frame].page;
            if (pages != instance.flipbookPages.end() && page < (int)pages->second.size()){
                Texture& texture = pages->second[(size_t)page];
                TextureRender* render = texture.getRender();
                // until the page is on the GPU the still shows (and the frame's rect is not used)
                if (render && render->isCreated()) instance.animationPage = &texture;
                else frame = -1;
            }else{
                frame = -1;
            }
        }
    }
    instance.animationDrawn = player.active() && (instance.animationSummary.any() || frame >= 0);
    if (!instance.animationDrawn) return;
    const ta::TitleFrameGeometry geometry = frameOf(instance);
    ta::companionUniforms(instance.animationSummary, geometry, base ? ta::titlePivotOf(*base->animation) : ta::TrackPivot::Origin,
                          frame >= 0 ? flipbook : nullptr, frame, next, blend, instance.animationUniforms);
    // the frame in the companion's own space: an Image spans its rect (y down), a Sprite its
    // size about its pivot (y up)
    if (instance.space == TitleArtSpace::UI){
        const RectTransformComponent* rect = scene->findComponent<RectTransformComponent>(entity);
        instance.animationSpace[0] = 0.0f;
        instance.animationSpace[1] = 0.0f;
        instance.animationSpace[2] = rect ? (float)rect->width : 1.0f;
        instance.animationSpace[3] = rect ? (float)rect->height : 1.0f;
    }else if (const SpriteComponent* sprite = scene->findComponent<SpriteComponent>(entity)){
        float px = 0.0f, py = 0.0f;
        const float w = (float)sprite->width, h = (float)sprite->height;
        switch (sprite->pivotPreset){
            case PivotPreset::BOTTOM_RIGHT: px = w; break;
            case PivotPreset::TOP_RIGHT: px = w; py = h; break;
            case PivotPreset::TOP_LEFT: py = h; break;
            case PivotPreset::TOP_CENTER: px = w / 2.0f; py = h; break;
            case PivotPreset::BOTTOM_CENTER: px = w / 2.0f; break;
            case PivotPreset::RIGHT_CENTER: px = w; py = h / 2.0f; break;
            case PivotPreset::LEFT_CENTER: py = h / 2.0f; break;
            case PivotPreset::CENTER: px = w / 2.0f; py = h / 2.0f; break;
            default: break;
        }
        // the frame's top-left in object space, the y axis down the frame
        instance.animationSpace[0] = -px;
        instance.animationSpace[1] = h - py;
        instance.animationSpace[2] = w;
        instance.animationSpace[3] = -h;
    }
}

void TitleArtSystem::dispatchAnimationEvents(Entity entity, TitleArtRendererComponent& title, TitleArtInstance& instance, double dt){
    (void)title;
    if (instance.firedMarkers.empty() && instance.finishedAnimations.empty()) return;
    // R28: in time order: C++ and Lua subscribers, then the script method of that name; then the
    // ends. What the handlers start takes effect next frame (they act on the player, not on this list)
    const std::vector<ta::FiredMarker> fired = instance.firedMarkers;
    const std::vector<std::string> finished = instance.finishedAnimations;
    for (const ta::FiredMarker& m : fired){
        if (!m.marker || !m.animation) continue;
        AnimationEvent event;
        event.functionName = m.marker->name;
        event.args = m.marker->args;
        event.state = m.animation->name;
        event.time = m.marker->time;
        instance.onAnimationEvent.call(event);
        if (!scene->isEntityCreated(entity)) return;
        LuaBinding::callLuaScriptMethod(scene, entity, event.functionName, event.args);
        if (!scene->isEntityCreated(entity)) return;
        if (!m.marker->sound.empty()) playMarkerSound(entity, *m.marker, m.fraction, dt);
    }
    for (const std::string& name : finished){
        instance.onAnimationFinished.call(name);
        if (!scene->isEntityCreated(entity)) return;
    }
}

void TitleArtSystem::playMarkerSound(Entity entity, const ta::CompiledMarker& marker, float fraction, double dt){
    const std::string path = soundPath(marker.sound);
    TitleArtRendererComponent* title = titleComponent(scene, entity);
    TitleArtInstance* instance = title ? &instanceOf(*title) : nullptr;
    auto unresolved = [&](){
        // R29: once per marker per Play
        if (instance && instance->sayOnce("marker_sound|" + marker.name + "|" + path))
            Log::warn("Title Art '%s': marker '%s' plays sound '%s', which does not resolve to an audio clip or a Sound Event",
                      scene->getEntityName(entity).c_str(), marker.name.c_str(), path.empty() ? marker.sound.guid.str().c_str() : path.c_str());
    };
    if (path.empty()){
        unresolved();
        return;
    }
    const float volume = (float)std::pow(10.0, (double)marker.volumeDb / 20.0);
    AudioSourceComponent* source = scene->isComponentRegistered<AudioSourceComponent>() ? scene->findComponent<AudioSourceComponent>(entity) : nullptr;
#if defined(JASPERENGINE_SOUND_LAYER) && JASPERENGINE_SOUND_LAYER
    // S40 R5, R6: a free voice placed at the crossing's sub-frame position; a .soundevent through
    // S37's runtime; 3D on the GameObject when its Audio Source attenuates, else 2D
    PlayOptions options;
    options.scene = scene;
    options.volume = volume;
    options.group = marker.group;
    if (source){
        if (options.group.empty()) options.group = source->outputGroup;
        if (source->attenuationModel != AudioSourceAttenuation::NO_ATTENUATION){
            options.targetScene = scene;
            options.target = entity;
        }
    }
    if (SoundEventLibrary::isEventName(path) && SoundEventLibrary::find(path) == nullptr){
        unresolved();
        return;
    }
    const SoundHandle handle = AudioSystem::playFreeFromTimeline(path, options, TimelineAudio::delayFrames(fraction, dt));
    if (!handle.isValid()) unresolved();
#else
    // without the sound layer: the Audio Source's one-shot, else a 2D voice on the group
    std::shared_ptr<AudioSystem> audio = scene->findSystem<AudioSystem>();
    if (!audio){
        unresolved();
        return;
    }
    (void)fraction;
    (void)dt;
    if (source){
        audio->playOneShot(*source, path, volume);
        return;
    }
    // at the scene camera: the listener's place, so the voice plays as 2D
    Vector3 at(0.0f, 0.0f, 0.0f);
    const Entity camera = scene->getCamera();
    if (camera != NULL_ENTITY)
        if (const Transform* transform = scene->findComponent<Transform>(camera)) at = transform->worldPosition;
    audio->playClipAtPoint(path, at, volume, marker.group);
#endif
}
