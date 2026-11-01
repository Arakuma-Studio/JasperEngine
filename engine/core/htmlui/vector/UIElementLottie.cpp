// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// UIElement's <lottie> control (phase 1, p1-vector): each method forwards to the
// UILottieElement behind the handle. An invalid handle logs the phase-1 lifetime message once
// per method; any other element logs an error naming its tag (once per method and tag).
// play(), pause(), setLoop(), isPlaying() and getDuration() serve a <video> too (phase 6:
// htmlui/media/UIElementVideo.cpp has the rest of its API).

#include "UIElement.h"
#include "UIElementInternal.h"
#include "HtmlUIScriptInternal.h"
#include "UILottieElement.h"
#if JASPERENGINE_VIDEO
#include "media/UIVideoElement.h"
#endif

#include "Log.h"

#include <set>
#include <string>

using namespace jasperengine;

namespace {

    UILottieElement* lottieOf(Rml::Element* element, bool& warnedInvalid, std::set<std::string>& warnedTags, const char* method){
        if (!element){
            if (!warnedInvalid){
                warnedInvalid = true;
                Log::warn("[HTML UI] %s (UIElement::%s)", kHtmlUIDeadElementMessage, method);
            }
            return nullptr;
        }
        UILottieElement* lottie = rmlui_dynamic_cast<UILottieElement*>(element);
        if (!lottie){
            const std::string tag = element->GetTagName();
            if (warnedTags.insert(tag).second){
                Log::error("[HTML UI] UIElement::%s: <%s> is not a <lottie> element", method, tag.c_str());
            }
        }
        return lottie;
    }

}

// one pair of "already logged" flags per method, as the lifetime rule asks (once per call site)
#define LOTTIE_OF(method) \
    static bool warnedInvalid_##method = false; \
    static std::set<std::string> warnedTags_##method; \
    UILottieElement* lottie = lottieOf(get(), warnedInvalid_##method, warnedTags_##method, #method)

// a build without the video module (docs/specs/S34-player-size.md R1) has no <video> elements
#if JASPERENGINE_VIDEO
namespace {

    UIVideoElement* asVideo(Rml::Element* element){
        return element ? rmlui_dynamic_cast<UIVideoElement*>(element) : nullptr;
    }

}
#endif

void UIElement::play(){
#if JASPERENGINE_VIDEO
    if (UIVideoElement* video = asVideo(get())){
        video->play();
        return;
    }
#endif
    LOTTIE_OF(play);
    if (lottie)
        lottie->play();
}

void UIElement::pause(){
#if JASPERENGINE_VIDEO
    if (UIVideoElement* video = asVideo(get())){
        video->pause();
        return;
    }
#endif
    LOTTIE_OF(pause);
    if (lottie)
        lottie->pause();
}

void UIElement::stop(){
    LOTTIE_OF(stop);
    if (lottie)
        lottie->stop();
}

void UIElement::seek(float seconds){
    LOTTIE_OF(seek);
    if (lottie)
        lottie->seek(seconds);
}

void UIElement::setFrame(float frame){
    LOTTIE_OF(setFrame);
    if (lottie)
        lottie->setFrame(frame);
}

void UIElement::setSpeed(float speed){
    LOTTIE_OF(setSpeed);
    if (lottie)
        lottie->setSpeed(speed);
}

void UIElement::setDirection(int direction){
    LOTTIE_OF(setDirection);
    if (lottie)
        lottie->setDirection(direction);
}

void UIElement::setLoop(bool loop){
#if JASPERENGINE_VIDEO
    if (UIVideoElement* video = asVideo(get())){
        video->setLoop(loop);
        return;
    }
#endif
    LOTTIE_OF(setLoop);
    if (lottie)
        lottie->setLoop(loop);
}

void UIElement::setLoopCount(int count){
    LOTTIE_OF(setLoopCount);
    if (lottie)
        lottie->setLoopCount(count);
}

void UIElement::setSegment(float startFrame, float endFrame){
    LOTTIE_OF(setSegment);
    if (lottie)
        lottie->setSegment(startFrame, endFrame);
}

void UIElement::setMarker(const std::string& marker){
    LOTTIE_OF(setMarker);
    if (lottie)
        lottie->setMarker(marker);
}

bool UIElement::isPlaying() const{
#if JASPERENGINE_VIDEO
    if (UIVideoElement* video = asVideo(get()))
        return !video->isPaused();
#endif
    LOTTIE_OF(isPlaying);
    return lottie ? lottie->isPlaying() : false;
}

float UIElement::getCurrentFrame() const{
    LOTTIE_OF(getCurrentFrame);
    return lottie ? lottie->getCurrentFrame() : 0.0f;
}

float UIElement::getTotalFrames() const{
    LOTTIE_OF(getTotalFrames);
    return lottie ? lottie->getTotalFrames() : 0.0f;
}

float UIElement::getDuration() const{
#if JASPERENGINE_VIDEO
    if (UIVideoElement* video = asVideo(get()))
        return (float)video->getDuration();
#endif
    LOTTIE_OF(getDuration);
    return lottie ? lottie->getDuration() : 0.0f;
}
