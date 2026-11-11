// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArt.h"
#include "util/TitleArtLanguage.h"

#include "titleanim/TitleAnimPlayer.h"

#include "Log.h"
#include "Scene.h"
#include "System.h"
#include "subsystem/TitleArtSystem.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace {

    float finite(float value, float fallback){
        return std::isfinite(value) ? value : fallback;
    }

}

TitleArt::TitleArt(Scene* scene, Entity entity): GameObject(scene, entity){
}

TitleArt::~TitleArt() = default;

TitleArtRendererComponent* TitleArt::component() const{
    Scene* s = getScene();
    if (!s || !s->isEntityCreated(getEntity()) || !s->isComponentRegistered<TitleArtRendererComponent>()) return nullptr;
    return s->findComponent<TitleArtRendererComponent>(getEntity());
}

bool TitleArt::fail(const std::string& message, std::string* error) const{
    if (error) *error = message;
    else Log::error("%s", message.c_str());
    return false;
}

bool TitleArt::isValid() const{
    return component() != nullptr;
}

std::string TitleArt::getTitle() const{
    const TitleArtRendererComponent* c = component();
    return c ? c->title.path : std::string();
}

TitleArtRenderMode TitleArt::getRenderMode() const{
    const TitleArtRendererComponent* c = component();
    return c ? c->renderMode : TitleArtRenderMode::BAKED;
}

std::string TitleArt::getText() const{
    TitleArtValue value;
    std::string error;
    if (!component() || !TitleArtSystem::getParameter(getScene(), getEntity(), "text", value, error)) return std::string();
    return value.text;
}

bool TitleArt::setText(const std::string& text, std::string* error){
    return setParameter("text", TitleArtValue::fromString(text), error);
}

std::string TitleArt::getColorScheme() const{
    TitleArtRendererComponent* c = component();
    if (!c) return std::string();
    TitleArtSystem::parameterNames(getScene(), getEntity());
    for (const titleart::TitleArtExposedParam& p : TitleArtSystem::instanceOf(*c).exposed){
        if (p.type != "color_scheme") continue;
        TitleArtValue value;
        std::string error;
        if (TitleArtSystem::getParameter(getScene(), getEntity(), p.name, value, error)) return value.text;
    }
    return std::string();
}

