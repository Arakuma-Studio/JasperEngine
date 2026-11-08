// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleAnimPlayer.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::titleanim;

namespace {

    float srgbToLinear(double c){
        const double v = std::max(0.0, c);
        return (float)(v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4));
    }

    double lerp(double a, double b, double w){
        return a + (b - a) * w;
    }

    // the fade (R26): every value from the snapshot toward the new pose
    void blendSummary(const TitlePoseSummary& from, TitlePoseSummary& to, double w, const double* liveRest){
        for (int i = 0; i < 2; i++){
            to.position[i] = lerp(from.position[i], to.position[i], w);
            to.stretch[i] = lerp(from.stretch[i], to.stretch[i], w);
            to.pivot[i] = lerp(from.pivot[i], to.pivot[i], w);
        }
        to.rotation = lerp(from.rotation, to.rotation, w);
        to.scale = lerp(from.scale, to.scale, w);
        to.opacity = lerp(from.opacity, to.opacity, w);
        for (int i = 0; i < 3; i++){
            to.flashColor[i] = lerp(from.flashColor[i], to.flashColor[i], w);
        }
        to.flashAmount = lerp(from.flashAmount, to.flashAmount, w);
        to.transformed = to.transformed || from.transformed;
        for (size_t f = 0; f < (size_t)TitleField::Count; f++){
            if (!from.liveSet[f] && !to.liveSet[f]) continue;
            const double rest = liveRest ? liveRest[f] : (double)titleFieldInfo((TitleField)f).rest[0];
            const double a = from.liveSet[f] ? from.live[f] : rest;
            const double b = to.liveSet[f] ? to.live[f] : rest;
            to.live[f] = lerp(a, b, w);
            to.liveSet[f] = true;
        }
        const size_t tint = (size_t)TitleField::TintColor;
        if (from.liveSet[tint] || to.liveSet[tint]){
            for (int i = 0; i < 3; i++) to.tintColor[i] = lerp(from.tintColor[i], to.tintColor[i], w);
        }
    }

}

// ==================================================================== library

const CompiledTitleAnimation* TitleAnimLibrary::find(const std::string& name) const{
    for (const CompiledTitleAnimation& a : animations) if (a.name == name) return &a;
    return nullptr;
}

std::vector<std::string> TitleAnimLibrary::names() const{
    std::vector<std::string> out;
    for (const CompiledTitleAnimation& a : animations) out.push_back(a.name);
    return out;
}

double PlayingAnimation::localTime() const{
    if (!animation) return 0.0;
    return wrapAnimationTime(time, animation->duration, animation->loopStart, wrap);
}

// ==================================================================== player

TitleAnimPlayer::TitleAnimPlayer(){
    crossed.reserve(64);
}

void TitleAnimPlayer::setLibrary(std::shared_ptr<const TitleAnimLibrary> library){
    // what plays carries on by name in the new library (a hot reload), at the same time
    auto rebind = [&](PlayingAnimation& p) -> bool{
        if (!p.animation) return true;
        const CompiledTitleAnimation* again = library ? library->find(p.name) : nullptr;
        if (!again) return false;
        p.animation = again;
        p.pose.size(*again);
        samplePose(p);
        return true;
    };
    std::shared_ptr<const TitleAnimLibrary> previous = std::move(lib);
    lib = std::move(library);
    if (!rebind(base)) base = PlayingAnimation();
    if (!rebind(fadeFrom)){
        fadeFrom = PlayingAnimation();
        fadeDuration = 0.0f;
    }
    overlayList.erase(std::remove_if(overlayList.begin(), overlayList.end(), [&](PlayingAnimation& o){ return !rebind(o); }), overlayList.end());
}

const CompiledTitleAnimation* TitleAnimPlayer::find(const std::string& name) const{
    return lib ? lib->find(name) : nullptr;
}

bool TitleAnimPlayer::start(PlayingAnimation& slot, const CompiledTitleAnimation& animation, const TitleAnimPlayOptions& options){
    slot.animation = &animation;
    slot.name = animation.name;
    slot.time = std::isfinite(options.time) ? std::max(0.0, options.time) : 0.0;
    slot.speed = std::isfinite(options.speed) ? std::max(0.0f, options.speed) : 1.0f;
    slot.wrap = options.loop < 0 ? animation.wrap : (options.loop > 0 ? (animation.wrap == AnimWrap::PingPong ? AnimWrap::PingPong : AnimWrap::Loop)
                                                                        : AnimWrap::Once);
    slot.started = false;
    slot.finished = false;
    slot.held = false;
    slot.serial = ++serials;
    slot.pose.size(animation);
    samplePose(slot);
    return true;
}

