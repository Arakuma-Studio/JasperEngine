// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "audio/AudioImport.h"
#include "audio/ClipSource.h"
#include "pool/SoundPool.h"

#include "soloud.h"
#include "soloud_wav.h"
#include "soloud_wavstream.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace ClipSourceTest_cpp {

using namespace jasperengine;

namespace {

constexpr unsigned int RATE = 48000;

void clipSourcePutU32(std::vector<unsigned char>& out, uint32_t value){
    for (int i = 0; i < 4; i++) out.push_back((unsigned char)(value >> (8 * i)));
}

void putU16(std::vector<unsigned char>& out, uint16_t value){
    for (int i = 0; i < 2; i++) out.push_back((unsigned char)(value >> (8 * i)));
}

void putText(std::vector<unsigned char>& out, const char* text){
    for (const char* c = text; *c; c++) out.push_back((unsigned char)*c);
}

// a float32 WAV of planar channels
std::vector<unsigned char> clipSourceWavBytes(const std::vector<std::vector<float>>& channels){
    const uint32_t frames = (uint32_t)channels[0].size();
    const uint16_t count = (uint16_t)channels.size();
    const uint32_t dataBytes = frames * count * 4;
    std::vector<unsigned char> out;
    putText(out, "RIFF");
    clipSourcePutU32(out, 36 + dataBytes);
    putText(out, "WAVE");
    putText(out, "fmt ");
    clipSourcePutU32(out, 16);
    putU16(out, 3);
    putU16(out, count);
    clipSourcePutU32(out, RATE);
    clipSourcePutU32(out, RATE * count * 4);
    putU16(out, (uint16_t)(count * 4));
    putU16(out, 32);
    putText(out, "data");
    clipSourcePutU32(out, dataBytes);
    for (uint32_t i = 0; i < frames; i++){
        for (uint16_t c = 0; c < count; c++){
            uint32_t bits = 0;
            std::memcpy(&bits, &channels[c][i], 4);
            clipSourcePutU32(out, bits);
        }
    }
    return out;
}

std::vector<float> clipSourceRamp(uint32_t frames){
    std::vector<float> out(frames);
    for (uint32_t i = 0; i < frames; i++) out[i] = -0.5f + (float)i / (float)frames;
    return out;
}

std::vector<float> sine(uint32_t frames, double hz){
    std::vector<float> out(frames);
    for (uint32_t i = 0; i < frames; i++) out[i] = (float)(0.5 * std::sin(2.0 * 3.14159265358979323846 * hz * (double)i / RATE));
    return out;
}

std::shared_ptr<SoLoud::AudioSource> decoded(const std::vector<unsigned char>& bytes){
    auto wav = std::make_shared<SoLoud::Wav>();
    EXPECT_EQ(wav->loadMem(bytes.data(), (unsigned int)bytes.size(), true, false), (unsigned int)SoLoud::SO_NO_ERROR);
    return wav;
}

std::shared_ptr<SoLoud::AudioSource> streamed(const std::vector<unsigned char>& bytes){
    auto stream = std::make_shared<SoLoud::WavStream>();
    EXPECT_EQ(stream->loadMem(bytes.data(), (unsigned int)bytes.size(), true, false), (unsigned int)SoLoud::SO_NO_ERROR);
    return stream;
}

struct Voice {
    std::unique_ptr<SoLoud::AudioSourceInstance> instance;
    unsigned int channels = 0;

    Voice(SoLoud::AudioSource& source, bool looping, double loopPoint = 0.0){
        instance.reset(source.createInstance());
        instance->init(source, 0);
        channels = instance->mChannels;
        if (looping) instance->mFlags |= SoLoud::AudioSourceInstance::LOOPING;
        instance->mLoopPoint = loopPoint;
    }

    // what SoLoud's mixer reads, first channel: blocks of `block` frames, seeking to the loop point
    // when a looping read comes short
    std::vector<float> read(size_t frames, unsigned int block = 512, unsigned int channel = 0){
        std::vector<float> out;
        std::vector<float> buffer(block * channels);
        while (out.size() < frames){
            unsigned int got = instance->getAudio(buffer.data(), block, block);
            out.insert(out.end(), buffer.begin() + channel * block, buffer.begin() + channel * block + got);
            if (got < block){
                if (!(instance->mFlags & SoLoud::AudioSourceInstance::LOOPING)) break;
                instance->seek(instance->mLoopPoint, nullptr, 0);
            }
        }
        if (out.size() > frames) out.resize(frames);
        return out;
    }
};

}

