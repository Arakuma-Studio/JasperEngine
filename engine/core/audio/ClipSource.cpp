// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "component/AudioSourceComponent.h"

#if JASPERENGINE_SOUND_LAYER

#include "ClipSource.h"

#include "soloud_wav.h"
#include "soloud_wavstream.h"

// stb_vorbis's header (SoLoud's copy, the one the engine links) maps FILE to SoLoud's File after stdio
#include <cstdio>
#undef MAX_CHANNELS
// the stb_vorbis SoLoud compiles: declarations only
#define STB_VORBIS_NO_STDIO
#include "soloud/src/audiosource/wav/stb_vorbis.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

using namespace jasperengine;

namespace {

// Whether stb_vorbis's sample positions follow the decode: true for an Ogg Vorbis file as encoders
// write it, false for a part cut out of a longer stream with its packets (ffmpeg -ss -c copy).
// There the first audio page's granule counts fewer frames than its packets decode (a start trim,
// which neither stb_vorbis nor ffmpeg applies: the engine keeps the head), and once
// stb_vorbis reads that granule its positions, and its seeks, run behind the decode by the
// difference. The decode up to that page tells: a few kilobytes.
bool oggPositionsFollowDecode(const unsigned char* data, size_t length){
    // the end of the first page that gives a granule position (the first audio page)
    size_t pageEnd = 0;
    for (size_t pos = 0; pos + 27 <= length && std::memcmp(data + pos, "OggS", 4) == 0;) {
        const unsigned int segments = data[pos + 26];
        if (pos + 27 + segments > length) {
            return true;
        }
        size_t body = 0;
        for (unsigned int i = 0; i < segments; i++) {
            body += data[pos + 27 + i];
        }
        uint64_t granule = 0;
        for (int i = 7; i >= 0; i--) {
            granule = (granule << 8) | data[pos + 6 + i];
        }
        pos += 27 + segments + body;
        // 0 on the header pages, all ones on a page that ends no packet
        if (granule != 0 && granule != ~0ull) {
            pageEnd = pos;
            break;
        }
    }
    if (pageEnd == 0) {
        return true;
    }

    int failure = 0;
    stb_vorbis* vorbis = stb_vorbis_open_memory(data, (int)length, &failure, nullptr);
    if (!vorbis) {
        return true;
    }
    bool follows = true;
    uint64_t decoded = 0;
    float** outputs = nullptr;
    for (;;) {
        const int frames = stb_vorbis_get_frame_float(vorbis, nullptr, &outputs);
        if (frames <= 0) {
            break;
        }
        decoded += (uint64_t)frames;
        const int offset = stb_vorbis_get_sample_offset(vorbis);
        if (offset >= 0 && (uint64_t)offset != decoded) {
            follows = false;
            break;
        }
        if (stb_vorbis_get_file_offset(vorbis) >= pageEnd) {
            break;
        }
    }
    stb_vorbis_close(vorbis);
    return follows;
}

// the frame `seconds` names in a region of `frames` at `rate`: floor(seconds x rate), so a time
// computed as frame / rate (a loop point) gives its frame back despite the division's rounding
uint32_t frameAt(double seconds, float rate, uint32_t frames){
    if (!(seconds > 0.0) || rate <= 0.0f) {
        return 0;
    }
    const double frame = std::floor(seconds * (double)rate + 1e-6);
    return frame >= (double)frames ? frames : (uint32_t)frame;
}

}

AudioClipSource::AudioClipSource(std::shared_ptr<SoLoud::AudioSource> sample, const ClipImportSettings& settings,
                       const unsigned char* oggHead, size_t oggHeadBytes): sample(std::move(sample)), settings(settings){
    // not single-instance: the pool shares one clip between every component using the file, and
    // each component plays its own voice
    setVolume(1.0f);
    if (auto* wav = dynamic_cast<SoLoud::Wav*>(this->sample.get())) {
        samples = wav->mData;
        fileFrames = wav->mSampleCount;
        mChannels = wav->mChannels;
        mBaseSamplerate = wav->mBaseSamplerate;
    }else if (auto* wavStream = dynamic_cast<SoLoud::WavStream*>(this->sample.get())) {
        stream = wavStream;
        fileFrames = wavStream->mSampleCount;
        mChannels = wavStream->mChannels;
        mBaseSamplerate = wavStream->mBaseSamplerate;
        seekByDecoding = wavStream->mFiletype == SoLoud::WAVSTREAM_OGG && oggHead && !oggPositionsFollowDecode(oggHead, oggHeadBytes);
        // dr_mp3 seeks from the stream's start, dr_flac too without a SEEKTABLE (SoLoud compiles out its
        // binary search, DR_FLAC_NO_CRC): about 1.4 ms per minute decoded for MP3, 0.5 for FLAC
        seekDecodes = seekByDecoding || wavStream->mFiletype == SoLoud::WAVSTREAM_MP3 ||
                      wavStream->mFiletype == SoLoud::WAVSTREAM_FLAC;
    }
    applyRegion();
}

