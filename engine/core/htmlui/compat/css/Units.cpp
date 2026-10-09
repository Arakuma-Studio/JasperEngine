// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/css/Units.h"
#include "htmlui/compat/LiteralCompare.h"

#include <cmath>

using namespace jasperengine::htmlui::compat;

namespace {

    // a lowercase copy of a short unit on the stack (units longer than the buffer match nothing)
    struct LowerUnit {
        char buffer[16];
        std::string_view view;

        explicit LowerUnit(std::string_view unit){
            if (unit.size() > sizeof(buffer)){
                view = std::string_view("\x01too-long");
                return;
            }
            for (size_t i = 0; i < unit.size(); i++){
                const char c = unit[i];
                buffer[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
            }
            view = std::string_view(buffer, unit.size());
        }
    };

}

std::string jasperengine::htmlui::compat::lowerAscii(std::string_view text){
    std::string result(text);
    for (char& c : result){
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
    }
    return result;
}

bool jasperengine::htmlui::compat::equalsNoCase(std::string_view a, std::string_view b){
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); i++){
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z')
            x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z')
            y = (char)(y - 'A' + 'a');
        if (x != y)
            return false;
    }
    return true;
}

bool jasperengine::htmlui::compat::startsWithNoCase(std::string_view text, std::string_view prefix){
    return text.size() >= prefix.size() && equalsNoCase(text.substr(0, prefix.size()), prefix);
}

UnitCategory jasperengine::htmlui::compat::unitCategory(std::string_view unit){
    if (unit.empty())
        return UnitCategory::None;
    if (unit == "%")
        return UnitCategory::Percentage;
    const LowerUnit lower(unit);
    const std::string_view u = lower.view;
    if (u == "px" || u == "in" || u == "cm" || u == "mm" || u == "q" || u == "pt" || u == "pc")
        return UnitCategory::AbsoluteLength;
    if (u == "em" || u == "rem" || u == "ch" || u == "ex" || u == "cap" || u == "ic" || u == "lh" || u == "rlh" ||
        u == "rex" || u == "rch" || u == "rcap" || u == "ric")
        return UnitCategory::FontRelative;
    if (u == "vw" || u == "vh" || u == "vmin" || u == "vmax" || u == "vi" || u == "vb" || u == "dvw" || u == "dvh" ||
        u == "svw" || u == "svh" || u == "lvw" || u == "lvh" || u == "dvmin" || u == "dvmax" || u == "svmin" ||
        u == "svmax" || u == "lvmin" || u == "lvmax" || u == "dvi" || u == "dvb" || u == "svi" || u == "svb" ||
        u == "lvi" || u == "lvb")
        return UnitCategory::ViewportRelative;
    if (u == "cqw" || u == "cqh" || u == "cqi" || u == "cqb" || u == "cqmin" || u == "cqmax")
        return UnitCategory::ContainerRelative;
    if (u == "dp")
        return UnitCategory::EngineLength;
    if (u == "deg" || u == "rad" || u == "grad" || u == "turn")
        return UnitCategory::Angle;
    if (u == "s" || u == "ms")
        return UnitCategory::Time;
    if (u == "dppx" || u == "x" || u == "dpi" || u == "dpcm")
        return UnitCategory::Resolution;
    if (u == "hz" || u == "khz")
        return UnitCategory::Frequency;
    if (u == "fr")
        return UnitCategory::Flex;
    return UnitCategory::Unknown;
}

bool jasperengine::htmlui::compat::isLengthCategory(UnitCategory category){
    return category == UnitCategory::AbsoluteLength || category == UnitCategory::FontRelative ||
           category == UnitCategory::ViewportRelative || category == UnitCategory::ContainerRelative ||
           category == UnitCategory::EngineLength;
}