TEST(AudioClipSource, TrimsAndGainReadTheRegion){
    const std::vector<float> file = clipSourceRamp(4800);
    const auto bytes = clipSourceWavBytes({file});
    ClipImportSettings settings;
    settings.trimHeadFrames = 480;
    settings.trimTailFrames = 960;
    settings.gainDb = -6.0f;
    AudioClipSource clip(decoded(bytes), settings);
    EXPECT_EQ(clip.getStartFrame(), 480u);
    EXPECT_EQ(clip.getEndFrame(), 3840u);
    EXPECT_DOUBLE_EQ(clip.getLength(), (3840.0 - 480.0) / RATE);
    const float gain = std::pow(10.0f, -6.0f / 20.0f);
    Voice voice(clip, false);
    const std::vector<float> out = voice.read(100000);
    ASSERT_EQ(out.size(), 3360u);
    for (size_t i = 0; i < out.size(); i++) ASSERT_FLOAT_EQ(out[i], file[480 + i] * gain) << i;
    EXPECT_TRUE(voice.instance->hasEnded());
}

TEST(AudioClipSource, FadesFollowTheirWeightsAndFrameZeroIsZero){
    const std::vector<float> file(4800, 0.5f);
    ClipImportSettings settings;
    settings.fadeInFrames = 96;
    settings.fadeOutFrames = 480;
    AudioClipSource clip(decoded(clipSourceWavBytes({file})), settings);
    Voice voice(clip, false);
    const std::vector<float> out = voice.read(100000, 64);
    ASSERT_EQ(out.size(), 4800u);
    EXPECT_EQ(out[0], 0.0f);
    EXPECT_EQ(out[4799], 0.0f);
    for (uint32_t k = 0; k < 96; k++) ASSERT_FLOAT_EQ(out[k], 0.5f * clipFadeWeight(k, 96, true)) << k;
    for (uint32_t k = 0; k < 480; k++) ASSERT_FLOAT_EQ(out[4320 + k], 0.5f * clipFadeWeight(k, 480, false)) << k;
    for (uint32_t i = 96; i < 4320; i++) ASSERT_EQ(out[i], 0.5f) << i;
}

TEST(AudioClipSource, LoopRegionWrapsSampleExactAndFadesNeverShapeAWrap){
    // 1 kHz at 48 kHz: 48 frames a cycle; a loop of whole cycles has no seam above its own step
    const std::vector<float> file = sine(48000, 1000.0);
    ClipImportSettings settings;
    settings.loopStartFrame = 4800;
    settings.loopEndFrame = 9600;
    settings.fadeInFrames = 480;
    settings.fadeOutFrames = 480;
    AudioClipSource clip(decoded(clipSourceWavBytes({file})), settings);
    EXPECT_DOUBLE_EQ(clip.getDefaultLoopPoint(), 0.1);
    Voice voice(clip, true, clip.getDefaultLoopPoint());
    const std::vector<float> out = voice.read(4800 + 4800 * 4);
    // the first pass: the fade-in, then the file up to the loop end, no fade-out
    for (uint32_t k = 0; k < 480; k++) ASSERT_FLOAT_EQ(out[k], file[k] * clipFadeWeight(k, 480, true)) << k;
    for (uint32_t i = 480; i < 9600; i++) ASSERT_EQ(out[i], file[i]) << i;
    // every later pass is exactly the region, without a fade
    for (size_t i = 9600; i < out.size(); i++) ASSERT_EQ(out[i], file[4800 + (i - 9600) % 4800]) << i;
    double ownStep = 0.0;
    for (size_t i = 1; i < 4800; i++) ownStep = std::max(ownStep, (double)std::fabs(file[i] - file[i - 1]));
    EXPECT_LE(std::fabs(out[9600] - out[9599]), ownStep + 1e-6);
}

TEST(AudioClipSource, StreamedAndDecodedAgree){
    const std::vector<float> left = sine(24000, 440.0);
    const std::vector<float> right = sine(24000, 660.0);
    const auto bytes = clipSourceWavBytes({left, right});
    ClipImportSettings settings;
    settings.trimHeadFrames = 100;
    settings.trimTailFrames = 200;
    settings.gainDb = 3.0f;
    settings.fadeInFrames = 32;
    settings.fadeOutFrames = 64;
    settings.loopStartFrame = 1000;
    settings.loopEndFrame = 6000;
    AudioClipSource fromWav(decoded(bytes), settings);
    AudioClipSource fromStream(streamed(bytes), settings);
    for (unsigned int channel = 0; channel < 2; channel++){
        Voice a(fromWav, true, fromWav.getDefaultLoopPoint());
        Voice b(fromStream, true, fromStream.getDefaultLoopPoint());
        const std::vector<float> one = a.read(30000, 512, channel);
        const std::vector<float> two = b.read(30000, 512, channel);
        ASSERT_EQ(one.size(), two.size());
        for (size_t i = 0; i < one.size(); i++) ASSERT_FLOAT_EQ(one[i], two[i]) << "channel " << channel << " frame " << i;
    }
}

