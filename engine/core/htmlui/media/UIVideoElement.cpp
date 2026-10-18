// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "UIVideoElement.h"

#include "UIVideoControls.h"
#include "VideoColorConvert.h"
#include "VideoFormats.h"

#include "Engine.h"
#include "HtmlUIClock.h"
#include "HtmlUIDependencies.h"
#include "HtmlUITexture.h"
#include "Log.h"
#include "RmlRenderer.h"
#include "compat/DiagnosticCodes.h"
#include "compat/runtime/CompatProperties.h"
#include "object/ui/HtmlUIEventListener.h"
#include "object/ui/UIValue.h"
#include "render/SystemRender.h"
#include "subsystem/AudioSystem.h"
#include "texture/Framebuffer.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/ObjectFit.h>
#include <RmlUi/Core/PropertyIdSet.h>
#include <RmlUi/Core/RenderManager.h>
#include <RmlUi/Core/StringUtilities.h>
#include <RmlUi/Core/SystemInterface.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <limits>

using namespace jasperengine;

namespace {

    std::unique_ptr<Rml::ElementInstancerGeneric<UIVideoElement>>& videoInstancer(){
        static std::unique_ptr<Rml::ElementInstancerGeneric<UIVideoElement>> value;
        return value;
    }

    std::atomic<int> liveElements{0};
    bool externalAudio = false;
    int64_t externalLatency = 0;

    std::string toLower(std::string text){
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return text;
    }

    std::string trim(const std::string& text){
        const size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return std::string();
        const size_t last = text.find_last_not_of(" \t\r\n");
        return text.substr(first, last - first + 1);
    }

    const char* eventName(VideoPlayback::Event event){
        switch (event){
            case VideoPlayback::Event::PLAY: return "play";
            case VideoPlayback::Event::PLAYING: return "playing";
            case VideoPlayback::Event::PAUSE: return "pause";
            case VideoPlayback::Event::WAITING: return "waiting";
            case VideoPlayback::Event::SEEKING: return "seeking";
            case VideoPlayback::Event::SEEKED: return "seeked";
            case VideoPlayback::Event::TIMEUPDATE: return "timeupdate";
            case VideoPlayback::Event::ENDED: return "ended";
            case VideoPlayback::Event::RATECHANGE: return "ratechange";
        }
        return "";
    }

    // the media clock in sample frames from the start of loop iteration 0 (VideoAudioRing positions)
    int64_t mediaFrames(int iteration, double position, int64_t durationNs){
        const long double perIteration = std::llround((long double)durationNs * VideoAudioRing::SAMPLE_RATE / 1e9L);
        return (int64_t)((long double)iteration * perIteration + std::llround((long double)position * VideoAudioRing::SAMPLE_RATE));
    }

    const std::string diagnosticSuffix(htmlui::compat::DiagCode code){
        return " [" + code.id() + "]";
    }

}

RMLUI_RTTI_Define(UIVideoElement)

UIVideoElement::UIVideoElement(const Rml::String& tag): Rml::Element(tag){
    liveElements++;
}

UIVideoElement::~UIVideoElement(){
    if (controls)
        controls->forget();
    controls.reset();
    audioSource.reset();
    releaseTexture();
    stream.reset();
    liveElements--;
}

void UIVideoElement::registerInstancer(){
    if (!videoInstancer())
        videoInstancer() = std::make_unique<Rml::ElementInstancerGeneric<UIVideoElement>>();
    Rml::Factory::RegisterElementInstancer("video", videoInstancer().get());
}

void UIVideoElement::releaseInstancer(){
    videoInstancer().reset();
}

void UIVideoElement::setAudioConsumerForTesting(bool external, int64_t latencyFrames){
    externalAudio = external;
    externalLatency = std::max<int64_t>(latencyFrames, 0);
}

int UIVideoElement::getLiveCount(){
    return liveElements;
}

// ---------------------------------------------------------------- loading

std::string UIVideoElement::resolvePath(const std::string& source) const{
    // relative to the document, as <img> and <lottie> resolve sources
    std::string path = source;
    if (Rml::ElementDocument* document = const_cast<UIVideoElement*>(this)->GetOwnerDocument()){
        const Rml::String documentUrl = Rml::StringUtilities::Replace(document->GetSourceURL(), '|', ':');
        Rml::GetSystemInterface()->JoinPath(path, documentUrl, source);
    }
    return path;
}

void UIVideoElement::ensureLoaded(){
    if (loadPending)
        runLoad();
}

void UIVideoElement::resetMedia(bool fireEmptied){
    if (fireEmptied){
        if (networkState == NETWORK_LOADING || networkState == NETWORK_IDLE){
            if (!dataSent && fetchStarted)
                queueEvent("abort");
        }
        queueEvent("emptied");
    }
    if (audioSource)
        audioSource->stop();
    audioSource.reset();
    stream.reset();
    playback.reset();
    playbackEvents.clear();
    networkState = NETWORK_EMPTY;
    readyState = HAVE_NOTHING;
    error = MediaError();
    metadataSent = false;
    dataSent = false;
    fetchStarted = false;
    pendingSeek = -1.0;
    seekToStart = false;
    showPoster = true;
    stallSequence = -1;
    candidates.clear();
    candidateIndex = 0;
    currentSrc.clear();
    intrinsicSize = Rml::Vector2f(0.0f, 0.0f);
    frame = VideoFrame();
    hasPicture = false;
    uploadPending = false;
    shownSequence = -1;
    audioSynced = false;
    lastAudioNow = -1.0;
    geometryDirty = true;
}

