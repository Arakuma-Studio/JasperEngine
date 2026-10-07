// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LottiePlayback.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace {

    // A clock jump (a long hitch, a fixed time set far ahead) may wrap a short animation
    // thousands of times in one update; the iterations are all counted, the events capped
    const int maxLoopEventsPerUpdate = 64;
    const double frameEpsilon = 1e-6;

}

void LottiePlayback::setAnimation(float totalFrames, float frameRate){
    const bool valid = totalFrames > 0.0f && frameRate > 0.0f && std::isfinite(totalFrames) && std::isfinite(frameRate);
    this->totalFrames = valid ? totalFrames : 0.0f;
    this->frameRate = valid ? frameRate : 0.0f;
    segmentStart = 0.0f;
    segmentEnd = this->totalFrames;
    markers.clear();
    playing = false;
    complete = false;
    position = 0.0;
    iterations = 0;
    syncFrame();
}

bool LottiePlayback::hasAnimation() const{
    return totalFrames > 0.0f;
}

void LottiePlayback::setSpeed(float speed){
    if (speed > 0.0f && std::isfinite(speed))
        this->speed = speed;
}

float LottiePlayback::getSpeed() const{
    return speed;
}

void LottiePlayback::setDirection(int direction){
    if ((direction != 1 && direction != -1) || direction == this->direction)
        return;

    // At the start of an iteration the new direction starts from its own first frame (the
    // end of the segment for -1). Elsewhere the position is mirrored: the same frame stays on
    // screen and the motion reverses (the epsilon keeps an exact frame boundary on its frame).
    const double length = segmentLength();
    if (position > 0.0){
        if (mode == Mode::NORMAL || position <= length){
            position = std::clamp(length - position - 2.0 * frameEpsilon, 0.0, length);
        }else{
            position = std::clamp(3.0 * length - position - 2.0 * frameEpsilon, length, 2.0 * length);
        }
    }
    this->direction = direction;
    syncFrame();
}

int LottiePlayback::getDirection() const{
    return direction;
}

void LottiePlayback::setMode(Mode mode){
    if (mode == this->mode)
        return;
    this->mode = mode;
    position = std::min(position, (double)segmentLength());
    syncFrame();
}

LottiePlayback::Mode LottiePlayback::getMode() const{
    return mode;
}

void LottiePlayback::setLoop(bool loop){
    loopCount = loop ? 0 : 1;
}

void LottiePlayback::setLoopCount(int count){
    loopCount = std::max(0, count);
}

int LottiePlayback::getLoopCount() const{
    return loopCount;
}

void LottiePlayback::setSegment(float start, float end){
    if (!std::isfinite(start) || !std::isfinite(end))
        return;
    if (start > end)
        std::swap(start, end);
    segmentStart = std::clamp(start, 0.0f, totalFrames);
    segmentEnd = std::clamp(end, segmentStart, totalFrames);
    position = 0.0;
    iterations = 0;
    complete = false;
    syncFrame();
}

void LottiePlayback::clearSegment(){
    setSegment(0.0f, totalFrames);
}

float LottiePlayback::getSegmentStart() const{
    return segmentStart;
}

float LottiePlayback::getSegmentEnd() const{
    return segmentEnd;
}

void LottiePlayback::setMarkers(std::vector<Marker> markers){
    this->markers = std::move(markers);
}

const std::vector<LottiePlayback::Marker>& LottiePlayback::getMarkers() const{
    return markers;
}

const LottiePlayback::Marker* LottiePlayback::findMarker(const std::string& name) const{
    for (const Marker& marker : markers){
        if (marker.name == name)
            return &marker;
    }
    return nullptr;
}

bool LottiePlayback::setMarker(const std::string& name){
    const Marker* marker = findMarker(name);
    if (!marker)
        return false;
    setSegment(marker->start, marker->end);
    return true;
}

void LottiePlayback::play(double now, std::vector<Event>& events){
    if (playing)
        return;
    if (complete){
        position = 0.0;
        iterations = 0;
        complete = false;
    }
    playing = true;
    lastTime = now;
    syncFrame();
    events.push_back(Event::PLAY);
}

void LottiePlayback::pause(double now, std::vector<Event>& events){
    if (!playing)
        return;
    advance(now, events);
    if (!playing)   // it completed on the way
        return;
    playing = false;
    events.push_back(Event::PAUSE);
}

void LottiePlayback::stop(double now, std::vector<Event>& events){
    const bool wasPlaying = playing;
    playing = false;
    complete = false;
    position = 0.0;
    iterations = 0;
    lastTime = now;
    syncFrame();
    if (wasPlaying)
        events.push_back(Event::PAUSE);
}

void LottiePlayback::seek(double seconds, double now){
    if (!std::isfinite(seconds))
        return;
    setFrame(segmentStart + (float)(seconds * frameRate), now);
}