TEST(AudioClipSource, TrimsThatLeaveNothingEndAtOnce){
    ClipImportSettings settings;
    settings.trimHeadFrames = 4000;
    settings.trimTailFrames = 4000;
    AudioClipSource clip(decoded(clipSourceWavBytes({clipSourceRamp(4800)})), settings);
    Voice voice(clip, false);
    EXPECT_TRUE(voice.read(1000).empty());
    EXPECT_TRUE(voice.instance->hasEnded());
    EXPECT_EQ(clip.getLength(), 0.0);
}

TEST(ClipImportSettings, DefaultsAndHash){
    ClipImportSettings settings;
    EXPECT_TRUE(settings.isDefault());
    EXPECT_TRUE(settings.playsAsFile());
    const uint64_t base = settings.hash();
    settings.gainDb = -0.0f;
    EXPECT_EQ(settings.hash(), base);
    settings.gainDb = -3.0f;
    EXPECT_NE(settings.hash(), base);
    EXPECT_FALSE(settings.playsAsFile());
    settings = ClipImportSettings();
    settings.bpm = 120.0f;
    EXPECT_FALSE(settings.isDefault());
    EXPECT_TRUE(settings.playsAsFile());
    EXPECT_NE(settings.hash(), base);
}

TEST(AudioImportTable, ParseSetAndKeyOfTheSample){
    AudioImportTable::clear();
    EXPECT_TRUE(AudioImportTable::isEmpty());
    EXPECT_EQ(SoundPool::key("a.wav", AudioLoadType::DecompressOnLoad), "a.wav#decompress_on_load");
    const uint32_t generation = SoundPool::generation("a.wav");

    std::string error;
    EXPECT_FALSE(AudioImportTable::parse("{\"format\":\"other\",\"version\":1,\"clips\":{}}", &error));
    EXPECT_FALSE(error.empty());
    ASSERT_TRUE(AudioImportTable::parse(R"({"format":"jasperengine.audio-imports","version":1,
        "clips":{"a.wav":{"gain_db":-3,"trim_head_frames":10,"loop_end_frame":99}}})", &error)) << error;
    std::optional<ClipImportSettings> found = AudioImportTable::find("a.wav");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->gainDb, -3.0f);
    EXPECT_EQ(found->trimHeadFrames, 10u);
    EXPECT_EQ(found->loopStartFrame, -1);
    EXPECT_EQ(found->loopEndFrame, 99);
    EXPECT_FALSE(AudioImportTable::find("b.wav").has_value());

    const std::string id = SoundPool::key("a.wav", AudioLoadType::DecompressOnLoad);
    EXPECT_NE(id.find("#i"), std::string::npos);
    std::string file;
    AudioLoadType type = AudioLoadType::Auto;
    SoundPool::parseClipRef(id, file, type);
    EXPECT_EQ(file, "a.wav");
    EXPECT_EQ(type, AudioLoadType::DecompressOnLoad);

    ClipImportSettings other;
    other.gainDb = -6.0f;
    AudioImportTable::set("a.wav", other);
    EXPECT_GT(SoundPool::generation("a.wav"), generation);
    EXPECT_NE(SoundPool::key("a.wav", AudioLoadType::DecompressOnLoad), id);

    AudioImportTable::set("a.wav", ClipImportSettings());
    EXPECT_TRUE(AudioImportTable::isEmpty());
    EXPECT_EQ(SoundPool::key("a.wav", AudioLoadType::DecompressOnLoad), "a.wav#decompress_on_load");
}

