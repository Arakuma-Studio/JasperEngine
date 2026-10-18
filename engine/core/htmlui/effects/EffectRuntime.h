// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUI_EFFECTS_EFFECTRUNTIME_H
#define HTMLUI_EFFECTS_EFFECTRUNTIME_H

// The per-element side of the effects (phase-5 spec 3.6): the instances RmlUi's decorators and
// filters hold, the state that survives re-instancing (integrated clocks, pointer smoothing,
// the pointer trail, ripple events), the effect clock, and the frame block every draw uploads.

#include "Export.h"
#include "EffectTypes.h"
#include "EffectValue.h"

#include <RmlUi/Core/Plugin.h>
#include <RmlUi/Core/Types.h>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Rml {
    class Context;
    class Element;
}

namespace jasperengine {

    class ShaderRender;

}

namespace jasperengine::htmlui {

    // The effects' clock: a fixed time when one is set (setFixedTime; UIDocument.setEffectTime,
    // MCP set_ui_effect_time), else the UI clock (HtmlUIClock::now()). Fixed, integrated clocks use
    // the closed form, dt reads 0 and the pointer smoothing snaps.
    class JASPERENGINE_INTERNAL EffectClock {
    public:
        static double now();
        static void setFixedTime(std::optional<double> seconds);
        static bool isFixed();
        static std::optional<double> getFixedTime();
    };

    struct EffectStateImage;   // EffectSimulator.h: a pooled state image

    // an injection into a simulation (UIElement::effectSplat): area-local CSS px, px/s, linear RGBA
    struct EffectSplatData {
        Rml::Vector2f position{0.f, 0.f};
        float radius = 24.f;
        float strength = 1.f;
        Rml::Vector2f velocity{0.f, 0.f};
        float color[4] = {1.f, 1.f, 1.f, 1.f};
    };

    // One element's simulation of a stateful effect (phase 6, docs/html-ui/phase-6-stateful-effects.md
    // 3.1, 3.6-3.8): its state images, its clock and its inputs. Owned by the element's effect state,
    // so it survives re-instancing; its images return to the pool when it is destroyed.
    struct EffectSimulation {
        struct Images {
            std::shared_ptr<EffectStateImage> image[2];   // [1] empty for a single-image state
            int current = 0;                              // the image passes read and the draw samples
            int width = 0, height = 0;
        };
        const EffectManifest* manifest = nullptr;
        uint64_t signature = 0;                 // manifest->stateSignature() when allocated
        std::vector<Images> states;             // manifest order
        uint64_t poolGeneration = 0;            // EffectStatePool's when allocated (a view loss drops them)
        bool allocated = false;
        bool refused = false;                   // the hard cap or an unsupported format: not simulated
        Rml::Vector2f areaSize{0.f, 0.f};       // the area the images were sized for
        size_t bytes = 0;
        // the clock (3.6)
        int64_t steps = 0;                      // since the reset
        double epoch = 0.0;                     // the clock value of step 0 (live)
        bool anchored = false;
        double lastTime = 0.0;                  // the clock value last advanced at
        double resetTime = 0.0;                 // the clock value of the reset (warm-up ends there)
        double simTime = 0.0;                   // the end of the last step
        uint64_t lastAdvanceFrame = UINT64_MAX;
        bool paused = false;
        // the pinned clock's replay key
        bool pinned = false;
        double pinnedTime = 0.0;
        uint64_t pinnedKey = 0;
        // the pointer at the last step (area-local CSS px)
        Rml::Vector2f pointer{0.f, 0.f};
        bool pointerKnown = false;
        bool pointerDown = false;
        uint64_t pressSeen = 0;
        // splats: consumed by the next step (live); logged with their time (pinned, replayed)
        std::vector<EffectSplatData> pending;
        std::vector<std::pair<double, EffectSplatData>> log;
        uint64_t inputGeneration = 0;
        std::string reseedKey;                  // the @reseed parameters' values when it started
        // statistics
        uint64_t stepsRun = 0;
        uint64_t replays = 0;
        Rml::ElementDocument* document = nullptr;

        EffectSimulation() = default;
        EffectSimulation(const EffectSimulation&) = delete;
        EffectSimulation& operator=(const EffectSimulation&) = delete;
        ~EffectSimulation();
        // the images back to the pool; the clock and the inputs reset
        void releaseImages();
    };