bool TitleAnimPlayer::play(const std::string& name, const TitleAnimPlayOptions& options){
    const CompiledTitleAnimation* animation = find(name);
    if (!animation) return false;
    if (options.fade > 0.0f && base.animation){
        fadeFrom = base;
        fadeFrom.held = true;
        fadeDuration = options.fade;
        fadeElapsed = 0.0f;
    }else{
        fadeFrom = PlayingAnimation();
        fadeDuration = 0.0f;
    }
    stopped = false;
    pausedFlag = false;
    driven = false;
    start(base, *animation, options);
    return true;
}

bool TitleAnimPlayer::playOverlay(const std::string& name, const TitleAnimPlayOptions& options){
    const CompiledTitleAnimation* animation = find(name);
    if (!animation) return false;
    // the same overlay again restarts it (and it becomes the latest)
    overlayList.erase(std::remove_if(overlayList.begin(), overlayList.end(), [&](const PlayingAnimation& o){ return o.name == name; }),
                      overlayList.end());
    overlayList.emplace_back();
    start(overlayList.back(), *animation, options);
    stopped = false;
    return true;
}

void TitleAnimPlayer::stop(){
    stopped = true;
    base.held = true;
    for (PlayingAnimation& o : overlayList) o.held = true;
    fadeFrom = PlayingAnimation();
    fadeDuration = 0.0f;
}

bool TitleAnimPlayer::stop(const std::string& name){
    if (base.animation && base.name == name){
        base.held = true;
        return true;
    }
    const size_t before = overlayList.size();
    overlayList.erase(std::remove_if(overlayList.begin(), overlayList.end(), [&](const PlayingAnimation& o){ return o.name == name; }),
                      overlayList.end());
    return overlayList.size() != before;
}

void TitleAnimPlayer::reset(){
    base = PlayingAnimation();
    overlayList.clear();
    fadeFrom = PlayingAnimation();
    fadeDuration = 0.0f;
    stopped = false;
    pausedFlag = false;
    driven = false;
}

void TitleAnimPlayer::setPaused(bool paused){
    pausedFlag = paused;
}

bool TitleAnimPlayer::sample(const std::string& name, double time){
    const CompiledTitleAnimation* animation = find(name);
    if (!animation) return false;
    if (!base.animation || base.name != name){
        TitleAnimPlayOptions options;
        start(base, *animation, options);
    }
    fadeFrom = PlayingAnimation();
    fadeDuration = 0.0f;
    driven = false;
    base.time = std::isfinite(time) ? std::max(0.0, time) : 0.0;
    base.started = true;
    base.finished = false;
    pausedFlag = true;
    samplePose(base);
    return true;
}

void TitleAnimPlayer::seek(double time){
    if (!base.animation) return;
    base.time = std::isfinite(time) ? std::max(0.0, time) : 0.0;
    base.started = true;
    if (base.wrap != AnimWrap::Once || base.time < base.animation->duration) base.finished = false;
    samplePose(base);
}

bool TitleAnimPlayer::drive(const std::string& name, double time, std::vector<FiredMarker>* markers){
    const CompiledTitleAnimation* animation = find(name);
    if (!animation) return false;
    const double t = std::isfinite(time) ? std::max(0.0, time) : 0.0;
    if (!driven || !base.animation || base.name != name){
        TitleAnimPlayOptions options;
        start(base, *animation, options);
        base.started = true;
        driven = true;
        drivenTime = t;
        stopped = false;
        pausedFlag = false;
        fadeFrom = PlayingAnimation();
        fadeDuration = 0.0f;
    }else if (t > drivenTime && markers){
        // R27: forward between frames only, never backward
        emitMarkers(base, drivenTime, t, false, 1.0, 0.0, 0.0, *markers);
    }
    drivenTime = t;
    base.time = t;
    base.finished = false;
    samplePose(base);
    return true;
}

void TitleAnimPlayer::samplePose(PlayingAnimation& p){
    if (!p.animation) return;
    if (p.pose.values.size() != p.animation->poseSize) p.pose.size(*p.animation);
    sampleAnimation(*p.animation, p.localTime(), p.pose);
}

