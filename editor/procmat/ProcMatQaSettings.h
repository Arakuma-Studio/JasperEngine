// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PROCMATQASETTINGS_H
#define PROCMATQASETTINGS_H

#include <vector>

namespace jasperengine::editor::procmat {

    // Project Settings > Graphics > Material Checks: the thresholds of preview_procedural_material's checks,
    // bake_procedural_material's fail_on and check --materials (docs/specs/S90-procedural-material.md R36).
    // They are project settings, never tool parameters, so a check cannot be passed by asking for a looser
    // one; a result lists every threshold that differs from its default.
    struct ProcMatQaSettings {
        // tileability: the wrap seam's step over its neighbours' (synthesize_texture's score) that fails
        float seamFail = 1.5f;
        // repetition: the strongest autocorrelation peak away from the origin that warns
        float repetitionWarn = 0.9f;
        // histogram: % of the base colour's texels at 0 or 255 that warns
        float clippedPercent = 1.0f;
        // base_color: % of the dielectric texels outside sRGB 30..240 that warns
        float dielectricOutsidePercent = 10.0f;
        // normal: the mean error of a decoded normal's length that fails
        float normalLengthError = 0.05f;
        // roughness: a range (p99 - p1) below this warns that it is flat
        float roughnessMinRange = 0.05f;
        // metallic: % of the texels between 0.1 and 0.9 that warns
        float metallicMidPercent = 10.0f;
        int fixRounds = 2;

        bool operator==(const ProcMatQaSettings& other) const;
        bool operator!=(const ProcMatQaSettings& other) const { return !(*this == other); }
    };

    // One threshold: the settings registry, project.yaml, the Project Settings rows and a check's audit all
    // walk this table
    struct ProcMatQaField {
        const char* key;      // materials.qa.<name>, the MCP key
        const char* yaml;     // its key under project.yaml's proceduralMaterialQa
        const char* label;
        const char* help;
        float ProcMatQaSettings::*real;   // one of these two
        int ProcMatQaSettings::*whole;
        double minimum;
        double maximum;
    };

    const std::vector<ProcMatQaField>& procMatQaFields();

}

#endif //PROCMATQASETTINGS_H
