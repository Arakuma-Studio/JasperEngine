// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtSystem.h"

#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "animation/AnimationEvent.h"
#include "animation/ClipBinding.h"
#include "component/AnimationPlayerComponent.h"
#include "component/AnimatorComponent.h"
#include "component/CameraComponent.h"
#include "component/ImageComponent.h"
#include "component/MeshComponent.h"
#include "component/RectTransformComponent.h"
#include "component/SpriteComponent.h"
#include "component/Transform.h"
#include "component/UIComponent.h"
#include "diag/HangWatchdog.h"
#include "io/FileData.h"
#include "render/SystemRender.h"
#include "subsystem/RenderSystem.h"
#include "texture/Framebuffer.h"
#include "util/Color.h"
#include "util/Fnv.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <variant>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    // title_fx's features, in the order the shader declares them (R33-R35)
    enum LiveFeature : uint64_t {
        LIVE_SWEEP = 1u << 0,
        LIVE_GLOW = 1u << 1,
        LIVE_TINT = 1u << 2,
        LIVE_GLINTS = 1u << 3,
        LIVE_CROSSFADE = 1u << 4,
        LIVE_ANIM = 1u << 5
    };

    const char* const LIVE_FEATURE_NAMES[] = {"SWEEP", "GLOW", "TINT", "GLINTS", "CROSSFADE", "ANIM"};

    constexpr const char* TITLE_FX_UI = "library:title_fx";
    constexpr const char* TITLE_FX_SPRITE = "library:title_fx_sprite";
    constexpr const char* EVENT_TAG = "titleart";
    constexpr size_t MAX_TEXT_CODEPOINTS = 1024;

    double frameBudgetMs = 0.0;     // 0: the platform's (TitleArtRuntime)

    // R23: a document's (or an asset's it names) committed change, by title path
    std::map<std::string, uint64_t>& titleRevisions(){
        static std::map<std::string, uint64_t> revisions;
        return revisions;
    }
    uint64_t allTitlesRevision = 0;

    std::map<std::string, std::string>& simulatedCharacters(){
        static std::map<std::string, std::string> characters;
        return characters;
    }

    uint64_t revisionOf(const std::string& path){
        auto it = titleRevisions().find(path);
        return (it != titleRevisions().end() ? it->second : 0) + allTitlesRevision;
    }

    // the document's file name ("logo.titleart") for messages
    std::string fileName(const std::string& path){
        const size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    bool fileExists(const std::string& path){
        if (path.empty()) return false;
        FileData* file = FileData::newFile(path.c_str());
        const bool exists = file != nullptr && file->length() > 0;
        delete file;
        return exists;
    }

    size_t utf8Length(const std::string& text){
        size_t count = 0;
        for (unsigned char c : text) if ((c & 0xC0) != 0x80) count++;
        return count;
    }

    // the first `count` code points
    std::string utf8Prefix(const std::string& text, size_t count){
        size_t seen = 0;
        for (size_t i = 0; i < text.size(); i++){
            if (((unsigned char)text[i] & 0xC0) != 0x80){
                if (seen == count) return text.substr(0, i);
                seen++;
            }
        }
        return text;
    }

    // "#rrggbb" (or "#rgb") -> linear rgb
    bool parseHexColor(const std::string& text, Vector4& linear){
        if (text.empty() || text[0] != '#') return false;
        std::string hex = text.substr(1);
        if (hex.size() == 3) hex = {hex[0], hex[0], hex[1], hex[1], hex[2], hex[2]};
        if (hex.size() != 6) return false;
        for (char c : hex) if (!std::isxdigit((unsigned char)c)) return false;
        const unsigned value = (unsigned)std::stoul(hex, nullptr, 16);
        const Vector4 srgb(((value >> 16) & 0xff) / 255.0f, ((value >> 8) & 0xff) / 255.0f, (value & 0xff) / 255.0f, 1.0f);
        linear = Color::sRGBToLinear(srgb);
        return true;
    }

    // edit distance for "did you mean" (R65)
    size_t editDistance(const std::string& a, const std::string& b){
        std::vector<size_t> row(b.size() + 1);
        for (size_t j = 0; j <= b.size(); j++) row[j] = j;
        for (size_t i = 1; i <= a.size(); i++){
            size_t diagonal = row[0];
            row[0] = i;
            for (size_t j = 1; j <= b.size(); j++){
                const size_t above = row[j];
                const size_t cost = std::tolower((unsigned char)a[i - 1]) == std::tolower((unsigned char)b[j - 1]) ? 0 : 1;
                row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + cost});
                diagonal = above;
            }
        }
        return row[b.size()];
    }

    std::string nearest(const std::string& word, const std::vector<std::string>& candidates){
        std::string best;
        size_t bestDistance = word.size() / 3 + 2;
        for (const std::string& c : candidates){
            const size_t d = editDistance(word, c);
            if (d < bestDistance){
                bestDistance = d;
                best = c;
            }
        }
        return best;
    }

    std::string joinNames(const std::vector<std::string>& names){
        std::string out;
        for (size_t i = 0; i < names.size(); i++) out += (i ? ", " : "") + names[i];
        return out.empty() ? std::string("none") : out;
    }

    const TitleArtExposedParam* findExposed(const TitleArtInstance& instance, const std::string& name){
        for (const TitleArtExposedParam& p : instance.exposed)
            if (p.name == name) return &p;
        return nullptr;
    }

    TitleArtOverride* findOverride(TitleArtRendererComponent& title, const std::string& name){
        for (TitleArtOverride& o : title.overrides)
            if (o.name == name) return &o;
        return nullptr;
    }

    // an override whose name and type the document exposes (R4: the others are kept, ignored)
    bool overrideApplies(const TitleArtInstance& instance, const TitleArtOverride& o){
        const TitleArtExposedParam* p = findExposed(instance, o.name);
        return p && p->type == o.type;
    }

    // the "what" of R65's Baked sentence
    std::string bakedWhat(const std::string& name, const std::string& type){
        if (type == "text" && name == "text") return "text";
        if (type == "color_scheme") return "color scheme";
        return "'" + name + "'";
    }

    std::string bakedPer(const std::string& name, const std::string& type){
        if (type == "text" && name == "text") return "text";
        if (type == "color_scheme") return "color scheme";
        return "value of " + name;
    }

    // every entity from `entity` up its Transform parents (the clip root may be any of them)
    std::vector<Entity> selfAndAncestors(Scene* scene, Entity entity){
        std::vector<Entity> out;
        Entity current = entity;
        int guard = 0;
        while (current != NULL_ENTITY && guard++ < 256){
            out.push_back(current);
            const Transform* transform = scene->findComponent<Transform>(current);
            current = transform ? transform->parent : NULL_ENTITY;
        }
        return out;
    }

    std::string eventArgString(const AnimationEvent& event, size_t index){
        return index < event.args.size() && std::holds_alternative<std::string>(event.args[index]) ? std::get<std::string>(event.args[index]) : std::string();
    }

    TitleArtValue eventArgValue(const AnimationEvent& event, size_t index){
        if (index >= event.args.size()) return TitleArtValue::fromString("");
        const AnimationEventArg& arg = event.args[index];
        if (std::holds_alternative<double>(arg)) return TitleArtValue::fromNumber(std::get<double>(arg));
        if (std::holds_alternative<bool>(arg)) return TitleArtValue::fromBool(std::get<bool>(arg));
        return TitleArtValue::fromString(std::get<std::string>(arg));
    }

    float fract(double value){
        return (float)(value - std::floor(value));
    }

    // the component of a GameObject of any registry (null when the type is not registered there)
    TitleArtRendererComponent* titleOf(EntityRegistry* registry, Entity entity){
        if (!registry || !registry->isComponentRegistered<TitleArtRendererComponent>() || !registry->isEntityCreated(entity)) return nullptr;
        return registry->findComponent<TitleArtRendererComponent>(entity);
    }

}

