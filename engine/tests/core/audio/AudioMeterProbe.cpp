// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// tools/mcp/tests/test_audio_meter.py compiles this with AudioMeter.cpp and TruePeak.cpp alone, no
// engine, and compares what it prints with pyloudnorm: a float32 WAV in, one JSON object out per
// file, in order. Not named *Test.cpp, so jasperengine-core-tests does not pick it up.

#include "audio/AudioMeter.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

using namespace jasperengine;

static uint32_t readLittle(const unsigned char* bytes, int count){
    uint32_t value = 0;
    for (int i = 0; i < count; i++) {
        value |= (uint32_t)bytes[i] << (8 * i);
    }
    return value;
}

static bool readFloatWav(const char* path, std::vector<float>& samples, unsigned int& rate, unsigned int& channels){
    std::ifstream file(path, std::ios::binary);
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        return false;
    }
    bool format = false;
    size_t position = 12;
    while (position + 8 <= bytes.size()) {
        const uint32_t size = readLittle(bytes.data() + position + 4, 4);
        const unsigned char* body = bytes.data() + position + 8;
        if (position + 8 + size > bytes.size()) {
            return false;
        }
        if (std::memcmp(bytes.data() + position, "fmt ", 4) == 0 && size >= 16) {
            // FFmpeg writes float32 as WAVE_FORMAT_EXTENSIBLE, the real tag at offset 24
            const uint32_t tag = readLittle(body, 2);
            const uint32_t subFormat = (tag == 0xFFFE && size >= 26) ? readLittle(body + 24, 2) : tag;
            if (subFormat != 3 || readLittle(body + 14, 2) != 32) {
                return false;
            }
            channels = readLittle(body + 2, 2);
            rate = readLittle(body + 4, 4);
            format = true;
        } else if (std::memcmp(bytes.data() + position, "data", 4) == 0 && format) {
            samples.resize(size / 4);
            std::memcpy(samples.data(), body, samples.size() * 4);
            return true;
        }
        position += 8 + (size_t)size + (size & 1);
    }
    return false;
}

static std::string number(const std::optional<double>& value){
    if (!value) {
        return "null";
    }
    char text[64];
    std::snprintf(text, sizeof(text), "%.6f", *value);
    return text;
}

int main(int argc, char** argv){
    for (int i = 1; i < argc; i++) {
        std::vector<float> samples;
        unsigned int rate = 0;
        unsigned int channels = 0;
        if (!readFloatWav(argv[i], samples, rate, channels) || channels == 0) {
            std::fprintf(stderr, "%s: not a float32 WAV\n", argv[i]);
            return 1;
        }
        const LoudnessResult r = AudioMeter::measure(samples.data(), samples.size() / channels, rate, channels);
        std::printf("{\"integrated\": %s, \"momentary_max\": %s, \"short_term_max\": %s, \"lra\": %s, \"true_peak\": %s, \"frames\": %llu}\n",
                    number(r.integrated).c_str(), number(r.momentaryMax).c_str(), number(r.shortTermMax).c_str(),
                    number(r.lra).c_str(), number(r.truePeakDb).c_str(), (unsigned long long)r.frames);
    }
    return 0;
}
