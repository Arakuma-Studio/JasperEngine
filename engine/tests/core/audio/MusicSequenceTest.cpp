// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/S39-music.md AC1 (R2, R5): a music track's voice read as SoLoud's mixer reads it, in
// 512-frame blocks, with the main thread's half (offering the next section when the current one
// began) between the blocks: the sections, their passes and tails, joined at the next frame.

#include "audio/AudioImport.h"
#include "audio/ClipSource.h"
#include "audio/MusicSequenceSource.h"
#include "support/AllocationCounter.h"

#include "soloud.h"
#include "soloud_wav.h"
#include "soloud_wavstream.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

using namespace jasperengine;

namespace {

constexpr unsigned int RATE = 48000;

void putU32(std::vector<unsigned char>& out, uint32_t value){
    for (int i = 0; i < 4; i++) out.push_back((unsigned char)(value >> (8 * i)));
}

void putU16(std::vector<unsigned char>& out, uint16_t value){
    for (int i = 0; i < 2; i++) out.push_back((unsigned char)(value >> (8 * i)));
}

// a float32 WAV of planar channels
std::vector<unsigned char> wavBytes(const std::vector<std::vector<float>>& channels){
    const uint32_t frames = (uint32_t)channels[0].size();
    const uint16_t count = (uint16_t)channels.size();
    const uint32_t dataBytes = frames * count * 4;
    std::vector<unsigned char> out;
    for (const char* c = "RIFF"; *c; c++) out.push_back((unsigned char)*c);
    putU32(out, 36 + dataBytes);
    for (const char* c = "WAVEfmt "; *c; c++) out.push_back((unsigned char)*c);
    putU32(out, 16);
    putU16(out, 3);
    putU16(out, count);
    putU32(out, RATE);
    putU32(out, RATE * count * 4);
    putU16(out, (uint16_t)(count * 4));
    putU16(out, 32);
    for (const char* c = "data"; *c; c++) out.push_back((unsigned char)*c);
    putU32(out, dataBytes);
    for (uint32_t i = 0; i < frames; i++) {
        for (uint16_t c = 0; c < count; c++) {
            uint32_t bits = 0;
            std::memcpy(&bits, &channels[c][i], 4);
            putU32(out, bits);
        }
    }
    return out;
}

// audio_refs.triangle: exact in float32 with a power-of-two period
std::vector<float> triangle(int period, uint32_t frames){
    std::vector<float> out(frames);
    const double half = period / 2.0;
    for (uint32_t i = 0; i < frames; i++) {
        const double t = (double)(i % (uint32_t)period);
        const double rise = t < half ? t / half : 2.0 - t / half;
        out[i] = (float)((rise * 2.0 - 1.0) * 0.5);
    }
    return out;
}

std::vector<std::vector<float>> stereo(const std::vector<float>& mono){
    std::vector<float> negated(mono.size());
    for (size_t i = 0; i < mono.size(); i++) negated[i] = -mono[i];
    return {mono, negated};
}

struct Clip{
    std::vector<std::vector<float>> channels;
    std::vector<unsigned char> bytes;
    std::shared_ptr<AudioClipSource> source;

    Clip(std::vector<std::vector<float>> planar, const ClipImportSettings& settings): channels(std::move(planar)){
        bytes = wavBytes(channels);
        auto wav = std::make_shared<SoLoud::Wav>();
        EXPECT_EQ(wav->loadMem(bytes.data(), (unsigned int)bytes.size(), true, false), (unsigned int)SoLoud::SO_NO_ERROR);
        source = std::make_shared<AudioClipSource>(wav, settings);
    }
};

std::unique_ptr<MusicSection> section(const Clip& clip, int index, unsigned int loops){
    auto out = std::make_unique<MusicSection>();
    out->clip = clip.source;
    out->instance.reset(clip.source->createInstance());
    out->instance->init(*clip.source, 0);
    out->instance->mFlags = 0;
    out->instance->mLoopPoint = clip.source->getDefaultLoopPoint();
    out->index = index;
    out->clipIndex = index;
    out->loops = loops;
    return out;
}

// audio_refs.float_hash
uint64_t floatHash(const std::vector<std::vector<float>>& channels){
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < channels[0].size(); i++) {
        for (const std::vector<float>& channel : channels) {
            uint32_t bits = 0;
            std::memcpy(&bits, &channel[i], 4);
            for (int b = 0; b < 4; b++) {
                h ^= (bits >> (8 * b)) & 0xFF;
                h *= 1099511628211ull;
            }
        }
    }
    return h;
}

struct Sequence{
    Clip clip0;
    Clip clip1;
    Clip clip2;