// ==================================================================== TitleArtValue

TitleArtValue TitleArtValue::fromNumber(double value){
    TitleArtValue v;
    v.kind = Kind::Number;
    v.number = value;
    return v;
}

TitleArtValue TitleArtValue::fromBool(bool value){
    TitleArtValue v;
    v.kind = Kind::Bool;
    v.boolean = value;
    return v;
}

TitleArtValue TitleArtValue::fromString(std::string value){
    TitleArtValue v;
    v.kind = Kind::String;
    v.text = std::move(value);
    return v;
}

TitleArtValue TitleArtValue::fromColor(const Vector4& linear, bool hasAlpha){
    TitleArtValue v;
    v.kind = Kind::Color;
    v.color = linear;
    v.colorHasAlpha = hasAlpha;
    return v;
}

const char* TitleArtValue::typeName() const{
    switch (kind){
        case Kind::Number: return "number";
        case Kind::Bool: return "boolean";
        case Kind::String: return "string";
        case Kind::Color: return colorHasAlpha ? "Vector4" : "Vector3";
    }
    return "value";
}

// ==================================================================== state

struct TitleArtSystem::State {
    // the entities whose players' onEvent carry our subscription
    std::set<Entity> subscribedPlayers;
    std::set<Entity> subscribedAnimators;
    bool lending = false;               // some title lends an image this frame
    uint64_t contextId = 0;
};

TitleArtSystem::TitleArtSystem(Scene* scene): SubSystem(scene), state(std::make_unique<State>()){
}

TitleArtSystem::~TitleArtSystem() = default;

void TitleArtSystem::load(){
}

void TitleArtSystem::draw(){
    // Realtime and Glyph Atlas titles render in RenderSystem's frame passes (renderTitles)
}

void TitleArtSystem::destroy(){
    // a view loss or the scene's end: nothing is lent any more; the live states keep their
    // documents and render again with the next view (R22)
    state->lending = false;
    if (!scene->isComponentRegistered<TitleArtRendererComponent>())
        return;
    auto titles = scene->getComponentArray<TitleArtRendererComponent>();
    for (size_t i = 0; i < titles->size(); i++){
        TitleArtRendererComponent& title = titles->getComponentFromIndex(i);
        if (!title.runtime) continue;
        title.runtime->lending = false;
        title.runtime->lendingRealtime = false;
        title.runtime->bakeTexture.destroy();
        title.runtime->glowTexture.destroy();
    }
}

TitleArtInstance& TitleArtSystem::instanceOf(TitleArtRendererComponent& title){
    if (!title.runtime) title.runtime = std::make_shared<TitleArtInstance>();
    return *title.runtime;
}

TitleArtSpace TitleArtSystem::spaceOf(EntityRegistry* registry, Entity entity){
    if (registry->findComponent<UIComponent>(entity) && registry->findComponent<ImageComponent>(entity)) return TitleArtSpace::UI;
    if (registry->findComponent<MeshComponent>(entity) && registry->findComponent<SpriteComponent>(entity)) return TitleArtSpace::World;
    return TitleArtSpace::None;
}

bool TitleArtSystem::hasBothCompanions(EntityRegistry* registry, Entity entity){
    return registry->findComponent<UIComponent>(entity) && registry->findComponent<ImageComponent>(entity) &&
           registry->findComponent<MeshComponent>(entity) && registry->findComponent<SpriteComponent>(entity);
}

void TitleArtSystem::onComponentAdded(Entity entity, ComponentId componentId){
    if (!scene->isComponentRegistered<TitleArtRendererComponent>() || componentId != scene->getComponentId<TitleArtRendererComponent>())
        return;
    TitleArtRendererComponent& title = scene->getComponent<TitleArtRendererComponent>(entity);
    title.needUpdate = true;
}

void TitleArtSystem::onComponentRemoved(Entity entity, ComponentId componentId){
    if (!scene->isComponentRegistered<TitleArtRendererComponent>() || componentId != scene->getComponentId<TitleArtRendererComponent>())
        return;
    TitleArtRendererComponent& title = scene->getComponent<TitleArtRendererComponent>(entity);
    if (!title.runtime) return;
    // R2: the companion's own values back, its own texture bound again
    restoreCompanion(entity, *title.runtime);
    applyLending(entity, *title.runtime, false, false);
    title.runtime->live.reset();
}

// ==================================================================== the companion

TitleArtCompanionScope::TitleArtCompanionScope(const EntityRegistry* registry, Entity entity)
    : registry(const_cast<EntityRegistry*>(registry)), entity(entity){
    if (registry && entity != NULL_ENTITY && TitleArtSystem::getSavedCompanion(registry, entity)){
        swap();
        swapped = true;
    }
}

TitleArtCompanionScope::~TitleArtCompanionScope(){
    if (swapped) swap();
}

void TitleArtCompanionScope::swap(){
    TitleArtRendererComponent* title = registry->findComponent<TitleArtRendererComponent>(entity);
    if (!title || !title->runtime) return;
    TitleArtCompanionSaved& saved = title->runtime->saved;
    if (title->runtime->space == TitleArtSpace::UI){
        if (UIComponent* ui = registry->findComponent<UIComponent>(entity)){
            std::swap(ui->customShader, saved.customShader);
            std::swap(ui->shaderUniforms, saved.shaderUniforms);
            std::swap(ui->shaderTextures, saved.shaderTextures);
            std::swap(ui->shaderFeatures, saved.shaderFeatures);
        }
    }else if (MeshComponent* mesh = registry->findComponent<MeshComponent>(entity)){
        std::swap(mesh->customShader, saved.customShader);
        std::swap(mesh->shaderUniforms, saved.shaderUniforms);
        if (mesh->numSubmeshes > 0){
            std::swap(mesh->submeshes[0].material.shaderTextures, saved.shaderTextures);
            std::swap(mesh->submeshes[0].material.shaderFeatures, saved.shaderFeatures);
        }
    }
}

const TitleArtCompanionSaved* TitleArtSystem::getSavedCompanion(const EntityRegistry* registry, Entity entity){
    EntityRegistry* mutableRegistry = const_cast<EntityRegistry*>(registry);
    if (!mutableRegistry || entity == NULL_ENTITY || !mutableRegistry->isComponentRegistered<TitleArtRendererComponent>() ||
        !mutableRegistry->isEntityCreated(entity)) return nullptr;
    const TitleArtRendererComponent* title = mutableRegistry->findComponent<TitleArtRendererComponent>(entity);
    if (!title || !title->runtime || !title->runtime->saved.active) return nullptr;
    return &title->runtime->saved;
}

void TitleArtSystem::applyLending(Entity entity, TitleArtInstance& instance, bool lending, bool realtime){
    if (instance.lending == lending && instance.lendingRealtime == realtime) return;
    const bool changed = instance.lending != lending;
    instance.lending = lending;
    instance.lendingRealtime = realtime;
    // a lent image binds where the texture goes; an untextured Image or Sprite has no texture
    // coordinates in its variant: it is loaded again with one, and again without (FluidSystem)
    UIComponent* ui = scene->findComponent<UIComponent>(entity);
    if (ui && scene->findComponent<ImageComponent>(entity)){
        ui->needUpdateTexture = true;
        if (changed && ui->texture.empty()) ui->needReload = true;
    }else if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)){
        if (mesh->numSubmeshes > 0){
            mesh->submeshes[0].needUpdateTexture = true;
            if (changed && mesh->submeshes[0].material.baseColorTexture.empty()) mesh->needReload = true;
        }
    }
}

