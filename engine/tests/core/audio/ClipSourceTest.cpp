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

using namespace jasperengine;

namespace {

constexpr unsigned int RATE = 48000;

void putU32(std::vector<unsigned char>& out, uint32_t value){
    for (int i = 0; i < 4; i++) out.push_back((unsigned char)(value >> (8 * i)));
}

void putU16(std::vector<unsigned char>& out, uint16_t value){
    for (int i = 0; i < 2; i++) out.push_back((unsigned char)(value >> (8 * i)));
}

void putText(std::vector<unsigned char>& out, const char* text){
    for (const char* c = text; *c; c++) out.push_back((unsigned char)*c);
}

// a float32 WAV of planar channels
std::vector<unsigned char> wavBytes(const std::vector<std::vector<float>>& channels){
    const uint32_t frames = (uint32_t)channels[0].size();
    const uint16_t count = (uint16_t)channels.size();
    const uint32_t dataBytes = frames * count * 4;
    std::vector<unsigned char> out;
    putText(out, "RIFF");
    putU32(out, 36 + dataBytes);
    putText(out, "WAVE");
    putText(out, "fmt ");
    putU32(out, 16);
    putU16(out, 3);
    putU16(out, count);
    putU32(out, RATE);
    putU32(out, RATE * count * 4);
    putU16(out, (uint16_t)(count * 4));
    putU16(out, 32);
    putText(out, "data");
    putU32(out, dataBytes);
    for (uint32_t i = 0; i < frames; i++){
        for (uint16_t c = 0; c < count; c++){
            uint32_t bits = 0;
            std::memcpy(&bits, &channels[c][i], 4);
            putU32(out, bits);
        }
    }
    return out;
}

std::vector<float> ramp(uint32_t frames){
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
    const std::vector<float> file = ramp(4800);
    const auto bytes = wavBytes({file});
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
    AudioClipSource clip(decoded(wavBytes({file})), settings);
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
    AudioClipSource clip(decoded(wavBytes({file})), settings);
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
    const auto bytes = wavBytes({left, right});
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
    AudioClipSource clip(decoded(wavBytes({ramp(4800)})), settings);
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
