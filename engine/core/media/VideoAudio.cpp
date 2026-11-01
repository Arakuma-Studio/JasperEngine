// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VideoAudio.h"

#include "subsystem/AudioSystem.h"
#include "audio/AudioMixer.h"

#include "soloud.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace jasperengine;

// ---------------------------------------------------------------- VideoAudioRing

VideoAudioRing::VideoAudioRing(int channelCount, size_t capacityFrames):
    channels(std::max(channelCount, 1)), capacity(std::max<size_t>(capacityFrames, 1)){
    buffer.assign(capacity * (size_t)channels, 0.0f);
}

int VideoAudioRing::getChannels() const{
    return channels;
}

size_t VideoAudioRing::getCapacity() const{
    return capacity;
}

size_t VideoAudioRing::write(const float* interleaved, size_t frames){
    std::lock_guard<std::mutex> lock(mutex);
    size_t taken = 0;
    // positions already read: dropped (the reader played silence there)
    if (writePosition < readPosition){
        const size_t late = (size_t)std::min<int64_t>(readPosition - writePosition, (int64_t)frames);
        writePosition += (int64_t)late;
        taken += late;
        interleaved += late * (size_t)channels;
        frames -= late;
    }
    const size_t room = capacity - count;
    const size_t n = std::min(frames, room);
    size_t tail = (head + count) % capacity;
    for (size_t i = 0; i < n;){
        const size_t chunk = std::min(n - i, capacity - tail);
        std::memcpy(&buffer[tail * (size_t)channels], interleaved + i * (size_t)channels, chunk * (size_t)channels * sizeof(float));
        i += chunk;
        tail = (tail + chunk) % capacity;
    }
    count += n;
    writePosition += (int64_t)n;
    return taken + n;
}

size_t VideoAudioRing::getFreeFrames() const{
    std::lock_guard<std::mutex> lock(mutex);
    return capacity - count;
}

int64_t VideoAudioRing::getWritePosition() const{
    std::lock_guard<std::mutex> lock(mutex);
    return writePosition;
}

void VideoAudioRing::read(float* out, size_t frames){
    std::lock_guard<std::mutex> lock(mutex);
    size_t done = 0;
    // inserted silence: the read position stands
    if (correction > 0){
        const size_t insert = (size_t)std::min<int64_t>({correction, (int64_t)frames, (int64_t)MAX_CORRECTION_PER_READ});
        std::memset(out, 0, insert * (size_t)channels * sizeof(float));
        correction -= (int64_t)insert;
        done = insert;
    }else if (correction < 0){
        // dropped frames: the read position moves on without them being heard
        const size_t drop = (size_t)std::min<int64_t>(-correction, (int64_t)MAX_CORRECTION_PER_READ);
        const size_t fromBuffer = std::min(drop, count);
        head = (head + fromBuffer) % capacity;
        count -= fromBuffer;
        readPosition += (int64_t)drop;
        correction += (int64_t)drop;
    }
    while (done < frames){
        if (count == 0){
            // nothing buffered: silence, and the position moves on
            const size_t silent = frames - done;
            std::memset(out + done * (size_t)channels, 0, silent * (size_t)channels * sizeof(float));
            readPosition += (int64_t)silent;
            underrunFrames += silent;
            break;
        }
        const size_t chunk = std::min({frames - done, count, capacity - head});
        std::memcpy(out + done * (size_t)channels, &buffer[head * (size_t)channels], chunk * (size_t)channels * sizeof(float));
        head = (head + chunk) % capacity;
        count -= chunk;
        readPosition += (int64_t)chunk;
        done += chunk;
    }
}

int64_t VideoAudioRing::getReadPosition() const{
    std::lock_guard<std::mutex> lock(mutex);
    return readPosition;
}

size_t VideoAudioRing::getBufferedFrames() const{
    std::lock_guard<std::mutex> lock(mutex);
    return count;
}

void VideoAudioRing::reset(int64_t position){
    std::lock_guard<std::mutex> lock(mutex);
    head = 0;
    count = 0;
    readPosition = position;
    writePosition = position;
    correction = 0;
}

void VideoAudioRing::requestCorrection(int64_t frames){
    std::lock_guard<std::mutex> lock(mutex);
    correction = frames;
}

int64_t VideoAudioRing::getPendingCorrection() const{
    std::lock_guard<std::mutex> lock(mutex);
    return correction;
}

uint64_t VideoAudioRing::getUnderrunFrames() const{
    std::lock_guard<std::mutex> lock(mutex);
    return underrunFrames;
}

// ---------------------------------------------------------------- VideoAudioSync

VideoAudioSync::Decision VideoAudioSync::decide(int64_t readPosition, int64_t pendingCorrection, int64_t mediaFrames, int64_t latencyFrames){
    Decision decision;
    // what is heard now, counting the correction already on its way as done
    const int64_t audible = readPosition - latencyFrames - pendingCorrection;
    decision.drift = (double)(audible - mediaFrames) / (double)VideoAudioRing::SAMPLE_RATE;
    if (std::fabs(decision.drift) > RESYNC){
        decision.action = Action::RESYNC;
        decision.frames = mediaFrames + latencyFrames;
        return decision;
    }
    if (std::fabs(decision.drift) > DEAD_BAND){
        decision.action = Action::CORRECT;
        // ahead: insert silence (positive); behind: drop (negative)
        decision.frames = audible - mediaFrames + pendingCorrection;
        return decision;
    }
    return decision;
}

