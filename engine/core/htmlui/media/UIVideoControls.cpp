// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "UIVideoControls.h"

#include "UIVideoElement.h"

#include "HtmlUIClock.h"
#include "subsystem/UIDocumentSystem.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementUtilities.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/XMLParser.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace jasperengine;

UIVideoControls::UIVideoControls(UIVideoElement& owner): video(owner){
    bar = make(&video, "videocontrols", "htmlui-video-controls", false);
    if (!bar)
        return;
    playButton = make(bar, "videobutton", "htmlui-video-play", true);
    timeText = make(bar, "videotime", "htmlui-video-time", true);
    timeline = make(bar, "videotimeline", "htmlui-video-timeline", true);
    track = timeline ? make(timeline, "videotrack", "htmlui-video-track", true) : nullptr;
    progress = track ? make(track, "videoprogress", "htmlui-video-progress", true) : nullptr;
    thumb = timeline ? make(timeline, "videothumb", "htmlui-video-thumb", true) : nullptr;
    muteButton = make(bar, "videobutton", "htmlui-video-mute", true);
    update();
}

UIVideoControls::~UIVideoControls(){
    // the attribute went: the bar goes
    if (bar && bar->GetParentNode() == &video)
        video.RemoveChild(bar);
}

void UIVideoControls::forget(){
    bar = playButton = timeText = timeline = track = progress = thumb = muteButton = nullptr;
}

Rml::Element* UIVideoControls::make(Rml::Element* parent, const char* tag, const char* className, bool dom){
    Rml::ElementPtr element = Rml::Factory::InstanceElement(parent, tag, tag, Rml::XMLAttributes());
    if (!element)
        return nullptr;
    element->SetClass(className, true);
    return parent->AppendChild(std::move(element), dom);
}

std::string UIVideoControls::formatTime(double seconds, bool hours){
    if (!std::isfinite(seconds) || seconds < 0.0)
        seconds = 0.0;
    const long total = (long)std::floor(seconds);
    char buffer[32];
    if (hours)
        std::snprintf(buffer, sizeof(buffer), "%ld:%02ld:%02ld", total / 3600, (total / 60) % 60, total % 60);
    else
        std::snprintf(buffer, sizeof(buffer), "%ld:%02ld", total / 60, total % 60);
    return buffer;
}

void UIVideoControls::update(){
    if (!bar)
        return;
    const bool paused = video.playback.isPaused();
    const double now = HtmlUIClock::now();
    shown = paused || scrubbing || video.IsPseudoClassSet("hover") || video.IsPseudoClassSet("focus-visible") ||
            now - lastPointerMove < HIDE_AFTER;
    bar->SetClass("htmlui-playing", !paused);
    bar->SetClass("htmlui-muted", video.isMuted() || video.getVolume() == 0.0f);
    bar->SetClass("htmlui-shown", shown);

    const double duration = video.playback.getDuration();
    const double current = video.playback.getPosition();
    const bool hours = std::isfinite(duration) && duration >= 3600.0;
    const std::string text = formatTime(current, hours) + " / " + formatTime(std::isfinite(duration) ? duration : 0.0, hours);
    if (text != shownTime && timeText){
        shownTime = text;
        timeText->SetInnerRML(text);
    }
    const float fraction = (std::isfinite(duration) && duration > 0.0) ? (float)std::clamp(current / duration, 0.0, 1.0) : 0.0f;
    if (fraction != shownFraction)
        placeProgress(fraction);
    // the bar hides on the UI clock while the video plays: look again then
    if (shown && !paused){
        if (Rml::Context* context = video.GetContext())
            context->RequestNextUpdate(std::max(0.0, HIDE_AFTER - (now - lastPointerMove)));
    }
}

void UIVideoControls::placeProgress(float fraction){
    shownFraction = fraction;
    // transforms, not sizes: the progress moves every frame without a layout
    char buffer[64];
    if (progress){
        std::snprintf(buffer, sizeof(buffer), "scaleX(%.5f)", fraction);
        progress->SetProperty("transform", buffer);
    }
    if (thumb && timeline){
        const float width = timeline->GetBox().GetSize(Rml::BoxArea::Content).x;
        std::snprintf(buffer, sizeof(buffer), "translateX(%.3fpx)", fraction * width);
        thumb->SetProperty("transform", buffer);
    }
}

void UIVideoControls::layout(){
    if (!bar)
        return;
    // a replaced element's hidden children are its own to lay out (as RmlUi's form widgets do):
    // the bar spans the content box at its bottom
    const Rml::Vector2f content = video.GetBox().GetSize(Rml::BoxArea::Content);
    Rml::ElementUtilities::FormatElement(bar, content);
    const float height = bar->GetBox().GetSize(Rml::BoxArea::Border).y;
    bar->SetOffset(video.GetBox().GetPosition(Rml::BoxArea::Content) + Rml::Vector2f(0.0f, content.y - height), &video);
    placeProgress(shownFraction < 0.0f ? 0.0f : shownFraction);
}