void UIVideoElement::selectCandidates(){
    candidates.clear();
    candidateIndex = 0;
    if (HasAttribute("src")){
        Candidate candidate;
        candidate.attribute = trim(GetAttribute<Rml::String>("src", ""));
        candidate.path = candidate.attribute.empty() ? std::string() : resolvePath(candidate.attribute);
        candidates.push_back(candidate);
        return;
    }
    // the <source> children in order; their type decides before any file is opened
    for (int i = 0; i < GetNumChildren(); i++){
        Rml::Element* child = GetChild(i);
        if (!child || child->GetTagName() != "source")
            continue;
        const std::string source = trim(child->GetAttribute<Rml::String>("src", ""));
        if (source.empty())
            continue;
        if (child->HasAttribute("type")){
            const std::string type = child->GetAttribute<Rml::String>("type", "");
            if (VideoFormats::canPlayType(type) == VideoFormats::CanPlay::NO){
                Log::verbose("[HTML UI] <video>: <source src=\"%s\" type=\"%s\"> skipped: the engine plays video/webm (AV1, Opus)",
                             source.c_str(), type.c_str());
                continue;
            }
        }
        Candidate candidate;
        candidate.attribute = source;
        candidate.path = resolvePath(source);
        candidate.source = child->GetObserverPtr();
        candidates.push_back(candidate);
    }
}

void UIVideoElement::runLoad(){
    loadPending = false;
    const bool hadMedia = networkState != NETWORK_EMPTY;
    resetMedia(hadMedia);
    playback.setLoop(getLoop());
    autoplayFlag = true;
    // the muted content attribute sets the muted state once, when the element is made (HTML)
    if (!mutedInitialized){
        mutedInitialized = true;
        muted = HasAttribute("muted");
    }
    selectCandidates();

    if (candidates.empty()){
        // no src and no usable <source>: nothing to load (HTML: networkState empty, or no source)
        bool anySource = false;
        for (int i = 0; i < GetNumChildren(); i++){
            if (Rml::Element* child = GetChild(i); child && child->GetTagName() == "source")
                anySource = true;
        }
        if (anySource){
            networkState = NETWORK_NO_SOURCE;
            Log::warn("[HTML UI] <video>: none of its <source> elements has a type the engine plays (video/webm; AV1, Opus): %s%s",
                      VideoFormats::conversionHint(), diagnosticSuffix(htmlui::compat::codes::UnsupportedMediaFormat).c_str());
        }
        return;
    }
    queueEvent("loadstart");
    networkState = NETWORK_LOADING;
    holdPending();

    // preload="none" waits for play() or a seek (autoplay overrides it, as HTML says)
    if (getPreload() == "none" && !getAutoplay() && !fetchWanted){
        networkState = NETWORK_IDLE;
        queueEvent("suspend");
        DirtyLayout();
        return;
    }
    startFetch();
}

bool UIVideoElement::startFetch(){
    while (candidateIndex < candidates.size()){
        const Candidate& candidate = candidates[candidateIndex];
        if (candidate.path.empty()){
            tryNextCandidate("the src attribute is empty");
            continue;
        }
        if (VideoFormats::isRejectedExtension(candidate.path)){
            tryNextCandidate("'" + candidate.attribute + "' is in a format the engine does not play; " + VideoFormats::conversionHint() +
                             diagnosticSuffix(htmlui::compat::codes::UnsupportedMediaFormat));
            continue;
        }
        fetchStarted = true;
        currentSrc = candidate.path;
        // hot reload watches the file (a document load scope records it)
        HtmlUIFileLog::recordFile(candidate.path);
        stream = std::make_unique<VideoStream>(candidate.path, VideoStream::platformHasThreads(), true);
        stats.streamsCreated++;
        if (frameExactNow()){
            stream->waitOpen();
            pollStream();
        }
        return true;
    }
    return false;
}

void UIVideoElement::tryNextCandidate(const std::string& reason){
    const Candidate candidate = candidates[candidateIndex];
    candidateIndex++;
    stream.reset();
    if (!candidate.source){
        // the src attribute: the element fails
        failSource(reason, MediaErrorCode::SRC_NOT_SUPPORTED);
        return;
    }
    // a <source>: an error there, then the next one (HTML)
    Log::warn("[HTML UI] <video>: <source src=\"%s\"> could not be played: %s", candidate.attribute.c_str(), reason.c_str());
    queueEvent("error", reason, candidate.source.get());
    if (candidateIndex >= candidates.size()){
        networkState = NETWORK_NO_SOURCE;
        fetchStarted = false;
        Log::warn("[HTML UI] <video>: no <source> could be played");
        return;
    }
    startFetch();
}

void UIVideoElement::failSource(const std::string& message, MediaErrorCode code){
    error.code = code;
    error.message = message;
    networkState = NETWORK_NO_SOURCE;
    fetchStarted = false;
    Log::error("[HTML UI] <video src=\"%s\">: %s", GetAttribute<Rml::String>("src", "").c_str(), message.c_str());
    queueEvent("error", message);
    holdPending();
}

void UIVideoElement::pollStream(){
    if (!stream || metadataSent)
        return;
    VideoStream::State state = stream->getState();
    if (state == VideoStream::State::OPENING){
        stream->pump(1);
        state = stream->getState();
    }
    if (state == VideoStream::State::FAILED){
        const MediaError streamError = stream->getError();
        std::string message = streamError.message;
        if (streamError.code == MediaErrorCode::SRC_NOT_SUPPORTED)
            message += "; " + std::string(VideoFormats::conversionHint()) + diagnosticSuffix(htmlui::compat::codes::UnsupportedMediaFormat);
        if (streamError.code == MediaErrorCode::SRC_NOT_SUPPORTED || streamError.code == MediaErrorCode::NETWORK){
            tryNextCandidate(message);
        }else{
            stream.reset();
            failSource(message, streamError.code);
        }
        return;
    }
    if (state == VideoStream::State::READY)
        onMetadata();
}