// ---------------------------------------------------------------- SoLoud source

namespace {

class VideoAudioInstance : public SoLoud::AudioSourceInstance {
public:
    explicit VideoAudioInstance(std::shared_ptr<VideoAudioRing> ring): ring(std::move(ring)){
    }

    unsigned int getAudio(float* aBuffer, unsigned int aSamplesToRead, unsigned int aBufferSize) override{
        const int channels = ring->getChannels();
        scratch.resize((size_t)aSamplesToRead * (size_t)channels);
        ring->read(scratch.data(), aSamplesToRead);
        // SoLoud wants one plane per channel, aBufferSize apart
        for (int ch = 0; ch < channels; ch++){
            float* plane = aBuffer + (size_t)ch * aBufferSize;
            for (unsigned int i = 0; i < aSamplesToRead; i++)
                plane[i] = scratch[(size_t)i * (size_t)channels + (size_t)ch];
        }
        return aSamplesToRead;
    }

    bool hasEnded() override{
        return false;
    }

private:
    std::shared_ptr<VideoAudioRing> ring;
    std::vector<float> scratch;
};

}

class VideoAudioSource::Source : public SoLoud::AudioSource {
public:
    explicit Source(std::shared_ptr<VideoAudioRing> ring): ring(std::move(ring)){
        mChannels = (unsigned int)this->ring->getChannels();
        mBaseSamplerate = (float)VideoAudioRing::SAMPLE_RATE;
    }

    ~Source() override{
        stop();
    }

    SoLoud::AudioSourceInstance* createInstance() override{
        return new VideoAudioInstance(ring);
    }

private:
    std::shared_ptr<VideoAudioRing> ring;
};

VideoAudioSource::VideoAudioSource(std::shared_ptr<VideoAudioRing> ring): source(std::make_unique<Source>(std::move(ring))){
}

VideoAudioSource::~VideoAudioSource(){
    stop();
}

bool VideoAudioSource::start(float volume){
    if (isPlaying())
        return true;
    // no device (the NoSound / Null mixing): the video runs on its own clock, as before
    if (!AudioSystem::init() || !AudioSystem::isOutputAvailable())
        return false;
    // through the mixer's Master, so app pause and the master volume reach it
    handle = AudioMixer::play("", *source, false, Vector3(0, 0, 0), volume, false, 0.0);
    paused = true;
    return handle != 0;
}

bool VideoAudioSource::start3d(float volume, float x, float y, float z){
    if (isPlaying())
        return true;
    if (!AudioSystem::init())
        return false;
    handle = AudioSystem::getSoloud().play3d(*source, x, y, z, 0.0f, 0.0f, 0.0f, volume, true);
    paused = true;
    return handle != 0;
}

void VideoAudioSource::set3d(float x, float y, float z, float minDistance, float maxDistance, unsigned int attenuation, float rolloff,
                             const float listener[9]){
    if (!isPlaying())
        return;
    SoLoud::Soloud& soloud = AudioSystem::getSoloud();
    soloud.set3dSourceMinMaxDistance(handle, minDistance, maxDistance);
    soloud.set3dSourceAttenuation(handle, attenuation, rolloff);
    soloud.set3dSourcePosition(handle, x, y, z);
    soloud.set3dListenerParameters(listener[0], listener[1], listener[2], listener[3], listener[4], listener[5], listener[6], listener[7], listener[8]);
    soloud.update3dAudio();
}

bool VideoAudioSource::isPlaying() const{
    return handle != 0 && AudioSystem::isOutputAvailable() && AudioSystem::getSoloud().isValidVoiceHandle(handle);
}

void VideoAudioSource::setPaused(bool value){
    paused = value;
    if (isPlaying())
        AudioSystem::getSoloud().setPause(handle, value);
}

void VideoAudioSource::setVolume(float volume){
    if (isPlaying())
        AudioSystem::getSoloud().setVolume(handle, volume);
}

void VideoAudioSource::setSpeed(float speed){
    if (isPlaying())
        AudioSystem::getSoloud().setRelativePlaySpeed(handle, speed);
}

void VideoAudioSource::stop(){
    if (isPlaying())
        AudioSystem::getSoloud().stop(handle);
    handle = 0;
}

int64_t VideoAudioSource::getLatencyFrames() const{
    if (!isPlaying())
        return 0;
    SoLoud::Soloud& soloud = AudioSystem::getSoloud();
    const unsigned int rate = soloud.getBackendSamplerate();
    if (rate == 0)
        return 0;
    return (int64_t)soloud.getBackendBufferSize() * VideoAudioRing::SAMPLE_RATE / (int64_t)rate;
}