    static ClipImportSettings loop(int64_t start, int64_t end){
        ClipImportSettings settings;
        settings.loopStartFrame = start;
        settings.loopEndFrame = end;
        return settings;
    }

    static ClipImportSettings trimmed(){
        ClipImportSettings settings;
        settings.trimHeadFrames = 320;
        settings.trimTailFrames = 160;
        return settings;
    }

    // loops 2, 1, 0 with loop regions of whole periods; the second trimmed with no loop region
    Sequence(): clip0(stereo(triangle(64, 9600)), loop(1600, 8000)), clip1(stereo(triangle(32, 4800)), trimmed()),
                clip2(stereo(triangle(128, 12800)), loop(1280, 6400)){}
};

}

// AC1: the output is audio_refs.music_sequence's concatenation sample for sample (its hash pinned from
// the Python oracle), advanceMusic ends the 0-loop section at its next loop end, and the hand-overs
// step no further than the signals themselves
TEST(MusicSequence, PlaysPassesAndTailsOfEachSectionGaplessly){
    Sequence clips;
    MusicSequenceSource source(2, (float)RATE);
    source.setFirst(section(clips.clip0, 0, 2));
    ASSERT_TRUE(source.offerNext(section(clips.clip1, 1, 1)));
    std::unique_ptr<SoLoud::AudioSourceInstance> voice(source.createInstance());
    voice->init(source, 0);

    std::vector<std::vector<float>> out(2);
    std::vector<MusicSequenceSource::Report> reports;
    std::vector<float> block(512 * 2);
    bool advanced = false;
    // R22: what the audio thread does allocates nothing, hand-overs included
    uint64_t allocations = 0;
    while (!voice->hasEnded() && out[0].size() < 100000) {
        const testsupport::ThreadAllocationCount mixing;
        const unsigned int got = voice->getAudio(block.data(), 512, 512);
        allocations += mixing.count();
        for (unsigned int c = 0; c < 2; c++) out[c].insert(out[c].end(), block.begin() + c * 512, block.begin() + c * 512 + got);
        MusicSequenceSource::Report report;
        while (source.takeReport(report)) {
            reports.push_back(report);
            if (report.section == 1) {
                ASSERT_TRUE(source.wantsNext());
                ASSERT_TRUE(source.offerNext(section(clips.clip2, 2, 0)));
            } else if (report.section == 2) {
                source.endAfterCurrent();
            }
        }
        source.collectRetired();
        // in the third section's third pass (it began at 20320: 6400, then 5120 a pass)
        if (!advanced && out[0].size() >= 20320 + 6400 + 5120 + 1024) {
            source.requestAdvance();
            advanced = true;
        }
    }

    if (testsupport::countsAllocations()) {
        EXPECT_EQ(allocations, 0u) << "the mix allocated";
    }
    ASSERT_EQ(reports.size(), 3u);
    EXPECT_EQ(reports[0].startFrame, 0u);
    EXPECT_EQ(reports[1].startFrame, 16000u);
    EXPECT_EQ(reports[2].startFrame, 20320u);
    ASSERT_EQ(out[0].size(), 43360u);

    // the concatenation, built as R2 says
    std::vector<std::vector<float>> expected(2);
    auto spans = [&](const Clip& clip, std::vector<std::pair<uint32_t, uint32_t>> parts) {
        for (unsigned int c = 0; c < 2; c++) {
            for (const auto& [a, b] : parts) expected[c].insert(expected[c].end(), clip.channels[c].begin() + a, clip.channels[c].begin() + b);
        }
    };
    spans(clips.clip0, {{0, 8000}, {1600, 8000}, {8000, 9600}});
    spans(clips.clip1, {{320, 4640}});
    spans(clips.clip2, {{0, 6400}, {1280, 6400}, {1280, 6400}, {6400, 12800}});
    for (unsigned int c = 0; c < 2; c++) {
        for (size_t i = 0; i < expected[c].size(); i++) ASSERT_EQ(out[c][i], expected[c][i]) << "channel " << c << " frame " << i;
    }
    // python3 -c "import audio_refs as r; ..." (tools/mcp/audio_refs.py music_sequence, float_hash)
    EXPECT_EQ(floatHash(out), 0x36514c25af883645ull);

    // the seams: no step above the largest the signals take themselves (1/16 for the 32-frame period)
    float ownStep = 0.0f;
    for (size_t i = 1; i < expected[0].size(); i++) {
        if (i != 16000 && i != 20320) ownStep = std::max(ownStep, std::fabs(expected[0][i] - expected[0][i - 1]));
    }
    for (size_t seam : {16000u, 20320u}) EXPECT_LE(std::fabs(out[0][seam] - out[0][seam - 1]), ownStep) << seam;
}