AudioClipSource::~AudioClipSource(){
    // its voices read this clip's memory: they go before it does
    stop();
}

const std::shared_ptr<SoLoud::AudioSource>& AudioClipSource::getSample() const{
    return sample;
}

void AudioClipSource::applyRegion(){
    // trims that leave nothing leave an empty region at the trimmed start (a voice ends at once)
    startFrame = std::min(settings.trimHeadFrames, fileFrames);
    const uint32_t tail = std::min(settings.trimTailFrames, fileFrames);
    endFrame = std::max(startFrame, fileFrames - tail);

    // the editor refuses loop points outside the region; anything else plays the whole region
    loopStartFrame = startFrame;
    loopEndFrame = endFrame;
    if (settings.loopStartFrame >= (int64_t)startFrame && settings.loopStartFrame < (int64_t)endFrame) {
        loopStartFrame = (uint32_t)settings.loopStartFrame;
    }
    if (settings.loopEndFrame > (int64_t)loopStartFrame && settings.loopEndFrame <= (int64_t)endFrame) {
        loopEndFrame = (uint32_t)settings.loopEndFrame;
    }

    gain = settings.gainDb != 0.0f ? std::pow(10.0f, settings.gainDb / 20.0f) : 1.0f;

    // each fade at most the region (a region shorter than both gets both, overlapping)
    const uint32_t frames = endFrame - startFrame;
    fadeInFrames = std::min(settings.fadeInFrames, frames);
    fadeOutFrames = std::min(settings.fadeOutFrames, frames);
    fadeInWeights.resize(fadeInFrames);
    for (uint32_t k = 0; k < fadeInFrames; k++) {
        fadeInWeights[k] = clipFadeWeight(k, fadeInFrames, true);
    }
    fadeOutWeights.resize(fadeOutFrames);
    for (uint32_t k = 0; k < fadeOutFrames; k++) {
        fadeOutWeights[k] = clipFadeWeight(k, fadeOutFrames, false);
    }
}

const ClipImportSettings& AudioClipSource::getImportSettings() const{
    return settings;
}

double AudioClipSource::getLength() const{
    return mBaseSamplerate > 0.0f ? (double)(endFrame - startFrame) / (double)mBaseSamplerate : 0.0;
}

double AudioClipSource::getFileLength() const{
    return mBaseSamplerate > 0.0f ? (double)fileFrames / (double)mBaseSamplerate : 0.0;
}

double AudioClipSource::getDefaultLoopPoint() const{
    return mBaseSamplerate > 0.0f ? (double)(loopStartFrame - startFrame) / (double)mBaseSamplerate : 0.0;
}

unsigned int AudioClipSource::getChannels() const{
    return mChannels;
}

float AudioClipSource::getSampleRate() const{
    return mBaseSamplerate;
}

uint32_t AudioClipSource::getFileFrames() const{
    return fileFrames;
}

uint32_t AudioClipSource::getFrames() const{
    return endFrame - startFrame;
}

uint32_t AudioClipSource::getStartFrame() const{
    return startFrame;
}

uint32_t AudioClipSource::getEndFrame() const{
    return endFrame;
}

uint32_t AudioClipSource::getLoopStartFrame() const{
    return loopStartFrame;
}

uint32_t AudioClipSource::getLoopEndFrame() const{
    return loopEndFrame;
}

uint32_t AudioClipSource::getFadeInFrames() const{
    return fadeInFrames;
}

uint32_t AudioClipSource::getFadeOutFrames() const{
    return fadeOutFrames;
}

SoLoud::AudioSourceInstance* AudioClipSource::createInstance(){
    return new AudioClipSourceInstance(this);
}

AudioClipSourceInstance::AudioClipSourceInstance(AudioClipSource* parent): parent(parent), position(parent->startFrame){
    std::memset(scratch, 0, sizeof(scratch));
    if (parent->stream) {
        stream.reset(parent->stream->createInstance());
        // what SoLoud's play would set: the decoder's channels and rate
        stream->init(*parent->stream, 0);
        if (position > 0) {
            streamTo(position);
        }
        // made here, on the thread that starts the voice (Soloud::play creates instances outside
        // the audio mutex), so the mix never allocates a decoder: it is used once the voice loops
        if (parent->seekDecodes) {
            spare.reset(parent->stream->createInstance());
            spare->init(*parent->stream, 0);
        }
    }
}