void TitleArtSystem::restoreCompanion(Entity entity, TitleArtInstance& instance){
    if (!instance.saved.active) return;
    if (UIComponent* ui = scene->findComponent<UIComponent>(entity); ui && instance.space == TitleArtSpace::UI){
        ui->customShader = instance.saved.customShader;
        ui->shaderUniforms = instance.saved.shaderUniforms;
        ui->shaderTextures = instance.saved.shaderTextures;
        ui->shaderFeatures = instance.saved.shaderFeatures;
        ui->needReload = true;
    }else if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity); mesh && instance.space == TitleArtSpace::World){
        mesh->customShader = instance.saved.customShader;
        mesh->shaderUniforms = instance.saved.shaderUniforms;
        if (mesh->numSubmeshes > 0){
            mesh->submeshes[0].material.shaderTextures = instance.saved.shaderTextures;
            mesh->submeshes[0].material.shaderFeatures = instance.saved.shaderFeatures;
        }
        mesh->needReload = true;
    }
    instance.saved = TitleArtCompanionSaved();
    instance.liveFeatures = 0;
    instance.liveFeatureNames.clear();
}

TextureRender* TitleArtSystem::getDisplayTexture(Entity entity, bool* transparent) const{
    if (!state->lending) return nullptr;
    TitleArtRendererComponent* title = scene->findComponent<TitleArtRendererComponent>(entity);
    if (!title || !title->runtime || !title->runtime->lending) return nullptr;
    TitleArtInstance& instance = *title->runtime;
    if (transparent) *transparent = true;
    if (instance.lendingRealtime && instance.live){
        if (TextureRender* texture = TitleArtRuntime::display(*instance.live)) return texture;
    }
    TextureRender* bake = instance.bakeTexture.getRender();
    return bake && bake->isCreated() ? bake : nullptr;
}

bool TitleArtSystem::hasDisplayTextures() const{
    return state->lending;
}

void TitleArtSystem::renderTitles(){
    runRuntimeFrame(paused);
}

void TitleArtSystem::runRuntimeFrame(bool editorEditMode){
    // one scheduler for the engine (R13): whichever asks first in a frame runs its steps. A frame
    // ends at a main-loop heartbeat or at a commit, as TitleFirstDraws counts them: the editor
    // commits only when a view redraws, so commits alone stop a render nothing else draws for
    static uint64_t lastFrame = ~uint64_t(0);
    const uint64_t frame = diag::heartbeats() + SystemRender::getFrameCount();
    if (frame == lastFrame) return;
    lastFrame = frame;
    if (!TitleArtRuntime::isBuilt()) return;
    TitleArtRuntime::runFrame(Engine::getUnscaledDeltatime(), editorEditMode);
}

// ==================================================================== update

void TitleArtSystem::update(double dt){
    state->lending = false;
    if (!scene->isComponentRegistered<TitleArtRendererComponent>())
        return;
    // R22: a lost device takes every target; the live states render again from their documents
    const uint64_t contextId = SystemRender::getContextId();
    const bool contextChanged = state->contextId != 0 && contextId != state->contextId;
    state->contextId = contextId;

    auto titles = scene->getComponentArray<TitleArtRendererComponent>();
    for (size_t i = 0; i < titles->size(); i++){
        const Entity entity = titles->getEntity(i);
        TitleArtRendererComponent& title = titles->getComponentFromIndex(i);
        TitleArtInstance& instance = instanceOf(title);
        if (contextChanged && instance.live) TitleArtRuntime::invalidate(*instance.live);
        updateTitle(entity, title, dt);
        // a component added or decoded during the loop moves the array
        titles = scene->getComponentArray<TitleArtRendererComponent>();
        if (instance.lending) state->lending = true;
    }
}

void TitleArtSystem::setSimulatedCharacters(const std::string& titlePath, const std::string& characters){
    if (characters.empty()) simulatedCharacters().erase(titlePath);
    else simulatedCharacters()[titlePath] = characters;
}

void TitleArtSystem::clearSimulatedCharacters(){
    simulatedCharacters().clear();
}

void TitleArtSystem::notifyTitleChanged(const std::string& titlePath){
    if (titlePath.empty()) allTitlesRevision++;
    else titleRevisions()[titlePath]++;
}

void TitleArtSystem::resolve(Entity entity, TitleArtRendererComponent& title, TitleArtInstance& instance){
    instance.resolved = true;
    instance.resolvedRevision = revisionOf(title.title.path);
    instance.prebakeChecked = false;
    instance.resolvedTitle = title.title.path;
    instance.resolvedMode = title.renderMode;
    instance.hasBake = false;
    instance.bake = TitleArtBakeInfo();
    instance.bakePath.clear();
    instance.glowPath.clear();
    instance.bakeTexture = Texture();
    instance.glowTexture = Texture();
    instance.exposed.clear();
    instance.exposedKnown = false;
    instance.documentError.clear();
    instance.moduleMissing = false;
    instance.live.reset();
    instance.liveStatus = TitleArtLiveStatus();
    instance.anchors.clear();
    instance.readyHash = 0;
    instance.failedHash = 0;
    if (title.title.path.empty()) return;

    const std::string name = scene->getEntityName(entity);
    std::string error;
    if (TitleArtBakeInfo::load(titleArtSidecarPath(title.title.path), instance.bake, error)){
        const TitleArtBakeOutput* main = instance.bake.output("main");
        if (!main) main = instance.bake.firstOutput("all");
        if (!main && !instance.bake.outputs.empty()) main = &instance.bake.outputs.front();
        if (main){
            instance.bakePath = titleArtSiblingPath(title.title.path, main->file);
            instance.hasBake = true;
        }
        if (const TitleArtBakeOutput* glow = instance.bake.firstOutput("glow"))
            instance.glowPath = titleArtSiblingPath(title.title.path, glow->file);
        instance.anchors = instance.bake.anchors;
    }else{
        // no sidecar: the main PNG of the default name may still be there (a bake before the
        // sidecar existed); it draws without a frame or anchors
        const std::string fallback = titleArtSiblingPath(title.title.path, titleArtDefaultBakeFile(title.title.path));
        if (fileExists(fallback)){
            instance.bakePath = fallback;
            instance.hasBake = true;
        }
    }
    if (instance.hasBake){
        instance.bakeTexture.setPath(instance.bakePath);
        if (!instance.glowPath.empty()) instance.glowTexture.setPath(instance.glowPath);
    }else if (instance.sayOnce("no_bake|" + title.title.path)){
        // R7: a missing bake shows nothing and says so once
        Log::warn("Title Art '%s': %s has no bake (%s). Bake it in the Title Designer (Bake) or with export_title.", name.c_str(),
                  title.title.path.c_str(), titleArtDefaultBakeFile(title.title.path).c_str());
    }

    std::string exposedError;
    instance.exposedKnown = TitleArtRuntime::readExposed(title.title.path, instance.exposed, &exposedError);
    if (!instance.exposedKnown) instance.documentError = exposedError;

    if (title.renderMode != TitleArtRenderMode::BAKED){
        if (!TitleArtRuntime::isBuilt()){
            instance.moduleMissing = true;
            if (instance.sayOnce("module|" + title.title.path)){
                // R51
                Log::warn("Title Art '%s' is %s, but this build has no Title Art module (titleart): showing its bake. Builds add the "
                          "module when a shipped scene or prefab uses Realtime or Glyph Atlas; check Build Settings > Modules.",
                          name.c_str(), title.renderMode == TitleArtRenderMode::REALTIME ? "Realtime" : "Glyph Atlas");
            }
        }else{
            std::string liveError;
            instance.live = TitleArtRuntime::create(title.title.path, title.renderMode, &liveError);
            if (!instance.live){
                instance.failed = true;
                instance.error = "Title Art '" + name + "': " + liveError;
                if (instance.sayOnce("create|" + liveError)) Log::error("%s", instance.error.c_str());
            }
        }
    }
}