bool TitleArt::setColorScheme(const std::string& id, std::string* error){
    TitleArtRendererComponent* c = component();
    if (!c) return fail("TitleArt: GameObject '" + getName() + "' has no Title Art Renderer. Add one in the Inspector (Add Component > "
                        "Rendering > Title Art Renderer).", error);
    TitleArtSystem::parameterNames(getScene(), getEntity());
    std::string name;
    for (const titleart::TitleArtExposedParam& p : TitleArtSystem::instanceOf(*c).exposed){
        if (p.type == "color_scheme"){
            name = p.name;
            break;
        }
    }
    if (name.empty()){
        if (c->renderMode == TitleArtRenderMode::BAKED) return setParameter("color_scheme", TitleArtValue::fromString(id), error);
        const std::string& path = c->title.path;
        const std::string file = path.substr(path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/') + 1);
        return fail("TitleArt '" + getName() + "': " + file + " exposes no color scheme parameter. Expose its color scheme in the Title "
                    "Designer (Exposed Parameters > Expose Color Scheme).", error);
    }
    return setParameter(name, TitleArtValue::fromString(id), error);
}

std::vector<std::string> TitleArt::getColorSchemes() const{
    return component() ? TitleArtSystem::colorSchemes(getScene(), getEntity()) : std::vector<std::string>();
}

std::vector<std::string> TitleArt::getParameterNames() const{
    return component() ? TitleArtSystem::parameterNames(getScene(), getEntity()) : std::vector<std::string>();
}

bool TitleArt::setParameter(const std::string& name, const TitleArtValue& value, std::string* error){
    std::string message;
    std::string warning;
    if (!TitleArtSystem::setParameter(getScene(), getEntity(), name, value, message, &warning)) return fail(message, error);
    // §6.6: a clamp or a cut text warns, it is no error
    if (!warning.empty()) Log::warn("%s", warning.c_str());
    return true;
}

bool TitleArt::setParameter(const std::string& name, double value, std::string* error){
    return setParameter(name, TitleArtValue::fromNumber(value), error);
}

bool TitleArt::setParameter(const std::string& name, bool value, std::string* error){
    return setParameter(name, TitleArtValue::fromBool(value), error);
}

bool TitleArt::setParameter(const std::string& name, const std::string& value, std::string* error){
    return setParameter(name, TitleArtValue::fromString(value), error);
}

bool TitleArt::setParameter(const std::string& name, const Vector4& linearColor, std::string* error){
    return setParameter(name, TitleArtValue::fromColor(linearColor, true), error);
}

bool TitleArt::getParameter(const std::string& name, TitleArtValue& out, std::string* error) const{
    std::string message;
    if (!TitleArtSystem::getParameter(getScene(), getEntity(), name, out, message)) return fail(message, error);
    return true;
}

bool TitleArt::hasParameter(const std::string& name) const{
    return component() && TitleArtSystem::hasParameter(getScene(), getEntity(), name);
}

bool TitleArt::resetParameter(const std::string& name, std::string* error){
    std::string message;
    if (!TitleArtSystem::resetParameter(getScene(), getEntity(), name, message)) return fail(message, error);
    return true;
}

bool TitleArt::isReady() const{
    TitleArtRendererComponent* c = component();
    return c && c->runtime && c->runtime->ready;
}

std::string TitleArt::getPending() const{
    TitleArtRendererComponent* c = component();
    if (!c) return std::string();
    if (!c->runtime || !c->runtime->resolved) return c->renderMode == TitleArtRenderMode::BAKED ? std::string() : std::string("queued");
    return c->runtime->pending;
}

bool TitleArt::isFailed() const{
    TitleArtRendererComponent* c = component();
    return c && c->runtime && c->runtime->failed;
}

std::string TitleArt::getError() const{
    TitleArtRendererComponent* c = component();
    return c && c->runtime ? c->runtime->error : std::string();
}

std::string TitleArt::getPixelPath() const{
    TitleArtRendererComponent* c = component();
    return c && c->runtime ? c->runtime->pixelPath : std::string();
}

int TitleArt::getPriority() const{
    const TitleArtRendererComponent* c = component();
    return c ? c->priority : 0;
}

void TitleArt::setPriority(int priority){
    if (TitleArtRendererComponent* c = component()) c->priority = std::max(-100, std::min(100, priority));
}

std::string TitleArt::getQualityTier() const{
    const TitleArtRendererComponent* c = component();
    return c ? c->qualityTier : std::string("auto");
}

bool TitleArt::setQualityTier(const std::string& tier, std::string* error){
    TitleArtRendererComponent* c = component();
    if (!c) return fail("TitleArt: GameObject '" + getName() + "' has no Title Art Renderer.", error);
    if (tier != "auto" && tier != "high" && tier != "medium" && tier != "low"){
        return fail("TitleArt '" + getName() + "': qualityTier \"" + tier + "\" is not auto, high, medium or low.", error);
    }
    c->qualityTier = tier;
    return true;
}

bool TitleArt::getMeshStats(const std::string& layerPath, titleart::TitleArtMeshStats& out, bool& found, std::string* error) const{
    std::string message;
    if (!TitleArtSystem::meshStats(getScene(), getEntity(), layerPath, out, found, message)) return fail(message, error);
    return true;
}

void TitleArt::refresh(){
    if (component()) TitleArtSystem::refresh(getScene(), getEntity());
}

bool TitleArt::setNativeSize(std::string* error){
    std::string message;
    if (!TitleArtSystem::setNativeSize(getScene(), getEntity(), message)) return fail(message, error);
    return true;
}

// ---- Live Effects: plain values the system reads every frame ----

#define TITLE_ART_FLOAT(Getter, Setter, field, lo, hi, fallback) \
    float TitleArt::Getter() const{ const TitleArtRendererComponent* c = component(); return c ? c->field : fallback; } \
    void TitleArt::Setter(float value){ if (TitleArtRendererComponent* c = component()) c->field = std::max(lo, std::min(hi, finite(value, fallback))); }

TITLE_ART_FLOAT(getLiveTime, setLiveTime, liveTime, 0.0f, 86400.0f, 0.0f)
TITLE_ART_FLOAT(getSweepPosition, setSweepPosition, sweepPosition, 0.0f, 1.0f, 0.0f)
TITLE_ART_FLOAT(getSweepWidth, setSweepWidth, sweepWidth, 0.0f, 4.0f, 0.15f)
TITLE_ART_FLOAT(getSweepAngle, setSweepAngle, sweepAngle, -360.0f, 360.0f, 25.0f)
TITLE_ART_FLOAT(getSweepSoftness, setSweepSoftness, sweepSoftness, 0.0f, 1.0f, 0.6f)
TITLE_ART_FLOAT(getGlowPulse, setGlowPulse, glowPulse, 0.0f, 16.0f, 0.0f)
TITLE_ART_FLOAT(getTintAmount, setTintAmount, tintAmount, 0.0f, 1.0f, 0.0f)
TITLE_ART_FLOAT(getGlintAmount, setGlintAmount, glintAmount, 0.0f, 16.0f, 1.0f)
TITLE_ART_FLOAT(getGlintSize, setGlintSize, glintSize, 0.0f, 1.0f, 0.06f)
TITLE_ART_FLOAT(getGlintRate, setGlintRate, glintRate, 0.0f, 60.0f, 0.5f)

#undef TITLE_ART_FLOAT

#define TITLE_ART_COLOR(Getter, Setter, field, fallback) \
    Vector4 TitleArt::Getter() const{ const TitleArtRendererComponent* c = component(); return c ? c->field : fallback; } \
    void TitleArt::Setter(const Vector4& value){ if (TitleArtRendererComponent* c = component()) c->field = value; }

TITLE_ART_COLOR(getSweepColor, setSweepColor, sweepColor, Vector4(1.0f, 1.0f, 1.0f, 0.8f))
TITLE_ART_COLOR(getGlowColor, setGlowColor, glowColor, Vector4(1.0f, 0.85f, 0.4f, 1.0f))
TITLE_ART_COLOR(getTintColor, setTintColor, tintColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f))
TITLE_ART_COLOR(getGlintColor, setGlintColor, glintColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f))