// R5: a stop frame cuts the voice exactly there, whatever the block
TEST(MusicSequence, StopsAtItsStopFrameExactly){
    Sequence clips;
    MusicSequenceSource source(2, (float)RATE);
    source.setFirst(section(clips.clip0, 0, 0));
    source.stopAt(3000);
    std::unique_ptr<SoLoud::AudioSourceInstance> voice(source.createInstance());
    voice->init(source, 0);
    std::vector<float> block(512 * 2);
    size_t total = 0;
    while (!voice->hasEnded() && total < 100000) total += voice->getAudio(block.data(), 512, 512);
    EXPECT_EQ(total, 3000u);
    EXPECT_TRUE(source.hasEnded());
}

// R5: with no next section offered in time the track waits in silence, and ends when told no more come
TEST(MusicSequence, WaitsInSilenceForALateSectionAndEndsAfterTheLast){
    Sequence clips;
    MusicSequenceSource source(2, (float)RATE);
    source.setFirst(section(clips.clip1, 0, 1));
    std::unique_ptr<SoLoud::AudioSourceInstance> voice(source.createInstance());
    voice->init(source, 0);
    std::vector<float> block(512 * 2);
    size_t total = 0;
    for (int i = 0; i < 12; i++) total += voice->getAudio(block.data(), 512, 512);
    EXPECT_EQ(total, 12u * 512u) << "silence fills the wait";
    EXPECT_FALSE(voice->hasEnded());
    EXPECT_EQ(block[0], 0.0f);
    source.endAfterCurrent();
    voice->getAudio(block.data(), 512, 512);
    EXPECT_TRUE(voice->hasEnded());
}

namespace {

// what a voice costs the mix: SoLoud's null driver mixing it block by block, the fastest of several runs
double mixSeconds(SoLoud::AudioSource& source, bool looping, int blocks){
    SoLoud::Soloud engine;
    EXPECT_EQ(engine.init(SoLoud::Soloud::CLIP_ROUNDOFF, SoLoud::Soloud::NULLDRIVER, RATE, 512, 2), (unsigned int)SoLoud::SO_NO_ERROR);
    const SoLoud::handle voice = engine.play(source);
    engine.setLooping(voice, looping);
    std::vector<float> buffer(1024);
    const auto start = std::chrono::steady_clock::now();
    float sink = 0.0f;
    for (int i = 0; i < blocks; i++) {
        engine.mix(buffer.data(), 512);
        sink += buffer[0];
    }
    const std::chrono::duration<double> took = std::chrono::steady_clock::now() - start;
    EXPECT_LT(sink, 1e9f);
    EXPECT_GT(engine.getActiveVoiceCount(), 0u);
    engine.deinit();
    // the engine goes with this call: a source that outlives it must not stop through it
    source.mSoloud = nullptr;
    return took.count();
}

}

// R22: a music track's voice adds at most 2 % to the mix cost of the same clip streamed as a plain looping
// voice. Opt-in, as S36's ClipSourceBudget: a wall-clock ratio on a shared builder is noise, so a person runs it
// by filter on a quiet machine, BUILD_TYPE=Release
TEST(MusicSequenceBudget, DISABLED_AddsAtMostTwoPercentToAStreamedVoice){
    std::vector<float> tone(480000);
    for (size_t i = 0; i < tone.size(); i++) tone[i] = (float)std::sin((double)i * 0.0576);
    const std::vector<unsigned char> bytes = wavBytes({tone, tone});
    auto stream = std::make_shared<SoLoud::WavStream>();
    ASSERT_EQ(stream->loadMem(bytes.data(), (unsigned int)bytes.size(), true, false), (unsigned int)SoLoud::SO_NO_ERROR);
    ClipImportSettings settings;
    settings.trimHeadFrames = 480;
    auto clip = std::make_shared<AudioClipSource>(stream, settings);
    clip->setSingleInstance(false);
    const int blocks = 20000;
    double plain = 1e9, music = 1e9;
    for (int run = 0; run < 15; run++) {
        plain = std::min(plain, mixSeconds(*clip, true, blocks));
        MusicSequenceSource sequence(2, (float)RATE);
        auto first = std::make_unique<MusicSection>();
        first->clip = clip;
        first->instance.reset(clip->createInstance());
        first->instance->init(*clip, 0);
        first->instance->mFlags = 0;
        first->loops = 0;
        sequence.setFirst(std::move(first));
        music = std::min(music, mixSeconds(sequence, false, blocks));
    }
    std::printf("[budget] streamed clip %.3f ms, as a music track %.3f ms over %d blocks: %+.1f %%\n", plain * 1000.0, music * 1000.0, blocks,
                (music / plain - 1.0) * 100.0);
    EXPECT_LE(music, plain * 1.02);
}