TEST(AudioClipSource, ForceMonoMeanOfChannels){
    const std::vector<float> left = sine(24000, 440.0);
    const std::vector<float> right = sine(24000, 660.0);
    const auto bytes = clipSourceWavBytes({left, right});
    ClipImportSettings settings;
    settings.trimHeadFrames = 100;
    settings.trimTailFrames = 200;
    settings.fadeInFrames = 32;
    settings.fadeOutFrames = 64;
    settings.loopStartFrame = 1000;
    settings.loopEndFrame = 6000;
    ClipImportSettings mono = settings;
    mono.forceMono = true;
    AudioClipSource stereo(decoded(bytes), settings);
    for (const bool stream : {false, true}){
        AudioClipSource clip(stream ? streamed(bytes) : decoded(bytes), mono);
        EXPECT_EQ(clip.getChannels(), 1u);
        EXPECT_EQ(clip.getFileChannels(), 2u);
        // the region, its loop and lengths are the stereo clip's
        EXPECT_EQ(clip.getStartFrame(), stereo.getStartFrame());
        EXPECT_EQ(clip.getEndFrame(), stereo.getEndFrame());
        EXPECT_EQ(clip.getLoopStartFrame(), stereo.getLoopStartFrame());
        EXPECT_EQ(clip.getLoopEndFrame(), stereo.getLoopEndFrame());
        EXPECT_DOUBLE_EQ(clip.getLength(), stereo.getLength());
        // looping: the fade-in, then the loop wrapping at the same frames as the stereo clip
        Voice voice(clip, true, clip.getDefaultLoopPoint());
        EXPECT_EQ(voice.channels, 1u);
        Voice l(stereo, true, stereo.getDefaultLoopPoint());
        Voice r(stereo, true, stereo.getDefaultLoopPoint());
        const std::vector<float> out = voice.read(20000);
        const std::vector<float> a = l.read(20000, 512, 0);
        const std::vector<float> b = r.read(20000, 512, 1);
        ASSERT_EQ(out.size(), a.size());
        for (size_t i = 0; i < out.size(); i++) { ASSERT_NEAR(out[i], (a[i] + b[i]) * 0.5f, 1e-6f) << (stream ? "streamed" : "held") << " frame " << i; }
        // not looping: the fade-out at the region's end, then the voice ends
        Voice once(clip, false);
        Voice onceL(stereo, false);
        Voice onceR(stereo, false);
        const std::vector<float> all = once.read(100000);
        const std::vector<float> allL = onceL.read(100000, 512, 0);
        const std::vector<float> allR = onceR.read(100000, 512, 1);
        ASSERT_EQ(all.size(), allL.size());
        for (size_t i = 0; i < all.size(); i++) { ASSERT_NEAR(all[i], (allL[i] + allR[i]) * 0.5f, 1e-6f) << i; }
        EXPECT_EQ(all.back(), 0.0f);
        EXPECT_TRUE(once.instance->hasEnded());
    }
    // a mono file ignores it
    AudioClipSource single(decoded(clipSourceWavBytes({left})), mono);
    EXPECT_EQ(single.getChannels(), 1u);
    EXPECT_EQ(single.getFileChannels(), 1u);
}

TEST(ClipImportSettings, ExistingHashesStayAndForceMonoChangesIt){
    // the values every clip without Force To Mono had before it: wrapped samples and analysis cache
    // names keep their names
    ClipImportSettings settings;
    EXPECT_EQ(settings.hash(), 0x6f9470ddbb445651ull);
    settings.gainDb = -3.0f;
    EXPECT_EQ(settings.hash(), 0x87eeaa36692b55d1ull);
    settings = ClipImportSettings();
    settings.gainDb = 3.0f;
    settings.trimHeadFrames = 100;
    settings.trimTailFrames = 200;
    settings.fadeInFrames = 32;
    settings.fadeOutFrames = 64;
    settings.loopStartFrame = 1000;
    settings.loopEndFrame = 6000;
    EXPECT_EQ(settings.hash(), 0x62554a1eba2c7e09ull);
    settings = ClipImportSettings();
    settings.bpm = 120.0f;
    settings.beatsPerBar = 3;
    settings.firstBeatFrame = 480;
    EXPECT_EQ(settings.hash(), 0x201e3faf4bfb4a57ull);

    settings = ClipImportSettings();
    settings.forceMono = true;
    EXPECT_FALSE(settings.isDefault());
    EXPECT_FALSE(settings.playsAsFile());
    EXPECT_NE(settings.hash(), ClipImportSettings().hash());
}

TEST(AudioImportTable, ImportTableForceMono){
    AudioImportTable::clear();
    std::string error;
    ASSERT_TRUE(AudioImportTable::parse(R"({"format":"jasperengine.audio-imports","version":1,
        "clips":{"a.wav":{"gain_db":-3},"b.wav":{"force_mono":true}}})", &error)) << error;
    std::optional<ClipImportSettings> a = AudioImportTable::find("a.wav");
    ASSERT_TRUE(a.has_value());
    EXPECT_FALSE(a->forceMono);
    std::optional<ClipImportSettings> b = AudioImportTable::find("b.wav");
    ASSERT_TRUE(b.has_value());
    EXPECT_TRUE(b->forceMono);
    // the forced clip plays wrapped, under a key of its own
    EXPECT_NE(SoundPool::key("b.wav", AudioLoadType::DecompressOnLoad).find("#i"), std::string::npos);
    AudioImportTable::clear();
}

}  // namespace ClipSourceTest_cpp
