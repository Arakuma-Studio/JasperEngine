// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S36 R31: AudioClipSource adds at most 3 % to the mix cost of a voice, against a plain Wav voice, with gain and
// no fades (the common case: its fades and trims cost only the frames they cover). Measured on the mix, what a
// voice costs an audio thread: SoLoud's null driver mixing one looping voice block by block, the fastest of
// several runs. Opt-in: a wall-clock budget on a shared builder is noise, so a person runs it by filter on a quiet
// machine, with BUILD_TYPE=Release as players ship: the unit tests' default -O2 leaves the gain loop scalar.

#include "audio/AudioImport.h"
#include "audio/ClipSource.h"

#include "soloud.h"
#include "soloud_wav.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

using namespace jasperengine;

namespace {

std::vector<unsigned char> toneWav(uint32_t frames){
    std::vector<unsigned char> out;
    auto u32 = [&out](uint32_t v){ for (int i = 0; i < 4; i++) out.push_back((unsigned char)(v >> (8 * i))); };
    auto u16 = [&out](uint16_t v){ out.push_back((unsigned char)v); out.push_back((unsigned char)(v >> 8)); };
    const uint32_t data = frames * 4;
    out.insert(out.end(), {'R', 'I', 'F', 'F'});
    u32(36 + data);
    out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    u32(16);
    u16(3);
    u16(1);
    u32(48000);
    u32(192000);
    u16(4);
    u16(32);
    out.insert(out.end(), {'d', 'a', 't', 'a'});
    u32(data);
    for (uint32_t i = 0; i < frames; i++){
        const float x = (float)std::sin((double)i * 0.0576);
        uint32_t bits = 0;
        std::memcpy(&bits, &x, 4);
        u32(bits);
    }
    return out;
}

double secondsOf(SoLoud::AudioSource& source, int blocks){
    SoLoud::Soloud engine;
    EXPECT_EQ(engine.init(SoLoud::Soloud::CLIP_ROUNDOFF, SoLoud::Soloud::NULLDRIVER, 48000, 512, 2), (unsigned int)SoLoud::SO_NO_ERROR);
    const SoLoud::handle voice = engine.play(source);
    engine.setLooping(voice, true);
    std::vector<float> buffer(1024);
    const auto start = std::chrono::steady_clock::now();
    float sink = 0.0f;
    for (int i = 0; i < blocks; i++){
        engine.mix(buffer.data(), 512);
        sink += buffer[0];
    }
    const std::chrono::duration<double> took = std::chrono::steady_clock::now() - start;
    EXPECT_LT(sink, 1e9f);
    EXPECT_GT(engine.getActiveVoiceCount(), 0u);
    engine.deinit();
    return took.count();
}

}

TEST(ClipSourceBudget, DISABLED_AddsAtMostThreePercentToAVoice){
    const std::vector<unsigned char> bytes = toneWav(480000);
    auto wav = std::make_shared<SoLoud::Wav>();
    ASSERT_EQ(wav->loadMem(bytes.data(), (unsigned int)bytes.size(), true, false), (unsigned int)SoLoud::SO_NO_ERROR);
    ClipImportSettings settings;
    settings.gainDb = -3.0f;
    settings.trimHeadFrames = 480;
    AudioClipSource clip(wav, settings);
    double plain = 1e9, wrapped = 1e9;
    const int blocks = 20000;
    clip.setSingleInstance(false);
    ClipImportSettings trimOnly;
    trimOnly.trimHeadFrames = 480;
    AudioClipSource noGain(wav, trimOnly);
    noGain.setSingleInstance(false);
    double withoutGain = 1e9;
    for (int run = 0; run < 15; run++){
        plain = std::min(plain, secondsOf(*wav, blocks));
        wrapped = std::min(wrapped, secondsOf(clip, blocks));
        withoutGain = std::min(withoutGain, secondsOf(noGain, blocks));
    }
    std::printf("[budget] without gain %.3f ms: %+.1f %%\n", withoutGain * 1000.0, (withoutGain / plain - 1.0) * 100.0);
    std::printf("[budget] Wav %.3f ms, AudioClipSource %.3f ms over %d blocks: %+.1f %%\n", plain * 1000.0, wrapped * 1000.0, blocks, (wrapped / plain - 1.0) * 100.0);
    EXPECT_LE(wrapped, plain * 1.03);
}
