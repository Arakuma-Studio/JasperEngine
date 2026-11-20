// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MonoWavCook.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

using namespace jasperengine::editor;

namespace {

uint32_t monoWavU32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint16_t monoWavU16(const unsigned char* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

void monoWavPutU32(std::string& out, uint32_t value) {
    for (int i = 0; i < 4; i++) out.push_back((char)(unsigned char)(value >> (8 * i)));
}

void monoWavPutU16(std::string& out, uint16_t value) {
    out.push_back((char)(unsigned char)value);
    out.push_back((char)(unsigned char)(value >> 8));
}

constexpr uint16_t MONO_WAV_PCM = 1;
constexpr uint16_t MONO_WAV_FLOAT = 3;
constexpr uint16_t MONO_WAV_EXTENSIBLE = 0xFFFE;

// one sample of a frame as a signed integer (PCM; 8-bit widened to 16) or a float
int32_t monoWavSample(const unsigned char* p, uint16_t bits) {
    switch (bits) {
        case 8: return ((int32_t)p[0] - 128) * 256;
        case 16: return (int16_t)monoWavU16(p);
        default: {
            const int32_t value = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
            return (value & 0x800000) ? value - 0x1000000 : value;
        }
    }
}

} // namespace

bool MonoWavCook::cook(const std::string& wav, std::string& out) {
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(wav.data());
    const size_t size = wav.size();
    if (size < 12 || std::memcmp(bytes, "RIFF", 4) != 0 || std::memcmp(bytes + 8, "WAVE", 4) != 0) return false;

    uint16_t format = 0, channels = 0, bits = 0, blockAlign = 0;
    uint32_t rate = 0;
    const unsigned char* data = nullptr;
    uint32_t dataBytes = 0;
    for (size_t at = 12; at + 8 <= size;) {
        const uint32_t chunk = monoWavU32(bytes + at + 4);
        const unsigned char* body = bytes + at + 8;
        const size_t room = size - (at + 8);
        if (std::memcmp(bytes + at, "fmt ", 4) == 0) {
            if (chunk < 16 || chunk > room) return false;
            format = monoWavU16(body);
            channels = monoWavU16(body + 2);
            rate = monoWavU32(body + 4);
            blockAlign = monoWavU16(body + 12);
            bits = monoWavU16(body + 14);
            if (format == MONO_WAV_EXTENSIBLE) {
                // the sub-format GUID's first two bytes are the format tag
                if (chunk < 40) return false;
                format = monoWavU16(body + 24);
            }
        } else if (std::memcmp(bytes + at, "data", 4) == 0) {
            // a data chunk cut short is not cooked: what it holds is not what its header says
            if (chunk > room) return false;
            data = body;
            dataBytes = chunk;
            break;
        }
        at += 8 + (size_t)chunk + (chunk & 1);
    }
    const bool pcm = format == MONO_WAV_PCM && (bits == 8 || bits == 16 || bits == 24);
    const bool floating = format == MONO_WAV_FLOAT && bits == 32;
    if (!data || (!pcm && !floating) || channels < 2 || rate == 0 || blockAlign != channels * (bits / 8)) return false;

    const uint32_t frames = dataBytes / blockAlign;
    const uint16_t outBits = bits == 8 ? 16 : bits;
    const uint32_t outBytes = frames * (outBits / 8);
    std::string cooked;
    cooked.reserve(44 + outBytes + 1);
    cooked.append("RIFF");
    monoWavPutU32(cooked, 36 + outBytes + (outBytes & 1));
    cooked.append("WAVEfmt ");
    monoWavPutU32(cooked, 16);
    monoWavPutU16(cooked, floating ? MONO_WAV_FLOAT : MONO_WAV_PCM);
    monoWavPutU16(cooked, 1);
    monoWavPutU32(cooked, rate);
    monoWavPutU32(cooked, rate * (outBits / 8));
    monoWavPutU16(cooked, (uint16_t)(outBits / 8));
    monoWavPutU16(cooked, outBits);
    cooked.append("data");
    monoWavPutU32(cooked, outBytes);

    const unsigned int sampleBytes = bits / 8;
    for (uint32_t frame = 0; frame < frames; frame++) {
        const unsigned char* at = data + (size_t)frame * blockAlign;
        if (floating) {
            double sum = 0.0;
            for (uint16_t c = 0; c < channels; c++) {
                float value = 0.0f;
                std::memcpy(&value, at + c * sampleBytes, 4);
                sum += (double)value;
            }
            const float mean = (float)(sum / (double)channels);
            uint32_t word = 0;
            std::memcpy(&word, &mean, 4);
            monoWavPutU32(cooked, word);
            continue;
        }
        int64_t sum = 0;
        for (uint16_t c = 0; c < channels; c++) sum += monoWavSample(at + c * sampleBytes, bits);
        const double mean = (double)sum / (double)channels;
        const int64_t limit = outBits == 16 ? 32767 : 8388607;
        const int64_t value = std::clamp<int64_t>((int64_t)std::llround(mean), -limit - 1, limit);
        if (outBits == 16) {
            monoWavPutU16(cooked, (uint16_t)(int16_t)value);
        } else {
            const uint32_t word = (uint32_t)value;
            cooked.push_back((char)(unsigned char)word);
            cooked.push_back((char)(unsigned char)(word >> 8));
            cooked.push_back((char)(unsigned char)(word >> 16));
        }
    }
    if (outBytes & 1) cooked.push_back('\0');
    out = std::move(cooked);
    return true;
}