void UIVideoElement::onMetadata(){
    const VideoStream::Info& info = stream->getInfo();
    metadataSent = true;
    playback.setDuration(info.duration);
    playback.setLoop(getLoop());
    stream->setLoop(getLoop());
    intrinsicSize = info.hasVideo ? Rml::Vector2f((float)info.displayWidth, (float)info.displayHeight) : Rml::Vector2f(0.0f, 0.0f);
    readyState = HAVE_METADATA;
    queueEvent("durationchange");
    queueEvent("loadedmetadata");
    if (info.audioIgnored){
        Log::warn("[HTML UI] <video src=\"%s\">: its %s audio is not played (the engine plays Opus); %s%s", currentSrc.c_str(),
                  VideoFormats::codecName(info.audioCodec).c_str(), VideoFormats::conversionHint(),
                  diagnosticSuffix(htmlui::compat::codes::UnsupportedMediaFormat).c_str());
    }
    if (pendingSeek >= 0.0){
        playback.seek(pendingSeek, HtmlUIClock::now(), playbackEvents);
        pendingSeek = -1.0;
        audioSynced = false;
    }
    autoplayPending = getAutoplay() && autoplayFlag;
    DirtyLayout();
    geometryDirty = true;
}

// ---------------------------------------------------------------- playback

bool UIVideoElement::frameExactNow() const{
    return HtmlUIClock::getFixedTime().has_value() || Engine::getFixedDeltatime() > 0.0;
}

void UIVideoElement::advance(bool fromUpdate){
    if (!stream || !metadataSent)
        return;
    frameExact = frameExactNow();
    if (fromUpdate)
        playback.update(HtmlUIClock::now(), playbackEvents);
    showFrameForTime();
    handlePlaybackEvents();
}

void UIVideoElement::showFrameForTime(){
    const VideoStream::Info& info = stream->getInfo();
    const double now = HtmlUIClock::now();
    if (!info.hasVideo){
        // audio only: the data is there once open
        if (playback.isSeeking())
            playback.completeSeek(now, playbackEvents);
        if (!dataSent){
            dataSent = true;
            readyState = HAVE_ENOUGH_DATA;
            networkState = NETWORK_IDLE;
            queueEvent("loadeddata");
            queueEvent("canplay");
            queueEvent("canplaythrough");
        }
        playback.notifyReady(now, playbackEvents);
        if (autoplayPending){
            autoplayPending = false;
            play();
        }
        return;
    }

    VideoFrame next;
    const VideoStream::FrameResult result = stream->acquireFrame(playback.getIteration(), playback.getPosition(), frameExact, shownSequence, next);
    bool exact = false;
    switch (result){
        case VideoStream::FrameResult::NEW:
        case VideoStream::FrameResult::STALE:
            shownSequence = next.sequence;
            if (!next.y.empty()){
                frame = std::move(next);
                uploadPending = true;
                stats.framesShown++;
            }
            exact = result == VideoStream::FrameResult::NEW;
            break;
        case VideoStream::FrameResult::SAME:
            exact = true;
            break;
        case VideoStream::FrameResult::NOT_READY:
            break;
        case VideoStream::FrameResult::FAILED: {
            if (!error){
                const MediaError streamError = stream->getError();
                error.code = streamError.code == MediaErrorCode::NONE ? MediaErrorCode::DECODE : streamError.code;
                error.message = streamError.message;
                Log::error("[HTML UI] <video src=\"%s\">: %s", currentSrc.c_str(), error.message.c_str());
                networkState = NETWORK_IDLE;
                queueEvent("error", error.message);
                // the last good frame stays; playback stops there
                playback.pause(now, playbackEvents);
            }
            return;
        }
    }

    if (exact){
        stallSequence = -1;
        playback.setStalled(false, now, playbackEvents);
        if (playback.isSeeking())
            playback.completeSeek(now, playbackEvents);
        if (!dataSent){
            dataSent = true;
            // a local file: everything is there (HTML's "suspend" of a finished fetch)
            networkState = NETWORK_IDLE;
            queueEvent("loadeddata");
            queueEvent("canplay");
            queueEvent("canplaythrough");
        }
        readyState = HAVE_ENOUGH_DATA;
        playback.notifyReady(now, playbackEvents);
        if (autoplayPending){
            autoplayPending = false;
            play();
        }
        return;
    }

    // real time without the frame for the time
    if (playback.isSeeking()){
        readyState = HAVE_METADATA;
        return;
    }
    const int64_t target = stream->sequenceFor(playback.getIteration(), playback.getPosition());
    readyState = shownSequence >= 0 ? HAVE_CURRENT_DATA : HAVE_METADATA;
    // a quarter of a second behind: the clock waits for the decoder (a browser buffering)
    const int lag = (int)std::max<int64_t>(target - std::max<int64_t>(shownSequence, 0), 0);
    const double frameSeconds = info.frameCount > 0 ? info.duration / info.frameCount : 1.0 / 30.0;
    if (!playback.isPaused() && (shownSequence < 0 || lag * frameSeconds > 0.25)){
        stallSequence = target;
        playback.setStalled(true, now, playbackEvents);
    }
}

void UIVideoElement::handlePlaybackEvents(){
    for (VideoPlayback::Event event : playbackEvents){
        queueEvent(eventName(event));
        if (event == VideoPlayback::Event::SEEKING)
            audioSynced = false;
    }
    playbackEvents.clear();
}

