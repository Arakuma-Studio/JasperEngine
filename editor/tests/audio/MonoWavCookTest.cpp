// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "audio/MonoWavCook.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace MonoWavCookTest_cpp {

using namespace jasperengine::editor;

namespace {

void putU32(std::string& out, uint32_t v) { for (int i = 0; i < 4; i++) out.push_back((char)(unsigned char)(v >> (8 * i))); }
void putU16(std::string& out, uint16_t v) { out.push_back((char)(unsigned char)v); out.push_back((char)(unsigned char)(v >> 8)); }
uint32_t readU32(const std::string& s, size_t at) {
    return (uint32_t)(unsigned char)s[at] | ((uint32_t)(unsigned char)s[at + 1] << 8) | ((uint32_t)(unsigned char)s[at + 2] << 16) |
           ((uint32_t)(unsigned char)s[at + 3] << 24);
}
uint16_t readU16(const std::string& s, size_t at) { return (uint16_t)((unsigned char)s[at] | ((unsigned char)s[at + 1] << 8)); }

// interleaved samples already encoded, with a LIST chunk before the data
std::string wav(uint16_t format, uint16_t channels, uint16_t bits, const std::string& data) {
    std::string out = "RIFF";
    putU32(out, 4 + 24 + 12 + 8 + (uint32_t)data.size());
    out += "WAVEfmt ";
    putU32(out, 16);
    putU16(out, format);
    putU16(out, channels);
    putU32(out, 44100);
    putU32(out, 44100u * channels * (bits / 8));
    putU16(out, (uint16_t)(channels * (bits / 8)));
    putU16(out, bits);
    out += "LIST";
    putU32(out, 4);
    out += "INFO";
    out += "data";
    putU32(out, (uint32_t)data.size());
    return out + data;
}

} // namespace

TEST(MonoWavCook, PcmAndFloatBecomeTheMeanAtHalfTheBytes) {
    // 16-bit: (100 + 301) / 2 rounds half away from zero, as (-100 + -301) / 2 does
    std::string pcm16;
    for (int16_t v : {100, 301, -100, -301, 32767, 32767, -32768, -32768}) putU16(pcm16, (uint16_t)v);
    std::string mono;
    ASSERT_TRUE(MonoWavCook::cook(wav(1, 2, 16, pcm16), mono));
    ASSERT_EQ(mono.size(), 44u + 8u);
    EXPECT_EQ(mono.substr(0, 4), "RIFF");
    EXPECT_EQ(readU32(mono, 4), 36u + 8u);
    EXPECT_EQ(readU16(mono, 20), 1);       // PCM
    EXPECT_EQ(readU16(mono, 22), 1);       // one channel
    EXPECT_EQ(readU32(mono, 24), 44100u);  // the rate
    EXPECT_EQ(readU16(mono, 34), 16);
    EXPECT_EQ(readU32(mono, 40), 8u);      // 4 frames, as many as before
    EXPECT_EQ((int16_t)readU16(mono, 44), 201);
    EXPECT_EQ((int16_t)readU16(mono, 46), -201);
    EXPECT_EQ((int16_t)readU16(mono, 48), 32767);
    EXPECT_EQ((int16_t)readU16(mono, 50), -32768);
    std::string again;
    ASSERT_TRUE(MonoWavCook::cook(wav(1, 2, 16, pcm16), again));
    EXPECT_EQ(again, mono);

    // 24-bit, three channels: odd data bytes get their pad byte
    std::string pcm24;
    for (int32_t v : {300000, 600000, -300000}) {
        const uint32_t w = (uint32_t)v;
        pcm24 += {(char)(w & 0xFF), (char)((w >> 8) & 0xFF), (char)((w >> 16) & 0xFF)};
    }
    ASSERT_TRUE(MonoWavCook::cook(wav(1, 3, 24, pcm24), mono));
    EXPECT_EQ(readU32(mono, 40), 3u);
    ASSERT_EQ(mono.size(), 44u + 3u + 1u);
    const int32_t sample = (int32_t)((uint32_t)(unsigned char)mono[44] | ((uint32_t)(unsigned char)mono[45] << 8) |
                                     ((uint32_t)(unsigned char)mono[46] << 16));
    EXPECT_EQ(sample, 200000);

    // 8-bit becomes PCM16
    ASSERT_TRUE(MonoWavCook::cook(wav(1, 2, 8, std::string({(char)200, (char)100})), mono));
    EXPECT_EQ(readU16(mono, 34), 16);
    EXPECT_EQ((int16_t)readU16(mono, 44), ((200 - 128) * 256 + (100 - 128) * 256) / 2);

    // float32
    std::string f32;
    for (float v : {0.5f, 0.25f}) {
        uint32_t w;
        std::memcpy(&w, &v, 4);
        putU32(f32, w);
    }
    ASSERT_TRUE(MonoWavCook::cook(wav(3, 2, 32, f32), mono));
    EXPECT_EQ(readU16(mono, 20), 3);
    float value = 0.0f;
    const uint32_t w = readU32(mono, 44);
    std::memcpy(&value, &w, 4);
    EXPECT_EQ(value, 0.375f);
}

TEST(MonoWavCook, LeavesEverythingElse) {
    std::string out = "untouched";
    std::string pcm16;
    putU16(pcm16, 1);
    putU16(pcm16, 2);
    EXPECT_FALSE(MonoWavCook::cook(wav(1, 1, 16, pcm16), out));    // already mono
    EXPECT_FALSE(MonoWavCook::cook(wav(1, 2, 32, pcm16 + pcm16), out));   // 32-bit PCM
    EXPECT_FALSE(MonoWavCook::cook(wav(2, 2, 16, pcm16), out));    // ADPCM
    EXPECT_FALSE(MonoWavCook::cook("OggS....", out));
    std::string cut = wav(1, 2, 16, pcm16 + pcm16);
    cut.resize(cut.size() - 3);
    EXPECT_FALSE(MonoWavCook::cook(cut, out));
    EXPECT_EQ(out, "untouched");
}

}  // namespace MonoWavCookTest_cpp
