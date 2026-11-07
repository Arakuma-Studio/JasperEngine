// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MusicSequenceSource.h"

#include "audio/ClipSource.h"

#include <algorithm>
#include <cstring>

using namespace jasperengine;

// docs/specs/S39-music.md R5, D2. `first` is taken by the instance when SoLoud makes it (on the
// thread that starts the voice, before the audio thread sees it), `next` is a single-slot hand-over
// (the audio thread takes it with an exchange, so the section it took is its alone), and the reports
// and the retired sections are single-producer single-consumer rings (the audio thread writes the
// tail, the main thread reads the head).

MusicSequenceSource::MusicSequenceSource(unsigned int channels, float sampleRate){
    mChannels = channels;
    mBaseSamplerate = sampleRate;
}

MusicSequenceSource::~MusicSequenceSource(){
    // its voice reads the sections this source holds: it goes first
    stop();
    delete next.exchange(nullptr);
    collectRetired();
}

SoLoud::AudioSourceInstance* MusicSequenceSource::createInstance(){
    return new MusicSequenceSourceInstance(this);
}

void MusicSequenceSource::setFirst(std::unique_ptr<MusicSection> section){
    first = std::move(section);
}

bool MusicSequenceSource::offerNext(std::unique_ptr<MusicSection> section){
    if (!section) return false;
    MusicSection* expected = nullptr;
    if (!next.compare_exchange_strong(expected, section.get(), std::memory_order_acq_rel)) return false;
    section.release();
    return true;
}

bool MusicSequenceSource::wantsNext() const{
    return next.load(std::memory_order_acquire) == nullptr && !endRequested.load(std::memory_order_acquire) &&
           !ended.load(std::memory_order_acquire);
}

void MusicSequenceSource::endAfterCurrent(){
    endRequested.store(true, std::memory_order_release);
}

void MusicSequenceSource::requestAdvance(){
    advanceRequested.store(true, std::memory_order_release);
}

void MusicSequenceSource::stopAt(uint64_t trackFrame){
    stopFrame.store(trackFrame, std::memory_order_release);
}

bool MusicSequenceSource::takeReport(Report& out){
    const unsigned int head = reportHead.load(std::memory_order_relaxed);
    if (head == reportTail.load(std::memory_order_acquire)) return false;
    out = reports[head % REPORT_CAPACITY];
    reportHead.store(head + 1, std::memory_order_release);
    return true;
}

void MusicSequenceSource::collectRetired(){
    unsigned int head = retiredHead.load(std::memory_order_relaxed);
    while (head != retiredTail.load(std::memory_order_acquire)) {
        delete retired[head % RETIRED_CAPACITY].exchange(nullptr, std::memory_order_acq_rel);
        head++;
        retiredHead.store(head, std::memory_order_release);
    }
}

bool MusicSequenceSource::hasEnded() const{
    return ended.load(std::memory_order_acquire);
}

void MusicSequenceSource::report(int section, uint64_t startFrame){
    const unsigned int tail = reportTail.load(std::memory_order_relaxed);
    // full: the main thread has not read for 64 sections (6.4 s of music at least); this one is lost
    // and the status catches up at the next report
    if (tail - reportHead.load(std::memory_order_acquire) >= REPORT_CAPACITY) return;
    reports[tail % REPORT_CAPACITY] = Report{section, startFrame};
    reportTail.store(tail + 1, std::memory_order_release);
}

void MusicSequenceSource::retire(MusicSection* section){
    if (!section) return;
    const unsigned int tail = retiredTail.load(std::memory_order_relaxed);
    if (tail - retiredHead.load(std::memory_order_acquire) >= RETIRED_CAPACITY) {
        // the main thread has not collected for 16 sections: freed here rather than lost
        delete section;
        return;
    }
    retired[tail % RETIRED_CAPACITY].store(section, std::memory_order_release);
    retiredTail.store(tail + 1, std::memory_order_release);
}

// -- the voice ------------------------------------------------------------------------------------

namespace {