void TitleArtSystem::updateTitle(Entity entity, TitleArtRendererComponent& title, double dt){
    TitleArtInstance& instance = *title.runtime;
    if (title.needUpdate || !instance.resolved || instance.resolvedTitle != title.title.path || instance.resolvedMode != title.renderMode ||
            instance.resolvedRevision != revisionOf(title.title.path)){
        const bool reload = !instance.resolved || instance.resolvedTitle != title.title.path || instance.resolvedMode != title.renderMode ||
                            instance.resolvedRevision != revisionOf(title.title.path);
        if (reload) resolve(entity, title, instance);
        title.needUpdate = false;
    }

    const bool active = title.enabled && scene->isEntityActiveInHierarchy(entity);
    instance.space = spaceOf(scene, entity);
    if (!active || instance.space == TitleArtSpace::None || title.title.path.empty()){
        applyLending(entity, instance, false, false);
        restoreCompanion(entity, instance);
        instance.wasEnabled = false;
        title.sweepPositionKeyed = false;
        return;
    }

    // R38: Live Time starts at 0 when the title is enabled, and runs in Play only
    if (!instance.wasEnabled && instance.liveTimeStarted) title.liveTime = 0.0f;
    instance.wasEnabled = true;
    instance.liveTimeStarted = true;
    if (!paused){
        const double step = title.liveUnscaledTime ? (double)Engine::getUnscaledDeltatime() : dt;
        if (std::isfinite(step) && step > 0.0) title.liveTime = (float)std::fmod((double)title.liveTime + step, 86400.0);
        // R34: the system writes the sweep unless a clip keyed it this frame
        if (title.sweepAuto && !title.sweepPositionKeyed){
            const double interval = std::max(0.01, (double)title.sweepInterval);
            title.sweepPosition = std::min(1.0f, fract((double)title.liveTime / interval) * (float)interval * std::max(0.0f, title.sweepSpeed));
        }
    }
    title.sweepPositionKeyed = false;

    updateLive(entity, title, instance);
    updateLiveEffects(entity, title, instance);
    updateStatus(entity, title, instance);
    subscribeEvents(entity);
}

// ---- Realtime and Glyph Atlas ----

bool TitleArtSystem::isVisible(Entity entity, const TitleArtRendererComponent& title, const TitleArtInstance& instance) const{
    (void)title;
    if (instance.space == TitleArtSpace::UI){
        const UIComponent* ui = scene->findComponent<UIComponent>(entity);
        if (!ui || !ui->enabled) return false;
        const Transform* transform = scene->findComponent<Transform>(entity);
        if (transform && !transform->visible) return false;
        // the rect against the canvas (UI coordinates are canvas pixels)
        if (!ui->worldAABB.isNull() && !ui->worldAABB.isInfinite()){
            const Vector3 lo = ui->worldAABB.getMinimum();
            const Vector3 hi = ui->worldAABB.getMaximum();
            const float width = (float)Engine::getCanvasWidth();
            const float height = (float)Engine::getCanvasHeight();
            if (width > 0.0f && height > 0.0f && (hi.x < 0.0f || hi.y < 0.0f || lo.x > width || lo.y > height)) return false;
        }
        return true;
    }
    const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
    if (!mesh) return false;
    const Transform* transform = scene->findComponent<Transform>(entity);
    if (transform && !transform->visible) return false;
    const Entity cameraEntity = scene->getCamera();
    CameraComponent* camera = cameraEntity != NULL_ENTITY ? scene->findComponent<CameraComponent>(cameraEntity) : nullptr;
    std::shared_ptr<RenderSystem> render = scene->findSystem<RenderSystem>();
    if (!camera || !render || mesh->worldAABB.isNull() || mesh->worldAABB.isInfinite()) return true;
    return render->isInsideCamera(*camera, mesh->worldAABB);
}

float TitleArtSystem::resolutionFor(const TitleArtBakeInfo& bake, bool ui, unsigned int rectWidth, float uiScale, float resolutionScale){
    // the display scale is in canvas (reference) units, so a build's prebaked state matches
    // whatever the device's resolution; the main output's own scale first (a 2x bake renders 2
    // output pixels per canvas pixel)
    double displayScale = 1.0;
    const int frameWidth = bake.frame.width;
    if (frameWidth > 0){
        const TitleArtBakeOutput* main = bake.output("main");
        if (!main) main = bake.firstOutput("all");
        if (main && main->width > 0) displayScale = (double)main->width / (double)frameWidth;
    }
    if (ui && frameWidth > 0 && rectWidth > 0) displayScale *= (double)rectWidth / ((double)frameWidth * std::max(0.01f, uiScale));
    const double scale = std::min(2.0, std::max(0.25, (double)resolutionScale));
    const double resolution = std::round(displayScale * scale * 8.0) / 8.0;
    return (float)std::min(4.0, std::max(0.125, resolution));
}

float TitleArtSystem::displayResolution(Entity entity, const TitleArtRendererComponent& title, const TitleArtInstance& instance) const{
    // R9: display scale x resolutionScale, rounded to 1/8
    const RectTransformComponent* rect = instance.space == TitleArtSpace::UI ? scene->findComponent<RectTransformComponent>(entity) : nullptr;
    return resolutionFor(instance.bake, instance.space == TitleArtSpace::UI, rect ? rect->width : 0u, title.uiScale, title.resolutionScale);
}

