// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef GPUQUIRKS_H
#define GPUQUIRKS_H

// GPU quirks (docs/device-tiers.md "GPU quirks", docs/specs/release-performance-and-gpu-quirks.md
// P2): named driver workarounds that change how a frame is made, never what it shows. Each quirk
// is a lever in C++ (GpuQuirk, read in exactly one place of the engine); the rules saying where
// one applies are data (engine/core/device/gpu_quirks.json, embedded as GpuQuirksData.cpp by
// tools/device/convert_forge_gpu_data.py --embed). Resolved once per process, after the render
// device exists and before the shader creation is set up; overridable with --gpu-quirk,
// ?gpu-quirk= and JASPERENGINE_GPU_QUIRKS.

#include "Export.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine {

    enum class GpuQuirk : uint8_t {
        // GL: no worker context (SokolAsyncCreate::chooseGlMode)
        GlNoWorkerContext,
        // GL: the driver's compile threads stay off (SokolAsyncCreate::chooseGlMode)
        GlNoCompileThreads,
        // GL: no warm draws on the worker context (SokolAsyncCreate's warmOn)
        GlNoWarmDraws,
        // tests only (builds with JASPERENGINE_TESTS): the clear colour changes, the red control of
        // the equivalence checks (GpuQuirksGpu.EquivalenceCheckCatchesAVisibleChange)
        TestTint,
        Count
    };

    // what a rule is matched against (R15)
    struct GpuQuirkDevice {
        std::string renderer;   // the renderer as the device names it (WebGL: the unmasked one)
        std::string driver;     // SystemRender::getDriverVersion
        std::string api;        // "glcore", "gles3", "metal", "d3d11", "vulkan"
        std::string platform;   // "android", "apple", "pc", "web" (the tier database's)
        std::string osVersion;  // "14.1.0", "10.0.22631"; on Android the API level
    };

    // one forced override: --gpu-quirk name:on|off
    struct GpuQuirkOverride {
        GpuQuirk quirk = GpuQuirk::Count;
        bool on = false;
    };

    struct GpuQuirkState {
        bool active = false;
        // "rule", "forced_on", "forced_off" or "" (not active, no override)
        std::string source;
        // the matching rule's reason (empty when forced on without a rule)
        std::string reason;
    };

    class JASPERENGINE_API GpuQuirks {
    public:
        // ---- the catalogue (R13)
        // the quirks a build knows, in enum order (test_tint only with the test hooks)
        static std::vector<GpuQuirk> known();
        static const char* name(GpuQuirk quirk);
        static const char* description(GpuQuirk quirk);
        static bool fromName(const std::string& name, GpuQuirk& quirk);
        // "gl_no_worker_context, gl_no_compile_threads, gl_no_warm_draws"
        static std::string knownNames();

        // ---- the rules (R14, R15)
        struct Rule {
            GpuQuirk quirk = GpuQuirk::Count;
            std::string quirkName;  // as written (an unknown name keeps quirk == Count)
            std::string reason;
            std::string issue;
            std::vector<std::string> checks;
            std::vector<std::string> api, platform, vendor, renderer, driver;
            std::string driverAtLeast, driverBelow, osAtLeast, osBelow;
        };
        // the embedded gpu_quirks.json, parsed on first use
        static const std::vector<Rule>& builtInRules();
        static std::string embeddedJson();
        static bool parseRules(const std::string& json, std::vector<Rule>& rules, std::string& error);
        static bool ruleMatches(const Rule& rule, const GpuQuirkDevice& device);
        // the first number of the form \d+(\.\d+)+ in text ("" none)
        static std::string dottedNumber(const std::string& text);
        // a < b component-wise (missing components are 0); false when either has no number
        static bool versionLess(const std::string& a, const std::string& b);

        // ---- overrides (R21)
        // "name:on|off[,name:on|off...]"; false with the error (R21's text) for a bad item
        static bool parseOverrides(const std::string& text, std::vector<GpuQuirkOverride>& out, std::string& error);
        // a player's --gpu-quirk values, and the benchmark's (before the render device is set up)
        static void setCommandLineOverrides(const std::vector<GpuQuirkOverride>& overrides);
        // JASPERENGINE_GPU_QUIRKS and the deprecated aliases (JASPER_GL_SOFTWARE_WORKER=1,
        // JASPER_GL_WARM_DRAWS=1), each problem a warning; the command line goes over them
        static std::vector<GpuQuirkOverride> environmentOverrides();

        // ---- resolution (R17)
        // the rules, then the overrides (later ones win)
        static std::vector<GpuQuirkState> resolve(const GpuQuirkDevice& device, const std::vector<Rule>& rules,
                                                  const std::vector<GpuQuirkOverride>& overrides);
        // what the running render device is (SystemRender, System, Engine::getPlatform)
        static GpuQuirkDevice runningDevice();
        // the running device's set, once per process (SystemRender::setup), logged in one line
        static void resolveForDevice();
        // installs a set (tests: a forced set on the running device, then the creation set up again)
        static void install(const std::vector<GpuQuirkState>& states);
        static bool isResolved();

        // ---- consumers and reports (R18, R22, R23)
        static bool active(GpuQuirk quirk) {
            return quirk < GpuQuirk::Count && activeFlags[(size_t)quirk];
        }
        static const GpuQuirkState& state(GpuQuirk quirk);
        static std::vector<std::string> activeNames();
        static std::vector<std::string> forcedOffNames();
        // "gl_no_worker_context (rule: ...), gl_no_warm_draws (forced on)" or "none"
        static std::string describe();

    private:
        static bool activeFlags[(size_t)GpuQuirk::Count];
    };

}

#endif // GPUQUIRKS_H