AudioClipSourceInstance::~AudioClipSourceInstance(){
}

void AudioClipSourceInstance::streamTo(uint32_t frame){
    exhausted = false;
    if (parent->seekByDecoding) {
        // from the start, decoding what comes before the frame: exact, and as costly as the frame
        // is far in (only cut Ogg files, oggPositionsFollowDecode)
        if (stream->seek(0.0, nullptr, 0) != SoLoud::SO_NO_ERROR) {
            exhausted = true;
            return;
        }
        for (uint32_t skipped = 0; skipped < frame;) {
            const unsigned int chunk = std::min<uint32_t>(frame - skipped, SAMPLE_GRANULARITY);
            const unsigned int got = stream->getAudio(scratch, chunk, chunk);
            skipped += chunk;
            if (got < chunk) {
                exhausted = true;
                return;
            }
        }
        return;
    }
    // half a frame on: the patched WavStreamInstance::seek takes floor(seconds x rate), which a
    // time of exactly frame / rate could round to the frame before
    const double seconds = ((double)frame + 0.5) / (double)parent->mBaseSamplerate;
    if (stream->seek(seconds, nullptr, 0) != SoLoud::SO_NO_ERROR) {
        exhausted = true;
    }
}

bool AudioClipSourceInstance::readsToLoopEnd() const{
    if (!(mFlags & AudioSourceInstance::LOOPING)) {
        return false;
    }
    return position < parent->loopEndFrame || (position == parent->loopEndFrame && atLoopEnd);
}

uint32_t AudioClipSourceInstance::readLimit() const{
    return readsToLoopEnd() ? parent->loopEndFrame : parent->endFrame;
}

// Fades never shape a wrap: the fade-in only before the first wrap, the fade-out only on a pass
// that ends the region. Each multiplies the frames it covers after the gain (fade-in, then
// fade-out where both cover a frame), from the clip's tables.
unsigned int AudioClipSourceInstance::getAudio(float* buffer, unsigned int samplesToRead, unsigned int bufferSize){
    const bool toLoopEnd = readsToLoopEnd();
    const uint32_t limit = toLoopEnd ? parent->loopEndFrame : parent->endFrame;
    if ((!parent->samples && !stream) || position >= limit) {
        shortRead = samplesToRead > 0;
        return 0;
    }
    const unsigned int count = std::min<uint32_t>(samplesToRead, limit - position);
    const bool gained = parent->settings.gainDb != 0.0f;
    // a local: the buffer could alias the member, which would keep the loops from vectorizing (R31)
    const float gain = parent->gain;

    if (parent->samples) {
        const float* data = parent->samples;
        for (unsigned int c = 0; c < mChannels; c++) {
            float* out = buffer + (size_t)c * bufferSize;
            const float* from = data + (size_t)c * parent->fileFrames + position;
            if (gained) {
                for (unsigned int i = 0; i < count; i++) {
                    out[i] = from[i] * gain;
                }
            } else {
                std::memcpy(out, from, sizeof(float) * count);
            }
        }
    }else{
        // Block by block into this voice's scratch, read with its own size as the channel pitch
        // (WavStream's WAV, FLAC and MP3 branches step channels by the frames asked for). Only the
        // frames the decoder returned are copied: after a short read it leaves the rest of its
        // block stale, and the rest of the region is silence, as a Wav leaves frames it could not
        // decode. Reads stop at the region's end, so an Ogg's overshoot never plays.
        unsigned int written = 0;
        while (written < count) {
            const unsigned int chunk = std::min<unsigned int>(count - written, SAMPLE_GRANULARITY);
            unsigned int got = 0;
            if (!exhausted) {
                got = std::min(stream->getAudio(scratch, chunk, chunk), chunk);
                exhausted = got < chunk;
            }
            for (unsigned int c = 0; c < mChannels; c++) {
                float* out = buffer + (size_t)c * bufferSize + written;
                std::memcpy(out, scratch + (size_t)c * chunk, sizeof(float) * got);
                std::memset(out + got, 0, sizeof(float) * (chunk - got));
                if (gained) {
                    for (unsigned int i = 0; i < got; i++) {
                        out[i] *= gain;
                    }
                }
            }
            written += chunk;
        }
        if ((mFlags & AudioSourceInstance::LOOPING) && parent->seekDecodes) {
            advanceSpare();
        }
    }
    if (parent->fadeInFrames > 0 || parent->fadeOutFrames > 0) {
        applyFades(buffer, count, bufferSize, toLoopEnd);
    }
    position += count;
    shortRead = count < samplesToRead;
    atLoopEnd = toLoopEnd && position == parent->loopEndFrame;
    return count;
}

