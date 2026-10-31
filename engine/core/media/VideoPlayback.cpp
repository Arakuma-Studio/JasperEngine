// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VideoPlayback.h"

#include <cmath>
#include <limits>

using namespace jasperengine;

void VideoPlayback::reset(){
    duration = -1.0;
    position = 0.0;
    rate = defaultRate;
    paused = true;
    ended = false;
    seeking = false;
    stalled = false;
    playingPending = false;
    iteration = 0;
    lastTime = 0.0;
    lastTimeupdate = 0.0;
    lastTimeupdatePosition = 0.0;
}

void VideoPlayback::setDuration(double seconds){
    duration = (std::isfinite(seconds) && seconds > 0.0) ? seconds : -1.0;
    if (duration > 0.0 && position > duration)
        position = duration;
}

bool VideoPlayback::hasDuration() const{
    return duration > 0.0;
}

double VideoPlayback::getDuration() const{
    return duration > 0.0 ? duration : std::numeric_limits<double>::quiet_NaN();
}

void VideoPlayback::setLoop(bool value){
    loop = value;
    // a looping element is never ended (HTML)
    if (loop)
        ended = false;
}

bool VideoPlayback::getLoop() const{
    return loop;
}

bool VideoPlayback::isValidRate(double value){
    return std::isfinite(value) && (value == 0.0 || (value >= MIN_RATE && value <= MAX_RATE));
}

bool VideoPlayback::setPlaybackRate(double value, double now, std::vector<Event>& events){
    if (!isValidRate(value))
        return false;
    advance(now, events);
    if (value != rate){
        rate = value;
        events.push_back(Event::RATECHANGE);
    }
    return true;
}

double VideoPlayback::getPlaybackRate() const{
    return rate;
}

bool VideoPlayback::setDefaultPlaybackRate(double value, std::vector<Event>& events){
    if (!isValidRate(value))
        return false;
    if (value != defaultRate){
        defaultRate = value;
        events.push_back(Event::RATECHANGE);
    }
    return true;
}

double VideoPlayback::getDefaultPlaybackRate() const{
    return defaultRate;
}

void VideoPlayback::play(double now, std::vector<Event>& events){
    advance(now, events);
    // HTML: playback that has ended starts again from the beginning
    if (ended && duration > 0.0)
        seek(0.0, now, events);
    if (paused){
        paused = false;
        events.push_back(Event::PLAY);
        playingPending = true;
        lastTime = now;
        lastTimeupdate = now;
    }
}

void VideoPlayback::pause(double now, std::vector<Event>& events){
    advance(now, events);
    if (!paused){
        paused = true;
        playingPending = false;
        events.push_back(Event::TIMEUPDATE);
        events.push_back(Event::PAUSE);
        lastTimeupdate = now;
        lastTimeupdatePosition = position;
    }
}

void VideoPlayback::seek(double seconds, double now, std::vector<Event>& events){
    advance(now, events);
    if (!std::isfinite(seconds))
        return;
    double target = seconds < 0.0 ? 0.0 : seconds;
    if (duration > 0.0 && target > duration)
        target = duration;
    position = target;
    iteration = 0;
    ended = false;
    seeking = true;
    events.push_back(Event::SEEKING);
}

void VideoPlayback::completeSeek(double now, std::vector<Event>& events){
    if (!seeking)
        return;
    seeking = false;
    lastTime = now;
    lastTimeupdate = now;
    lastTimeupdatePosition = position;
    events.push_back(Event::TIMEUPDATE);
    events.push_back(Event::SEEKED);
    // a seek to the end of media that does not loop ends it (HTML "reaches the end")
    if (duration > 0.0 && position >= duration && !loop){
        ended = true;
        if (!paused){
            paused = true;
            playingPending = false;
            events.push_back(Event::PAUSE);
        }
        events.push_back(Event::ENDED);
    }
}

void VideoPlayback::notifyReady(double now, std::vector<Event>& events){
    if (stalled)
        return;
    if (playingPending && !paused && !seeking){
        playingPending = false;
        lastTime = now;
        events.push_back(Event::PLAYING);
    }
}

void VideoPlayback::setStalled(bool value, double now, std::vector<Event>& events){
    if (value == stalled)
        return;
    advance(now, events);
    stalled = value;
    lastTime = now;
    if (stalled){
        if (!paused){
            events.push_back(Event::WAITING);
            playingPending = true;
        }
    }
}

void VideoPlayback::update(double now, std::vector<Event>& events){
    advance(now, events, true);
}

void VideoPlayback::advance(double now, std::vector<Event>& events, bool periodic){
    if (!isAdvancing() || now <= lastTime){
        if (now > lastTime)
            lastTime = now;
        return;
    }
    const double elapsed = now - lastTime;
    lastTime = now;
    position += elapsed * rate;

    if (duration > 0.0 && position >= duration){
        if (loop){
            const double wraps = std::floor(position / duration);
            position -= wraps * duration;
            if (position < 0.0 || position >= duration)
                position = 0.0;
            iteration += (int)wraps;
            events.push_back(Event::SEEKING);
            events.push_back(Event::TIMEUPDATE);
            events.push_back(Event::SEEKED);
            lastTimeupdate = now;
            lastTimeupdatePosition = position;
        }else{
            position = duration;
            ended = true;
            paused = true;
            playingPending = false;
            events.push_back(Event::TIMEUPDATE);
            events.push_back(Event::PAUSE);
            events.push_back(Event::ENDED);
            lastTimeupdate = now;
            lastTimeupdatePosition = position;
        }
        return;
    }

    if (periodic && now - lastTimeupdate >= TIMEUPDATE_INTERVAL - 1e-9 && position != lastTimeupdatePosition){
        events.push_back(Event::TIMEUPDATE);
        lastTimeupdate = now;
        lastTimeupdatePosition = position;
    }
}

double VideoPlayback::getPosition() const{
    return position;
}

int VideoPlayback::getIteration() const{
    return iteration;
}

bool VideoPlayback::isPaused() const{
    return paused;
}

bool VideoPlayback::isEnded() const{
    return ended;
}

bool VideoPlayback::isSeeking() const{
    return seeking;
}

bool VideoPlayback::isStalled() const{
    return stalled;
}

bool VideoPlayback::isAdvancing() const{
    return !paused && !ended && !seeking && !stalled && rate > 0.0 && duration > 0.0;
}