void UIVideoElement::afterControl(){
    advance(false);
    if (Rml::Context* context = GetContext())
        context->RequestNextUpdate(0.0);
}

void UIVideoElement::internalPause(){
    if (!playback.isPaused()){
        playback.pause(HtmlUIClock::now(), playbackEvents);
        handlePlaybackEvents();
    }
    if (audioSource)
        audioSource->setPaused(true);
}

void UIVideoElement::play(){
    ensureLoaded();
    fetchWanted = true;
    autoplayPending = false;
    autoplayFlag = false;
    if (!fetchStarted && networkState == NETWORK_IDLE && !candidates.empty())
        startFetch();
    showPoster = false;
    playback.play(HtmlUIClock::now(), playbackEvents);
    afterControl();
}

void UIVideoElement::pause(){
    ensureLoaded();
    autoplayPending = false;
    autoplayFlag = false;
    playback.pause(HtmlUIClock::now(), playbackEvents);
    afterControl();
}

void UIVideoElement::load(){
    loadPending = true;
    fetchWanted = false;
    runLoad();
    if (Rml::Context* context = GetContext())
        context->RequestNextUpdate(0.0);
}

std::string UIVideoElement::canPlayType(const std::string& type){
    return VideoFormats::toString(VideoFormats::canPlayType(type));
}

void UIVideoElement::fastSeek(double seconds){
    ensureLoaded();
    if (!std::isfinite(seconds))
        return;
    if (stream && metadataSent)
        seconds = stream->keyframeTimeAtOrBefore(seconds);
    setCurrentTime(seconds);
}

double UIVideoElement::getCurrentTime(){
    ensureLoaded();
    if (!metadataSent)
        return pendingSeek >= 0.0 ? pendingSeek : 0.0;
    playback.update(HtmlUIClock::now(), playbackEvents);
    handlePlaybackEvents();
    return playback.getPosition();
}

void UIVideoElement::setCurrentTime(double seconds){
    ensureLoaded();
    if (!std::isfinite(seconds)){
        Log::warn("[HTML UI] <video>: currentTime = %g is not a finite number", seconds);
        return;
    }
    showPoster = false;
    fetchWanted = true;
    if (!fetchStarted && networkState == NETWORK_IDLE && !candidates.empty())
        startFetch();
    if (!metadataSent){
        pendingSeek = std::max(seconds, 0.0);
        return;
    }
    playback.seek(seconds, HtmlUIClock::now(), playbackEvents);
    audioSynced = false;
    afterControl();
}

double UIVideoElement::getDuration(){
    ensureLoaded();
    return playback.getDuration();
}

bool UIVideoElement::isPaused(){
    ensureLoaded();
    if (metadataSent){
        playback.update(HtmlUIClock::now(), playbackEvents);
        handlePlaybackEvents();
    }
    return playback.isPaused();
}

bool UIVideoElement::isEnded(){
    ensureLoaded();
    if (metadataSent){
        playback.update(HtmlUIClock::now(), playbackEvents);
        handlePlaybackEvents();
    }
    return playback.isEnded();
}

bool UIVideoElement::isSeeking(){
    ensureLoaded();
    return playback.isSeeking();
}

bool UIVideoElement::isMuted() const{
    return muted;
}

void UIVideoElement::setMuted(bool value){
    if (value == muted)
        return;
    muted = value;
    queueEvent("volumechange");
    if (audioSource)
        audioSource->setVolume(effectiveVolume());
    if (Rml::Context* context = GetContext())
        context->RequestNextUpdate(0.0);
}

float UIVideoElement::getVolume() const{
    return volume;
}

bool UIVideoElement::setVolume(float value){
    if (!std::isfinite(value) || value < 0.0f || value > 1.0f){
        Log::warn("[HTML UI] <video>: volume = %g is outside [0, 1] (HTML throws IndexSizeError); unchanged", value);
        return false;
    }
    if (value != volume){
        volume = value;
        queueEvent("volumechange");
        if (audioSource)
            audioSource->setVolume(effectiveVolume());
        if (Rml::Context* context = GetContext())
            context->RequestNextUpdate(0.0);
    }
    return true;
}

double UIVideoElement::getPlaybackRate(){
    return playback.getPlaybackRate();
}

bool UIVideoElement::setPlaybackRate(double rate){
    if (!playback.setPlaybackRate(rate, HtmlUIClock::now(), playbackEvents)){
        Log::warn("[HTML UI] <video>: playbackRate = %g is not 0 or in [0.0625, 16] (HTML throws NotSupportedError); unchanged", rate);
        return false;
    }
    handlePlaybackEvents();
    audioSynced = false;
    if (Rml::Context* context = GetContext())
        context->RequestNextUpdate(0.0);
    return true;
}

double UIVideoElement::getDefaultPlaybackRate(){
    return playback.getDefaultPlaybackRate();
}

bool UIVideoElement::setDefaultPlaybackRate(double rate){
    if (!playback.setDefaultPlaybackRate(rate, playbackEvents)){
        Log::warn("[HTML UI] <video>: defaultPlaybackRate = %g is not 0 or in [0.0625, 16]; unchanged", rate);
        return false;
    }
    handlePlaybackEvents();
    return true;
}

int UIVideoElement::getReadyState(){
    ensureLoaded();
    return readyState;
}

int UIVideoElement::getNetworkState(){
    ensureLoaded();
    return networkState;
}

int UIVideoElement::getVideoWidth(){
    ensureLoaded();
    return (int)intrinsicSize.x;
}

