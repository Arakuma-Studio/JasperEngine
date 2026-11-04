// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// jasperengine-noise-golden (T2 R72): the self-check of the noise library on any platform. There is no
// CI, so this is what a person runs where this repository's host cannot. Exits 0 on pass and 1 on any
// mismatch: a mismatch is a kernel bug, never a new table.

#include "NoiseGoldenCases.h"
#include "noise/NoiseKernels.h"
#include "noise_presets.gen.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace jasperengine::noise;

namespace {

    const char* targetName() {
#if defined(__ANDROID__)
    #if defined(__aarch64__)
        return "android-arm64-v8a";
    #elif defined(__arm__)
        return "android-armeabi-v7a";
    #elif defined(__x86_64__)
        return "android-x86_64";
    #elif defined(__i386__)
        return "android-x86";
    #else
        return "android";
    #endif
#elif defined(_WIN32)
        return "windows-x64";
#elif defined(__APPLE__)
    #if defined(__aarch64__)
        return "apple-arm64";
    #else
        return "apple-x86_64";
    #endif
#elif defined(__EMSCRIPTEN__)
        return "web";
#elif defined(__aarch64__)
        return "linux-arm64";
#elif defined(__arm__)
        return "linux-arm";
#elif defined(__x86_64__)
        return "linux-x86_64";
#else
        return "unknown";
#endif
    }

    std::vector<golden::Preset> embeddedPresets() {
        std::vector<golden::Preset> presets;
        for (const NoisePresetEmbeddedFile* file = noise_preset_embedded_files; file->id; file++) {
            golden::Preset preset;
            preset.id = file->id;
            preset.json.assign(reinterpret_cast<const char*>(file->data), file->size);
            presets.push_back(std::move(preset));
        }
        return presets;
    }

    // One back end against the table. flip: the self-test's wrong expected hash at that case.
    int checkBackend(Backend backend, const char* backendName, const std::vector<golden::Preset>& presets, FILE* report, bool flip) {
        const auto cases = golden::computeCases(backend, presets);
        size_t expectedCount = 0;
        const golden::Expected* expected = golden::expectedTable(expectedCount);
        int failures = 0;
        for (size_t i = 0; i < cases.size(); i++) {
            const golden::Expected* match = nullptr;
            for (size_t e = 0; e < expectedCount; e++) {
                if (cases[i].first == expected[e].name) { match = &expected[e]; break; }
            }
            if (!match) {
                std::fprintf(report, "noise-golden case %s not in the table\n", cases[i].first.c_str());
                failures++;
                continue;
            }
            const uint64_t want = match->hash ^ (flip && i == 0 ? 1ull : 0ull);
            if (cases[i].second != want) {
                std::fprintf(report, "noise-golden case %s mismatch got=0x%016llX want=0x%016llX\n", cases[i].first.c_str(),
                             static_cast<unsigned long long>(cases[i].second), static_cast<unsigned long long>(want));
                failures++;
            }
        }
        for (size_t e = 0; e < expectedCount; e++) {
            bool found = false;
            for (const auto& entry : cases) found = found || entry.first == expected[e].name;
            if (!found) {
                std::fprintf(report, "noise-golden case %s in the table but not computed\n", expected[e].name);
                failures++;
            }
        }
        std::fprintf(report, "%s\n", golden::reportLine(targetName(), backendName, cases.size(), failures == 0).c_str());
        return failures;
    }

    int writeTable(const char* path, const std::vector<golden::Preset>& presets) {
        FILE* file = std::fopen(path, "wb");
        if (!file) {
            std::fprintf(stderr, "cannot write %s\n", path);
            return 1;
        }
        std::fprintf(file, "// (c) Eduardo Doria and contributors\n// SPDX-License-Identifier: MIT\n\n");
        std::fprintf(file, "// T2 R41: one table for every target. A difference on a target is a bug in the kernel or its flags: never\n// regenerate this for one target. Written by jasperengine-noise-golden --write-table.\n\n");
        for (const auto& entry : golden::computeCases(Backend::Native, presets)) {
            std::fprintf(file, "{\"%s\", 0x%016llXull},\n", entry.first.c_str(), static_cast<unsigned long long>(entry.second));
        }
        std::fclose(file);
        return 0;
    }

}

int main(int argc, char** argv) {
    bool allBackends = false;
    bool selfTest = false;
    const char* reportPath = "-";
    const char* tablePath = nullptr;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--all-backends") == 0) allBackends = true;
        else if (std::strcmp(argv[i], "--self-test") == 0) selfTest = true;
        else if (std::strcmp(argv[i], "--report") == 0 && i + 1 < argc) reportPath = argv[++i];
        else if (std::strcmp(argv[i], "--write-table") == 0 && i + 1 < argc) tablePath = argv[++i];
        else {
            std::fprintf(stderr, "usage: jasperengine-noise-golden [--all-backends] [--report <file>|-] [--self-test] [--write-table <file>]\n");
            return 2;
        }
    }
    const std::vector<golden::Preset> presets = embeddedPresets();
    if (tablePath) return writeTable(tablePath, presets);

    FILE* report = stdout;
    if (std::strcmp(reportPath, "-") != 0) {
        report = std::fopen(reportPath, "w");
        if (!report) {
            std::fprintf(stderr, "cannot write %s\n", reportPath);
            return 2;
        }
    }
    int failures = checkBackend(Backend::Native, nativeBackendName(), presets, report, selfTest);
    if (allBackends && hasScalarBackend() && std::strcmp(nativeBackendName(), "scalar") != 0) {
        failures += checkBackend(Backend::Scalar, "scalar", presets, report, false);
    }
    if (report != stdout) std::fclose(report);
    return failures == 0 ? 0 : 1;
}