void TitleAnimPlayer::emitMarkers(const PlayingAnimation& p, double from, double to, bool includeStart, double rate, double frameDt,
                                  double frameOffset, std::vector<FiredMarker>& markers){
    crossed.clear();
    collectCrossedMarkers(*p.animation, p.wrap, from, to, includeStart, crossed);
    for (const CrossedMarker& c : crossed){
        FiredMarker m;
        m.marker = &p.animation->markers[c.index];
        m.animation = p.animation;
        // the crossing's place in the frame: seconds of the frame / the frame's seconds
        const double seconds = frameOffset + (rate > 0.0 ? (c.position - from) / rate : 0.0);
        m.fraction = frameDt > 0.0 ? (float)std::min(1.0, std::max(0.0, seconds / frameDt)) : 0.0f;
        markers.push_back(m);
    }
}

bool TitleAnimPlayer::advance(PlayingAnimation& p, double dt, double frameDt, double frameOffset, std::vector<FiredMarker>& markers,
                              double* overflow, double* spent){
    const double rate = (double)p.speed * (double)p.animation->speed * (double)speedMultiplier;
    const bool includeStart = !p.started;
    // the first update after a start shows the start and fires what sits there
    const double step = p.started ? std::max(0.0, dt) * rate : 0.0;
    p.started = true;
    const double from = p.time;
    double to = from + step;
    const double duration = p.animation->duration;
    bool ended = false;
    if (p.wrap == AnimWrap::Once && to >= duration && step >= 0.0 && (from < duration || includeStart)){
        ended = true;
        if (overflow) *overflow = to - duration;
        if (spent) *spent = rate > 0.0 ? (duration - from) / rate : 0.0;
        to = std::max(from, duration);
    }
    emitMarkers(p, from, to, includeStart, rate, frameDt, frameOffset, markers);
    p.time = to;
    return ended;
}

void TitleAnimPlayer::update(double dt, std::vector<FiredMarker>& markers, std::vector<std::string>& finished){
    if (!lib) return;
    const size_t firstMarker = markers.size();
    const double frameDt = std::isfinite(dt) ? std::max(0.0, dt) : 0.0;
    const bool advancing = !pausedFlag && !stopped;
    if (base.animation && advancing && !base.held && !base.finished && !driven){
        double remaining = frameDt, offset = 0.0;
        // a chain of `next` within one frame (bounded: a cycle of short animations)
        for (int chain = 0; chain < 8; chain++){
            double overflow = 0.0, spent = 0.0;
            if (!advance(base, remaining, frameDt, offset, markers, &overflow, &spent)) break;
            base.finished = true;
            finished.push_back(base.name);
            offset += spent;
            remaining = std::max(0.0, remaining - spent);
            const CompiledTitleAnimation* next = base.animation->next.empty() ? nullptr : find(base.animation->next);
            if (!next) break;
            // R26: the next one starts in the same frame, at the overflow time
            TitleAnimPlayOptions options;
            options.speed = base.speed;
            start(base, *next, options);
            base.started = true;
            emitMarkers(base, 0.0, 0.0, true, 1.0, frameDt, offset, markers);
            if (remaining <= 0.0) break;
        }
    }
    for (size_t i = 0; i < overlayList.size();){
        PlayingAnimation& o = overlayList[i];
        if (advancing && !o.held && !o.finished){
            if (advance(o, frameDt, frameDt, 0.0, markers, nullptr, nullptr)){
                o.finished = true;
                finished.push_back(o.name);
            }
        }
        // a once overlay ends on its own (its last pose was relative to the base: R25)
        if (o.finished){
            overlayList.erase(overlayList.begin() + (std::ptrdiff_t)i);
            continue;
        }
        samplePose(o);
        i++;
    }
    if (fadeDuration > 0.0f && advancing){
        fadeElapsed += (float)frameDt;
        if (fadeElapsed >= fadeDuration){
            fadeDuration = 0.0f;
            fadeFrom = PlayingAnimation();
        }
    }
    if (base.animation) samplePose(base);
    // R28: base and overlays in time order
    // (an insertion sort: a few markers, and no buffer to allocate)
    for (size_t a = firstMarker + 1; a < markers.size(); a++){
        const FiredMarker v = markers[a];
        size_t b = a;
        while (b > firstMarker && markers[b - 1].fraction > v.fraction){
            markers[b] = markers[b - 1];
            b--;
        }
        markers[b] = v;
    }
}

bool TitleAnimPlayer::isPlaying() const{
    if (base.animation && !base.held && !base.finished) return true;
    for (const PlayingAnimation& o : overlayList) if (!o.held && !o.finished) return true;
    return false;
}

bool TitleAnimPlayer::isPlaying(const std::string& name) const{
    if (base.animation && base.name == name && !base.held && !base.finished) return true;
    for (const PlayingAnimation& o : overlayList) if (o.name == name && !o.held && !o.finished) return true;
    return false;
}