void AudioClipSourceInstance::applyFades(float* buffer, unsigned int count, unsigned int bufferSize, bool toLoopEnd){
    const uint32_t end = position + count;
    // the fade-in: frames from the region's start, until the voice first wraps
    const uint32_t inEnd = parent->startFrame + parent->fadeInFrames;
    if (!wrapped && position < inEnd) {
        const float* weights = parent->fadeInWeights.data() + (position - parent->startFrame);
        const unsigned int frames = std::min(end, inEnd) - position;
        for (unsigned int c = 0; c < mChannels; c++) {
            float* out = buffer + (size_t)c * bufferSize;
            for (unsigned int i = 0; i < frames; i++) {
                out[i] *= weights[i];
            }
        }
    }
    // the fade-out: the region's last frames, on a read that goes to the region's end
    const uint32_t outStart = parent->endFrame - parent->fadeOutFrames;
    if (!toLoopEnd && parent->fadeOutFrames > 0 && end > outStart) {
        const uint32_t from = std::max(position, outStart);
        const float* weights = parent->fadeOutWeights.data() + (from - outStart);
        const unsigned int skip = from - position;
        const unsigned int frames = end - from;
        for (unsigned int c = 0; c < mChannels; c++) {
            float* out = buffer + (size_t)c * bufferSize + skip;
            for (unsigned int i = 0; i < frames; i++) {
                out[i] *= weights[i];
            }
        }
    }
}

uint32_t AudioClipSourceInstance::loopFrame() const{
    return parent->startFrame + frameAt(mLoopPoint, mBaseSamplerate, parent->endFrame - parent->startFrame);
}

void AudioClipSourceInstance::advanceSpare(){
    if (spareBroken || !spare) {
        return;
    }
    const uint32_t target = loopFrame();
    if (spareAt > target) {
        // back to the start: a seek to frame 0 reads no audio in any format
        if (spare->seek(0.0, nullptr, 0) != SoLoud::SO_NO_ERROR) {
            spareBroken = true;
            return;
        }
        spareAt = 0;
    }
    uint32_t budget = SPARE_FRAMES_PER_READ;
    while (spareAt < target && budget > 0) {
        const unsigned int chunk = std::min<uint32_t>(std::min<uint32_t>(target - spareAt, budget), SAMPLE_GRANULARITY);
        const unsigned int got = spare->getAudio(scratch, chunk, chunk);
        spareAt += chunk;
        budget -= chunk;
        if (got < chunk) {
            // the file holds fewer frames than its headers count: the wraps seek as before
            spareBroken = true;
            return;
        }
    }
}

bool AudioClipSourceInstance::hasEnded(){
    // only once the region's last frame is returned, so SoLoud's tail patch mixes that block
    return !(mFlags & AudioSourceInstance::LOOPING) && position >= parent->endFrame;
}

SoLoud::result AudioClipSourceInstance::seek(SoLoud::time seconds, float* mixScratch, unsigned int mixScratchSize){
    (void)mixScratch;
    (void)mixScratchSize;
    // exact, from the clip's own frames: SoLoud's generic seek counts from mStreamPosition, which
    // the mixer keeps behind the frames already read, and from the speed-scaled rate
    const uint32_t frames = parent->endFrame - parent->startFrame;
    const uint32_t was = position;
    // the wrap SoLoud makes after a looping voice's read stopped at its limit (any other seek, a
    // script's included, leaves wrapped as it is)
    if ((mFlags & AudioSourceInstance::LOOPING) && shortRead) {
        wrapped = true;
    }
    shortRead = false;
    atLoopEnd = false;
    position = parent->startFrame + frameAt(seconds, mBaseSamplerate, frames);
    mStreamPosition = seconds > 0.0 ? std::min(seconds, parent->getLength()) : 0.0;
    if (stream) {
        if (spare && !spareBroken && spareAt == position) {
            // the wrap: the spare is there, and the decoder that reached the loop end becomes the
            // spare (past the loop point, it rewinds on the next read; after a short read, where it
            // stands is not known, so it rewinds too)
            std::swap(stream, spare);
            spareAt = exhausted ? UINT32_MAX : was;
            exhausted = false;
        }else{
            streamTo(position);
        }
    }
    return SoLoud::SO_NO_ERROR;
}

SoLoud::result AudioClipSourceInstance::rewind(){
    return seek(0.0, nullptr, 0);
}

#endif // JASPERENGINE_SOUND_LAYER