int UIVideoElement::getVideoHeight(){
    ensureLoaded();
    return (int)intrinsicSize.y;
}

std::string UIVideoElement::getCurrentSrc() const{
    return currentSrc;
}

const MediaError& UIVideoElement::getError() const{
    return error;
}

bool UIVideoElement::getLoop() const{
    return HasAttribute("loop");
}

void UIVideoElement::setLoop(bool value){
    if (value)
        SetAttribute("loop", "");
    else
        RemoveAttribute("loop");
}

bool UIVideoElement::getAutoplay() const{
    return HasAttribute("autoplay");
}

void UIVideoElement::setAutoplay(bool value){
    if (value)
        SetAttribute("autoplay", "");
    else
        RemoveAttribute("autoplay");
}

bool UIVideoElement::getControls() const{
    return HasAttribute("controls");
}

void UIVideoElement::setControls(bool value){
    if (value)
        SetAttribute("controls", "");
    else
        RemoveAttribute("controls");
}

bool UIVideoElement::getDefaultMuted() const{
    return HasAttribute("muted");
}

void UIVideoElement::setDefaultMuted(bool value){
    if (value)
        SetAttribute("muted", "");
    else
        RemoveAttribute("muted");
}

std::string UIVideoElement::getPoster() const{
    return GetAttribute<Rml::String>("poster", "");
}

void UIVideoElement::setPoster(const std::string& poster){
    SetAttribute("poster", poster);
}

std::string UIVideoElement::getPreload() const{
    if (!HasAttribute("preload"))
        return "auto";
    const std::string value = toLower(trim(GetAttribute<Rml::String>("preload", "")));
    if (value == "none")
        return "none";
    if (value == "auto" || value.empty())
        return "auto";
    return "metadata";
}

void UIVideoElement::setPreload(const std::string& preload){
    SetAttribute("preload", preload);
}

// ---------------------------------------------------------------- audio

float UIVideoElement::effectiveVolume() const{
    const double rate = const_cast<UIVideoElement*>(this)->playback.getPlaybackRate();
    // Chromium's audible range of rates
    if (muted || rate < 0.25 || rate > 4.0)
        return 0.0f;
    return volume;
}

void UIVideoElement::updateAudio(){
    if (!stream || !metadataSent || !stream->getInfo().hasAudio)
        return;
    std::shared_ptr<VideoAudioRing> ring = stream->getAudioRing();
    if (!ring)
        return;
    const double now = HtmlUIClock::now();
    // the media clock moves this update: playing, and the UI clock advanced (a pinned or held
    // clock freezes the picture, so the sound stops too)
    const bool moving = playback.isAdvancing() && lastAudioNow >= 0.0 && now > lastAudioNow;
    const bool clockRuns = playback.isAdvancing() && (lastAudioNow < 0.0 || now > lastAudioNow);
    lastAudioNow = now;

    int64_t latency = externalLatency;
    if (!externalAudio){
        if (!audioSource && playback.isAdvancing()){
            audioSource = std::make_unique<VideoAudioSource>(ring);
            if (!audioSource->start(effectiveVolume()))
                audioSource.reset();
        }
        if (!audioSource)
            return;
        if (!audioSource->isPlaying()){
            // SoLoud was torn down (AudioSystem deinit): a new voice
            audioSource->stop();
            if (!audioSource->start(effectiveVolume())){
                audioSource.reset();
                return;
            }
            audioSynced = false;
        }
        latency = audioSource->getLatencyFrames();
        audioSource->setVolume(effectiveVolume());
        audioSource->setSpeed((float)std::max(playback.getPlaybackRate(), 0.0625));
    }

    const int64_t media = mediaFrames(playback.getIteration(), playback.getPosition(), stream->getInfo().durationNs);
    if (!audioSynced){
        stream->syncAudio(media + latency);
        audioSynced = true;
        audioDrift = 0.0;
        driftCount = 0;
        // frame-exact: the next decisions see the ring at its new position
        if (frameExact)
            stream->waitAudioSync();
    }else if (moving && !stream->isAudioSyncPending()){
        const VideoAudioSync::Decision decision = VideoAudioSync::decide(ring->getReadPosition(), ring->getPendingCorrection(), media, latency);
        audioDrift = decision.drift;
        if (decision.action == VideoAudioSync::Action::RESYNC){
            stream->syncAudio(decision.frames);
            driftCount = 0;
        }else{
            // the mixer reads in blocks (10 ms or more): one sample says little; the mean of the
            // last updates outside the dead band is corrected
            driftSamples[driftCount % DRIFT_SAMPLES] = decision.drift;
            driftCount++;
            if (driftCount >= DRIFT_SAMPLES){
                double mean = 0.0;
                for (double sample : driftSamples)
                    mean += sample;
                mean /= DRIFT_SAMPLES;
                if (std::fabs(mean) > VideoAudioSync::DEAD_BAND){
                    ring->requestCorrection(ring->getPendingCorrection() + (int64_t)std::llround(mean * VideoAudioRing::SAMPLE_RATE));
                    driftCount = 0;
                }
            }
        }
    }
    if (audioSource)
        audioSource->setPaused(!clockRuns);
}

// ---------------------------------------------------------------- events

void UIVideoElement::queueEvent(const std::string& type, const std::string& message, Rml::Element* target){
    PendingEvent event;
    event.type = type;
    event.message = message;
    if (target)
        event.target = target->GetObserverPtr();
    pendingEvents.push_back(std::move(event));
}

void UIVideoElement::holdPending(){
    // HTML queues media element tasks: the events of a load reach the listeners the document's
    // scripts add after it, on a later engine frame (as <img> and <lottie> load events)
    holding = true;
    holdFrame = SystemRender::getFrameCount();
}