bool UIVideoControls::inside(Rml::Element* element, Rml::Element* container) const{
    for (Rml::Element* walk = element; walk; walk = walk->GetParentNode()){
        if (walk == container)
            return true;
        if (walk == &video)
            return false;
    }
    return false;
}

void UIVideoControls::togglePlayback(){
    if (video.isPaused() || video.isEnded())
        video.play();
    else
        video.pause();
}

void UIVideoControls::seekBy(double seconds){
    const double duration = video.getDuration();
    double target = video.getCurrentTime() + seconds;
    if (std::isfinite(duration))
        target = std::min(target, duration);
    video.setCurrentTime(std::max(target, 0.0));
}

void UIVideoControls::seekToPointer(float x){
    if (!timeline)
        return;
    const float left = timeline->GetAbsoluteOffset(Rml::BoxArea::Border).x;
    const float width = timeline->GetBox().GetSize(Rml::BoxArea::Border).x;
    const double duration = video.getDuration();
    if (width <= 0.0f || !std::isfinite(duration))
        return;
    const double fraction = std::clamp((double)(x - left) / (double)width, 0.0, 1.0);
    video.setCurrentTime(fraction * duration);
}

bool UIVideoControls::processDefaultAction(Rml::Event& event){
    if (!bar)
        return false;
    Rml::Element* target = event.GetTargetElement();
    const double now = HtmlUIClock::now();
    switch (event.GetId()){
        case Rml::EventId::Mousemove:
            lastPointerMove = now;
            return false;
        case Rml::EventId::Mousedown:
            lastPointerMove = now;
            if (event.GetParameter<int>("button", 0) == 0 && inside(target, timeline)){
                scrubbing = true;
                seekToPointer(event.GetParameter<float>("mouse_x", 0.0f));
                event.StopPropagation();
                return true;
            }
            return false;
        case Rml::EventId::Drag:
            if (scrubbing){
                seekToPointer(event.GetParameter<float>("mouse_x", 0.0f));
                return true;
            }
            return false;
        case Rml::EventId::Dragend:
        case Rml::EventId::Mouseup:
            scrubbing = false;
            return false;
        case Rml::EventId::Click:
            if (inside(target, playButton)){
                togglePlayback();
            }else if (inside(target, muteButton)){
                video.setMuted(!video.isMuted());
            }else if (inside(target, bar)){
                return false;   // the bar's background and the time do nothing
            }else if (target == &video){
                // a click on the picture toggles playback (Chromium)
                togglePlayback();
            }else{
                return false;
            }
            return true;
        case Rml::EventId::Keydown: {
            if (target != &video)
                return false;
            lastPointerMove = now;
            const int key = event.GetParameter<int>("key_identifier", 0);
            const int gamepad = event.GetParameter<int>("gamepad_key", -1);
            using K = Rml::Input::KeyIdentifier;
            bool used = true;
            if (key == K::KI_SPACE || key == K::KI_RETURN || key == K::KI_K){
                togglePlayback();
            }else if (key == K::KI_LEFT || gamepad == (int)GamepadUIKey::DPAD_LEFT || gamepad == (int)GamepadUIKey::LEFT_STICK_LEFT){
                seekBy(-KEY_SEEK);
            }else if (key == K::KI_RIGHT || gamepad == (int)GamepadUIKey::DPAD_RIGHT || gamepad == (int)GamepadUIKey::LEFT_STICK_RIGHT){
                seekBy(KEY_SEEK);
            }else if (key == K::KI_J){
                seekBy(-KEY_SEEK_LONG);
            }else if (key == K::KI_L){
                seekBy(KEY_SEEK_LONG);
            }else if (key == K::KI_HOME){
                video.setCurrentTime(0.0);
            }else if (key == K::KI_END){
                const double duration = video.getDuration();
                if (std::isfinite(duration))
                    video.setCurrentTime(duration);
            }else if (key == K::KI_M){
                video.setMuted(!video.isMuted());
            }else{
                used = false;
            }
            if (used)
                event.StopPropagation();
            return used;
        }
        default:
            return false;
    }
}

UIVideoControls::Rects UIVideoControls::getRects() const{
    Rects rects;
    rects.visible = shown;
    auto box = [](Rml::Element* element){
        if (!element)
            return Rml::Rectanglef::FromPositionSize(Rml::Vector2f(0.0f), Rml::Vector2f(0.0f));
        return Rml::Rectanglef::FromPositionSize(element->GetAbsoluteOffset(Rml::BoxArea::Border), element->GetBox().GetSize(Rml::BoxArea::Border));
    };
    rects.bar = box(bar);
    rects.play = box(playButton);
    rects.time = box(timeText);
    rects.timeline = box(timeline);
    rects.mute = box(muteButton);
    return rects;
}

std::string UIVideoControls::getTimeText() const{
    return shownTime;
}

bool UIVideoControls::isShown() const{
    return shown;
}
