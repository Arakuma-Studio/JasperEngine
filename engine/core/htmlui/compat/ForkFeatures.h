// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_FORKFEATURES_H
#define COMPAT_FORKFEATURES_H

#include "Export.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace jasperengine::htmlui::compat {

    // Features of the RmlUi fork that turn a compat rewrite or emulation off (phase-2 §3.6).
    // Append only, before Count: phase 3 appends CurrentColor ExtraUnits LineHeightNormal Has
    // FocusPseudoClasses SelectorsLevel4 ListMarkers Outline BackgroundLayers ObjectFit
    // TextShadow TextProperties Sticky BoxParity AnimationLonghands IndividualTransforms
    // (phase-3 Appendix A), phase 5 RegisteredProperties TextFillFilter. Each new value gets
    // a kPresent entry, a name and a probe in ForkFeatures.cpp.
    enum class ForkFeature : uint8_t {
        None, Calc, BorderStyle, PercentRadius, PseudoElements, RootIsWhere, Fixed,
        GroupOpacity, TimingFunctions, Important, CssWideKeywords, Grid,
        // appended by phase 3 (phase-3 spec Appendix A)
        TextShadow, TextProperties,
        CurrentColor, ExtraUnits, LineHeightNormal,
        Count
    };

    // A set of fork features. present() is the compiled-in table (kPresent in
    // ForkFeatures.cpp; true = the fork has the feature, so compat passes the CSS through,
    // normalized, and reports no fallback). Tests force both states with with().
    class JASPERENGINE_API ForkFeatures {
    public:
        ForkFeatures() = default;

        static ForkFeatures present();
        static ForkFeatures none();           // every feature off (the phase-2 fork)
        static ForkFeatures all();            // every feature on
        // Runtime probes of the fork, without trusting the table: needs Rml::Initialise()
        // (HtmlUI acquired). Features that cannot be probed (GroupOpacity) take the table's
        // value. Used by the test ForkFeatures.TableMatchesProbes.
        static ForkFeatures probe();
        // One feature's probe: nullopt when it cannot be observed without rendering, or when
        // RmlUi is not initialised.
        static std::optional<bool> probe(ForkFeature feature);

        bool has(ForkFeature feature) const;
        ForkFeatures with(ForkFeature feature, bool present) const;

        uint64_t bits() const { return mask; }   // cache keys
        static ForkFeatures fromBits(uint64_t bits);

        bool operator==(const ForkFeatures& other) const { return mask == other.mask; }
        bool operator!=(const ForkFeatures& other) const { return mask != other.mask; }

    private:
        uint64_t mask = 0;
    };

    static_assert((int)ForkFeature::Count <= 64, "ForkFeatures keeps one bit per feature");

    JASPERENGINE_API const char* forkFeatureName(ForkFeature feature);          // "Calc"; "None" for None
    JASPERENGINE_API ForkFeature findForkFeature(std::string_view name);         // None when unknown

}

#endif //COMPAT_FORKFEATURES_H
