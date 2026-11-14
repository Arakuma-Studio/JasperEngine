// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PlatformVideoSource.h"

#include "Log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

using namespace jasperengine;

namespace {

    std::atomic<int> liveSources{0};

    constexpr auto WAIT_LIMIT = std::chrono::seconds(10);
    // a frame-exact frame (fixed time steps) is waited for at most this long, then the caller's
    // clock waits a frame (no main-thread freeze on a decoder that stopped)
    constexpr auto FRAME_WAIT_LIMIT = std::chrono::seconds(1);
    // audio is decoded while this much of the ring is free (a platform decoder's chunk is far
    // smaller; the rest of a larger one waits in `pending`)
    constexpr size_t AUDIO_MIN_FREE = 9600;
    constexpr int64_t SNAP_FRAMES = 480;
    // gap entries a missing stretch of frames may get at most (a damaged or variable-rate file)
    constexpr int64_t MAX_GAP_FILL = 600;

    int64_t secondsToFrames(double seconds){
        return (int64_t)std::llround(seconds * VideoAudioRing::SAMPLE_RATE);
    }

    uint64_t nowNs(){
        return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void copyPlane(uint8_t* dst, int dstStride, const uint8_t* src, int srcStride, int width, int height){
        if (dstStride == srcStride && dstStride == width){
            std::memcpy(dst, src, (size_t)width * (size_t)height);
            return;
        }
        for (int row = 0; row < height; row++)
            std::memcpy(dst + (size_t)row * dstStride, src + (ptrdiff_t)row * srcStride, (size_t)width);
    }

    // 16-bit little-endian samples to 8 bits, rounded
    void copyPlane16(uint8_t* dst, const uint8_t* src, int srcStride, int width, int height, int bits, bool msb){
        const int shift = msb ? 8 : bits - 8;
        const int round = shift > 0 ? 1 << (shift - 1) : 0;
        for (int row = 0; row < height; row++){
            const uint8_t* line = src + (ptrdiff_t)row * srcStride;
            uint8_t* out = dst + (size_t)row * width;
            for (int x = 0; x < width; x++){
                const int value = line[2 * x] | (line[2 * x + 1] << 8);
                out[x] = (uint8_t)std::min((value + round) >> shift, 255);
            }
        }
    }

}

// ---------------------------------------------------------------- planes

void VideoFramePlanes::prepare(VideoFrame& frame, int width, int height){
    frame.width = width;
    frame.height = height;
    frame.chromaShiftX = 1;
    frame.chromaShiftY = 1;
    frame.bitDepth = std::max(frame.bitDepth, 8);
    frame.y.resize((size_t)width * (size_t)height);
    const size_t chroma = (size_t)frame.chromaWidth() * (size_t)frame.chromaHeight();
    frame.u.resize(chroma);
    frame.v.resize(chroma);
}

void VideoFramePlanes::copyI420(VideoFrame& frame, int width, int height, const uint8_t* y, int yStride,
                                const uint8_t* u, int uStride, const uint8_t* v, int vStride){
    prepare(frame, width, height);
    const int cw = frame.chromaWidth(), ch = frame.chromaHeight();
    copyPlane(frame.y.data(), width, y, yStride, width, height);
    copyPlane(frame.u.data(), cw, u, uStride, cw, ch);
    copyPlane(frame.v.data(), cw, v, vStride, cw, ch);
}

void VideoFramePlanes::copyNV12(VideoFrame& frame, int width, int height, const uint8_t* y, int yStride,
                                const uint8_t* uv, int uvStride, bool vu){
    prepare(frame, width, height);
    const int cw = frame.chromaWidth(), ch = frame.chromaHeight();
    copyPlane(frame.y.data(), width, y, yStride, width, height);
    uint8_t* u = frame.u.data();
    uint8_t* v = frame.v.data();
    if (vu)
        std::swap(u, v);
    // de-interleaved on the worker (about 0.3 ms for 720p): the Yuv pass reads three R8 planes
    for (int row = 0; row < ch; row++){
        const uint8_t* line = uv + (ptrdiff_t)row * uvStride;
        uint8_t* ou = u + (size_t)row * cw;
        uint8_t* ov = v + (size_t)row * cw;
        for (int x = 0; x < cw; x++){
            ou[x] = line[2 * x];
            ov[x] = line[2 * x + 1];
        }
    }
}

void VideoFramePlanes::copyI420x16(VideoFrame& frame, int width, int height, int bits, bool msb, const uint8_t* y, int yStride,
                                   const uint8_t* u, int uStride, const uint8_t* v, int vStride){
    prepare(frame, width, height);
    frame.bitDepth = bits;
    const int cw = frame.chromaWidth(), ch = frame.chromaHeight();
    copyPlane16(frame.y.data(), y, yStride, width, height, bits, msb);
    copyPlane16(frame.u.data(), u, uStride, cw, ch, bits, msb);
    copyPlane16(frame.v.data(), v, vStride, cw, ch, bits, msb);
}

void VideoFramePlanes::copyP01x(VideoFrame& frame, int width, int height, int bits, const uint8_t* y, int yStride,
                                const uint8_t* uv, int uvStride){
    prepare(frame, width, height);
    frame.bitDepth = bits;
    const int cw = frame.chromaWidth(), ch = frame.chromaHeight();
    // P010/P016: the significant bits at the top of each 16-bit sample
    copyPlane16(frame.y.data(), y, yStride, width, height, bits, true);
    for (int row = 0; row < ch; row++){
        const uint8_t* line = uv + (ptrdiff_t)row * uvStride;
        uint8_t* ou = frame.u.data() + (size_t)row * cw;
        uint8_t* ov = frame.v.data() + (size_t)row * cw;
        for (int x = 0; x < cw; x++){
            const int cu = line[4 * x] | (line[4 * x + 1] << 8);
            const int cv = line[4 * x + 2] | (line[4 * x + 3] << 8);
            ou[x] = (uint8_t)std::min((cu + 128) >> 8, 255);
            ov[x] = (uint8_t)std::min((cv + 128) >> 8, 255);
        }
    }
}

namespace {