double TitleAnimPlayer::duration(const std::string& name) const{
    const CompiledTitleAnimation* a = find(name);
    return a ? a->duration : -1.0;
}

const PlayingAnimation* TitleAnimPlayer::fadingFrom(float* weight) const{
    if (fadeDuration <= 0.0f || !fadeFrom.animation) return nullptr;
    if (weight) *weight = std::min(1.0f, fadeElapsed / fadeDuration);
    return &fadeFrom;
}

void TitleAnimPlayer::summary(const double* liveRest, TitlePoseSummary& out) const{
    out = TitlePoseSummary();
    if (base.animation) accumulateTitlePose(*base.animation, base.pose, false, liveRest, out);
    float w = 1.0f;
    if (const PlayingAnimation* from = fadingFrom(&w)){
        TitlePoseSummary snapshot;
        accumulateTitlePose(*from->animation, from->pose, false, liveRest, snapshot);
        blendSummary(snapshot, out, w, liveRest);
    }
    for (const PlayingAnimation& o : overlayList) accumulateTitlePose(*o.animation, o.pose, true, liveRest, out);
}

int TitleAnimPlayer::flipbookFrame(int* nextFrame, float* blend) const{
    if (nextFrame) *nextFrame = -1;
    if (blend) *blend = 0.0f;
    if (!base.animation || base.animation->flipbook.empty()) return -1;
    const CompiledFlipbook& fb = base.animation->flipbook;
    const int n = (int)fb.frames.size();
    const double f = base.localTime() * (double)fb.fps;
    int k = (int)std::floor(f + 1e-6);
    k = std::max(0, std::min(n - 1, k));
    int next = k + 1;
    if (next >= n){
        next = base.wrap == AnimWrap::Loop ? std::max(0, std::min(n - 1, (int)std::floor(base.animation->loopStart * fb.fps + 1e-6))) : k;
    }
    if (nextFrame) *nextFrame = next;
    if (blend && fb.blend && next != k) *blend = (float)std::min(1.0, std::max(0.0, f - std::floor(f + 1e-6)));
    return k;
}

// ==================================================================== the companion's draw

void titleanim::companionUniforms(const TitlePoseSummary& s, const TitleFrameGeometry& still, TrackPivot pivot, const CompiledFlipbook* flipbook,
                                  int frame, int next, float blend, CompanionAnimUniforms& out){
    out = CompanionAnimUniforms();
    FrameAffine motion = titleFrameAffine(s, still, pivot);
    FrameAffine placement;
    const bool frames = flipbook && !flipbook->empty() && frame >= 0 && frame < (int)flipbook->frames.size();
    if (frames) placement = flipbookFrameAffine(still, flipbook->frameX, flipbook->frameY, flipbook->frameWidth, flipbook->frameHeight);
    const FrameAffine v = placement.then(motion);
    out.vertex0[0] = (float)v.a; out.vertex0[1] = (float)v.b; out.vertex0[2] = (float)v.c;
    out.vertex1[0] = (float)v.d; out.vertex1[1] = (float)v.e; out.vertex1[2] = (float)v.f;
    out.affine0[3] = (float)std::min(1.0, std::max(0.0, s.opacity));
    out.color[0] = srgbToLinear(s.flashColor[0]);
    out.color[1] = srgbToLinear(s.flashColor[1]);
    out.color[2] = srgbToLinear(s.flashColor[2]);
    out.color[3] = (float)s.flashAmount;
    if (frames){
        auto rect = [&](int index, float* r){
            const FlipbookFrame& f = flipbook->frames[(size_t)index];
            const FlipbookPage& page = flipbook->pages[(size_t)f.page];
            const float pw = (float)std::max(1, page.width), ph = (float)std::max(1, page.height);
            r[0] = (float)f.x / pw;
            r[1] = (float)f.y / ph;
            r[2] = (float)flipbook->pixelWidth / pw;
            r[3] = (float)flipbook->pixelHeight / ph;
        };
        rect(frame, out.frameRect);
        // a blend needs both frames on the bound page
        const bool blendable = next >= 0 && next < (int)flipbook->frames.size() && flipbook->frames[(size_t)next].page == flipbook->frames[(size_t)frame].page;
        if (blendable){
            rect(next, out.frameRect2);
            out.affine1[3] = blend;
        }else{
            for (int i = 0; i < 4; i++) out.frameRect2[i] = out.frameRect[i];
            out.affine1[3] = 0.0f;
        }
    }
}
