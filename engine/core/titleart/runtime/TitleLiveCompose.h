// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S81-title-art-runtime.md R9, R15; S83 D10, R35, §6.4, §6.12)
// - Purpose: what makes a Realtime title's change a live compose instead of a re-render. A change
//   whose every changed field is an L param of an effect or generator with a live variant
//   (`scale_split` or `kernel`) runs that effect's passes (and what follows them in its stack, the
//   composites above it and the output) in the same frame over inputs the previous render kept:
//   the layer's content and the stack input of the live effect, or a generator's followed layer.
//   Nothing before the live effect, no analysis and no readback runs again.
// - Rules: a live effect followed in its stack by an enabled effect without a live variant
//   re-renders (S83 §6.12's suffix rule, S85's live class), as does a change whose predicted cost
//   on the device's tier (S83 §6.4's model: a scale_split costs one full-resolution equivalent,
//   a kernel its estimate) is over the title's live budget (0.5 ms on mobile and web, 1 ms on
//   desktop). check_title's fx_live_budget reports both with the same functions.
// - Tiers: S83's mobile tier (FxTier::Mobile: its reductions, D11, D12, §6.7) is the device's on
//   S56's Mobile and Web platform groups and on a device of S56's Low tier; the editor's Game
//   view simulation sets both, so Play shows the simulated device's tier.
// - Thread-safety: pure functions of the document and the registry.

#pragma once

#include "Export.h"
#include "device/DeviceTypes.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/TitleEffectRegistry.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::titleart {

    // a field an override writes, when it is an L param of a live variant
    struct JASPERENGINE_API TitleLiveTarget {
        std::string layerId;
        int effect = -1;                        // its index in the layer's `effects`; -1: a generator layer's params
        std::string param;
        const EffectDef* effectDef = nullptr;
        const GeneratorDef* generatorDef = nullptr;
    };
    // the live target `path` (a field path, shorthands accepted) of layer `layerId` writes; false
    // for any other field (part effects, styles, layers, effects without a live variant)
    JASPERENGINE_API bool titleLiveTargetOf(const TitleArtDoc& doc, const std::string& layerId, const std::string& path, TitleLiveTarget& out);

    // the inputs (RenderRequest::keepLive) a full render of `doc` keeps: those of every Exposed
    // parameter whose targets are all live
    JASPERENGINE_API std::vector<std::string> titleLiveKeeps(const TitleArtDoc& doc);

    // the inputs a live compose of `changed` starts from (RenderRequest::liveInputs' names)
    JASPERENGINE_API std::vector<std::string> titleLiveInputsOf(const TitleArtDoc& doc, const std::vector<TitleLiveTarget>& changed);

    // what live composes changed since the render that kept the inputs: layer id -> the lowest
    // effect index changed (-1: a generator layer's params)
    using TitleLiveTouched = std::map<std::string, int>;
    JASPERENGINE_API void touchTitleLive(TitleLiveTouched& touched, const std::vector<TitleLiveTarget>& changed);
    // the kept input `name` still shows what it showed when it was kept: no live change since
    // touched its layer's effects before it, a layer inside it, or the layer a generator follows
    JASPERENGINE_API bool titleLiveInputValid(const TitleArtDoc& doc, const std::string& name, const TitleLiveTouched& touched);

    // why a live compose of `changed` re-renders under the suffix rule ("" when it does not)
    JASPERENGINE_API std::string titleLiveSuffixReason(const TitleArtDoc& doc, const std::vector<TitleLiveTarget>& changed);

    // predicted milliseconds of the live compose of `changed` on `tier` (S83 §6.4: the desktop tier
    // at the canvas size, the mobile tier on a 1024 x 512 surface)
    JASPERENGINE_API double titleLiveCostMs(const TitleArtDoc& doc, const std::vector<TitleLiveTarget>& changed, FxTier tier);

    struct JASPERENGINE_API TitleLiveDecision {
        bool live = false;                      // a live compose
        std::vector<TitleLiveTarget> changed;   // the live targets that changed
        std::vector<std::string> inputs;        // what it starts from
        double costMs = 0.0;                    // predicted on the tier
        // a live param changed but the change re-renders: why (the suffix rule, the budget)
        std::string reason;
    };
    // a change of the instance values from `from` to `to` (the field overrides of two states of
    // `doc`, whatever else of the states is equal)
    JASPERENGINE_API TitleLiveDecision decideTitleLive(const TitleArtDoc& doc, const std::vector<FieldOverride>& from,
                                                       const std::vector<FieldOverride>& to, FxTier tier, double budgetMs);

    // R9: one title's live budget on a platform group (desktop 1 ms, mobile and web 0.5 ms)
    JASPERENGINE_API double titleLiveBudgetMs(DevicePlatformGroup group);
    // S83's tier on a device: Mobile on S56's Mobile and Web groups and on its Low tier
    JASPERENGINE_API FxTier titleFxTierFor(DevicePlatformGroup group, std::optional<DeviceTier> tier);

}