    // one plane turned clockwise
    std::vector<uint8_t> rotatePlane(const std::vector<uint8_t>& plane, int width, int height, int degrees){
        std::vector<uint8_t> out(plane.size());
        if (degrees == 180){
            for (int y = 0; y < height; y++){
                const uint8_t* src = plane.data() + (size_t)y * width;
                uint8_t* dst = out.data() + (size_t)(height - 1 - y) * width;
                for (int x = 0; x < width; x++)
                    dst[width - 1 - x] = src[x];
            }
        }else if (degrees == 90){
            // (x, y) -> (height - 1 - y, x) in a height x width image
            for (int y = 0; y < height; y++){
                const uint8_t* src = plane.data() + (size_t)y * width;
                for (int x = 0; x < width; x++)
                    out[(size_t)x * height + (height - 1 - y)] = src[x];
            }
        }else{
            // 270: (x, y) -> (y, width - 1 - x)
            for (int y = 0; y < height; y++){
                const uint8_t* src = plane.data() + (size_t)y * width;
                for (int x = 0; x < width; x++)
                    out[(size_t)(width - 1 - x) * height + y] = src[x];
            }
        }
        return out;
    }

}

void VideoFramePlanes::rotate(VideoFrame& frame, int degrees){
    degrees = ((degrees % 360) + 360) % 360;
    if (degrees != 90 && degrees != 180 && degrees != 270)
        return;
    const int cw = frame.chromaWidth(), ch = frame.chromaHeight();
    frame.y = rotatePlane(frame.y, frame.width, frame.height, degrees);
    if (frame.hasChroma()){
        frame.u = rotatePlane(frame.u, cw, ch, degrees);
        frame.v = rotatePlane(frame.v, cw, ch, degrees);
    }
    if (degrees != 180){
        std::swap(frame.width, frame.height);
        std::swap(frame.chromaShiftX, frame.chromaShiftY);
        // an odd size's chroma covers a little more than the picture: still ceil(size / 2)
        if (frame.hasChroma() && ((size_t)frame.chromaWidth() * frame.chromaHeight() != frame.u.size())){
            // (4:2:0 of odd sizes: the turned chroma plane is ch x cw, which is what the
            // swapped size asks for; this only guards unexpected layouts)
            frame.u.resize((size_t)frame.chromaWidth() * frame.chromaHeight(), 128);
            frame.v.resize((size_t)frame.chromaWidth() * frame.chromaHeight(), 128);
        }
    }
}

// ---------------------------------------------------------------- resampler

void AudioResampler48k::reset(int inputRate, int inputChannels){
    rate = std::max(inputRate, 1);
    channels = std::max(inputChannels, 1);
    flush();
}

void AudioResampler48k::flush(){
    position = 0.0;
    hasLast = false;
    last[0] = last[1] = 0.0f;
}

void AudioResampler48k::processInt16(const int16_t* interleaved, size_t frames, std::vector<float>& out){
    std::vector<float> floats(frames * (size_t)channels);
    for (size_t i = 0; i < floats.size(); i++)
        floats[i] = interleaved[i] / 32768.0f;
    process(floats.data(), frames, out);
}

void AudioResampler48k::process(const float* interleaved, size_t frames, std::vector<float>& out){
    // to stereo first: mono doubled, more channels down-mixed to front left/right (+ centre)
    stereo.resize(frames * 2);
    for (size_t i = 0; i < frames; i++){
        const float* in = interleaved + i * (size_t)channels;
        float l, r;
        if (channels == 1){
            l = r = in[0];
        }else if (channels == 2){
            l = in[0];
            r = in[1];
        }else{
            const float centre = channels >= 3 ? in[2] * 0.7071f : 0.0f;
            l = in[0] + centre;
            r = in[1] + centre;
        }
        stereo[2 * i] = l;
        stereo[2 * i + 1] = r;
    }
    if (rate == VideoAudioRing::SAMPLE_RATE){
        out.insert(out.end(), stereo.begin(), stereo.end());
        if (frames > 0){
            last[0] = stereo[2 * (frames - 1)];
            last[1] = stereo[2 * (frames - 1) + 1];
            hasLast = true;
        }
        return;
    }
    // linear interpolation; the frame before this chunk is `last` (index -1)
    const double step = (double)rate / VideoAudioRing::SAMPLE_RATE;
    auto sample = [&](long index, int channel) -> float {
        if (index < 0)
            return hasLast ? last[channel] : stereo.empty() ? 0.0f : stereo[(size_t)channel];
        return stereo[(size_t)index * 2 + (size_t)channel];
    };
    while (position < (double)frames - 1.0 || (position < (double)frames && frames == 1 && position < 0.0)){
        const long i0 = (long)std::floor(position);
        const double t = position - (double)i0;
        for (int c = 0; c < 2; c++){
            const float a = sample(i0, c);
            const float b = sample(i0 + 1, c);
            out.push_back((float)(a + (b - a) * t));
        }
        position += step;
    }
    if (frames > 0){
        last[0] = stereo[2 * (frames - 1)];
        last[1] = stereo[2 * (frames - 1) + 1];
        hasLast = true;
        // continue from the last input frame (index -1 of the next chunk)
        position -= (double)frames;
    }
}

// ---------------------------------------------------------------- PlatformVideoSource

PlatformVideoSource::PlatformVideoSource(std::shared_ptr<MediaByteSource> source, const Mp4ProbeResult* mp4, PlatformBackendFactory makeBackend,
                                         const char* backend, bool audioWanted):
    bytes(std::move(source)), factory(std::move(makeBackend)), name(backend), wantAudio(audioWanted){
    if (mp4){
        probe = *mp4;
        hasProbe = true;
    }
    liveSources++;
    worker = std::thread([this]{ threadMain(); });
}

PlatformVideoSource::~PlatformVideoSource(){
    {
        std::lock_guard<std::mutex> lock(mutex);
        quit = true;
    }
    // a decoder waiting for download bytes wakes up and fails its read
    if (bytes)
        bytes->abort();
    workerWake.notify_all();
    frameReady.notify_all();
    if (worker.joinable())
        worker.join();
    if (video)
        video->close();
    if (audio)
        audio->close();
    liveSources--;
}

int PlatformVideoSource::getLiveCount(){
    return liveSources;
}

const char* PlatformVideoSource::backendName() const{
    return name;
}

bool PlatformVideoSource::isFrameExact() const{
    return true;
}

bool PlatformVideoSource::hasExactIndex() const{
    return exactIndex;
}

// ---------------------------------------------------------------- the index

int PlatformVideoSource::indexOf(int64_t ptsNs) const{
    if (exactIndex){
        int i = index.frameAt(ptsNs);
        // the nearest (decoders round timestamps to their own units)
        if (i >= 0 && i + 1 < (int)index.ptsNs.size() &&
            std::llabs(index.ptsNs[(size_t)i + 1] - ptsNs) < std::llabs(ptsNs - index.ptsNs[(size_t)i]))
            i++;
        return i;
    }
    const double rate = info.frameRate > 0.0 ? info.frameRate : 30.0;
    const int count = std::max(info.frameCount, 1);
    const int i = (int)std::floor((double)ptsNs * rate / 1e9 + 1e-6);
    return std::clamp(i, 0, count - 1);
}

int64_t PlatformVideoSource::ptsOf(int frame) const{
    if (exactIndex && !index.ptsNs.empty())
        return index.ptsNs[(size_t)std::clamp(frame, 0, (int)index.ptsNs.size() - 1)];
    const double rate = info.frameRate > 0.0 ? info.frameRate : 30.0;
    return (int64_t)std::llround((double)std::max(frame, 0) * 1e9 / rate);
}

int PlatformVideoSource::keyframeAtOrBefore(int frame) const{
    if (exactIndex)
        return std::max(index.keyframeAtOrBefore(frame), 0);
    // unknown keyframes: the decoder seeks to the one before the time itself
    return std::max(frame, 0);
}

int PlatformVideoSource::keyframeAfter(int frame) const{
    return exactIndex ? index.keyframeAfter(frame) : -1;
}

int64_t PlatformVideoSource::audioFramesPerIteration() const{
    return (int64_t)std::llround((long double)info.durationNs * VideoAudioRing::SAMPLE_RATE / 1e9L);
}

// ---------------------------------------------------------------- worker

void PlatformVideoSource::threadMain(){
    for (;;){
        const Work work = step();
        if (work == Work::DONE)
            continue;
        std::unique_lock<std::mutex> lock(mutex);
        if (work == Work::IDLE){
            // the decoder is waiting for data (a hardware decoder's output, a download): a short
            // nap, woken at once by a seek or the end
            workerWake.wait_for(lock, std::chrono::milliseconds(2), [this]{ return quit || pendingSeek >= 0 || pendingAudioSync >= 0; });
            if (quit)
                return;
            continue;
        }
        if (quit)
            return;
        if (ring && !audioEnd)
            workerWake.wait_for(lock, std::chrono::milliseconds(10), [this]{ return quit || hasWorkLocked(); });
        else
            workerWake.wait(lock, [this]{ return quit || hasWorkLocked(); });
        if (quit)
            return;
    }
}

bool PlatformVideoSource::hasWorkLocked() const{
    if (state == State::OPENING)
        return true;
    if (state == State::FAILED)
        return false;
    if (pendingSeek >= 0 || pendingAudioSync >= 0)
        return true;
    if (info.hasVideo && video && !videoEnd && queue.size() < QUEUE_FRAMES)
        return true;
    if (ring && audio && !audioEnd){
        const int64_t ahead = ring->getWritePosition() - ring->getReadPosition();
        if (ahead < (int64_t)(AUDIO_AHEAD * VideoAudioRing::SAMPLE_RATE) && ring->getFreeFrames() >= AUDIO_MIN_FREE)
            return true;
    }
    return false;
}

PlatformVideoSource::Work PlatformVideoSource::step(){
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
    if (ring && audio && !audioEnd){
        const int64_t ahead = ring->getWritePosition() - ring->getReadPosition();
        const bool room = ring->getFreeFrames() >= AUDIO_MIN_FREE;
        const bool low = ahead < (int64_t)(AUDIO_AHEAD * VideoAudioRing::SAMPLE_RATE / 2);
        const bool videoWaits = !info.hasVideo || !video || videoEnd || queue.size() >= QUEUE_FRAMES;
        if (room && (low || (videoWaits && ahead < (int64_t)(AUDIO_AHEAD * VideoAudioRing::SAMPLE_RATE)))){
            const Work work = decodeAudio(lock);
            if (work == Work::DONE)
                return work;
        }
    }
    if (info.hasVideo && video && !videoEnd && queue.size() < QUEUE_FRAMES)
        return decodeVideo(lock);
    return Work::NONE;
}

void PlatformVideoSource::failLocked(MediaErrorCode code, const std::string& message){
    if (!error){
        error.code = code;
        error.message = message;
    }
    state = State::FAILED;
    frameReady.notify_all();
}

bool PlatformVideoSource::openBackend(PlatformDecoderBackend::Stream stream, std::unique_ptr<PlatformDecoderBackend>& out,
                                      VideoInfo& found, MediaError& failure){
    for (int attempt = 0; attempt < 2; attempt++){
        std::unique_ptr<PlatformDecoderBackend> backend = factory ? factory() : nullptr;
        if (!backend){
            failure.code = MediaErrorCode::SRC_NOT_SUPPORTED;
            failure.message = "no video decoder on this platform";
            return false;
        }
        failure = MediaError();
        if (backend->open(*bytes, stream, found, failure)){
            out = std::move(backend);
            return true;
        }
        const bool limited = backend->wasResourceLimited();
        backend->close();
        if (!limited)
            return false;
        if (attempt == 0){
            // the device's decoder instances are all in use: one more try after another player let go
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        failure.code = MediaErrorCode::DECODE;
        failure.message = "too many videos playing at once (the device's video decoders are all in use)";
    }
    return false;
}

void PlatformVideoSource::openNow(){
    VideoInfo found;
    MediaError failure;
    const std::string& path = bytes->path();

    // the frame index of an MP4 (its sample tables): frame numbers and keyframes exactly
    Mp4VideoIndex built;
    bool exact = false;
    if (hasProbe && probe.ok && probe.video.kind == Mp4Track::Kind::VIDEO && !probe.fragmented){
        std::string message;
        exact = Mp4Probe::buildVideoIndex(*bytes, probe, built, message);
        if (!exact)
            Log::verbose("[Video] '%s': no frame index (%s): frame numbers are estimated from the frame rate", path.c_str(), message.c_str());
    }

    std::unique_ptr<PlatformDecoderBackend> videoBackend;
    std::unique_ptr<PlatformDecoderBackend> audioBackend;
    const bool videoTrack = !hasProbe || probe.video.kind == Mp4Track::Kind::VIDEO;
    const bool audioTrack = !hasProbe || probe.audio.kind == Mp4Track::Kind::AUDIO;
    if (videoTrack){
        if (!openBackend(PlatformDecoderBackend::Stream::VIDEO, videoBackend, found, failure)){
            // a file without a picture is still played for its sound (an audio-only MP4)
            if (hasProbe || failure.code != MediaErrorCode::SRC_NOT_SUPPORTED || failure.message.find("no video") == std::string::npos){
                std::lock_guard<std::mutex> lock(mutex);
                failLocked(failure.code == MediaErrorCode::NONE ? MediaErrorCode::DECODE : failure.code, "'" + path + "': " + failure.message);
                return;
            }
        }else{
            found.hasVideo = true;
        }
    }
    std::shared_ptr<VideoAudioRing> audioRing;
    if (audioTrack && wantAudio){
        VideoInfo audioInfo = found;
        MediaError audioFailure;
        if (openBackend(PlatformDecoderBackend::Stream::AUDIO, audioBackend, audioInfo, audioFailure)){
            found.hasAudio = true;
            found.audioChannels = audioInfo.audioChannels;
            found.audioCodec = audioInfo.audioCodec;
            if (found.durationNs <= 0)
                found.durationNs = audioInfo.durationNs;
            audioRing = std::make_shared<VideoAudioRing>(2, (size_t)VideoAudioRing::SAMPLE_RATE);
        }else if (hasProbe && probe.audio.kind == Mp4Track::Kind::AUDIO){
            found.audioCodec = probe.audio.fourcc;
            found.audioIgnored = true;
            Log::warn("[Video] '%s': the audio is not played: %s", path.c_str(), audioFailure.message.c_str());
        }
    }else if (hasProbe && probe.audio.kind == Mp4Track::Kind::AUDIO){
        found.audioCodec = probe.audio.fourcc;
        found.audioChannels = probe.audio.channels;
        found.hasAudio = true;
    }
    if (!found.hasVideo && !found.hasAudio){
        std::lock_guard<std::mutex> lock(mutex);
        failLocked(MediaErrorCode::SRC_NOT_SUPPORTED, "'" + path + "' has nothing to play");
        return;
    }

    // what the container says wins over the decoder's guesses
    if (hasProbe && probe.ok){
        found.container = probe.container;
        if (probe.duration > 0.0)
            found.durationNs = (int64_t)std::llround(probe.duration * 1e9);
        if (probe.video.kind == Mp4Track::Kind::VIDEO){
            found.videoCodec = probe.video.fourcc;
            if (probe.video.frameRate > 0.0)
                found.frameRate = probe.video.frameRate;
            found.rotation = probe.video.rotation;
        }
        if (probe.audio.kind == Mp4Track::Kind::AUDIO)
            found.audioCodec = probe.audio.fourcc;
    }
    found.duration = (double)found.durationNs / 1e9;
    if (found.hasVideo){
        if (found.displayWidth <= 0 || found.displayHeight <= 0){
            found.displayWidth = found.width;
            found.displayHeight = found.height;
        }
        // shown turned: the display size is the turned one
        if (found.rotation == 90 || found.rotation == 270)
            std::swap(found.displayWidth, found.displayHeight);
        if (exact){
            found.frameCount = (int)built.ptsNs.size();
            if (found.frameRate <= 0.0 && found.duration > 0.0)
                found.frameRate = found.frameCount / found.duration;
        }else{
            if (found.frameRate <= 0.0)
                found.frameRate = 30.0;
            found.frameCount = std::max(1, (int)std::lround(found.duration * found.frameRate));
        }
        found.decoderThreads = videoBackend ? videoBackend->getDecoderThreads() : 0;
    }

    // The first picture: the decoder's time origin against the index's (some decoders apply an
    // edit list, others give the composition times), learnt before any seek can move it
    VideoFrame first;
    bool hasFirst = false;
    int64_t bias = 0;
    if (videoBackend){
        // up to 2 s of decoding (hardware decoders take a while to give their first picture);
        // a growing download blocks in its reads meanwhile, so it gets as long as it needs to
        // arrive, the closing of the source ending the wait
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(bytes->isGrowing() ? 120 : 2);
        while (std::chrono::steady_clock::now() < deadline){
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (quit)
                    return;
            }
            std::vector<float> unused;
            double unusedStart = 0.0;
            MediaError failure;
            VideoFrame frame;
            const PlatformDecoderBackend::Step result = videoBackend->decodeNext(frame, unused, unusedStart, failure);
            if (result == PlatformDecoderBackend::Step::FRAME){
                if (frame.ptsNs < 0)
                    continue;       // pre-roll before the first frame shown
                bias = frame.ptsNs - (exact && !built.ptsNs.empty() ? built.ptsNs[0] : 0);
                frame.ptsNs -= bias;
                if (found.rotation != 0)
                    VideoFramePlanes::rotate(frame, found.rotation);
                first = std::move(frame);
                hasFirst = true;
                break;
            }
            if (result == PlatformDecoderBackend::Step::AGAIN){
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (result == PlatformDecoderBackend::Step::FAILURE){
                std::lock_guard<std::mutex> lock(mutex);
                failLocked(failure.code == MediaErrorCode::NONE ? MediaErrorCode::DECODE : failure.code, "'" + path + "': " + failure.message);
                return;
            }
            break;      // END: no picture at all
        }
    }

    std::lock_guard<std::mutex> lock(mutex);
    info = found;
    index = std::move(built);
    exactIndex = exact;
    video = std::move(videoBackend);
    audio = std::move(audioBackend);
    ring = audioRing;
    decodeIteration = 0;
    nextSequence = 0;
    lastPushed = -1;
    ptsBias = bias;
    audioIteration = 0;
    audioRestart = true;
    audioEnd = !audio;
    if (hasFirst){
        stats.framesDecoded++;
        const int firstIndex = indexOf(std::max<int64_t>(first.ptsNs, 0));
        nextSequence = firstIndex + 1;
        pushFrame(first, firstIndex);
    }else if (video){
        videoEnd = true;
    }
    opened = true;
    if (state == State::OPENING)
        state = State::READY;
    frameReady.notify_all();
}

void PlatformVideoSource::requestSeekLocked(int64_t targetSequence){
    pendingSeek = std::max<int64_t>(targetSequence, 0);
    seekGeneration++;
    queue.clear();
    videoEnd = false;
    workerWake.notify_all();
}

void PlatformVideoSource::handleSeek(std::unique_lock<std::mutex>& lock){
    const int64_t target = pendingSeek;
    pendingSeek = -1;
    const int count = std::max(info.frameCount, 1);
    const int iteration = (int)(target / count);
    const int frameIndex = (int)(target % count);
    const int key = keyframeAtOrBefore(frameIndex);
    queue.clear();
    videoEnd = false;
    decodeIteration = iteration;
    discardBelow = target;
    lastPushed = (int64_t)iteration * count + key - 1;
    nextSequence = (int64_t)iteration * count + key;
    queuedSinceSeek = false;
    lastDiscarded = VideoFrame();
    restartPending = false;
    stats.seeks++;
    const double seconds = (double)(ptsOf(key) + ptsBias) / 1e9;
    const uint64_t generation = seekGeneration;
    lock.unlock();
    const bool ok = video ? video->seek(std::max(seconds, 0.0)) : true;
    lock.lock();
    if (!ok && generation == seekGeneration)
        failLocked(MediaErrorCode::DECODE, "'" + bytes->path() + "': the decoder could not seek to " + std::to_string(seconds) + " s");
}

void PlatformVideoSource::pushFrame(VideoFrame& frame, int64_t sequence){
    // frames the decoder never gave (a dropped frame, an estimated index): the entries in between
    // keep the numbering and show the previous picture
    const int64_t gapStart = std::max(lastPushed + 1, discardBelow);
    if (sequence > gapStart && sequence - gapStart <= MAX_GAP_FILL){
        for (int64_t gap = gapStart; gap < sequence; gap++){
            VideoFrame entry;
            // right after a seek the picture on screen is not the one before the gap: the
            // last frame decoded on the way there is
            if (!queuedSinceSeek && !lastDiscarded.y.empty())
                entry = std::move(lastDiscarded);
            queuedSinceSeek = true;
            entry.sequence = gap;
            entry.index = (int)(gap % std::max(info.frameCount, 1));
            queue.push_back(std::move(entry));
        }
    }
    queuedSinceSeek = true;
    lastDiscarded = VideoFrame();
    frame.sequence = sequence;
    frame.index = (int)(sequence % std::max(info.frameCount, 1));
    queue.push_back(std::move(frame));
    lastPushed = sequence;
    frameReady.notify_all();
}

bool PlatformVideoSource::restartLocked(std::unique_lock<std::mutex>& lock, uint64_t generation){
    // the next iteration from frame 0: the frames still queued stay
    const int count = std::max(info.frameCount, 1);
    restartPending = false;
    decodeIteration++;
    discardBelow = (int64_t)decodeIteration * count;
    lastPushed = std::max(lastPushed, discardBelow - 1);
    nextSequence = discardBelow;
    lock.unlock();
    const bool ok = video->seek(std::max((double)ptsBias / 1e9, 0.0));
    lock.lock();
    if (!ok && generation == seekGeneration){
        failLocked(MediaErrorCode::DECODE, "'" + bytes->path() + "': the decoder could not loop to the start");
        return false;
    }
    return true;
}

PlatformVideoSource::Work PlatformVideoSource::decodeVideo(std::unique_lock<std::mutex>& lock){
    const uint64_t generation = seekGeneration;
    if (restartPending){
        restartLocked(lock, generation);
        return Work::DONE;
    }
    lock.unlock();

    VideoFrame frame;
    std::vector<float> unused;
    double unusedStart = 0.0;
    MediaError failure;
    const uint64_t start = nowNs();
    const PlatformDecoderBackend::Step result = video->decodeNext(frame, unused, unusedStart, failure);
    const uint64_t elapsed = nowNs() - start;
    if (result == PlatformDecoderBackend::Step::FRAME && info.rotation != 0)
        VideoFramePlanes::rotate(frame, info.rotation);

    lock.lock();
    stats.decodeNs += elapsed;
    if (generation != seekGeneration)
        return Work::DONE;
    const int count = std::max(info.frameCount, 1);
    switch (result){
        case PlatformDecoderBackend::Step::AGAIN:
            return Work::IDLE;
        case PlatformDecoderBackend::Step::FAILURE:
            failLocked(failure.code == MediaErrorCode::NONE ? MediaErrorCode::DECODE : failure.code, "'" + bytes->path() + "': " + failure.message);
            return Work::DONE;
        case PlatformDecoderBackend::Step::END:
            if (loop){
                restartLocked(lock, generation);
            }else{
                videoEnd = true;
                frameReady.notify_all();
            }
            return Work::DONE;
        case PlatformDecoderBackend::Step::AUDIO:
            return Work::DONE;
        case PlatformDecoderBackend::Step::FRAME:
            break;
    }
    stats.framesDecoded++;
    const int64_t pts = frame.ptsNs - ptsBias;
    const int64_t halfFrame = (int64_t)(5e8 / (info.frameRate > 0.0 ? info.frameRate : 30.0));
    if (pts < -halfFrame)
        return Work::DONE;
    const int frameIndex = indexOf(std::max<int64_t>(pts, 0));
    const int64_t sequence = (int64_t)decodeIteration * count + frameIndex;
    nextSequence = std::max(nextSequence, sequence + 1);
    // a frame the queue is past (a duplicate timestamp) or one a seek skips
    if (sequence <= lastPushed){
        return Work::DONE;
    }
    if (sequence < discardBelow){
        lastPushed = sequence;
        lastDiscarded = std::move(frame);
        return Work::DONE;
    }
    frame.ptsNs = pts;
    pushFrame(frame, sequence);
    return Work::DONE;
}

void PlatformVideoSource::handleAudioSync(std::unique_lock<std::mutex>& lock){
    const int64_t target = std::max<int64_t>(pendingAudioSync, 0);
    pendingAudioSync = -1;
    audioGeneration++;
    frameReady.notify_all();
    if (!ring || !audio)
        return;
    ring->reset(target);
    const int64_t perIteration = std::max<int64_t>(audioFramesPerIteration(), 1);
    const int64_t local = loop ? target % perIteration : std::min(target, perIteration);
    audioIteration = loop ? (int)(target / perIteration) : 0;
    audioRestart = true;
    audioEnd = false;
    const double seconds = (double)local / VideoAudioRing::SAMPLE_RATE;
    lock.unlock();
    audio->seek(seconds);
    lock.lock();
}

PlatformVideoSource::Work PlatformVideoSource::decodeAudio(std::unique_lock<std::mutex>& lock){
    const uint64_t generation = audioGeneration;
    const int iteration = audioIteration;
    lock.unlock();
    VideoFrame unusedFrame;
    double pcmStart = 0.0;
    MediaError failure;
    pcm.clear();
    const PlatformDecoderBackend::Step result = audio->decodeNext(unusedFrame, pcm, pcmStart, failure);
    lock.lock();
    if (generation != audioGeneration)
        return Work::DONE;
    const int64_t perIteration = audioFramesPerIteration();
    switch (result){
        case PlatformDecoderBackend::Step::AGAIN:
            return Work::IDLE;
        case PlatformDecoderBackend::Step::FRAME:
            return Work::DONE;
        case PlatformDecoderBackend::Step::FAILURE:
            Log::warn("[Video] '%s': audio stopped: %s", bytes->path().c_str(), failure.message.c_str());
            audioEnd = true;
            return Work::DONE;
        case PlatformDecoderBackend::Step::END:
            if (loop && perIteration > 0){
                // the rest of the iteration is silence (audio shorter than the media), then the start again
                const int64_t iterationEnd = (int64_t)(audioIteration + 1) * perIteration;
                const int64_t written = ring->getWritePosition();
                if (written < iterationEnd){
                    const size_t frames = (size_t)std::min<int64_t>(iterationEnd - written, (int64_t)ring->getFreeFrames());
                    if (frames > 0){
                        std::vector<float> silence(frames * 2, 0.0f);
                        ring->write(silence.data(), frames);
                    }
                    if (ring->getWritePosition() < iterationEnd)
                        return Work::DONE;   // the end of the silence comes once the mixer read more
                }
                audioIteration++;
                audioRestart = true;
                lock.unlock();
                audio->seek(0.0);
                lock.lock();
                return Work::DONE;
            }
            audioEnd = true;
            return Work::NONE;
        case PlatformDecoderBackend::Step::AUDIO:
            break;
    }
    stats.audioPackets++;
    int64_t count = (int64_t)(pcm.size() / 2);
    if (count <= 0)
        return Work::DONE;
    const int64_t iterationBase = (int64_t)iteration * std::max<int64_t>(perIteration, 0);
    int64_t local = secondsToFrames(pcmStart);
    const int64_t expected = ring->getWritePosition();
    int64_t global = iterationBase + local;
    // consecutive chunks are placed by counting samples; the timestamp decides where a stream
    // starts (time 0, a sync target) and real gaps or overlaps
    if (std::llabs(global - expected) <= SNAP_FRAMES && !audioRestart){
        global = expected;
        local = global - iterationBase;
    }
    const float* data = pcm.data();
    if (perIteration > 0 && local + count > perIteration)
        count = std::max<int64_t>(0, perIteration - local);
    if (global < expected){
        const int64_t skip = std::min(expected - global, count);
        data += skip * 2;
        count -= skip;
        global += skip;
    }
    if (count <= 0)
        return Work::DONE;
    audioRestart = false;
    const int64_t gap = global - expected;
    if (gap > 0){
        std::vector<float> silence((size_t)std::min<int64_t>(gap, (int64_t)ring->getFreeFrames()) * 2, 0.0f);
        ring->write(silence.data(), silence.size() / 2);
    }
    // a chunk larger than the room left: what does not fit now is dropped by the ring; the
    // AUDIO_MIN_FREE margin keeps that from happening with the decoders' chunk sizes
    ring->write(data, (size_t)count);
    return Work::DONE;
}

// ---------------------------------------------------------------- main thread

IVideoSource::State PlatformVideoSource::getState() const{
    return state;
}

IVideoSource::State PlatformVideoSource::waitOpen(){
    std::unique_lock<std::mutex> lock(mutex);
    frameReady.wait_for(lock, WAIT_LIMIT, [this]{ return state != State::OPENING || quit; });
    return state;
}

const VideoInfo& PlatformVideoSource::getInfo() const{
    return info;
}

MediaError PlatformVideoSource::getError() const{
    std::lock_guard<std::mutex> lock(mutex);
    return error;
}

void PlatformVideoSource::setLoop(bool value){
    std::lock_guard<std::mutex> lock(mutex);
    if (loop == value)
        return;
    loop = value;
    if (state != State::READY)
        return;
    // the last frame was decoded already (the worker stopped there): on to the next iteration
    if (loop && videoEnd && info.frameCount > 0 && video){
        videoEnd = false;
        restartPending = true;
    }
    if (loop && audioEnd && audio)
        audioEnd = false;
    workerWake.notify_all();
}

int PlatformVideoSource::frameIndexFor(double seconds) const{
    if (!opened || !info.hasVideo)
        return -1;
    const int64_t ns = (int64_t)std::floor(std::max(seconds, 0.0) * 1e9 + 0.5);
    if (exactIndex)
        return index.frameAt(ns);
    return indexOf(ns);
}

int64_t PlatformVideoSource::sequenceFor(int iteration, double seconds) const{
    const int frameIndex = frameIndexFor(seconds);
    if (frameIndex < 0)
        return -1;
    return (int64_t)std::max(iteration, 0) * info.frameCount + frameIndex;
}

double PlatformVideoSource::keyframeTimeAtOrBefore(double seconds) const{
    const int frameIndex = frameIndexFor(seconds);
    if (frameIndex < 0)
        return 0.0;
    return (double)ptsOf(keyframeAtOrBefore(frameIndex)) / 1e9;
}

IVideoSource::FrameResult PlatformVideoSource::acquireFrame(int iteration, double seconds, bool wait, int64_t shownSequence, VideoFrame& frame){
    if (state == State::OPENING){
        if (!wait || waitOpen() == State::OPENING)
            return FrameResult::NOT_READY;
    }
    if (!info.hasVideo)
        return state == State::FAILED ? FrameResult::FAILED : FrameResult::SAME;

    const int64_t target = sequenceFor(iteration, seconds);
    if (target < 0)
        return FrameResult::NOT_READY;
    if (target == shownSequence)
        return FrameResult::SAME;
    const int count = std::max(info.frameCount, 1);
    const int64_t behindLimit = std::max<int64_t>(SEEK_AHEAD_FRAMES * 2, (int64_t)std::ceil(0.5 * (info.frameRate > 0.0 ? info.frameRate : 30.0)));

    const auto deadline = std::chrono::steady_clock::now() + FRAME_WAIT_LIMIT;
    std::unique_lock<std::mutex> lock(mutex);
    for (;;){
        while (queue.size() >= 2 && queue[1].sequence <= target){
            // an empty entry stands for the picture before it: a real one there becomes it
            if (queue[1].y.empty() && !queue.front().y.empty()){
                VideoFrame kept = std::move(queue.front());
                queue.pop_front();
                kept.sequence = queue.front().sequence;
                kept.index = queue.front().index;
                queue.front() = std::move(kept);
                continue;
            }
            if (queue.front().sequence > shownSequence && !queue.front().y.empty())
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

        const bool behind = !queue.empty() ? target < queue.front().sequence : (pendingSeek >= 0 ? target < pendingSeek : target < nextSequence);
        const int64_t from = pendingSeek >= 0 ? pendingSeek : nextSequence;
        const int64_t iterationStart = (target / count) * count;
        const int targetIndex = (int)(target % count);
        int64_t seekTo = -1;
        if (behind){
            seekTo = target;
        }else if (target > from + SEEK_AHEAD_FRAMES){
            const int64_t keyBefore = iterationStart + keyframeAtOrBefore(targetIndex);
            if (keyBefore > from)
                seekTo = target;
            if (!wait && target - from > behindLimit){
                const int keyAfter = keyframeAfter(targetIndex);
                seekTo = keyAfter > 0 ? iterationStart + keyAfter : target;
            }
        }
        if (seekTo >= 0 && pendingSeek != seekTo)
            requestSeekLocked(seekTo);
        if (videoEnd && queue.empty() && target >= nextSequence)
            return FrameResult::SAME;

        if (!wait){
            if (!queue.empty() && queue.front().sequence < target && queue.front().sequence > shownSequence){
                frame = std::move(queue.front());
                queue.pop_front();
                workerWake.notify_all();
                return FrameResult::STALE;
            }
            return FrameResult::NOT_READY;
        }
        workerWake.notify_all();
        if (frameReady.wait_until(lock, deadline) == std::cv_status::timeout){
            if (!lateWarned){
                lateWarned = true;
                Log::warn("[Video] '%s': frame %lld was not decoded within a second: the video waits", bytes->path().c_str(), (long long)target);
            }
            return FrameResult::NOT_READY;
        }
        if (quit)
            return FrameResult::NOT_READY;
    }
}

void PlatformVideoSource::syncAudio(int64_t frames){
    std::lock_guard<std::mutex> lock(mutex);
    if (!ring)
        return;
    pendingAudioSync = std::max<int64_t>(frames, 0);
    audioGeneration++;
    workerWake.notify_all();
}

bool PlatformVideoSource::isAudioSyncPending() const{
    std::lock_guard<std::mutex> lock(mutex);
    return pendingAudioSync >= 0;
}

void PlatformVideoSource::waitAudioSync(){
    std::unique_lock<std::mutex> lock(mutex);
    workerWake.notify_all();
    frameReady.wait_for(lock, WAIT_LIMIT, [this]{ return pendingAudioSync < 0 || quit || state != State::READY; });
}

std::shared_ptr<VideoAudioRing> PlatformVideoSource::getAudioRing() const{
    std::lock_guard<std::mutex> lock(mutex);
    return ring;
}

bool PlatformVideoSource::pump(int steps){
    (void)steps;
    return false;
}

VideoStats PlatformVideoSource::getStats() const{
    std::lock_guard<std::mutex> lock(mutex);
    VideoStats copy = stats;
    copy.queueBytes = 0;
    for (const VideoFrame& queued : queue)
        copy.queueBytes += queued.bytes();
    return copy;
}

double PlatformVideoSource::getBufferedSeconds() const{
    if (!opened)
        return 0.0;
    if (!bytes->isGrowing())
        return info.duration;
    const int64_t total = bytes->size();
    if (total <= 0)
        return 0.0;
    return info.duration * std::clamp((double)bytes->availableBytes() / (double)total, 0.0, 1.0);
}

size_t PlatformVideoSource::getQueuedFrames() const{
    std::lock_guard<std::mutex> lock(mutex);
    return queue.size();
}