    // per (Element, effect name, ordinal); survives re-instancing
    struct EffectElementState {
        double lastClock = -1.0;
        uint64_t lastFrame = UINT64_MAX;        // the runtime frame of the last integration
        float anim[4] = {0.f, 0.f, 0.f, 0.f};
        bool animStarted = false;
        Rml::Vector2f smoothPointer{0.f, 0.f};  // context px
        bool smoothValid = false;
        float presence = 0.f;
        Rml::Vector2f lastLocal{0.f, 0.f};       // the pointer last frame (local px), for the velocity
        Rml::Vector2f velocity{0.f, 0.f};
        bool lastLocalValid = false;
        double enterTime = -1.0;                 // the clock when the pointer entered (-1 outside)
        double pressTime = -1.0;                 // the clock of the last press inside (-1 none)
        // context px, z age s, w valid; newest first
        std::array<Rml::Vector4f, 32> trail{};
        int trailCount = 0;
        Rml::Vector2f lastTrailSample{0.f, 0.f};
        bool trailSampled = false;
        // context px, z age s, w strength
        std::array<Rml::Vector4f, 8> ripples{};
        int rippleCount = 0;
        Rml::Vector2f lastRippleSample{0.f, 0.f};
        float moveSinceRipple = 0.f;
        bool rippleTracking = false;            // the pointer was inside last frame (trigger move)
        uint64_t lastPressSeen = 0;
        // phase 6: the simulation of a stateful effect, and the live instances using this state
        // (the runtime releases the images of a simulation nothing uses any more)
        std::shared_ptr<EffectSimulation> simulation;
        int liveInstances = 0;
        uint64_t orphanFrame = UINT64_MAX;      // the frame liveInstances went to 0
    };

    // u_fs_effectFrame, member for member: 14 x vec4 = 224 bytes
    struct EffectFrameBlock {
        float size[4], time[4], anim[4], pointer[4], pointerSmooth[4], pointerEx[4], layer[4];
        float toLocalX[4], toLocalY[4], toLayerX[4], toLayerY[4], srcRect[4], radii[4], misc[4];
    };
    static_assert(sizeof(EffectFrameBlock) == 224, "u_fs_effectFrame is 224 bytes");

    // what an instancer read from its typed RmlUi properties
    struct EffectParamsSnapshot {
        const EffectManifest* manifest = nullptr;
        std::vector<EffectValue> values;   // manifest order
        float opacity = 1.f, timeScale = 1.f, timeOffset = 0.f;
        int blend = 0;                     // normal add screen multiply
        // the canonical text of every value, universal ones included (getEffects, the cache key)
        std::vector<std::pair<std::string, std::string>> canonical;
    };

    // One element's use of one effect; its address | 1 is the RmlUi shader/filter handle
    struct EffectInstance {
        bool alive = false;                   // acquired and not released (EffectRuntime::isLive)
        const EffectManifest* manifest = nullptr;
        bool filterForm = false;
        Rml::Element* element = nullptr;
        Rml::Context* context = nullptr;      // the element's when acquired (its pointer state, redraws)
        std::shared_ptr<EffectElementState> state;
        std::shared_ptr<const EffectParamsSnapshot> snapshot;
        float opacity = 1.f, timeScale = 1.f, timeOffset = 0.f;
        int blend = 0;
        float timeRates[4] = {};              // the @time parameters' values (the snapshot's, set by acquire)
        int paintArea = 0;                    // Rml::BoxArea of the decorator's area (filters: the border box)
        std::shared_ptr<ShaderRender> shader;
        uint16_t customShaderId = 0;          // the id the shader was asked with (re-asked when it changes)
        uint32_t customShaderGeneration = 0;  // ShaderPool's custom-shader generation the id was resolved in
        int8_t shaderForm = -1;               // the form the shader was asked for (1 filter, 0 paint)
        // the EffectRenderer program last drawn with (EffectRenderer::programFor): valid while
        // that renderer's program generation is the same
        void* program = nullptr;
        const void* programOwner = nullptr;
        uint64_t programGeneration = 0;
        EffectFrameBlock frame{};
        // u_fs_effectParams packed for the shader's reflection; repacked when the inputs change
        std::vector<uint8_t> params;
        const void* paramsLayout = nullptr;   // the reflection the bytes were packed for
        float paramsKey[6] = {-1.f, -1.f, -1.f, -1.f, -1.f, -1.f};   // area size, font sizes, viewport
        // what the packed values read besides the area (found once per snapshot): a length in em,
        // rem or a viewport unit (the font sizes, the viewport), a currentColor (the element's colour)
        int8_t paramsContext = -1;            // -1 unknown, 0 nothing, else bit 0 lengths, bit 1 colour
        // the area in the element's border box (CSS px) and its radii, set by prepare
        Rml::Vector2f areaOrigin{0.f, 0.f};   // the area's top-left in the context (CSS px)
        Rml::Vector2f areaOffset{0.f, 0.f};
        Rml::Vector2f areaSize{0.f, 0.f};
        float radii[4] = {0.f, 0.f, 0.f, 0.f};
        bool clipToArea = true;               // @geometry fill: the shader clips to the rounded area
        // trail and ripples in local px (filled by prepare for an effect with @pointer trail or
        // ripples, the entries past the current ones zero; uploaded only when the shader has them)
        float trail[32][4] = {};
        float ripples[8][4] = {};
        int trailUsed = 0, ripplesUsed = 0;   // the entries prepare wrote last
        uint64_t preparedFrame = UINT64_MAX;
        float seed = -1.f;                    // seedOf(element), once per instance (< 0: not yet)
    };

