// Differential checks and paired timings against the actual baseline translation unit.
#include "StringUtils.h"
#undef StringUtils_h
#define jasperengine baseline
#include "StringUtils.h"
#undef jasperengine

#include <algorithm>
#include <ctime>
#include <cstdlib>
#include <iostream>
#include <new>
#include <random>

#ifdef TRACK_ALLOCATIONS
size_t allocationCalls = 0;
size_t allocationBytes = 0;
void* operator new(size_t size) {
    ++allocationCalls;
    allocationBytes += size;
    if (void* result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc();
}
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, size_t) noexcept { std::free(ptr); }
#endif

using Before = baseline::StringUtils;
using After = jasperengine::StringUtils;
volatile size_t sink = 0;

void check(const std::string& text) {
    for (bool initial : {false, true}) {
        bool invalidBefore = initial, invalidAfter = initial;
        if (Before::decodeUtf8ToCodepoints(text, invalidBefore) != After::decodeUtf8ToCodepoints(text, invalidAfter)
            || invalidBefore != invalidAfter || Before::countCodepoints(text) != After::countCodepoints(text))
            std::abort();
        invalidBefore = invalidAfter = initial;
        if (Before::utf8ToWString(text, invalidBefore) != After::utf8ToWString(text, invalidAfter)
            || invalidBefore != invalidAfter)
            std::abort();
    }
}

template <typename Work>
double timed(Work work, size_t iterations) {
    const auto start = std::clock();
    for (size_t i = 0; i < iterations; ++i) sink = sink + work();
    return double(std::clock() - start) * 1e9 / CLOCKS_PER_SEC / iterations;
}

template <typename BeforeWork, typename AfterWork>
void measure(const std::string& name, const std::string& text, BeforeWork before, AfterWork after) {
    const size_t iterations = std::max<size_t>(1000, 16000000 / std::max<size_t>(1, text.size()));
    std::vector<double> beforeTimes, afterTimes;
    for (int trial = 0; trial < 9; ++trial) {
        // Alternate order to reduce sensitivity to clock/temperature drift.
        if (trial % 2) {
            afterTimes.push_back(timed(after, iterations));
            beforeTimes.push_back(timed(before, iterations));
        } else {
            beforeTimes.push_back(timed(before, iterations));
            afterTimes.push_back(timed(after, iterations));
        }
    }
    std::sort(beforeTimes.begin(), beforeTimes.end());
    std::sort(afterTimes.begin(), afterTimes.end());
    std::cout << name << " bytes=" << text.size() << " baseline_ns=" << beforeTimes[4]
              << " after_ns=" << afterTimes[4] << " speedup=" << beforeTimes[4] / afterTimes[4] << '\n';
}

int main(int argc, char**) {
#ifdef TRACK_ALLOCATIONS
    for (size_t length : {size_t(0), size_t(32), size_t(1024), size_t(16384)}) {
        std::string text(length, 'a');
        allocationCalls = allocationBytes = 0;
        sink = Before::countCodepoints(text);
        const size_t beforeCalls = allocationCalls, beforeBytes = allocationBytes;
        allocationCalls = allocationBytes = 0;
        sink = After::countCodepoints(text);
        const size_t afterCalls = allocationCalls, afterBytes = allocationBytes;
        if (afterCalls != 0 || afterBytes != 0) std::abort();
        std::cout << "count bytes=" << length << " baseline_allocations=" << beforeCalls
                  << " baseline_allocated_bytes=" << beforeBytes << " after_allocations=" << afterCalls
                  << " after_allocated_bytes=" << afterBytes << '\n';
    }
    return 0;
#endif
    check("");
    for (unsigned a = 0; a < 256; ++a) {
        check(std::string(1, static_cast<char>(a)));
        for (unsigned b = 0; b < 256; ++b)
            check(std::string{static_cast<char>(a), static_cast<char>(b)});
    }
    std::mt19937 rng(49017);
    for (unsigned i = 0; i < 10000; ++i) {
        std::string bytes(rng() % 128, '\0');
        for (char& byte : bytes) byte = static_cast<char>(rng());
        check(bytes);
    }
    // Cover every Unicode scalar, surrogate encodings, and malformed boundary sequences.
    std::string scalars;
    for (uint32_t cp = 0; cp <= 0x10ffff; ++cp) {
        if (cp < 0x80) scalars += static_cast<char>(cp);
        else if (cp < 0x800) {
            scalars += static_cast<char>(0xc0 | (cp >> 6));
            scalars += static_cast<char>(0x80 | (cp & 63));
        } else if (cp < 0x10000) {
            scalars += static_cast<char>(0xe0 | (cp >> 12));
            scalars += static_cast<char>(0x80 | ((cp >> 6) & 63));
            scalars += static_cast<char>(0x80 | (cp & 63));
        } else {
            scalars += static_cast<char>(0xf0 | (cp >> 18));
            scalars += static_cast<char>(0x80 | ((cp >> 12) & 63));
            scalars += static_cast<char>(0x80 | ((cp >> 6) & 63));
            scalars += static_cast<char>(0x80 | (cp & 63));
        }
    }
    check(scalars);
    for (const std::string bytes : {"\xe0\x80\x80", "\xed\xa0\x80", "\xf0\x80\x80\x80", "\xf4\x90\x80\x80", "\xf5\xbf\xbf\xbf"})
        for (size_t len = 0; len <= bytes.size(); ++len) check(bytes.substr(0, len));
    std::cout << "Differential checks passed (all Unicode scalars, 65,536 byte pairs, 10,000 random inputs).\n";
    if (argc > 1) return 0;
    for (size_t length : {size_t(32), size_t(1024), size_t(16384)}) {
        for (const std::string kind : {"ascii", "multilingual", "invalid"}) {
            std::string text;
            const std::string unit = kind == "ascii" ? "hello world " : kind == "multilingual" ? "Hello \xc3\xa9 \xe4\xb8\xad \xf0\x9f\x8c\x8d " : "a\xff\xc0\x80";
            while (text.size() < length) text += unit;
            text.resize(length);
            measure("count/" + kind, text, [&] { return Before::countCodepoints(text); }, [&] { return After::countCodepoints(text); });
            measure("wide/" + kind, text, [&] { bool invalid; return Before::utf8ToWString(text, invalid).size(); }, [&] { bool invalid; return After::utf8ToWString(text, invalid).size(); });
        }
    }
}