void TitleArtSystem::updateLive(Entity entity, TitleArtRendererComponent& title, TitleArtInstance& instance){
    if (title.renderMode == TitleArtRenderMode::BAKED || !instance.live){
        // Baked (or Realtime without the module: R51): the bake, as any PNG
        applyLending(entity, instance, instance.hasBake, false);
        return;
    }
    TitleArtLiveState liveState;
    for (const TitleArtOverride& o : title.overrides){
        if (!overrideApplies(instance, o)) continue;
        TitleArtOverride value = o;
        if (value.type == "text" && utf8Length(value.text) > MAX_TEXT_CODEPOINTS) value.text = utf8Prefix(value.text, MAX_TEXT_CODEPOINTS);
        liveState.overrides.push_back(std::move(value));
    }
    liveState.resolution = displayResolution(entity, title, instance);
    liveState.region = instance.bake.frame;
    liveState.priority = std::max(-100, std::min(100, title.priority));
    liveState.visible = isVisible(entity, title, instance);
    liveState.renderWhenHidden = title.renderWhenHidden;
    liveState.prebake = title.prebakeInitialState;
    liveState.transition = title.transition;
    liveState.transitionDuration = std::max(0.0f, std::min(2.0f, title.transitionDuration));
    liveState.atlasPageSize = title.atlasPageSize == 512 || title.atlasPageSize == 2048 ? title.atlasPageSize : 1024;
    liveState.atlasMaxPages = std::max(1, std::min(8, title.atlasMaxPages));
    liveState.qualityTier = title.qualityTier;
    liveState.label = "TitleArt/" + scene->getEntityName(entity);
    liveState.testFlags = instance.testFlags;
    liveState.glowOutput = title.glowEnabled;
    auto simulated = simulatedCharacters().find(title.title.path);
    if (simulated != simulatedCharacters().end() && !paused) liveState.characters = simulated->second + title.additionalCharacters;
    TitleArtRuntime::setState(*instance.live, liveState);
    if (!instance.prebakeChecked){
        // R11: a build ships the initial state's image; the first frame shows it at once
        instance.prebakeChecked = true;
        if (title.prebakeInitialState){
            const uint64_t hash = TitleArtRuntime::stateHashOf(*instance.live, liveState);
            const std::string prebaked = titleArtPrebakePath(title.title.path, TitleArtRuntime::prebakeKeyOf(*instance.live, liveState));
            if (fileExists(prebaked)) TitleArtRuntime::setPrebaked(*instance.live, prebaked, hash);
        }
    }
    instance.liveStatus = TitleArtRuntime::status(*instance.live);
    if (instance.liveStatus.anchorsChanged) instance.anchors = instance.liveStatus.frame.anchors;
    // R30, R31: what Glyph Atlas cannot do with this document, said once per title
    if (!instance.liveStatus.glyphFallback.empty() && instance.sayOnce("glyph_fallback|" + instance.liveStatus.glyphFallback)){
        Log::warn("Title Art '%s': glyph_atlas_unsupported: %s: it renders as Realtime", scene->getEntityName(entity).c_str(),
                  instance.liveStatus.glyphFallback.c_str());
    }
    if (!instance.liveStatus.glyphUnsafe.empty() && instance.sayOnce("glyph_effects")){
        std::string list;
        for (const std::string& name : instance.liveStatus.glyphUnsafe) list += (list.empty() ? "" : ", ") + name;
        Log::warn("Title Art '%s': glyph_atlas_effects: %zu effects don't apply per glyph: %s (check_title {checks: [\"glyph_atlas\"]} "
                  "lists the fixes)", scene->getEntityName(entity).c_str(), instance.liveStatus.glyphUnsafe.size(), list.c_str());
    }

    // R11: until the first image of the live state, the bake shows when the state is the
    // document's own (no override in effect), else nothing: a wrong text never flashes
    if (instance.liveStatus.hasImage){
        applyLending(entity, instance, true, true);
    }else{
        const bool defaults = liveState.overrides.empty();
        applyLending(entity, instance, defaults && instance.hasBake, false);
    }
}

// ---- live layers ----

void TitleArtSystem::updateLiveEffects(Entity entity, TitleArtRendererComponent& title, TitleArtInstance& instance){
    float crossfade = 1.0f;
    Framebuffer* previous = nullptr;
    if (instance.lendingRealtime && instance.live && title.transition == TitleArtTransition::CROSSFADE)
        previous = TitleArtRuntime::previousFramebuffer(*instance.live, &crossfade);

    uint64_t features = 0;
    if (title.sweepEnabled) features |= LIVE_SWEEP;
    // R35: Glow Pulse needs a glow output (the bake's, or the render's)
    Framebuffer* liveGlow = instance.lendingRealtime && instance.live ? TitleArtRuntime::glowFramebuffer(*instance.live) : nullptr;
    const bool glowAvailable = instance.lendingRealtime ? liveGlow != nullptr : !instance.glowPath.empty();
    if (title.glowEnabled && glowAvailable) features |= LIVE_GLOW;
    if (title.tintEnabled) features |= LIVE_TINT;
    if (title.glintsEnabled && !instance.anchors.empty()) features |= LIVE_GLINTS;
    if (previous) features |= LIVE_CROSSFADE;

    UIComponent* ui = instance.space == TitleArtSpace::UI ? scene->findComponent<UIComponent>(entity) : nullptr;
    MeshComponent* mesh = instance.space == TitleArtSpace::World ? scene->findComponent<MeshComponent>(entity) : nullptr;
    if (mesh && mesh->numSubmeshes == 0) mesh = nullptr;
    if (features == 0 || (!ui && !mesh)){
        // R33: no Live Effect, the built-in shader (and nothing of title_fx collected)
        restoreCompanion(entity, instance);
        return;
    }

    if (!instance.saved.active){
        // R2, R33: the companion's own shader (a user's custom one too) is kept to put back
        instance.saved.active = true;
        if (ui){
            instance.saved.customShader = ui->customShader;
            instance.saved.shaderUniforms = ui->shaderUniforms;
            instance.saved.shaderTextures = ui->shaderTextures;
            instance.saved.shaderFeatures = ui->shaderFeatures;
            if (!ui->customShader.empty() && ui->customShader.path != TITLE_FX_UI && instance.sayOnce("custom_shader"))
                Log::warn("Title Art '%s': the Image's custom shader '%s' is replaced by library:title_fx while a Live Effect is on",
                          scene->getEntityName(entity).c_str(), ui->customShader.path.c_str());
        }else{
            instance.saved.customShader = mesh->customShader;
            instance.saved.shaderUniforms = mesh->shaderUniforms;
            instance.saved.shaderTextures = mesh->submeshes[0].material.shaderTextures;
            instance.saved.shaderFeatures = mesh->submeshes[0].material.shaderFeatures;
            if (!mesh->customShader.empty() && mesh->customShader.path != TITLE_FX_SPRITE && instance.sayOnce("custom_shader"))
                Log::warn("Title Art '%s': the Sprite's custom shader '%s' is replaced by library:title_fx_sprite while a Live Effect is on",
                          scene->getEntityName(entity).c_str(), mesh->customShader.path.c_str());
        }
    }

    // the shader and its features: a reload only when they change (not per frame)
    if (features != instance.liveFeatures){
        instance.liveFeatures = features;
        instance.liveFeatureNames.clear();
        for (size_t f = 0; f < 6; f++) if (features & (uint64_t(1) << f)) instance.liveFeatureNames.push_back(LIVE_FEATURE_NAMES[f]);
        if (ui){
            ui->customShader = ShaderRef(TITLE_FX_UI);
            ui->shaderFeatures = instance.liveFeatureNames;
            ui->needReload = true;
        }else{
            mesh->customShader = ShaderRef(TITLE_FX_SPRITE);
            mesh->submeshes[0].material.shaderFeatures = instance.liveFeatureNames;
            mesh->needReload = true;
        }
    }

    // the textures the features sample (glow output, previous image)
    std::vector<std::pair<std::string, Texture>> textures;
    if (features & LIVE_GLOW){
        Texture glow;
        if (liveGlow) glow.setFramebuffer(liveGlow);
        else glow = instance.glowTexture;
        textures.push_back({"u_glowTexture", glow});
    }
    if (features & LIVE_CROSSFADE){
        Texture prev;
        prev.setFramebuffer(previous);
        textures.push_back({"u_prevTexture", prev});
    }
    std::vector<std::pair<std::string, Texture>>& slot = ui ? ui->shaderTextures : mesh->submeshes[0].material.shaderTextures;
    if (slot != textures){
        slot = textures;
        if (ui) ui->needUpdateTexture = true;
        else mesh->submeshes[0].needUpdateTexture = true;
    }

    // the uniforms (R34-R37), written when a value changed
    ShaderUniformValues values;
    auto put = [&values](const std::string& name, const Vector4& v){ values.push_back({name, v}); };
    if (features & LIVE_SWEEP){
        put("sweepParams", Vector4(std::min(1.0f, std::max(0.0f, title.sweepPosition)), std::max(0.0f, title.sweepWidth), title.sweepAngle,
                                   std::min(1.0f, std::max(0.0f, title.sweepSoftness))));
        put("sweepColor", title.sweepColor);
    }
    if (features & LIVE_GLOW){
        put("glowColor", title.glowColor);
        put("glowParams", Vector4(std::max(0.0f, title.glowPulse), 0.0f, 0.0f, 0.0f));
    }
    if (features & LIVE_TINT){
        put("tintColor", title.tintColor);
        put("tintParams", Vector4(std::min(1.0f, std::max(0.0f, title.tintAmount)), 0.0f, 0.0f, 0.0f));
    }
    if (features & LIVE_GLINTS){
        const int frameWidth = instance.lendingRealtime ? instance.liveStatus.frame.frame.width : instance.bake.frame.width;
        const int frameHeight = instance.lendingRealtime ? instance.liveStatus.frame.frame.height : instance.bake.frame.height;
        const float aspect = frameHeight > 0 && frameWidth > 0 ? (float)frameWidth / (float)frameHeight : 1.0f;
        put("glintColor", title.glintColor);
        put("glintParams", Vector4(std::max(0.0f, title.glintAmount), std::max(0.0f, title.glintSize), title.glintRate,
                                   (float)(title.glintSeed & 0xFFFFu)));
        put("glintInfo", Vector4((float)std::min(instance.anchors.size(), TITLE_ART_MAX_ANCHORS), title.liveTime, aspect, 0.0f));
        for (size_t a = 0; a < TITLE_ART_MAX_ANCHORS; a++){
            Vector4 v(0.0f, 0.0f, 0.0f, 0.0f);
            if (a < instance.anchors.size()){
                const TitleArtAnchor& anchor = instance.anchors[a];
                v = Vector4(anchor.x, anchor.y, anchor.size > 0.0f ? anchor.size : 1.0f, anchor.strength * anchor.fade);
            }
            put("anchors[" + std::to_string(a) + "]", v);
        }
    }
    if (features & LIVE_CROSSFADE) put("crossfadeParams", Vector4(crossfade, 0.0f, 0.0f, 0.0f));

    ShaderUniformValues& uniforms = ui ? ui->shaderUniforms : mesh->shaderUniforms;
    if (uniforms != values){
        uniforms = std::move(values);
        if (ui) ui->needUpdateShaderUniforms = true;
        else mesh->needUpdateShaderUniforms = true;
    }
}