#undef TITLE_ART_COLOR

bool TitleArt::isSweepAuto() const{
    const TitleArtRendererComponent* c = component();
    return c && c->sweepAuto;
}

void TitleArt::setSweepAuto(bool automatic){
    if (TitleArtRendererComponent* c = component()) c->sweepAuto = automatic;
}

unsigned int TitleArt::getGlintSeed() const{
    const TitleArtRendererComponent* c = component();
    return c ? c->glintSeed : 0u;
}

void TitleArt::setGlintSeed(unsigned int seed){
    if (TitleArtRendererComponent* c = component()) c->glintSeed = seed;
}

// ---- S85 ----

std::vector<std::string> TitleArt::getAnimations() const{
    return component() ? TitleArtSystem::animationNames(getScene(), getEntity()) : std::vector<std::string>();
}

bool TitleArt::isPlaying() const{
    titleanim::TitleAnimPlayer* player = component() ? TitleArtSystem::animationPlayer(getScene(), getEntity()) : nullptr;
    return player && player->isPlaying();
}

bool TitleArt::play(const std::string& name){
    return play(name, titleanim::TitleAnimPlayOptions());
}

bool TitleArt::play(const std::string& name, const titleanim::TitleAnimPlayOptions& options){
    return component() && TitleArtSystem::playAnimation(getScene(), getEntity(), name, options);
}