void LottiePlayback::setFrame(float frame, double now){
    if (!std::isfinite(frame))
        return;
    frame = std::clamp(frame, segmentStart, segmentEnd);
    const double length = segmentLength();
    if (direction == 1){
        position = frame - segmentStart;
    }else{
        position = std::clamp((double)segmentEnd - 1.0 - frame, 0.0, length);
    }
    if (complete){
        // a seek after the end plays the last iteration again
        complete = false;
        if (loopCount > 0)
            iterations = loopCount - 1;
    }
    lastTime = now;
    syncFrame();
}

float LottiePlayback::update(double now, std::vector<Event>& events){
    advance(now, events);
    return currentFrame;
}

void LottiePlayback::advance(double now, std::vector<Event>& events){
    if (!playing || !hasAnimation()){
        lastTime = now;
        return;
    }

    const double delta = std::max(0.0, now - lastTime);
    lastTime = now;
    position += delta * speed * frameRate;

    const double cycle = cycleLength();
    if (cycle <= 0.0){
        // a one-frame segment: a finite playback ends at once, an infinite one shows it forever
        position = 0.0;
        if (loopCount > 0){
            iterations = loopCount;
            finish(events, 0);
        }
        syncFrame();
        return;
    }

    if (position >= cycle){
        const double wraps = std::floor(position / cycle);
        if (loopCount == 0 || iterations + wraps < loopCount){
            iterations = (int)std::min((double)iterations + wraps, 1e9);
            position -= wraps * cycle;
            const int loops = (int)std::min(wraps, (double)maxLoopEventsPerUpdate);
            for (int i = 0; i < loops; i++)
                events.push_back(Event::LOOP);
        }else{
            // the wraps before the last iteration ends are loops
            const int loops = std::max(0, loopCount - iterations - 1);
            iterations = loopCount;
            position = cycle;
            finish(events, loops);
        }
    }
    syncFrame();
}

void LottiePlayback::finish(std::vector<Event>& events, int loops){
    loops = std::min(loops, maxLoopEventsPerUpdate);
    for (int i = 0; i < loops; i++)
        events.push_back(Event::LOOP);
    complete = true;
    playing = false;
    events.push_back(Event::COMPLETE);
}

bool LottiePlayback::isPlaying() const{
    return playing;
}

bool LottiePlayback::isComplete() const{
    return complete;
}

float LottiePlayback::getCurrentFrame() const{
    return currentFrame;
}

float LottiePlayback::getTotalFrames() const{
    return totalFrames;
}

float LottiePlayback::getFrameRate() const{
    return frameRate;
}

float LottiePlayback::getDuration() const{
    return frameRate > 0.0f ? totalFrames / frameRate : 0.0f;
}

int LottiePlayback::getIterations() const{
    return iterations;
}

double LottiePlayback::getTimeToNextFrame() const{
    const double rate = (double)speed * frameRate;
    if (!playing || rate <= 0.0)
        return 0.0;

    const double length = segmentLength();
    const double cycle = cycleLength();
    if (cycle <= 0.0)
        return 0.0;

    // frames moved before the shown frame index changes: the distance to the next integer
    // of the frame position, in the direction of the current pass
    bool forward = direction == 1;
    double inPass = position;
    if (mode == Mode::BOUNCE && position > length){
        forward = !forward;
        inPass = position - length;
    }
    double distance;
    if (forward){
        const double x = segmentStart + inPass;
        distance = std::floor(x) + 1.0 - x;
    }else{
        const double x = segmentEnd - inPass;
        distance = x - (std::ceil(x) - 1.0);
    }
    distance = std::min(distance, cycle - position);
    return std::max(distance, 1e-4) / rate;
}

float LottiePlayback::segmentLength() const{
    return std::max(0.0f, segmentEnd - segmentStart);
}

float LottiePlayback::cycleLength() const{
    return mode == Mode::BOUNCE ? 2.0f * segmentLength() : segmentLength();
}

float LottiePlayback::frameAt(double position) const{
    if (!hasAnimation())
        return 0.0f;

    // integer frames shown by the segment: [first, last]
    const double first = std::max(0.0, std::ceil(segmentStart - 1e-4));
    const double last = std::max(first, std::min((double)totalFrames - 1.0, std::ceil(segmentEnd - 1e-4) - 1.0));

    const double length = segmentLength();
    bool forward = direction == 1;
    double inPass = position;
    if (mode == Mode::BOUNCE && position > length){
        forward = !forward;
        inPass = position - length;
    }

    double frame;
    if (forward){
        frame = std::floor(segmentStart + inPass + frameEpsilon);
    }else{
        frame = std::floor(segmentEnd - inPass - frameEpsilon);
    }
    return (float)std::clamp(frame, first, last);
}

void LottiePlayback::syncFrame(){
    currentFrame = frameAt(position);
}
