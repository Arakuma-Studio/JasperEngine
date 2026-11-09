// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ProcMatQaSettings.h"

using namespace jasperengine::editor::procmat;

const std::vector<ProcMatQaField>& jasperengine::editor::procmat::procMatQaFields() {
    using S = ProcMatQaSettings;
    static const std::vector<ProcMatQaField> fields = {
        {"materials.qa.seam_fail", "seamFail", "Seam Fail", "The wrap edge's step over its neighbours' that fails tileability (about 1 tiles seamlessly).", &S::seamFail, nullptr, 1, 10},
        {"materials.qa.repetition_warn", "repetitionWarn", "Repetition Warn", "The strongest autocorrelation peak away from the origin (0 to 1) that warns the texture repeats visibly.", &S::repetitionWarn, nullptr, 0.1, 1},
        {"materials.qa.clipped_percent", "clippedPercent", "Clipped (%)", "Share of the base colour's texels at black or white that warns.", &S::clippedPercent, nullptr, 0, 100},
        {"materials.qa.dielectric_outside_percent", "dielectricOutsidePercent", "Dielectric Outside (%)", "Share of the non-metal texels outside sRGB 30 to 240 that warns.", &S::dielectricOutsidePercent, nullptr, 0, 100},
        {"materials.qa.normal_length_error", "normalLengthError", "Normal Length Error", "Mean error of a decoded normal's length that fails.", &S::normalLengthError, nullptr, 0.001, 1},
        {"materials.qa.roughness_min_range", "roughnessMinRange", "Roughness Min Range", "A roughness range (p99 - p1) below this warns that it is flat.", &S::roughnessMinRange, nullptr, 0, 1},
        {"materials.qa.metallic_mid_percent", "metallicMidPercent", "Metallic Mid (%)", "Share of the texels between 0.1 and 0.9 metallic that warns: metals are near 0 or 1.", &S::metallicMidPercent, nullptr, 0, 100},
        {"materials.qa.fix_rounds", "fixRounds", "Fix Rounds", "Consecutive failures of a check after which an agent stops and reports.", nullptr, &S::fixRounds, 1, 10},
    };
    return fields;
}

bool ProcMatQaSettings::operator==(const ProcMatQaSettings& other) const {
    for (const ProcMatQaField& field : procMatQaFields()) {
        if (field.real && this->*field.real != other.*field.real) return false;
        if (field.whole && this->*field.whole != other.*field.whole) return false;
    }
    return true;
}