    // the section's instance reads to its loop end (and the wrap is the sequence's) while passes remain
    void setSectionLooping(MusicSection& section, bool looping){
        if (looping) section.instance->mFlags |= SoLoud::AudioSourceInstance::LOOPING;
        else section.instance->mFlags &= ~SoLoud::AudioSourceInstance::LOOPING;
    }

    // passes remain after the one in progress (counted from 1; loops 0: until advanced)
    bool passesRemain(const MusicSection& section, unsigned int passes){
        return section.loops == 0 || passes < section.loops;
    }

}

MusicSequenceSourceInstance::MusicSequenceSourceInstance(MusicSequenceSource* parent): parent(parent){
    current = std::move(parent->first);
    if (current && current->instance) {
        passes = 1;
        setSectionLooping(*current, passesRemain(*current, passes));
        parent->report(current->index, 0);
    }
}

MusicSequenceSourceInstance::~MusicSequenceSourceInstance(){
}

unsigned int MusicSequenceSourceInstance::getAudio(float* buffer, unsigned int samplesToRead, unsigned int bufferSize){
    if (parent->ended.load(std::memory_order_relaxed)) return 0;

    // a cut at a switch point: nothing past the stop frame
    const uint64_t stopFrame = parent->stopFrame.load(std::memory_order_acquire);
    if (stopFrame <= trackFrames) {
        parent->ended.store(true, std::memory_order_release);
        return 0;
    }
    const unsigned int limit = (unsigned int)std::min<uint64_t>(samplesToRead, stopFrame - trackFrames);

    unsigned int written = 0;
    // reads in a row that gave nothing: a section whose region gives no frame ends
    int emptyReads = 0;
    while (written < limit) {
        if (!current || !current->instance) {
            // between sections: the next one if the main thread offered it, else the end (none
            // follows) or silence until it comes (a main thread too late to prepare it)
            MusicSection* taken = parent->next.exchange(nullptr, std::memory_order_acq_rel);
            if (taken) {
                current.reset(taken);
                passes = 1;
                setSectionLooping(*current, passesRemain(*current, passes));
                parent->report(current->index, trackFrames);
                emptyReads = 0;
                continue;
            }
            if (parent->endRequested.load(std::memory_order_acquire)) {
                parent->ended.store(true, std::memory_order_release);
                break;
            }
            for (unsigned int c = 0; c < mChannels; c++) {
                std::memset(buffer + (size_t)c * bufferSize + written, 0, sizeof(float) * (limit - written));
            }
            trackFrames += limit - written;
            written = limit;
            break;
        }

        // Audio.advanceMusic: the pass in progress is the last, its read goes past the loop end into
        // the tail
        if (parent->advanceRequested.exchange(false, std::memory_order_acq_rel)) {
            setSectionLooping(*current, false);
        }

        SoLoud::AudioSourceInstance& clip = *current->instance;
        const unsigned int want = limit - written;
        const unsigned int got = std::min(clip.getAudio(buffer + written, want, bufferSize), want);
        if (got > 0 && current->gain != 1.0f) {
            for (unsigned int c = 0; c < mChannels; c++) {
                float* out = buffer + (size_t)c * bufferSize + written;
                for (unsigned int i = 0; i < got; i++) out[i] *= current->gain;
            }
        }
        written += got;
        trackFrames += got;
        if (got == want) break;
        emptyReads = got == 0 ? emptyReads + 1 : 0;

        // the read stopped at its limit: the loop end of a pass with passes after it (SoLoud's wrap,
        // as its mixer makes it for a looping voice), or the region's end
        if ((clip.mFlags & SoLoud::AudioSourceInstance::LOOPING) && emptyReads < 2) {
            clip.seek(clip.mLoopPoint, nullptr, 0);
            clip.mLoopCount++;
            passes++;
            setSectionLooping(*current, passesRemain(*current, passes));
            continue;
        }
        // done: the main thread frees it, and the next section begins on the very next frame above
        parent->retire(current.release());
        emptyReads = 0;
    }
    if (written < samplesToRead && trackFrames >= stopFrame) parent->ended.store(true, std::memory_order_release);
    return written;
}

bool MusicSequenceSourceInstance::hasEnded(){
    return parent->ended.load(std::memory_order_acquire);
}
