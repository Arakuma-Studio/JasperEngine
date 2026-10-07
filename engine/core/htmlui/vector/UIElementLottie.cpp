// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// UIElement's <lottie> control (phase 1, p1-vector): each method forwards to the
// UILottieElement behind the handle. An invalid handle logs the phase-1 lifetime message once
// per method; any other element logs an error naming its tag (once per method and tag).

#include "UIElement.h"
#include "UIElementInternal.h"
#include "UILottieElement.h"

#include "Log.h"

#include <set>
#include <string>

using namespace jasperengine;

namespace {

    UILottieElement* lottieOf(Rml::Element* element, bool& warnedInvalid, std::set<std::string>& warnedTags, const char* method){
        if (!element){
            if (!warnedInvalid){
                warnedInvalid = true;
                Log::error("UIElement: element no longer exists (its document was unloaded or it was removed)");
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

void UIElement::play(){
    LOTTIE_OF(play);
    if (lottie)
        lottie->play();
}

void UIElement::pause(){
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
    LOTTIE_OF(getDuration);
    return lottie ? lottie->getDuration() : 0.0f;
}