bool TitleArt::playOverlay(const std::string& name){
    return playOverlay(name, titleanim::TitleAnimPlayOptions());
}

bool TitleArt::playOverlay(const std::string& name, const titleanim::TitleAnimPlayOptions& options){
    return component() && TitleArtSystem::playAnimation(getScene(), getEntity(), name, options, true);
}

void TitleArt::stop(){
    if (component()) TitleArtSystem::stopAnimation(getScene(), getEntity(), "");
}

bool TitleArt::stop(const std::string& name){
    return component() && TitleArtSystem::stopAnimation(getScene(), getEntity(), name);
}

void TitleArt::reset(){
    if (component()) TitleArtSystem::resetAnimation(getScene(), getEntity());
}

bool TitleArt::isPaused() const{
    titleanim::TitleAnimPlayer* player = component() ? TitleArtSystem::animationPlayer(getScene(), getEntity()) : nullptr;
    return player && player->isPaused();
}

void TitleArt::setPaused(bool paused){
    if (titleanim::TitleAnimPlayer* player = component() ? TitleArtSystem::animationPlayer(getScene(), getEntity()) : nullptr) player->setPaused(paused);
}

bool TitleArt::isAnimationPlaying(const std::string& name) const{
    titleanim::TitleAnimPlayer* player = component() ? TitleArtSystem::animationPlayer(getScene(), getEntity()) : nullptr;
    return player && player->isPlaying(name);
}

bool TitleArt::prepare(const std::string& name){
    return component() && TitleArtSystem::prepareAnimation(getScene(), getEntity(), name);
}

bool TitleArt::isAnimationReady(const std::string& name) const{
    return component() && TitleArtSystem::isAnimationReady(getScene(), getEntity(), name);
}

bool TitleArt::sample(const std::string& name, double time){
    return component() && TitleArtSystem::sampleAnimation(getScene(), getEntity(), name, time);
}

double TitleArt::getDuration(const std::string& name) const{
    return component() ? TitleArtSystem::animationDuration(getScene(), getEntity(), name) : -1.0;
}

std::string TitleArt::getAnimation() const{
    titleanim::TitleAnimPlayer* player = component() ? TitleArtSystem::animationPlayer(getScene(), getEntity()) : nullptr;
    return player && player->baseAnimation() ? player->baseName() : std::string();
}

float TitleArt::getAnimationTime() const{
    TitleArtRendererComponent* c = component();
    return c ? c->animationTime : 0.0f;
}

void TitleArt::setAnimationTime(float seconds){
    if (TitleArtRendererComponent* c = component()){
        c->animationTime = std::isfinite(seconds) ? std::max(0.0f, seconds) : 0.0f;
        c->animationTimeKeyed = true;
        if (titleanim::TitleAnimPlayer* player = TitleArtSystem::animationPlayer(getScene(), getEntity())) player->seek(c->animationTime);
    }
}

float TitleArt::getAnimationSpeed() const{
    TitleArtRendererComponent* c = component();
    return c ? c->animationSpeed : 1.0f;
}

void TitleArt::setAnimationSpeed(float speed){
    if (TitleArtRendererComponent* c = component()) c->animationSpeed = std::isfinite(speed) ? std::max(0.0f, std::min(10.0f, speed)) : 1.0f;
}

FunctionSubscribe<void(const AnimationEvent&)>& TitleArt::getOnAnimationEvent(){
    static FunctionSubscribe<void(const AnimationEvent&)> none;
    TitleArtRendererComponent* c = component();
    return c ? TitleArtSystem::instanceOf(*c).onAnimationEvent : none;
}

// ---- events ----