std::optional<double> jasperengine::htmlui::compat::canonicalFactor(std::string_view unit){
    const LowerUnit lower(unit);
    const std::string_view u = lower.view;
    if (u.empty())
        return 1.0;
    if (u == "px") return 1.0;
    if (u == "in") return 96.0;
    if (u == "cm") return 96.0 / 2.54;
    if (u == "mm") return 96.0 / 25.4;
    if (u == "q") return 96.0 / 101.6;
    if (u == "pt") return 96.0 / 72.0;
    if (u == "pc") return 16.0;
    if (u == "deg") return 1.0;
    if (u == "rad") return 180.0 / 3.14159265358979323846;
    if (u == "grad") return 0.9;
    if (u == "turn") return 360.0;
    if (u == "s") return 1.0;
    if (u == "ms") return 0.001;
    if (u == "x" || u == "dppx") return 1.0;
    if (u == "dpi") return 1.0 / 96.0;
    if (u == "dpcm") return 2.54 / 96.0;
    if (u == "hz") return 1.0;
    if (u == "khz") return 1000.0;
    return std::nullopt;
}

const char* jasperengine::htmlui::compat::canonicalUnit(UnitCategory category){
    switch (category){
        case UnitCategory::AbsoluteLength: return "px";
        case UnitCategory::Angle: return "deg";
        case UnitCategory::Time: return "s";
        case UnitCategory::Resolution: return "x";
        case UnitCategory::Frequency: return "hz";
        case UnitCategory::Percentage: return "%";
        default: return "";
    }
}

RcssUnit jasperengine::htmlui::compat::rcssUnit(std::string_view unit){
    RcssUnit out;
    if (unit.size() > 16){
        out.unit = lowerAscii(unit);
        return out;
    }
    const LowerUnit lower(unit);
    const std::string_view u = lower.view;
    out.unit = u;
    if (u == "q"){
        out.factor = 0.25;
        out.unit = "mm";
    }else if (u == "dvh" || u == "svh" || u == "lvh" || u == "vb" || u == "dvb" || u == "svb" || u == "lvb"){
        out.unit = "vh";
    }else if (u == "dvw" || u == "svw" || u == "lvw" || u == "vi" || u == "dvi" || u == "svi" || u == "lvi"){
        out.unit = "vw";
    }else if (u == "vmin" || u == "dvmin" || u == "svmin" || u == "lvmin"){
        out.unit = "vmin";
        out.viewportOrientation = true;
    }else if (u == "vmax" || u == "dvmax" || u == "svmax" || u == "lvmax"){
        out.unit = "vmax";
        out.viewportOrientation = true;
    }else if (u == "ch" || u == "ex" || u == "rch" || u == "rex"){
        out.factor = 0.5;
        out.unit = (u[0] == 'r') ? "rem" : "em";
        out.approximated = true;
    }else if (u == "cap" || u == "rcap"){
        out.factor = 0.7;
        out.unit = (u[0] == 'r') ? "rem" : "em";
        out.approximated = true;
    }else if (u == "ic" || u == "ric"){
        out.factor = 1.0;
        out.unit = (u[0] == 'r') ? "rem" : "em";
        out.approximated = true;
    }else if (u == "lh"){
        out.factor = 1.2;
        out.unit = "em";
        out.approximated = true;
    }else if (u == "rlh"){
        out.factor = 1.2;
        out.unit = "rem";
        out.approximated = true;
    }else if (u == "cqw" || u == "cqh" || u == "cqi" || u == "cqb" || u == "cqmin" || u == "cqmax"){
        out.unsupported = true;
    }else if (u == "turn"){
        out.factor = 360.0;
        out.unit = "deg";
    }else if (u == "grad"){
        out.factor = 0.9;
        out.unit = "deg";
    }else if (u == "ms"){
        out.factor = 0.001;
        out.unit = "s";
    }else if (u == "dppx"){
        out.unit = "x";
    }else if (u == "dpi"){
        out.factor = 1.0 / 96.0;
        out.unit = "x";
    }else if (u == "dpcm"){
        out.factor = 2.54 / 96.0;
        out.unit = "x";
    }else if (u == "hz" || u == "khz"){
        out.ignored = true;
    }
    return out;
}
