// Compile this harness with the production Base64.cpp (see run-base64.py).
#include "util/Base64.h"
#include <algorithm>
#include <chrono>
#include <clocale>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <random>
#include <stdexcept>

using jasperengine::Base64;

static void require(bool value) {
    if (!value) throw std::runtime_error("Base64 output mismatch");
}

// Frozen compatibility oracle: deliberately matches the legacy decoder, including
// stopping at the first invalid byte and accepting unpadded input.
static std::vector<unsigned char> legacyDecode(const std::string& input) {
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned char values[4]{};
    std::vector<unsigned char> result;
    int count = 0;
    auto emit = [&] {
        for (int i = count; i < 4; ++i) values[i] = 0;
        for (auto& value : values) value = alphabet.find(value);
        if (count > 1) result.push_back((values[0] << 2) + ((values[1] & 0x30) >> 4));
        if (count > 2) result.push_back(((values[1] & 0xf) << 4) + ((values[2] & 0x3c) >> 2));
        if (count > 3) result.push_back(((values[2] & 0x3) << 6) + values[3]);
    };
    for (unsigned char c : input) {
        if (c == '=' || !(std::isalnum(c) || c == '+' || c == '/')) break;
        values[count++] = c;
        if (count == 4) { emit(); count = 0; }
    }
    if (count) emit();
    return result;
}

static void check() {
    const char* plain[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    const char* encoded[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    for (int i = 0; i < 7; ++i) {
        const std::string p = plain[i];
        require(Base64::encode(reinterpret_cast<const unsigned char*>(p.data()), p.size()) == encoded[i]);
        require(Base64::decode(encoded[i]) == std::vector<unsigned char>(p.begin(), p.end()));
    }
    std::mt19937 rng(42);
    for (size_t length = 0; length < 4096; ++length) {
        std::vector<unsigned char> bytes(length);
        for (auto& byte : bytes) byte = rng();
        std::string encodedBytes = Base64::encode(bytes.data(), bytes.size());
        require(Base64::decode(encodedBytes) == bytes);
        while (!encodedBytes.empty() && encodedBytes.back() == '=') encodedBytes.pop_back();
        require(Base64::decode(encodedBytes) == legacyDecode(encodedBytes));
    }
    // Every possible byte in each quartet position, including NUL/high bytes,
    // padding, whitespace, invalid punctuation, and noncanonical trailing bits.
    for (const char* locale : {"C", "C.UTF-8", ""}) {
        if (!std::setlocale(LC_CTYPE, locale)) continue;
        for (int byte = 0; byte < 256; ++byte) {
            for (size_t pos = 0; pos < 9; ++pos) {
                std::string s = "Zm9vYmFyZ";
                s[pos] = static_cast<char>(byte);
                require(Base64::decode(s) == legacyDecode(s));
                require(Base64::decode(s.substr(0, pos + 1)) == legacyDecode(s.substr(0, pos + 1)));
            }
        }
    }
    std::cout << "compatibility: PASS\n";
}

int main(int argc, char** argv) {
    check();
    if (argc > 1 && std::string(argv[1]) == "--check") return 0;
    std::mt19937 rng(2026);
    for (size_t bytes : {size_t(4096), size_t(1024 * 1024), size_t(8 * 1024 * 1024)}) {
        std::vector<unsigned char> input(bytes);
        for (auto& byte : input) byte = rng();
        const auto encoded = Base64::encode(input.data(), input.size());
        const size_t repeats = std::max(size_t(1), size_t(8 * 1024 * 1024) / bytes);
        for (bool decode : {false, true}) {
            std::vector<double> samples, cpuSamples;
            size_t checksum = 0;
            for (int sample = 0; sample < 7; ++sample) {
                const auto start = std::chrono::steady_clock::now();
                const auto cpuStart = std::clock();
                for (size_t repeat = 0; repeat < repeats; ++repeat) {
                    if (decode) { auto output = Base64::decode(encoded); checksum += output.size() + output[repeat % bytes]; }
                    else { auto output = Base64::encode(input.data(), input.size()); checksum += output.size() + output[repeat % output.size()]; }
                }
                const auto cpuEnd = std::clock();
                const auto end = std::chrono::steady_clock::now();
                cpuSamples.push_back(1000.0 * (cpuEnd - cpuStart) / CLOCKS_PER_SEC / repeats);
                samples.push_back(std::chrono::duration<double, std::milli>(end - start).count() / repeats);
            }
            std::sort(samples.begin(), samples.end());
            std::sort(cpuSamples.begin(), cpuSamples.end());
            std::cout << (decode ? "decode" : "encode") << ",bytes=" << bytes << ",median_wall_ms=" << samples[3] << ",median_cpu_ms=" << cpuSamples[3] << ",checksum=" << checksum << '\n';
        }
    }
}
