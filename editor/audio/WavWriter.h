// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// RIFF/WAVE files in memory with only a 16-byte `fmt ` chunk and a `data` chunk (sound layer
// phase 1): PCM16 (generate_sound's files) and float32. Samples are interleaved; the
// bytes depend only on the samples, so equal input gives equal files.
namespace jasperengine::editor::WavWriter {

    namespace detail {
        inline void put16(std::string& out, uint16_t value) {
            out.push_back(static_cast<char>(value & 0xFF));
            out.push_back(static_cast<char>((value >> 8) & 0xFF));
        }

        inline void put32(std::string& out, uint32_t value) {
            for (int i = 0; i < 4; i++) out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
        }

        inline std::string header(uint16_t format, unsigned int channels, unsigned int sampleRate, unsigned int bits, uint32_t dataBytes) {
            const uint16_t blockAlign = static_cast<uint16_t>(channels * bits / 8);
            std::string out;
            out.reserve(44 + dataBytes + (dataBytes & 1));
            out += "RIFF";
            put32(out, 4 + (8 + 16) + (8 + dataBytes + (dataBytes & 1)));
            out += "WAVE";
            out += "fmt ";
            put32(out, 16);
            put16(out, format);
            put16(out, static_cast<uint16_t>(channels));
            put32(out, sampleRate);
            put32(out, sampleRate * blockAlign);
            put16(out, blockAlign);
            put16(out, static_cast<uint16_t>(bits));
            out += "data";
            put32(out, dataBytes);
            return out;
        }
    }

    // The PCM16 value of a sample: lround(x * 32767), clamped, no dither
    inline int16_t pcm16Sample(float x) {
        const long value = std::lround(static_cast<double>(x) * 32767.0);
        return static_cast<int16_t>(std::clamp(value, -32768L, 32767L));
    }

    inline std::string pcm16(const std::vector<float>& interleaved, unsigned int channels, unsigned int sampleRate) {
        const uint32_t dataBytes = static_cast<uint32_t>(interleaved.size() * 2);
        std::string out = detail::header(1, channels, sampleRate, 16, dataBytes);
        for (float x : interleaved) detail::put16(out, static_cast<uint16_t>(pcm16Sample(x)));
        return out;
    }

    inline std::string float32(const std::vector<float>& interleaved, unsigned int channels, unsigned int sampleRate) {
        const uint32_t dataBytes = static_cast<uint32_t>(interleaved.size() * 4);
        std::string out = detail::header(3, channels, sampleRate, 32, dataBytes);
        for (float x : interleaved) {
            uint32_t bits;
            std::memcpy(&bits, &x, sizeof(bits));
            detail::put32(out, bits);
        }
        return out;
    }

}