// ---- readiness and failure (R18, R19, R55) ----

void TitleArtSystem::updateStatus(Entity entity, TitleArtRendererComponent& title, TitleArtInstance& instance){
    const std::string name = scene->getEntityName(entity);
    if (title.renderMode == TitleArtRenderMode::BAKED){
        instance.ready = instance.hasBake;
        instance.pending.clear();
        instance.failed = false;
        instance.error.clear();
        instance.pixelPath.clear();
        return;
    }
    if (instance.moduleMissing){
        // R51 / §6.6: values still work, the title waits forever with the sentence
        instance.ready = false;
        instance.pending = "failed";
        instance.failed = true;
        instance.error = "Title Art '" + name + "' is " + (title.renderMode == TitleArtRenderMode::REALTIME ? "Realtime" : "Glyph Atlas") +
                         ", but this build has no Title Art module (titleart): showing its bake. Builds add the module when a shipped scene or "
                         "prefab uses Realtime or Glyph Atlas; check Build Settings > Modules.";
        return;
    }
    if (!instance.live) return;
    const TitleArtLiveStatus& s = instance.liveStatus;
    instance.ready = s.ready;
    instance.pending = s.pending;
    instance.pixelPath = s.pixelPath;
    const bool wasFailed = instance.failed;
    instance.failed = s.failed;
    instance.error = s.failed ? "Title Art '" + name + "': " + s.error : std::string();
    if (s.ready && s.displayedHash != instance.readyHash){
        // R18: once per state, on the frame its image first shows
        instance.readyHash = s.displayedHash;
        instance.onReady.call();
    }
    if (s.failed && (!wasFailed || instance.failedHash != s.stateHash)){
        instance.failedHash = s.stateHash;
        if (instance.sayOnce("failed|" + s.error)) Log::error("%s", instance.error.c_str());
        instance.onFailed.call(instance.error);
    }
    if (!s.missingCharacters.empty() && s.missingCharacters != instance.missingReported){
        // R55: drawn with the next face that has them, else as nothing; said once per set
        instance.missingReported = s.missingCharacters;
        Log::warn("Title Art '%s': the characters \"%s\" are not in its Characters set or fonts and draw as nothing. Add them to "
                  "Characters > Custom in the Title Designer (Runtime) or the component's Additional Characters.",
                  name.c_str(), s.missingCharacters.c_str());
        instance.onMissingCharacters.call(s.missingCharacters);
    }
}

// ==================================================================== clip events (R41)

void TitleArtSystem::subscribeEvents(Entity entity){
    if (!scene->isComponentRegistered<AnimationPlayerComponent>() && !scene->isComponentRegistered<AnimatorComponent>()) return;
    for (Entity holder : selfAndAncestors(scene, entity)){
        if (scene->isComponentRegistered<AnimationPlayerComponent>()){
            if (AnimationPlayerComponent* player = scene->findComponent<AnimationPlayerComponent>(holder)){
                if (state->subscribedPlayers.insert(holder).second){
                    player->onEvent.add(EVENT_TAG, [this, holder](const AnimationEvent& event){ handleEvent(holder, event, "Animation Player"); });
                }
            }
        }
        if (scene->isComponentRegistered<AnimatorComponent>()){
            if (AnimatorComponent* animator = scene->findComponent<AnimatorComponent>(holder)){
                if (state->subscribedAnimators.insert(holder).second){
                    animator->onEvent.add(EVENT_TAG, [this, holder](const AnimationEvent& event){ handleEvent(holder, event, "Animator"); });
                }
            }
        }
    }
}

void TitleArtSystem::handleEvent(Entity player, const AnimationEvent& event, const std::string& source){
    const std::string& fn = event.functionName;
    if (fn != "titleArtSetText" && fn != "titleArtSetParameter" && fn != "titleArtSetColorScheme" && fn != "titleArtPlay") return;
    if (!scene->isEntityCreated(player)) return;
    // the args without `path` (R41's table), or with a leading `path`
    const size_t plain = fn == "titleArtSetParameter" ? 2 : 1;
    const bool withPath = event.args.size() == plain + 1;
    const std::string path = withPath ? eventArgString(event, 0) : std::string();
    const size_t first = withPath ? 1 : 0;
    const Entity target = withPath ? anim::resolvePath(scene, player, path) : player;
    const std::string key = event.state + "|" + fn + "|" + path;

    auto warn = [&](const std::string& message){
        static std::set<std::string> warned;
        if (warned.insert(std::to_string((uintptr_t)scene) + "|" + scene->getEntityName(player) + "|" + key).second)
            Log::warn("%s '%s': clip event '%s' of '%s': %s", source.c_str(), scene->getEntityName(player).c_str(), fn.c_str(),
                      event.state.c_str(), message.c_str());
    };
    if (event.args.size() != plain && !withPath){
        warn(std::string("takes ") + (plain == 2 ? "(name, value) or (path, name, value)" : "(value) or (path, value)") + " as its arguments");
        return;
    }
    TitleArtRendererComponent* title = target != NULL_ENTITY ? scene->findComponent<TitleArtRendererComponent>(target) : nullptr;
    if (!title){
        warn(withPath ? "no Title Art Renderer at path '" + path + "' (paths are relative to the player's GameObject, as clip tracks address children)"
                      : std::string("the player's GameObject has no Title Art Renderer: give the event a path to the title as its first argument"));
        return;
    }
    std::string error;
    if (fn == "titleArtPlay"){
        // S85 defines the animations; until it lands no name plays
        warn("no Title Art animation '" + eventArgString(event, first) + "' (Title Art animations arrive with the animation tools)");
        return;
    }
    bool ok = false;
    if (fn == "titleArtSetText"){
        ok = setParameter(scene, target, "text", TitleArtValue::fromString(eventArgString(event, first)), error);
    }else if (fn == "titleArtSetColorScheme"){
        std::string scheme;
        for (const std::string& n : parameterNames(scene, target)){
            const TitleArtInstance& instance = instanceOf(*title);
            if (const TitleArtExposedParam* p = findExposed(instance, n); p && p->type == "color_scheme"){ scheme = n; break; }
        }
        if (scheme.empty()) error = "the title exposes no color scheme parameter";
        else ok = setParameter(scene, target, scheme, TitleArtValue::fromString(eventArgString(event, first)), error);
    }else{
        ok = setParameter(scene, target, eventArgString(event, first), eventArgValue(event, first + 1), error);
    }
    if (!ok) warn(error);
}