void UIVideoElement::dispatchPending(){
    holding = false;
    if (pendingEvents.empty())
        return;
    // listeners may change this element (or remove it): dispatch a copy
    std::vector<PendingEvent> events;
    events.swap(pendingEvents);
    Rml::ObserverPtr<Rml::Element> self = GetObserverPtr();
    for (const PendingEvent& event : events){
        Rml::Element* target = event.target ? event.target.get() : this;
        if (!target || !self)
            continue;
        Rml::Dictionary parameters;
        UIValue detail;
        if (!event.message.empty()){
            parameters["message"] = event.message;
            detail = UIValue::object();
            detail["message"] = UIValue(event.message);
            parameters[HtmlUIEventNames::detailParameter] = (void*)&detail;
        }
        // media events do not bubble (capture listeners still see them)
        target->DispatchEvent(event.type, parameters, false, false);
    }
}

// ---------------------------------------------------------------- RmlUi hooks

bool UIVideoElement::GetIntrinsicDimensions(Rml::Vector2f& dimensions, float& ratio){
    ensureLoaded();
    if (posterDirty)
        loadPoster();
    // the video's size once known, else the poster's, else 300x150 (CSS 2 replaced elements)
    if (intrinsicSize.x > 0.0f && intrinsicSize.y > 0.0f)
        dimensions = intrinsicSize;
    else if (posterSize.x > 0.0f && posterSize.y > 0.0f)
        dimensions = posterSize;
    else
        dimensions = Rml::Vector2f(300.0f, 150.0f);
    ratio = dimensions.x / dimensions.y;
    dimensions *= Rml::ElementUtilities::GetDensityIndependentPixelRatio(this);
    return true;
}

void UIVideoElement::OnUpdate(){
    ensureLoaded();
    pollStream();
    if (metadataSent)
        advance(true);
    updateAudio();
    uploadFrame();
    updateControls();

    // in order; the events of a load wait for a later engine frame (holdPending)
    if (!holding || SystemRender::getFrameCount() != holdFrame)
        dispatchPending();

    Rml::Context* context = GetContext();
    if (context && (!pendingEvents.empty() || (stream && (!metadataSent || !dataSent || !playback.isPaused())) || uploadPending))
        context->RequestNextUpdate(0.0);
}

void UIVideoElement::OnResize(){
    geometryDirty = true;
}

void UIVideoElement::OnLayout(){
    Rml::Element::OnLayout();
    if (controls)
        controls->layout();
}

void UIVideoElement::OnAttributeChange(const Rml::ElementAttributes& changed){
    Rml::Element::OnAttributeChange(changed);
    if (changed.count("src")){
        loadPending = true;
        fetchWanted = false;
        DirtyLayout();
    }
    if (changed.count("loop")){
        playback.setLoop(getLoop());
        if (stream)
            stream->setLoop(getLoop());
    }
    if (changed.count("poster")){
        posterDirty = true;
        geometryDirty = true;
        DirtyLayout();
    }
    if (changed.count("preload") && !loadPending && !fetchStarted && networkState == NETWORK_IDLE && getPreload() != "none")
        startFetch();
    if (changed.count("controls"))
        updateControls();
    if (changed.count("autoplay") && getAutoplay() && metadataSent && playback.isPaused() && !dataSent)
        autoplayPending = true;
    if (Rml::Context* context = GetContext())
        context->RequestNextUpdate(0.0);
}

void UIVideoElement::OnPropertyChange(const Rml::PropertyIdSet& changed){
    Rml::Element::OnPropertyChange(changed);
    if (changed.Contains(Rml::PropertyId::ImageColor) || changed.Contains(Rml::PropertyId::Opacity) ||
        changed.Contains(Rml::PropertyId::ObjectFit) || changed.Contains(Rml::PropertyId::ObjectPositionX) ||
        changed.Contains(Rml::PropertyId::ObjectPositionY))
        geometryDirty = true;
    const Rml::PropertyId rendering = htmlui::compat::CompatProperties::id(htmlui::compat::CompatProperty::ImageRendering);
    if (rendering != Rml::PropertyId::Invalid && changed.Contains(rendering)){
        // nearest or linear sampling: another texture
        releaseTexture();
        uploadPending = !frame.y.empty();
        posterDirty = true;
        geometryDirty = true;
    }
}

void UIVideoElement::OnChildAdd(Rml::Element* child){
    Rml::Element::OnChildAdd(child);
    // a <source> added to a video without a resource starts the resource selection (HTML)
    if (child != this && child && child->GetParentNode() == this && child->GetTagName() == "source" && !HasAttribute("src") &&
        (networkState == NETWORK_EMPTY || networkState == NETWORK_NO_SOURCE))
        loadPending = true;
}

void UIVideoElement::OnChildRemove(Rml::Element* child){
    Rml::Element::OnChildRemove(child);
    // removed from its document: playback pauses (HTML's removal steps)
    if (child == this)
        internalPause();
}

void UIVideoElement::ProcessDefaultAction(Rml::Event& event){
    Rml::Element::ProcessDefaultAction(event);
    if (controls)
        controls->processDefaultAction(event);
}

// ---------------------------------------------------------------- drawing

void UIVideoElement::releaseTexture(){
    if (liveTexture){
        if (RmlRenderer* renderer = RmlRenderer::forContext(GetContext()))
            renderer->cancelVideoConversion(liveTexture);
    }
    // RmlUi gives the handle back to the renderer, which deletes the HtmlUITexture
    texture.Release();
    liveTexture = nullptr;
    textureSize = Rml::Vector2i(0, 0);
    hasPicture = false;
}

