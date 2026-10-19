// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// UIElement's <video> API (phase 6, p6-video; docs/html-ui/phase-6-video.md §3.9): each method
// forwards to the UIVideoElement behind the handle. An invalid handle logs the phase-1 lifetime
// message once per method; any other element logs an error naming its tag (once per method and
// tag), as UIElementLottie.cpp does. play(), pause(), setLoop(), isPlaying() and getDuration()
// live there and forward to a <video> first.

#include "UIElement.h"
#include "UIElementInternal.h"
#include "HtmlUIScriptInternal.h"
#include "UIVideoElement.h"
#include "vector/UILottieElement.h"

#include "Log.h"

#include <cmath>
#include <limits>
#include <set>
#include <string>

using namespace jasperengine;

namespace {

    UIVideoElement* videoOf(Rml::Element* element, bool& warnedInvalid, std::set<std::string>& warnedTags, const char* method){
        if (!element){
            if (!warnedInvalid){
                warnedInvalid = true;
                Log::warn("[HTML UI] %s (UIElement::%s)", kHtmlUIDeadElementMessage, method);
            }
            return nullptr;
        }
        UIVideoElement* video = rmlui_dynamic_cast<UIVideoElement*>(element);
        if (!video){
            const std::string tag = element->GetTagName();
            if (warnedTags.insert(tag).second)
                Log::error("[HTML UI] UIElement::%s: <%s> is not a <video> element", method, tag.c_str());
        }
        return video;
    }

}

// one pair of "already logged" flags per method, as the lifetime rule asks (once per call site)
#define VIDEO_OF(method) \
    static bool warnedInvalid_##method = false; \
    static std::set<std::string> warnedTags_##method; \
    UIVideoElement* video = videoOf(get(), warnedInvalid_##method, warnedTags_##method, #method)

void UIElement::load(){
    VIDEO_OF(load);
    if (video)
        video->load();
}

std::string UIElement::canPlayType(const std::string& type) const{
    VIDEO_OF(canPlayType);
    return video ? UIVideoElement::canPlayType(type) : std::string();
}

void UIElement::fastSeek(double seconds){
    VIDEO_OF(fastSeek);
    if (video)
        video->fastSeek(seconds);
}

double UIElement::getCurrentTime() const{
    VIDEO_OF(getCurrentTime);
    return video ? video->getCurrentTime() : 0.0;
}

void UIElement::setCurrentTime(double seconds){
    VIDEO_OF(setCurrentTime);
    if (video)
        video->setCurrentTime(seconds);
}

double UIElement::getMediaDuration() const{
    // a <lottie> answers too (its whole duration)
    if (Rml::Element* element = get()){
        if (UILottieElement* lottie = rmlui_dynamic_cast<UILottieElement*>(element))
            return lottie->getDuration();
    }
    VIDEO_OF(getMediaDuration);
    return video ? video->getDuration() : std::numeric_limits<double>::quiet_NaN();
}

bool UIElement::isPaused() const{
    VIDEO_OF(isPaused);
    return video ? video->isPaused() : true;
}

bool UIElement::isEnded() const{
    VIDEO_OF(isEnded);
    return video ? video->isEnded() : false;
}

bool UIElement::isSeeking() const{
    VIDEO_OF(isSeeking);
    return video ? video->isSeeking() : false;
}

bool UIElement::getLoop() const{
    if (Rml::Element* element = get()){
        if (UILottieElement* lottie = rmlui_dynamic_cast<UILottieElement*>(element))
            return lottie->getPlayback().getLoopCount() == 0;
    }
    VIDEO_OF(getLoop);
    return video ? video->getLoop() : false;
}

bool UIElement::getAutoplay() const{
    VIDEO_OF(getAutoplay);
    return video ? video->getAutoplay() : false;
}

void UIElement::setAutoplay(bool autoplay){
    VIDEO_OF(setAutoplay);
    if (video)
        video->setAutoplay(autoplay);
}

bool UIElement::getControls() const{
    VIDEO_OF(getControls);
    return video ? video->getControls() : false;
}

void UIElement::setControls(bool controls){
    VIDEO_OF(setControls);
    if (video)
        video->setControls(controls);
}

bool UIElement::isMuted() const{
    VIDEO_OF(isMuted);
    return video ? video->isMuted() : false;
}

void UIElement::setMuted(bool muted){
    VIDEO_OF(setMuted);
    if (video)
        video->setMuted(muted);
}

bool UIElement::getDefaultMuted() const{
    VIDEO_OF(getDefaultMuted);
    return video ? video->getDefaultMuted() : false;
}

void UIElement::setDefaultMuted(bool muted){
    VIDEO_OF(setDefaultMuted);
    if (video)
        video->setDefaultMuted(muted);
}

float UIElement::getVolume() const{
    VIDEO_OF(getVolume);
    return video ? video->getVolume() : 0.0f;
}

void UIElement::setVolume(float volume){
    VIDEO_OF(setVolume);
    if (video)
        video->setVolume(volume);
}

double UIElement::getPlaybackRate() const{
    VIDEO_OF(getPlaybackRate);
    return video ? video->getPlaybackRate() : 1.0;
}

void UIElement::setPlaybackRate(double rate){
    VIDEO_OF(setPlaybackRate);
    if (video)
        video->setPlaybackRate(rate);
}

double UIElement::getDefaultPlaybackRate() const{
    VIDEO_OF(getDefaultPlaybackRate);
    return video ? video->getDefaultPlaybackRate() : 1.0;
}

void UIElement::setDefaultPlaybackRate(double rate){
    VIDEO_OF(setDefaultPlaybackRate);
    if (video)
        video->setDefaultPlaybackRate(rate);
}

bool UIElement::getPreservesPitch() const{
    VIDEO_OF(getPreservesPitch);
    (void)video;
    return false;
}

void UIElement::setPreservesPitch(bool preserve){
    VIDEO_OF(setPreservesPitch);
    if (video && preserve)
        Log::warn("[HTML UI] <video>: preservesPitch = true is not supported: the pitch follows playbackRate");
}

int UIElement::getReadyState() const{
    VIDEO_OF(getReadyState);
    return video ? video->getReadyState() : HAVE_NOTHING;
}

int UIElement::getNetworkState() const{
    VIDEO_OF(getNetworkState);
    return video ? video->getNetworkState() : NETWORK_EMPTY;
}

int UIElement::getVideoWidth() const{
    VIDEO_OF(getVideoWidth);
    return video ? video->getVideoWidth() : 0;
}

int UIElement::getVideoHeight() const{
    VIDEO_OF(getVideoHeight);
    return video ? video->getVideoHeight() : 0;
}

std::string UIElement::getCurrentSrc() const{
    VIDEO_OF(getCurrentSrc);
    return video ? video->getCurrentSrc() : std::string();
}

std::string UIElement::getPoster() const{
    VIDEO_OF(getPoster);
    return video ? video->getPoster() : std::string();
}

void UIElement::setPoster(const std::string& poster){
    VIDEO_OF(setPoster);
    if (video)
        video->setPoster(poster);
}

std::string UIElement::getPreload() const{
    VIDEO_OF(getPreload);
    return video ? video->getPreload() : std::string();
}

void UIElement::setPreload(const std::string& preload){
    VIDEO_OF(setPreload);
    if (video)
        video->setPreload(preload);
}

std::optional<UIMediaError> UIElement::getMediaError() const{
    VIDEO_OF(getMediaError);
    if (!video || !video->getError())
        return std::nullopt;
    UIMediaError error;
    error.code = (int)video->getError().code;
    error.message = video->getError().message;
    return error;
}