// ==================================================================== the shared API

std::string TitleArtSystem::displayName(EntityRegistry* scene, Entity entity){
    return scene->getEntityName(entity);
}

void TitleArtSystem::setFrameBudget(double ms){
    frameBudgetMs = ms <= 0.0 ? 0.0 : std::min(16.0, std::max(0.25, ms));
    TitleArtRuntime::setFrameBudget(frameBudgetMs);
}

double TitleArtSystem::getFrameBudget(){
    return frameBudgetMs > 0.0 ? frameBudgetMs : TitleArtRuntime::getFrameBudget(false);
}

void TitleArtSystem::setDisabledFeaturesForTesting(EntityRegistry* scene, Entity entity, uint32_t flags){
    if (TitleArtRendererComponent* title = titleOf(scene, entity)){
        instanceOf(*title).testFlags = flags;
        if (title->runtime->live) TitleArtRuntime::invalidate(*title->runtime->live);
    }
}

void TitleArtSystem::refresh(EntityRegistry* scene, Entity entity){
    TitleArtRendererComponent* title = titleOf(scene, entity);
    if (title && title->runtime && title->runtime->live) TitleArtRuntime::invalidate(*title->runtime->live);
}

std::vector<std::string> TitleArtSystem::parameterNames(EntityRegistry* scene, Entity entity){
    std::vector<std::string> out;
    TitleArtRendererComponent* title = titleOf(scene, entity);
    if (!title) return out;
    TitleArtInstance& instance = instanceOf(*title);
    if (!instance.resolved || instance.resolvedTitle != title->title.path){
        instance.exposed.clear();
        instance.exposedKnown = TitleArtRuntime::readExposed(title->title.path, instance.exposed, &instance.documentError);
    }
    for (const TitleArtExposedParam& p : instance.exposed) out.push_back(p.name);
    return out;
}

std::vector<std::string> TitleArtSystem::colorSchemes(EntityRegistry* scene, Entity entity){
    TitleArtRendererComponent* title = titleOf(scene, entity);
    if (!title) return {};
    parameterNames(scene, entity);
    for (const TitleArtExposedParam& p : instanceOf(*title).exposed)
        if (p.type == "color_scheme") return p.options;
    return {};
}

bool TitleArtSystem::hasParameter(EntityRegistry* scene, Entity entity, const std::string& name){
    const std::vector<std::string> names = parameterNames(scene, entity);
    return std::find(names.begin(), names.end(), name) != names.end();
}