bool UIVideoElement::createTexture(const Rml::CallbackTextureInterface& textureInterface){
    liveTexture = nullptr;
    const int width = textureSize.x, height = textureSize.y;
    if (width <= 0 || height <= 0)
        return false;
    const std::string rendering = htmlui::compat::CompatProperties::text(*this, htmlui::compat::CompatProperty::ImageRendering);
    const TextureFilter filter = (rendering == "pixelated" || rendering == "crisp-edges") ? TextureFilter::NEAREST : TextureFilter::LINEAR;

    std::unique_ptr<HtmlUITexture> created;
    if (gpuConversion){
        created = std::make_unique<HtmlUITexture>();
        created->kind = HtmlUITexture::Kind::RENDER_TARGET;
        created->ownedTarget = std::make_unique<Framebuffer>();
        FramebufferDesc desc;
        desc.width = width;
        desc.height = height;
        desc.color = ColorFormat::RGBA;
        desc.depth = DepthStencilFormat::NONE;
        desc.sampleCount = 1;
        desc.minFilter = filter;
        desc.magFilter = filter;
        desc.label = "htmlui-video";
        if (!created->ownedTarget->create(desc)){
            Log::error("[HTML UI] <video>: could not create the %d x %d video texture", width, height);
            return false;
        }
        created->render = &created->ownedTarget->getRender().getColorTexture();
        created->width = width;
        created->height = height;
        const bool chroma = frame.hasChroma();
        const int chromaWidth = chroma ? frame.chromaWidth() : 1;
        const int chromaHeight = chroma ? frame.chromaHeight() : 1;
        const int sizes[3][2] = {{width, height}, {chromaWidth, chromaHeight}, {chromaWidth, chromaHeight}};
        const char* labels[3] = {"htmlui-video-y", "htmlui-video-u", "htmlui-video-v"};
        for (int i = 0; i < 3; i++){
            created->planes[i] = std::make_unique<TextureRender>();
            if (!created->planes[i]->createDynamicTexture(labels[i], sizes[i][0], sizes[i][1], ColorFormat::RED, TextureFilter::LINEAR)){
                Log::error("[HTML UI] <video>: could not create the %d x %d video plane", sizes[i][0], sizes[i][1]);
                return false;
            }
        }
        if (!chroma){
            // monochrome: neutral chroma
            const uint8_t neutral = 128;
            created->planes[1]->updateTexture(&neutral, 1);
            created->planes[2]->updateTexture(&neutral, 1);
        }
    }else{
        created = HtmlUITexture::createDynamic("htmlui-video", width, height, ColorFormat::RGBA, filter);
        if (!created)
            return false;
    }
    stats.texturesCreated++;
    HtmlUITexture* raw = created.release();
    textureInterface.SetTextureHandle(reinterpret_cast<Rml::TextureHandle>(raw), textureSize);
    liveTexture = raw;
    // a new texture holds nothing yet: the frame goes up again
    hasPicture = false;
    uploadPending = !frame.y.empty();
    uploadedFrame = UINT64_MAX;
    return true;
}

bool UIVideoElement::ensureTexture(){
    Rml::RenderManager* renderManager = GetRenderManager();
    if (!renderManager || frame.y.empty())
        return false;
    const bool gpu = RmlRenderer::forContext(GetContext()) != nullptr;
    const Rml::Vector2i size(frame.width, frame.height);
    if (texture && (textureSize != size || gpuConversion != gpu))
        releaseTexture();
    if (!texture){
        gpuConversion = gpu;
        textureSize = size;
        texture = renderManager->MakeCallbackTexture([this](const Rml::CallbackTextureInterface& textureInterface) -> bool {
            return createTexture(textureInterface);
        });
        geometryDirty = true;
    }
    // makes the texture if RmlUi has none (first use, after a view loss)
    Rml::Texture current = texture;
    return current.GetDimensions() == textureSize && liveTexture != nullptr;
}

void UIVideoElement::uploadFrame(){
    if (!uploadPending || frame.y.empty())
        return;
    // one update of an image per sokol frame: a second frame in the same one waits for the next
    const uint64_t engineFrame = SystemRender::getFrameCount();
    if (uploadedFrame == engineFrame)
        return;
    if (!ensureTexture())
        return;
    if (liveTexture->dead)
        return;
    if (gpuConversion){
        RmlRenderer* renderer = RmlRenderer::forContext(GetContext());
        if (!renderer)
            return;
        liveTexture->planes[0]->updateTexture(frame.y.data(), frame.y.size());
        if (frame.hasChroma()){
            liveTexture->planes[1]->updateTexture(frame.u.data(), frame.u.size());
            liveTexture->planes[2]->updateTexture(frame.v.data(), frame.v.size());
        }
        RmlRenderer::VideoConversion conversion;
        conversion.target = liveTexture;
        VideoColorConvert::matrix(frame.color, frame.height, conversion.colorMatrix);
        VideoColorConvert::chromaScale(frame, conversion.chromaScale);
        renderer->queueVideoConversion(conversion);
        stats.conversionsQueued++;
        stats.uploads += frame.hasChroma() ? 3 : 1;
    }else{
        std::vector<uint8_t> rgba;
        VideoColorConvert::toRgba(frame, rgba);
        if (!liveTexture->update(rgba.data(), rgba.size()))
            return;
        stats.cpuConversions++;
        stats.uploads++;
    }
    uploadedFrame = engineFrame;
    uploadPending = false;
    hasPicture = true;
    geometryDirty = true;
}