FunctionSubscribe<void()>& TitleArt::getOnReady(){
    static FunctionSubscribe<void()> none;
    TitleArtRendererComponent* c = component();
    return c ? TitleArtSystem::instanceOf(*c).onReady : none;
}

FunctionSubscribe<void(const std::string&)>& TitleArt::getOnFailed(){
    static FunctionSubscribe<void(const std::string&)> none;
    TitleArtRendererComponent* c = component();
    return c ? TitleArtSystem::instanceOf(*c).onFailed : none;
}

FunctionSubscribe<void(const std::string&)>& TitleArt::getOnMissingCharacters(){
    static FunctionSubscribe<void(const std::string&)> none;
    TitleArtRendererComponent* c = component();
    return c ? TitleArtSystem::instanceOf(*c).onMissingCharacters : none;
}

FunctionSubscribe<void(const std::string&)>& TitleArt::getOnAnimationFinished(){
    static FunctionSubscribe<void(const std::string&)> none;
    TitleArtRendererComponent* c = component();
    return c ? TitleArtSystem::instanceOf(*c).onAnimationFinished : none;
}

// ---- languages (S87) ----

std::string TitleArt::getLanguage() const{
    TitleArtRendererComponent* c = component();
    return c ? c->language : std::string();
}

void TitleArt::setLanguage(const std::string& code){
    TitleArtRendererComponent* c = component();
    if (!c) return;
    // a tag the title lacks is no error: it resolves to its best match (getResolvedLanguage)
    c->language = TitleArtLanguage::canonicalize(code);
}

std::string TitleArt::getResolvedLanguage() const{
    TitleArtRendererComponent* c = component();
    return c ? TitleArtSystem::instanceOf(*c).resolvedLanguage : std::string();
}

std::vector<std::string> TitleArt::getLanguages() const{
    TitleArtRendererComponent* c = component();
    return c ? TitleArtSystem::instanceOf(*c).languages : std::vector<std::string>();
}

bool TitleArt::isLanguageReady() const{
    TitleArtRendererComponent* c = component();
    if (!c) return false;
    const titleart::TitleArtInstance& instance = TitleArtSystem::instanceOf(*c);
    // a change made this frame reaches the system at its next update: a script that asks right
    // after setLanguage must not read the old language's "ready"
    if (!instance.languages.empty() && (instance.languageGeneration != TitleArtLanguage::generation() || instance.languageAsked != c->language)){
        const std::string& source = instance.languages.front();
        std::string picked = TitleArtLanguage::resolve(TitleArtLanguage::candidates(c->language, System::instance().getPreferredLocales(), source),
                                                       instance.languages);
        if (picked.empty()) picked = source;
        if (picked != instance.shownLanguage) return false;
    }
    return instance.languageReady;
}

FunctionSubscribe<void(const std::string&)>& TitleArt::getOnLanguageReady(){
    static FunctionSubscribe<void(const std::string&)> none;
    TitleArtRendererComponent* c = component();
    return c ? TitleArtSystem::instanceOf(*c).onLanguageReady : none;
}

bool TitleArt::setGlobalLanguage(const std::string& code, std::string* error){
    getOnLanguageChanged();     // the listener exists before the first change
    return TitleArtLanguage::setGlobal(code, error);
}

std::string TitleArt::getGlobalLanguage(){
    return TitleArtLanguage::global();
}

FunctionSubscribe<void(const std::string&)>& TitleArt::getOnLanguageChanged(){
    static FunctionSubscribe<void(const std::string&)>* event = []{
        auto* e = new FunctionSubscribe<void(const std::string&)>();
        TitleArtLanguage::addListener([e](const std::string& code){ e->call(code); });
        return e;
    }();
    return *event;
}

void TitleArt::setFrameBudget(double ms){
    TitleArtSystem::setFrameBudget(ms);
}

double TitleArt::getFrameBudget(){
    return TitleArtSystem::getFrameBudget();
}