bool TitleArtSystem::setParameter(EntityRegistry* scene, Entity entity, const std::string& name, const TitleArtValue& value, std::string& error,
                                  std::string* warning){
    TitleArtRendererComponent* title = titleOf(scene, entity);
    const std::string who = "TitleArt '" + scene->getEntityName(entity) + "'";
    if (!title){
        error = "TitleArt: GameObject '" + scene->getEntityName(entity) +
                "' has no Title Art Renderer. Add one in the Inspector (Add Component > Rendering > Title Art Renderer).";
        return false;
    }
    const std::vector<std::string> names = parameterNames(scene, entity);
    TitleArtInstance& instance = instanceOf(*title);
    const TitleArtExposedParam* param = findExposed(instance, name);
    if (title->renderMode == TitleArtRenderMode::BAKED){
        // R65: the Baked sentence names what the script tried to change
        const std::string type = param ? param->type : (name == "text" ? "text" : "");
        error = who + ": " + bakedWhat(name, type) + " cannot change on a Baked title (its pixels are the asset's bake). Set Render Mode to "
                "Realtime (or Glyph Atlas for text that changes every frame) in the Title Art Renderer, or bake one Title Art per " +
                bakedPer(name, type) + " and enable the one you need.";
        return false;
    }
    if (!param){
        if (name == "text"){
            error = who + ": " + fileName(title->title.path) + " exposes no 'text' parameter. Expose its text layer's text in the Title Designer "
                    "(Exposed Parameters > Expose Text).";
            return false;
        }
        const std::string near = nearest(name, names);
        error = who + ": no exposed parameter '" + name + "'" + (near.empty() ? "" : " (did you mean '" + near + "'?)") + ". Exposed: " +
                joinNames(names) + ".";
        return false;
    }

    TitleArtOverride next;
    next.name = name;
    next.type = param->type;
    const std::string& type = param->type;
    auto wrongType = [&](const char* expected){
        error = who + ": '" + name + "' is " + expected + "; got " + value.typeName() + ".";
        return false;
    };
    if (type == "number"){
        if (value.kind != TitleArtValue::Kind::Number) return wrongType("a number");
        double v = value.number;
        if (!std::isfinite(v)) return wrongType("a finite number");
        const double lo = param->hasMin ? param->min : -1e30, hi = param->hasMax ? param->max : 1e30;
        if (v < lo || v > hi){
            const double clamped = std::min(hi, std::max(lo, v));
            if (warning){
                char buffer[160];
                std::snprintf(buffer, sizeof(buffer), "%s: '%s' %g clamped to %g (range %g..%g)", who.c_str(), name.c_str(), v, clamped, lo, hi);
                *warning = buffer;
            }
            v = clamped;
        }
        next.value = Vector4((float)v, 0.0f, 0.0f, 0.0f);
    }else if (type == "bool"){
        if (value.kind == TitleArtValue::Kind::Bool) next.value = Vector4(value.boolean ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
        else if (value.kind == TitleArtValue::Kind::Number) next.value = Vector4(value.number != 0.0 ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
        else return wrongType("a boolean");
    }else if (type == "color"){
        Vector4 linear;
        if (value.kind == TitleArtValue::Kind::Color) linear = value.color;
        else if (value.kind == TitleArtValue::Kind::String && parseHexColor(value.text, linear)){}
        else return wrongType("a color (\"#rrggbb\" or a Vector3/Vector4 in linear values)");
        next.value = Vector4(linear.x, linear.y, linear.z, 1.0f);
    }else if (type == "color_scheme"){
        if (value.kind != TitleArtValue::Kind::String) return wrongType("a color scheme id (a string)");
        if (!param->options.empty() && std::find(param->options.begin(), param->options.end(), value.text) == param->options.end()){
            const std::string near = nearest(value.text, param->options);
            error = who + ": color scheme '" + value.text + "' is not one of " + joinNames(param->options) +
                    (near.empty() ? "" : " (did you mean '" + near + "'?)") + ".";
            return false;
        }
        next.text = value.text;
    }else{
        // text, font, image
        if (value.kind != TitleArtValue::Kind::String)
            return wrongType(type == "text" ? "a string" : type == "font" ? "a font asset path (a string)" : "an image asset path (a string)");
        next.text = value.text;
        if (type == "text" && utf8Length(next.text) > MAX_TEXT_CODEPOINTS){
            next.text = utf8Prefix(next.text, MAX_TEXT_CODEPOINTS);
            if (warning) *warning = who + ": the text is longer than 1024 characters and was cut";
        }
    }

    // the document's own value: no override needed (and none kept)
    if (param->hasDefault && param->defaultValue.value == next.value && param->defaultValue.text == next.text){
        auto it = std::find_if(title->overrides.begin(), title->overrides.end(), [&](const TitleArtOverride& o){ return o.name == name; });
        if (it != title->overrides.end()){
            title->overrides.erase(it);
            title->needUpdate = true;
        }
        return true;
    }
    if (TitleArtOverride* existing = findOverride(*title, name)){
        if (*existing == next) return true;     // §6.6: an equal value does nothing (no render)
        *existing = next;
    }else{
        title->overrides.push_back(next);
    }
    title->needUpdate = true;
    return true;
}

bool TitleArtSystem::getParameter(EntityRegistry* scene, Entity entity, const std::string& name, TitleArtValue& out, std::string& error){
    TitleArtRendererComponent* title = titleOf(scene, entity);
    if (!title){
        error = "TitleArt: GameObject '" + scene->getEntityName(entity) + "' has no Title Art Renderer.";
        return false;
    }
    const std::vector<std::string> names = parameterNames(scene, entity);
    TitleArtInstance& instance = instanceOf(*title);
    const TitleArtExposedParam* param = findExposed(instance, name);
    if (!param){
        const std::string near = nearest(name, names);
        error = "TitleArt '" + scene->getEntityName(entity) + "': no exposed parameter '" + name + "'" +
                (near.empty() ? "" : " (did you mean '" + near + "'?)") + ". Exposed: " + joinNames(names) + ".";
        return false;
    }
    // R5: the override (Baked titles ignore theirs: R7), else the document's value
    const TitleArtOverride* o = title->renderMode != TitleArtRenderMode::BAKED ? findOverride(*title, name) : nullptr;
    if (o && o->type != param->type) o = nullptr;
    const TitleArtOverride& v = o ? *o : param->defaultValue;
    if (param->type == "number") out = TitleArtValue::fromNumber(v.value.x);
    else if (param->type == "bool") out = TitleArtValue::fromBool(v.value.x != 0.0f);
    else if (param->type == "color") out = TitleArtValue::fromColor(Vector4(v.value.x, v.value.y, v.value.z, 1.0f), true);
    else out = TitleArtValue::fromString(v.text);
    return true;
}

bool TitleArtSystem::resetParameter(EntityRegistry* scene, Entity entity, const std::string& name, std::string& error){
    TitleArtRendererComponent* title = titleOf(scene, entity);
    if (!title){
        error = "TitleArt: GameObject '" + scene->getEntityName(entity) + "' has no Title Art Renderer.";
        return false;
    }
    if (!hasParameter(scene, entity, name)){
        const std::vector<std::string> names = parameterNames(scene, entity);
        const std::string near = nearest(name, names);
        error = "TitleArt '" + scene->getEntityName(entity) + "': no exposed parameter '" + name + "'" +
                (near.empty() ? "" : " (did you mean '" + near + "'?)") + ". Exposed: " + joinNames(names) + ".";
        return false;
    }
    auto it = std::find_if(title->overrides.begin(), title->overrides.end(), [&](const TitleArtOverride& o){ return o.name == name; });
    if (it != title->overrides.end()){
        title->overrides.erase(it);
        title->needUpdate = true;
    }
    return true;
}

bool TitleArtSystem::nativeSize(EntityRegistry* scene, Entity entity, float& width, float& height, float& pivotX, float& pivotY){
    TitleArtRendererComponent* title = titleOf(scene, entity);
    if (!title) return false;
    TitleArtInstance& instance = instanceOf(*title);
    TitleArtBakeInfo info;
    if (instance.resolved && instance.resolvedTitle == title->title.path && instance.bake.frame.width > 0){
        info = instance.bake;
    }else{
        std::string error;
        if (!TitleArtBakeInfo::load(titleArtSidecarPath(title->title.path), info, error)) return false;
    }
    if (info.frame.width <= 0 || info.frame.height <= 0) return false;
    const TitleArtSpace space = spaceOf(scene, entity);
    if (space == TitleArtSpace::World){
        // R3: frame px / Pixels Per Unit. Sprites are sized in whole units and a 2D scene's
        // unit is a pixel, so 0 (the default) means 1 pixel per unit (spec Deviations)
        const float ppu = title->pixelsPerUnit > 0.0f ? title->pixelsPerUnit : 1.0f;
        width = (float)info.frame.width / ppu;
        height = (float)info.frame.height / ppu;
    }else{
        const float scale = title->uiScale > 0.0f ? title->uiScale : 1.0f;
        width = (float)info.frame.width * scale;
        height = (float)info.frame.height * scale;
    }
    pivotX = info.pivotX();
    pivotY = info.pivotY();
    return true;
}

bool TitleArtSystem::setNativeSize(EntityRegistry* scene, Entity entity, std::string& error){
    TitleArtRendererComponent* title = titleOf(scene, entity);
    if (!title){
        error = "TitleArt: GameObject '" + scene->getEntityName(entity) + "' has no Title Art Renderer.";
        return false;
    }
    float width = 0.0f, height = 0.0f, pivotX = 0.5f, pivotY = 0.5f;
    if (!nativeSize(scene, entity, width, height, pivotX, pivotY)){
        error = "TitleArt '" + scene->getEntityName(entity) + "': " + (title->title.path.empty() ? std::string("no Title Art is assigned") : fileName(title->title.path) + " has no bake to size from") +
                ". Bake it in the Title Designer (Bake) or with export_title.";
        return false;
    }
    if (RectTransformComponent* rect = scene->findComponent<RectTransformComponent>(entity); rect && spaceOf(scene, entity) == TitleArtSpace::UI){
        rect->width = (unsigned int)std::max(1.0f, std::round(width));
        rect->height = (unsigned int)std::max(1.0f, std::round(height));
        if (title->pivotFromTitle) rect->pivot = Vector2(pivotX, pivotY);
        rect->needUpdateSizes = true;
        scene->markChanged<RectTransformComponent>(entity);
        if (Transform* transform = scene->findComponent<Transform>(entity)) transform->needUpdate = true;
        return true;
    }
    if (SpriteComponent* sprite = scene->findComponent<SpriteComponent>(entity)){
        sprite->width = (unsigned int)std::max(1.0f, std::round(width));
        sprite->height = (unsigned int)std::max(1.0f, std::round(height));
        if (title->pivotFromTitle){
            // the nearest of the nine presets (sprites have no free pivot)
            const int column = pivotX < 1.0f / 3.0f ? 0 : (pivotX > 2.0f / 3.0f ? 2 : 1);
            const int row = pivotY < 1.0f / 3.0f ? 0 : (pivotY > 2.0f / 3.0f ? 2 : 1);
            static const PivotPreset presets[3][3] = {
                {PivotPreset::TOP_LEFT, PivotPreset::TOP_CENTER, PivotPreset::TOP_RIGHT},
                {PivotPreset::LEFT_CENTER, PivotPreset::CENTER, PivotPreset::RIGHT_CENTER},
                {PivotPreset::BOTTOM_LEFT, PivotPreset::BOTTOM_CENTER, PivotPreset::BOTTOM_RIGHT}};
            sprite->pivotPreset = presets[row][column];
        }
        sprite->needUpdateSprite = true;
        scene->markChanged<SpriteComponent>(entity);
        return true;
    }
    error = "TitleArt '" + scene->getEntityName(entity) + "': no Image or Sprite to size (add one with the Title Art Renderer)";
    return false;
}