    struct EffectStats {
        uint64_t liveInstances = 0;
        uint64_t instancesCreated = 0;
        uint64_t compileCalls = 0;       // shader variants requested that were not ready
        uint64_t paintDraws = 0;
        uint64_t filterPasses = 0;
        uint64_t prepassPasses = 0;
        uint64_t pipelinesCreated = 0;
        uint64_t reinstances = 0;        // decorators and filters instanced again (var(), transitions)
        // phase 6 (stateful effects)
        uint64_t statefulActive = 0;     // simulations stepped this frame or the one before (frame stats: this frame)
        uint64_t stateImages = 0;        // live state images (current)
        uint64_t stateBytes = 0;         // their bytes (current)
        uint64_t stateImagesCreated = 0; // pool misses
        uint64_t statePoolBytes = 0;     // pooled, unused (current)
        uint64_t stepPasses = 0;         // fullscreen step passes
        uint64_t scatterPasses = 0;
        uint64_t stepsRun = 0;
        uint64_t replays = 0;            // pinned-clock replays
    };

    // the soft and hard limits of stateful simulations (3.10)
    struct EffectStatefulBudget {
        int perDocument = 8;
        int global = 32;
        size_t bytesPerDocument = 64u * 1024u * 1024u;
        int hardCap = 64;
    };

    // what getEffects / inspect_ui report of a simulation
    struct EffectSimulationInfo {
        bool exists = false;
        size_t bytes = 0;
        int64_t steps = 0;
        double time = 0.0;
        bool paused = false;
    };

    class JASPERENGINE_INTERNAL EffectRuntime : public Rml::Plugin {
    public:
        static EffectRuntime& get();

        // once per frame before contexts render (RmlRenderer::beginFrame; idempotent per frame)
        void beginFrame();
        uint64_t getFrame() const { return frame; }

        // pointer input in context px (HtmlUIContext)
        void onPointerMove(Rml::Context* context, Rml::Vector2f contextPx);
        void onPointerButton(Rml::Context* context, Rml::Vector2f contextPx, bool down);
        // the primary button at the last position the context was given
        void onPointerButton(Rml::Context* context, bool down);
        void onPointerLeave(Rml::Context* context);

        EffectInstance* acquire(Rml::Element* element, const EffectManifest& manifest, bool filterForm,
                                const std::shared_ptr<const EffectParamsSnapshot>& snapshot, int paintArea);
        void release(EffectInstance* instance);
        bool isLive(const EffectInstance* instance) const;
        // An animated effect (live clock) asks its context to update again at once; once per
        // context render, however many elements draw effects
        void requestRedraw(Rml::Context* context);
        // Fills the frame block and integrates the state, once per frame: the area (the paint
        // area of a decorator, the border box of a filter) from the element, the clocks, the
        // pointer mapped through the element's transform into area-local CSS px
        void prepare(EffectInstance& instance);

        const EffectStats& stats() const { return totals; }
        const EffectStats& frameStats() const { return frameTotals; }
        EffectStats& mutableStats() { return totals; }
        EffectStats& mutableFrameStats() { return frameTotals; }
        void countReinstance();
        // EffectFilter::CompileFilter calls (the TextFillFilter probe counts them)
        void countFilterCompile() { filterCompiles++; }
        uint64_t getFilterCompileCount() const { return filterCompiles; }
        // tests without frames: the next frame of the runtime (integration, ordinals)
        void advanceFrameForTests() { frame++; frameTotals = EffectStats(); frameTotals.liveInstances = live.size(); }

        // the element-state map (tests): elements and states held
        size_t elementStateCount() const;
        size_t liveInstanceCount() const { return live.size(); }

        // tests: a pointer of a context (nullopt: none), pressed `sincePress` seconds ago (<0: never)
        void setTestPointer(Rml::Context* context, std::optional<Rml::Vector2f> contextPx, bool down, float sincePress = -1.f);
        // tests: the trail every element of the context sees (context px, z age s), newest first
        void setTestTrail(Rml::Context* context, const std::vector<Rml::Vector4f>& samples);
        void clearTestInput();

        // the per-element seed: FNV-1a of the element's address string, in [0, 1)
        static float seedOf(Rml::Element* element);