void UIVideoElement::loadPoster(){
    posterDirty = false;
    posterTexture = Rml::Texture();
    posterSize = Rml::Vector2f(0.0f, 0.0f);
    posterSource = trim(GetAttribute<Rml::String>("poster", ""));
    if (posterSource.empty())
        return;
    Rml::RenderManager* renderManager = GetRenderManager();
    if (!renderManager)
        return;
    std::string documentPath;
    if (Rml::ElementDocument* document = GetOwnerDocument())
        documentPath = Rml::StringUtilities::Replace(document->GetSourceURL(), '|', ':');
    const std::string rendering = htmlui::compat::CompatProperties::text(*this, htmlui::compat::CompatProperty::ImageRendering);
    const std::string query = (rendering == "pixelated" || rendering == "crisp-edges") ? "htmlui-filter=nearest" : "";
    posterTexture = query.empty() ? renderManager->LoadTexture(posterSource, documentPath) : renderManager->LoadTexture(posterSource, documentPath, query);
    const Rml::Vector2i dimensions = posterTexture.GetDimensions();
    if (dimensions.x <= 0 || dimensions.y <= 0){
        Log::warn("[HTML UI] <video poster=\"%s\">: the poster image could not be loaded", posterSource.c_str());
        posterTexture = Rml::Texture();
        return;
    }
    posterSize = Rml::Vector2f((float)dimensions.x, (float)dimensions.y);
}

void UIVideoElement::generateGeometry(Rml::Texture drawTexture, Rml::Vector2f intrinsic){
    geometryDirty = false;
    const Rml::ComputedValues& computed = GetComputedValues();
    const Rml::ColourbPremultiplied colour = computed.image_color().ToPremultiplied(GetDrawOpacity());
    const Rml::Vector2f origin = GetAbsoluteOffset(Rml::BoxArea::Content).Round() - GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f box = GetBox().GetSize(Rml::BoxArea::Content).Round();
    (void)drawTexture;
    Rml::ObjectFitRect fit;
    if (!Rml::ComputeObjectFitRect(box, intrinsic, computed.object_fit(), computed.object_position_x(), computed.object_position_y(), fit)){
        geometry.Clear();
        return;
    }
    geometry.Set(Rml::Rectanglef::FromPositionSize(origin + fit.rect.TopLeft(), fit.rect.Size()), colour, fit.uv.TopLeft(), fit.uv.BottomRight());
}

void UIVideoElement::OnRender(){
    if (posterDirty)
        loadPoster();

    // the poster until playback starts or a seek, and while no frame is there (HTML)
    const bool usePoster = posterSize.x > 0.0f && (showPoster || !hasPicture);
    Rml::Texture drawTexture;
    Rml::Vector2f intrinsic;
    if (usePoster){
        drawTexture = posterTexture;
        intrinsic = posterSize;
    }else{
        if (!texture || !hasPicture)
            return;
        drawTexture = texture;
        // a view loss gave the texture back: made again (empty), the frame goes up next update
        if (drawTexture.GetDimensions() != textureSize || !liveTexture || !hasPicture)
            return;
        intrinsic = intrinsicSize.x > 0.0f ? intrinsicSize : Rml::Vector2f(textureSize);
    }

    if (usePoster != geometryForPoster){
        geometryDirty = true;
        geometryForPoster = usePoster;
    }
    if (geometryDirty)
        generateGeometry(drawTexture, intrinsic);
    if (!geometry.IsSet())
        return;
    geometry.Render(this, drawTexture);
}

// ---------------------------------------------------------------- controls

void UIVideoElement::updateControls(){
    if (getControls()){
        if (!controls)
            controls = std::make_unique<UIVideoControls>(*this);
        controls->update();
    }else if (controls){
        controls.reset();
    }
}

// ---------------------------------------------------------------- tooling

const UIVideoElement::Stats& UIVideoElement::getStats() const{
    return stats;
}

VideoStream::Stats UIVideoElement::getStreamStats() const{
    return stream ? stream->getStats() : VideoStream::Stats();
}

int UIVideoElement::getShownFrameIndex() const{
    return frame.y.empty() ? -1 : frame.index;
}

int64_t UIVideoElement::getShownSequence() const{
    return shownSequence;
}

bool UIVideoElement::isShowingPoster() const{
    return posterSize.x > 0.0f && (showPoster || !hasPicture);
}

bool UIVideoElement::isFrameExact() const{
    return frameExact;
}

HtmlUITexture* UIVideoElement::getTexture() const{
    return liveTexture;
}

double UIVideoElement::getDecodeMsPerFrame() const{
    const VideoStream::Stats s = getStreamStats();
    return s.framesDecoded ? (double)s.decodeNs / 1e6 / (double)s.framesDecoded : 0.0;
}

size_t UIVideoElement::getMemoryBytes() const{
    const VideoStream::Stats s = getStreamStats();
    size_t bytes = s.pictureBytes + s.queueBytes + s.cacheBytes + frame.bytes();
    if (liveTexture){
        bytes += (size_t)textureSize.x * (size_t)textureSize.y * 4;
        if (gpuConversion)
            bytes += frame.bytes();
    }
    if (std::shared_ptr<VideoAudioRing> ring = getAudioRing())
        bytes += ring->getCapacity() * (size_t)ring->getChannels() * sizeof(float);
    return bytes;
}

UIVideoControls* UIVideoElement::getControlsWidget() const{
    return controls.get();
}

std::shared_ptr<VideoAudioRing> UIVideoElement::getAudioRing() const{
    return (stream && metadataSent) ? stream->getAudioRing() : nullptr;
}

double UIVideoElement::getAudioDrift() const{
    return audioDrift;
}
