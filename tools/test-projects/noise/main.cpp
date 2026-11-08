// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T2's test project (R41, R72, AC37, AC48): the golden table on whatever runs this. On the web
// (WEB_PROJECT=noise tools/build.sh web, GATE=noise tools/build.sh web-gate, both also with WASM_SIMD=0),
// on Android (ANDROID_PROJECT=noise tools/build.sh android, then tools/android-emulator-test.sh) and
// on an iOS device from Xcode it computes every case with the back end the target ships, compares
// with the one table, and logs `noise_selfcheck=pass table=<hash>`. A difference is a kernel bug.
//
// With ?noise_bench=1 it first measures R46's web columns on one thread, and logs
// noise_web_budgets=pass when they are met (run on an idle machine: elsewhere it only reports).

#include "JasperEngine.h"
#include "noise/NoiseKernels.h"

// The cases are in this unit (R38 holds for them: a target's own flags cannot be assumed here, so the
// file pins no contraction itself)
#include "../../../engine/tests/core/noise/NoiseGoldenCases.cpp"
#include "NoisePresetData.h"

#include <string>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

EM_JS(int, noiseBenchRequested, (), {
    return new URLSearchParams(globalThis.location ? globalThis.location.search : "").get("noise_bench") === "1" ? 1 : 0;
});
#endif

using namespace jasperengine;

Scene scene;

namespace {

    void selfCheck() {
        std::vector<noise::golden::Preset> presets;
        for (const NoisePresetText* preset = kNoisePresets; preset->id; preset++) {
            presets.push_back(noise::golden::Preset{preset->id, preset->json});
        }
        const auto cases = noise::golden::computeCases(noise::Backend::Native, presets);
        size_t count = 0;
        const noise::golden::Expected* table = noise::golden::expectedTable(count);
        int failures = 0;
        for (const auto& entry : cases) {
            const noise::golden::Expected* expected = nullptr;
            for (size_t i = 0; i < count; i++) {
                if (entry.first == table[i].name) expected = &table[i];
            }
            if (!expected || expected->hash != entry.second) {
                Log::print("[gate] noise case %s got %016llx", entry.first.c_str(), (unsigned long long)entry.second);
                failures++;
            }
        }
        if (cases.size() != count) failures++;
        Log::print("[gate] noise kernels: %s", noise::nativeBackendName());
        Log::print("[gate] noise_selfcheck=%s table=%016llX", failures == 0 ? "pass" : "FAIL", (unsigned long long)noise::golden::tableHash());
    }

#ifdef __EMSCRIPTEN__
    double kernelMpts(noise::Basis basis, int dims) {
        const size_t count = 4096;
        std::vector<float> x(count), y(count), z(count), out(count);
        for (size_t i = 0; i < count; i++) {
            x[i] = static_cast<float>(i) * 0.137f;
            y[i] = static_cast<float>(i) * 0.071f;
            z[i] = static_cast<float>(i) * -0.093f;
        }
        noise::BasisParams params;
        params.basis = basis;
        params.dimensions = dims;
        params.frequency = 0.37f;
        const double start = emscripten_get_now();
        size_t points = 0;
        while (emscripten_get_now() - start < 200.0) {
            noise::basisBatch(params, x.data(), y.data(), z.data(), count, out.data());
            points += count;
        }
        return static_cast<double>(points) / ((emscripten_get_now() - start) * 1000.0);
    }

    // R46: simd128's column, and the scalar column in a WASM_SIMD=0 build
    void webBench() {
#if defined(JASPERENGINE_SIMD_DISABLE) || !defined(__wasm_simd128__)
        const double budgets[8] = {8, 6, 4, 1.2, 4, 3, 2, 0.3};
#else
        const double budgets[8] = {20, 15, 10, 3, 10, 7, 5, 0.75};
#endif
        const struct { noise::Basis basis; int dims; const char* name; } rows[8] = {
            {noise::Basis::Value, 2, "value2d"}, {noise::Basis::Perlin, 2, "perlin2d"}, {noise::Basis::Simplex, 2, "simplex2d"}, {noise::Basis::Cellular, 2, "cellular2d"},
            {noise::Basis::Value, 3, "value3d"}, {noise::Basis::Perlin, 3, "perlin3d"}, {noise::Basis::Simplex, 3, "simplex3d"}, {noise::Basis::Cellular, 3, "cellular3d"}};
        bool met = true;
        for (int i = 0; i < 8; i++) {
            const double rate = kernelMpts(rows[i].basis, rows[i].dims);
            Log::print("[bench] %s %.2f Mpts/s (budget %.2f)", rows[i].name, rate, budgets[i]);
            met = met && rate >= budgets[i];
        }
        Log::print("[bench] noise_web_budgets=%s", met ? "pass" : "fail");
    }
#endif

}

void init() {
#ifdef __EMSCRIPTEN__
    if (noiseBenchRequested()) webBench();
#endif
    selfCheck();
    Engine::setCanvasSize(320, 180);
    scene.setBackgroundColor(0.2f, 0.4f, 0.6f, 1.0f);
    Engine::setScene(&scene);
}