        // --- phase 6: stateful effects ---
        // the instance's simulation (created on first use; nullptr for a stateless effect)
        EffectSimulation* simulationOf(EffectInstance& instance);
        // a splat queued on the element's simulations (of the named effect, or all); how many took it
        int splat(Rml::Element* element, const EffectSplatData& splat, const std::string& effectName = std::string());
        // restarts the element's simulations (of the named effect, or all); how many
        int resetSimulations(Rml::Element* element, const std::string& effectName = std::string());
        EffectSimulationInfo simulationInfo(Rml::Element* element, const std::string& effectName, int ordinal) const;
        // the pointer of a context (context px, valid) and its press count, for the per-step pointer
        bool pointerOf(Rml::Context* context, Rml::Vector2f& position, bool& down, uint64_t& pressCount) const;
        // the recorded pointer history of an instance's state (context px, z age s), newest first
        void pointerHistory(const EffectInstance& instance, std::vector<Rml::Vector4f>& out) const;
        // budgets (3.10): a simulation asks to step this frame; false when the hard cap refuses it
        bool admitSimulation(EffectSimulation& simulation);
        void setStatefulBudget(const EffectStatefulBudget& budget) { statefulBudget = budget; }
        // a destroyed simulation leaves the active set
        void forgetSimulation(EffectSimulation* simulation);
        const EffectStatefulBudget& getStatefulBudget() const { return statefulBudget; }
        size_t simulationCount() const;
        // a stateful diagnostic (HTMLUI-FX-0017/0018) to the Console, once per call
        void reportStateful(Rml::ElementDocument* document, const std::string& codeId, const std::string& message, bool error);
        // the stateful diagnostics reported so far (tests): code id and message
        const std::vector<std::pair<std::string, std::string>>& statefulDiagnostics() const { return statefulReports; }
        void clearStatefulDiagnostics();

        // Rml::Plugin
        int GetEventClasses() override;
        void OnElementDestroy(Rml::Element* element) override;
        void OnContextDestroy(Rml::Context* context) override;
        void OnDocumentUnload(Rml::ElementDocument* document) override;
        void OnShutdown() override;

    private:
        EffectRuntime() = default;

        struct PointerState {
            Rml::Vector2f position{0.f, 0.f};
            bool valid = false;
            bool down = false;
            uint64_t pressCount = 0;
            Rml::Vector2f pressPosition{0.f, 0.f};
            double pressClock = -1.0;
            // tests
            bool testOverride = false;
            float testSincePress = -1.f;
            bool testTrail = false;
            std::vector<Rml::Vector4f> trail;
        };
        struct ElementEntry {
            uint64_t acquireFrame = UINT64_MAX;
            std::map<std::string, int> ordinals;
            // key (name#ordinal) -> state, least recently used first
            std::vector<std::pair<std::string, std::shared_ptr<EffectElementState>>> states;
        };

        void integrate(EffectInstance& instance, EffectElementState& state, double now, float dt, bool fixed,
                       Rml::Vector2f areaOrigin, const PointerState* pointer);

        uint64_t frame = 0;
        uint64_t frameKey = UINT64_MAX;
        std::unordered_map<Rml::Element*, ElementEntry> elements;
        std::unordered_map<Rml::Context*, PointerState> pointers;
        // the last context prepare looked up (a context renders its elements together): reset
        // whenever `pointers` gains or loses an entry
        Rml::Context* pointerCacheContext = nullptr;
        const PointerState* pointerCache = nullptr;
        bool pointerCacheValid = false;
        // the context whose next update the animated effects asked for, once per context render
        // (beginFrame counts the renders)
        Rml::Context* redrawContext = nullptr;
        uint64_t redrawRender = UINT64_MAX;
        uint64_t renders = 0;
        std::unordered_map<EffectInstance*, std::unique_ptr<EffectInstance>> live;
        // Released instances, kept for reuse: an instance's memory stays the runtime's, so
        // isLive reads its flag (the draw touches that memory anyway) instead of looking the
        // address up in `live` every draw
        std::vector<std::unique_ptr<EffectInstance>> released;
        EffectStats totals;
        EffectStats frameTotals;
        uint64_t filterCompiles = 0;

        // phase 6
        void sweepSimulations();                // at a frame start: orphans released, budgets checked
        EffectStatefulBudget statefulBudget;
        // simulations stepped this frame / the frame before, by document
        std::unordered_map<EffectSimulation*, uint64_t> activeSimulations;
        std::unordered_map<Rml::ElementDocument*, int> overBudgetDocuments;   // the frame it was reported (once while over)
        bool hardCapReported = false;
        std::vector<std::pair<std::string, std::string>> statefulReports;
    };

}

#endif //HTMLUI_EFFECTS_EFFECTRUNTIME_H
