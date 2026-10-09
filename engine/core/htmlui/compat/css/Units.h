// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_CSS_UNITS_H
#define COMPAT_CSS_UNITS_H

// CSS units as the compat layer reads them (phase-2 Appendix E): their category, their
// canonical form for arithmetic, and the RCSS unit a value is written in.

#include "Export.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace jasperengine::htmlui::compat {

    enum class UnitCategory : uint8_t {
        None,          // a plain number
        Percentage,
        AbsoluteLength,    // px, in, cm, mm, Q, pt, pc
        FontRelative,      // em, rem, ch, ex, cap, ic, lh, rlh
        ViewportRelative,  // vw, vh, vmin, vmax and the d/s/l variants, vi, vb
        ContainerRelative, // cqw, cqh, cqi, cqb, cqmin, cqmax
        EngineLength,      // dp (RmlUi's density-independent pixel)
        Angle,             // deg, rad, grad, turn
        Time,              // s, ms
        Resolution,        // dppx, x, dpi, dpcm
        Frequency,         // Hz, kHz
        Flex,              // fr
        Unknown,
    };

    // the category of a unit (case-insensitive; "" is None, "%" Percentage)
    JASPERENGINE_API UnitCategory unitCategory(std::string_view unit);
    JASPERENGINE_API bool isLengthCategory(UnitCategory category);

    // The factor that turns a value in `unit` into the category's canonical unit (px for
    // absolute lengths, deg for angles, s for times, x for resolutions, Hz); nullopt when the
    // unit has no fixed factor (relative units).
    JASPERENGINE_API std::optional<double> canonicalFactor(std::string_view unit);
    JASPERENGINE_API const char* canonicalUnit(UnitCategory category);

    // How a value of `unit` is written in RCSS (Appendix E): the same unit, or a conversion
    // (multiply by factor, write outUnit). approximated: ch/ex/cap/ic/lh/rlh (Info
    // HTMLUI-CSS-0005); viewportOrientation: vmin/vmax (orientation media duplication);
    // unsupported: container units (dropped, HTMLUI-CSS-0014).
    struct RcssUnit {
        double factor = 1.0;
        std::string unit;            // lowercase except "Q" never reaches the output
        bool approximated = false;
        bool viewportOrientation = false;
        bool unsupported = false;
        bool ignored = false;        // Hz/kHz: no effect
    };
    JASPERENGINE_API RcssUnit rcssUnit(std::string_view unit);

    // lowercase ASCII copy
    JASPERENGINE_API std::string lowerAscii(std::string_view text);
    JASPERENGINE_API bool equalsNoCase(std::string_view a, std::string_view b);
    JASPERENGINE_API bool startsWithNoCase(std::string_view text, std::string_view prefix);

}

#endif //COMPAT_CSS_UNITS_H
