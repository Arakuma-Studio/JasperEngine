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
        const EffectManifest* manifest = nullptr;
        bool filterForm = false;
        Rml::Element* element = nullptr;
        std::shared_ptr<EffectElementState> state;
        std::shared_ptr<const EffectParamsSnapshot> snapshot;
        float opacity = 1.f, timeScale = 1.f, timeOffset = 0.f;
        int blend = 0;
        int paintArea = 0;                    // Rml::BoxArea of the decorator's area (filters: the border box)
        std::shared_ptr<ShaderRender> shader;
        uint16_t customShaderId = 0;          // the id the shader was asked with (re-asked when it changes)
        uint32_t customShaderGeneration = 0;
        EffectFrameBlock frame{};
        // u_fs_effectParams packed for the shader's reflection; repacked when the inputs change
        std::vector<uint8_t> params;
        const void* paramsLayout = nullptr;   // the reflection the bytes were packed for
        float paramsKey[6] = {-1.f, -1.f, -1.f, -1.f, -1.f, -1.f};   // area size, font sizes, viewport
        // the area in the element's border box (CSS px) and its radii, set by prepare
        Rml::Vector2f areaOffset{0.f, 0.f};
        Rml::Vector2f areaSize{0.f, 0.f};
        float radii[4] = {0.f, 0.f, 0.f, 0.f};
        bool clipToArea = true;               // @geometry fill: the shader clips to the rounded area
        // trail and ripples in local px (filled by prepare)
        float trail[32][4];
        float ripples[8][4];
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
        std::unordered_map<EffectInstance*, std::unique_ptr<EffectInstance>> live;
        EffectStats totals;
        EffectStats frameTotals;
        uint64_t filterCompiles = 0;
    };

}

#endif //HTMLUI_EFFECTS_EFFECTRUNTIME_H
