// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VideoStream.h"
#include "jobs/detail/TimedWait.h"
#include "thread/ThreadUtil.h"

#include "Log.h"

#include <algorithm>
#include <chrono>
#include <cmath>

using namespace jasperengine;

namespace {

    std::atomic<int> decodeDelayMs{0};
    std::atomic<int> liveStreams{0};

    constexpr int64_t AUDIO_PREROLL_NS = 80000000;   // Opus: decode 80 ms before a seek target (RFC 7845 §4.6)
    constexpr auto WAIT_LIMIT = std::chrono::seconds(10);

    int64_t nsToFrames(int64_t ns){
        // rounded to the nearest sample frame at 48 kHz
        const long double frames = (long double)ns * (long double)VideoAudioRing::SAMPLE_RATE / 1e9L;
        return (int64_t)std::llround(frames);
    }

    uint64_t nowNs(){
        return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

}

VideoStream::VideoStream(const std::string& filePath, bool useThread, bool audio):
    path(filePath), threaded(useThread && platformHasThreads()), wantAudio(audio){
    liveStreams++;
    if (threaded)
        worker = jasperengine::thread::spawnNamed("je-video", [this]{ threadMain(); });
}

VideoStream::~VideoStream(){
    {
        std::lock_guard<std::mutex> lock(mutex);
        quit = true;
    }
    workerWake.notify_all();
    frameReady.notify_all();
    if (worker.joinable())
        worker.join();
    liveStreams--;
}

bool VideoStream::platformHasThreads(){
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
    return false;
#else
    return true;
#endif
}

void VideoStream::setDecodeDelayForTesting(int milliseconds){
    decodeDelayMs = std::max(milliseconds, 0);
}

int VideoStream::getLiveCount(){
    return liveStreams;
}

// ---------------------------------------------------------------- worker

void VideoStream::threadMain(){
    for (;;){
        const Work work = step();
        if (work != Work::NONE)
            continue;
        std::unique_lock<std::mutex> lock(mutex);
        if (quit)
            return;
        // the mixer drains the audio ring without telling: look again soon while there is audio
        if (ring && !audioEnd)
            jobs::detail::timedWait(workerWake, lock, std::chrono::milliseconds(10), [this]{ return quit || hasWorkLocked(); });
        else
            workerWake.wait(lock, [this]{ return quit || hasWorkLocked(); });
        if (quit)
            return;
    }
}

bool VideoStream::hasWorkLocked() const{
    if (state == State::OPENING)
        return true;
    if (state == State::FAILED)
        return false;
    if (pendingSeek >= 0 || pendingAudioSync >= 0)
        return true;
    if (info.hasVideo && !videoEnd && (queue.size() < QUEUE_FRAMES || flushPending))
        return true;
    if (ring && audioDecoder && !audioEnd){
        const int64_t ahead = ring->getWritePosition() - ring->getReadPosition();
        if (ahead < (int64_t)(AUDIO_AHEAD * VideoAudioRing::SAMPLE_RATE) && ring->getFreeFrames() >= (size_t)OpusAudioDecoder::MAX_FRAME_SAMPLES)
            return true;
    }
    return false;
}

VideoStream::Work VideoStream::step(){
    std::unique_lock<std::mutex> lock(mutex);
    if (quit)
        return Work::NONE;
    if (state == State::OPENING){
        lock.unlock();
        openNow();
        return Work::DONE;
    }
    if (state == State::FAILED)
        return Work::NONE;
    if (pendingSeek >= 0){
        handleSeek(lock);
        return Work::DONE;
    }
    if (pendingAudioSync >= 0){
        handleAudioSync(lock);
        return Work::DONE;
    }
    // audio first while its buffer is short (a packet costs far less than a frame)
    if (ring && audioDecoder && !audioEnd){
        const int64_t ahead = ring->getWritePosition() - ring->getReadPosition();
        const bool room = ring->getFreeFrames() >= (size_t)OpusAudioDecoder::MAX_FRAME_SAMPLES;
        const bool low = ahead < (int64_t)(AUDIO_AHEAD * VideoAudioRing::SAMPLE_RATE / 2);
        const bool videoWaits = !info.hasVideo || videoEnd || (queue.size() >= QUEUE_FRAMES && !flushPending);
        if (room && (low || (videoWaits && ahead < (int64_t)(AUDIO_AHEAD * VideoAudioRing::SAMPLE_RATE)))){
            const Work work = decodeAudio(lock);
            if (work == Work::DONE)
                return work;
        }
    }
    if (info.hasVideo && !videoEnd && (queue.size() < QUEUE_FRAMES || flushPending))
        return decodeVideo(lock);
    return Work::NONE;
}

void VideoStream::failLocked(MediaErrorCode code, const std::string& message){
    if (!error){
        error.code = code;
        error.message = message;
    }
    state = State::FAILED;
    frameReady.notify_all();
}

bool VideoStream::openNow(){
    if (!demuxer.open(path)){
        std::lock_guard<std::mutex> lock(mutex);
        failLocked(demuxer.getError().code, demuxer.getError().message);
        return false;
    }

    Info found;
    std::unique_ptr<VideoDecoder> videoDecoder;
    std::unique_ptr<OpusAudioDecoder> opusDecoder;
    found.hasVideo = demuxer.hasVideo();
    found.videoCodec = demuxer.getVideoCodecId();
    found.audioCodec = demuxer.getAudioCodecId();
    found.audioIgnored = !found.audioCodec.empty() && !demuxer.hasAudio();
    found.durationNs = demuxer.getDurationNs();
    found.duration = demuxer.getDuration();
    std::string message;
    if (found.hasVideo){
        const VideoTrackInfo& track = demuxer.getVideoTrack();
        found.width = track.width;
        found.height = track.height;
        found.displayWidth = track.displayWidth;
        found.displayHeight = track.displayHeight;
        found.frameCount = (int)demuxer.getVideoPackets().size();
        std::unique_ptr<Av1Decoder> av1 = std::make_unique<Av1Decoder>();
        if (!av1->open(track, message)){
            std::lock_guard<std::mutex> lock(mutex);
            failLocked(MediaErrorCode::DECODE, "'" + path + "': " + message);
            return false;
        }
        found.decoderThreads = av1->getThreadCount();
        videoDecoder = std::move(av1);
    }
    std::shared_ptr<VideoAudioRing> audioRing;
    if (demuxer.hasAudio()){
        found.audioChannels = demuxer.getAudioTrack().channels;
        if (wantAudio && !demuxer.getAudioPackets().empty()){
            std::unique_ptr<OpusAudioDecoder> opus = std::make_unique<OpusAudioDecoder>();
            if (opus->open(demuxer.getAudioTrack(), message)){
                audioRing = std::make_shared<VideoAudioRing>(opus->getOutputChannels(), (size_t)VideoAudioRing::SAMPLE_RATE);
                opusDecoder = std::move(opus);
                found.hasAudio = true;
            }else{
                Log::warn("[HTML UI] <video src=\"%s\">: the audio is not played: %s", path.c_str(), message.c_str());
            }
        }else{
            found.hasAudio = !demuxer.getAudioPackets().empty();
        }
    }
    if (!found.hasVideo && !found.hasAudio){
        std::lock_guard<std::mutex> lock(mutex);
        failLocked(MediaErrorCode::SRC_NOT_SUPPORTED, "'" + path + "' has nothing to play");
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex);
    info = found;
    decoder = std::move(videoDecoder);
    audioDecoder = std::move(opusDecoder);
    ring = audioRing;
    decodeIndex = 0;
    decodeIteration = 0;
    nextSequence = 0;
    audioIndex = 0;
    audioIteration = 0;
    audioRestart = true;
    audioEnd = !audioDecoder;
    opened = true;
    if (state == State::OPENING)
        state = State::READY;
    frameReady.notify_all();
    return true;
}

void VideoStream::requestSeekLocked(int64_t targetSequence){
    pendingSeek = std::max<int64_t>(targetSequence, 0);
    seekGeneration++;
    queue.clear();
    videoEnd = false;
    workerWake.notify_all();
}

void VideoStream::handleSeek(std::unique_lock<std::mutex>& lock){
    const int64_t target = pendingSeek;
    pendingSeek = -1;
    const int count = std::max(info.frameCount, 1);
    const int iteration = (int)(target / count);
    const int index = (int)(target % count);
    const int key = std::max(demuxer.keyframeAtOrBefore(index), 0);
    queue.clear();
    videoEnd = false;
    decodeIndex = key;
    decodeIteration = iteration;
    nextSequence = (int64_t)iteration * count + key;
    discardBelow = target;
    flushPending = true;
    stats.seeks++;
}

int64_t VideoStream::audioFramesPerIteration() const{
    return nsToFrames(info.durationNs);
}

void VideoStream::handleAudioSync(std::unique_lock<std::mutex>& lock){
    const int64_t target = std::max<int64_t>(pendingAudioSync, 0);
    pendingAudioSync = -1;
    audioGeneration++;
    frameReady.notify_all();   // waitAudioSync
    if (!ring || !audioDecoder)
        return;
    ring->reset(target);
    const int64_t perIteration = std::max<int64_t>(audioFramesPerIteration(), 1);
    const int64_t local = loop ? target % perIteration : std::min(target, perIteration);
    audioIteration = loop ? (int)(target / perIteration) : 0;
    const int64_t localNs = (int64_t)((long double)local * 1e9L / VideoAudioRing::SAMPLE_RATE);
    audioIndex = std::max(demuxer.audioPacketAt(localNs - AUDIO_PREROLL_NS), 0);
    audioRestart = true;
    audioEnd = false;
    lock.unlock();
    audioDecoder->reset();
    lock.lock();
}

VideoStream::Work VideoStream::decodeAudio(std::unique_lock<std::mutex>& lock){
    const std::vector<VideoPacketRef>& packets = demuxer.getAudioPackets();
    const int64_t perIteration = audioFramesPerIteration();
    if (audioIndex >= (int)packets.size()){
        if (loop && perIteration > 0){
            // the rest of the iteration is silence (audio shorter than the media), then packet 0 again
            const int64_t iterationEnd = (int64_t)(audioIteration + 1) * perIteration;
            const int64_t written = ring->getWritePosition();
            if (written < iterationEnd){
                std::vector<float> silence((size_t)std::min<int64_t>(iterationEnd - written, OpusAudioDecoder::MAX_FRAME_SAMPLES) *
                                           (size_t)ring->getChannels(), 0.0f);
                ring->write(silence.data(), silence.size() / (size_t)ring->getChannels());
                return Work::DONE;
            }
            audioIndex = 0;
            audioIteration++;
            // the next iteration starts at its time 0 (its pre-skip dropped again)
            audioRestart = true;
            lock.unlock();
            audioDecoder->reset();
            lock.lock();
            return Work::DONE;
        }
        audioEnd = true;
        return Work::NONE;
    }

    const uint64_t generation = audioGeneration;
    const VideoPacketRef packet = packets[(size_t)audioIndex];
    const int iteration = audioIteration;
    const bool last = audioIndex == (int)packets.size() - 1;
    lock.unlock();

    std::string message;
    int frames = -1;
    if (demuxer.read(packet, packetBuffer))
        frames = audioDecoder->decode(packetBuffer.data(), packetBuffer.size(), pcm, message);
    else
        message = demuxer.getError().message;

    lock.lock();
    if (generation != audioGeneration)
        return Work::DONE;
    audioIndex++;
    stats.audioPackets++;
    if (frames < 0){
        // damaged audio: the rest is silence (the picture goes on)
        Log::warn("[HTML UI] <video src=\"%s\">: audio stopped: %s", path.c_str(), message.c_str());
        audioEnd = true;
        return Work::DONE;
    }

    const int channels = ring->getChannels();
    const AudioTrackInfo& track = demuxer.getAudioTrack();
    const int64_t delayNs = track.codecDelayNs > 0 ? track.codecDelayNs : (int64_t)audioDecoder->getPreSkip() * 1000000000LL / 48000;
    int64_t count = frames;
    if (last && packet.discardPaddingNs > 0)
        count = std::max<int64_t>(0, count - nsToFrames(packet.discardPaddingNs));
    // Where the packet goes. Block timestamps have a millisecond resolution, so consecutive packets
    // are placed by counting samples (the ring's write position); the timestamp only decides where
    // a stream starts (time 0, a sync target: the pre-roll before it and the pre-skip are dropped)
    // and real gaps or overlaps (more than SNAP_FRAMES off)
    constexpr int64_t SNAP_FRAMES = 480;
    const int64_t iterationBase = (int64_t)iteration * std::max<int64_t>(perIteration, 0);
    int64_t local = nsToFrames(packet.ptsNs - delayNs);
    const int64_t expected = ring->getWritePosition();
    int64_t global = iterationBase + local;
    if (std::llabs(global - expected) <= SNAP_FRAMES && !audioRestart){
        global = expected;
        local = global - iterationBase;
    }
    const float* data = pcm.data();
    // only this iteration's part of the media (audio longer than the duration is cut)
    if (perIteration > 0 && local + count > perIteration)
        count = std::max<int64_t>(0, perIteration - local);
    // before what was written (the pre-skip at time 0, the pre-roll of a sync): dropped
    if (global < expected){
        const int64_t skip = std::min(expected - global, count);
        data += skip * channels;
        count -= skip;
        global += skip;
    }
    if (count <= 0)
        return Work::DONE;
    // from the first sample written on, packets are placed by counting
    audioRestart = false;
    // a gap before this packet (missing packets): silence
    const int64_t gap = global - expected;
    if (gap > 0){
        std::vector<float> silence((size_t)std::min<int64_t>(gap, (int64_t)ring->getFreeFrames()) * (size_t)channels, 0.0f);
        ring->write(silence.data(), silence.size() / (size_t)channels);
    }
    ring->write(data, (size_t)count);
    return Work::DONE;
}

VideoStream::Work VideoStream::decodeVideo(std::unique_lock<std::mutex>& lock){
    if (flushPending){
        flushPending = false;
        lock.unlock();
        decoder->flush();
        lock.lock();
        return Work::DONE;
    }
    const std::vector<VideoPacketRef>& packets = demuxer.getVideoPackets();
    const int count = (int)packets.size();
    const uint64_t generation = seekGeneration;
    const int index = decodeIndex;
    const int64_t sequence = nextSequence;
    const VideoPacketRef packet = packets[(size_t)index];
    lock.unlock();

    if (decodeDelayMs > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(decodeDelayMs.load()));

    VideoFrame frame;
    bool gotFrame = false;
    std::string message;
    bool ok = demuxer.read(packet, packetBuffer);
    MediaErrorCode code = MediaErrorCode::NETWORK;
    if (ok){
        const uint64_t start = nowNs();
        ok = decoder->decode(packetBuffer.data(), packetBuffer.size(), packet.ptsNs, frame, gotFrame, message);
        code = MediaErrorCode::DECODE;
        lock.lock();
        stats.decodeNs += nowNs() - start;
    }else{
        message = demuxer.getError().message;
        lock.lock();
    }
    stats.cacheBytes = demuxer.getCacheBytes();

    if (generation != seekGeneration)
        return Work::DONE;
    if (!ok){
        failLocked(code, "'" + path + "', frame " + std::to_string(index) + ": " + message);
        return Work::DONE;
    }
    stats.framesDecoded++;

    decodeIndex++;
    nextSequence++;
    if (decodeIndex >= count){
        if (loop){
            // the next iteration starts at frame 0, a keyframe, with no references
            decodeIndex = 0;
            decodeIteration++;
            flushPending = true;
        }else{
            videoEnd = true;
        }
    }
    if (sequence >= discardBelow){
        // no picture for this frame (a temporal unit without a shown frame): an empty entry keeps
        // the numbering, the previous picture stays on screen
        frame.index = index;
        frame.sequence = sequence;
        frame.ptsNs = packet.ptsNs;
        if (!gotFrame){
            frame.y.clear();
            frame.u.clear();
            frame.v.clear();
        }
        queue.push_back(std::move(frame));
        frameReady.notify_all();
    }
    return Work::DONE;
}

// ---------------------------------------------------------------- main thread

VideoStream::State VideoStream::getState() const{
    return state;
}

VideoStream::State VideoStream::waitOpen(){
    if (!threaded){
        while (state == State::OPENING)
            step();
        return state;
    }
    std::unique_lock<std::mutex> lock(mutex);
    jobs::detail::timedWait(frameReady, lock, WAIT_LIMIT, [this]{ return state != State::OPENING || quit; });
    return state;
}

const VideoStream::Info& VideoStream::getInfo() const{
    return info;
}

MediaError VideoStream::getError() const{
    std::lock_guard<std::mutex> lock(mutex);
    return error;
}

void VideoStream::setLoop(bool value){
    std::lock_guard<std::mutex> lock(mutex);
    if (loop == value)
        return;
    loop = value;
    if (state != State::READY)
        return;
    // the last frame was decoded already (the worker stopped there): go on with the next
    // iteration, keeping the frames still waiting
    if (loop && videoEnd && info.frameCount > 0){
        videoEnd = false;
        decodeIndex = 0;
        decodeIteration++;
        nextSequence = (int64_t)decodeIteration * info.frameCount;
        flushPending = true;
    }
    if (loop && audioEnd && audioDecoder)
        audioEnd = false;
    workerWake.notify_all();
}

int VideoStream::frameIndexFor(double seconds) const{
    // once opened the index stays (frames before a decode error are still served)
    if (!opened || !info.hasVideo)
        return -1;
    const int64_t ns = (int64_t)std::floor(std::max(seconds, 0.0) * 1e9 + 0.5);
    return demuxer.videoFrameAt(ns);
}

int64_t VideoStream::sequenceFor(int iteration, double seconds) const{
    const int index = frameIndexFor(seconds);
    if (index < 0)
        return -1;
    return (int64_t)std::max(iteration, 0) * info.frameCount + index;
}

double VideoStream::keyframeTimeAtOrBefore(double seconds) const{
    const int index = frameIndexFor(seconds);
    if (index < 0)
        return 0.0;
    const int key = demuxer.keyframeAtOrBefore(index);
    return key < 0 ? 0.0 : (double)demuxer.getVideoPackets()[(size_t)key].ptsNs / 1e9;
}

VideoStream::FrameResult VideoStream::acquireFrame(int iteration, double seconds, bool wait, int64_t shownSequence, VideoFrame& frame){
    if (state == State::OPENING){
        if (!wait){
            pump(1);
            if (state == State::OPENING)
                return FrameResult::NOT_READY;
        }else if (waitOpen() == State::OPENING){
            return FrameResult::NOT_READY;
        }
    }
    if (!info.hasVideo)
        return state == State::FAILED ? FrameResult::FAILED : FrameResult::SAME;

    const int64_t target = sequenceFor(iteration, seconds);
    if (target < 0)
        return FrameResult::NOT_READY;
    if (target == shownSequence)
        return FrameResult::SAME;
    const int count = std::max(info.frameCount, 1);
    // real time: half a second behind the decoder skips ahead
    const int64_t behindLimit = std::max<int64_t>(SEEK_AHEAD_FRAMES * 2, info.durationNs > 0 ?
        (int64_t)std::ceil(0.5 * count / ((double)info.durationNs / 1e9)) : 15);

    const auto deadline = std::chrono::steady_clock::now() + WAIT_LIMIT;
    std::unique_lock<std::mutex> lock(mutex);
    for (;;){
        // keep the newest frame at or before the target; the older ones were never shown
        while (queue.size() >= 2 && queue[1].sequence <= target){
            if (queue.front().sequence > shownSequence)
                stats.framesDropped++;
            queue.pop_front();
        }
        if (!queue.empty() && queue.front().sequence == target){
            frame = std::move(queue.front());
            queue.pop_front();
            workerWake.notify_all();
            return FrameResult::NEW;
        }
        if (state == State::FAILED)
            return FrameResult::FAILED;

        // can the worker get there by decoding on? Behind what it has: seek. Ahead: seek only
        // when the target's keyframe is past where the decoder is (that skips work); in real
        // time, when it is more than half a second behind, skip to the next keyframe (its frame
        // shows when the clock gets there; the frames in between are dropped)
        const bool behind = !queue.empty() ? target < queue.front().sequence : (pendingSeek >= 0 ? target < pendingSeek : target < nextSequence);
        const int64_t from = pendingSeek >= 0 ? pendingSeek : nextSequence;
        const int64_t iterationStart = (target / count) * count;
        const int targetIndex = (int)(target % count);
        int64_t seekTo = -1;
        if (behind){
            seekTo = target;
        }else if (target > from + SEEK_AHEAD_FRAMES){
            const int64_t keyBefore = iterationStart + std::max(demuxer.keyframeAtOrBefore(targetIndex), 0);
            if (keyBefore > from)
                seekTo = target;
            if (!wait && target - from > behindLimit){
                const int keyAfter = demuxer.keyframeAfter(targetIndex);
                seekTo = keyAfter > 0 ? iterationStart + keyAfter : target;
            }
        }
        if (seekTo >= 0 && pendingSeek != seekTo)
            requestSeekLocked(seekTo);
        if (videoEnd && queue.empty() && target >= nextSequence)
            return FrameResult::SAME;   // nothing will come (the target is past the last frame)

        if (!wait){
            if (!queue.empty() && queue.front().sequence < target && queue.front().sequence > shownSequence){
                frame = std::move(queue.front());
                queue.pop_front();
                workerWake.notify_all();
                return FrameResult::STALE;
            }
            if (!threaded){
                lock.unlock();
                pump(2);
                lock.lock();
                if (!queue.empty() && queue.front().sequence <= target)
                    continue;
            }
            return FrameResult::NOT_READY;
        }

        if (threaded){
            workerWake.notify_all();
            jobs::detail::timedWait(frameReady, lock, deadline - std::chrono::steady_clock::now());
            if (std::chrono::steady_clock::now() >= deadline){
                Log::error("[HTML UI] <video src=\"%s\">: frame %lld was not decoded in time", path.c_str(), (long long)target);
                return FrameResult::NOT_READY;
            }
        }else{
            lock.unlock();
            step();
            lock.lock();
        }
        if (quit)
            return FrameResult::NOT_READY;
    }
}

void VideoStream::syncAudio(int64_t frames){
    std::lock_guard<std::mutex> lock(mutex);
    if (!ring)
        return;
    pendingAudioSync = std::max<int64_t>(frames, 0);
    audioGeneration++;
    workerWake.notify_all();
}

bool VideoStream::isAudioSyncPending() const{
    std::lock_guard<std::mutex> lock(mutex);
    return pendingAudioSync >= 0;
}

void VideoStream::waitAudioSync(){
    if (!threaded){
        while (isAudioSyncPending() && state == State::READY)
            step();
        return;
    }
    std::unique_lock<std::mutex> lock(mutex);
    workerWake.notify_all();
    jobs::detail::timedWait(frameReady, lock, WAIT_LIMIT, [this]{ return pendingAudioSync < 0 || quit || state != State::READY; });
}

std::shared_ptr<VideoAudioRing> VideoStream::getAudioRing() const{
    std::lock_guard<std::mutex> lock(mutex);
    return ring;
}

bool VideoStream::pump(int steps){
    if (threaded)
        return false;
    for (int i = 0; i < steps; i++){
        if (step() == Work::NONE)
            return false;
    }
    std::lock_guard<std::mutex> lock(mutex);
    return hasWorkLocked();
}

VideoStream::Stats VideoStream::getStats() const{
    std::lock_guard<std::mutex> lock(mutex);
    Stats copy = stats;
    copy.pictureBytes = decoder ? decoder->getPictureBytes() : 0;
    copy.queueBytes = 0;
    for (const VideoFrame& frame : queue)
        copy.queueBytes += frame.bytes();
    return copy;
}

size_t VideoStream::getQueuedFrames() const{
    std::lock_guard<std::mutex> lock(mutex);
    return queue.size();
}
