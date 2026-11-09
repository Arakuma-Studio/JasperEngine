// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLEQASETTINGS_H
#define PARTICLEQASETTINGS_H

#include <vector>

namespace jasperengine::editor {

    // Project Settings > Graphics > Particle Checks: the thresholds of preview_particle_effect's checks
    // and the Particle Effect window's Checks (docs/specs/S91-effect-authoring.md R28). Changing one
    // does not fix an effect; a result lists every changed one.
    struct ParticleQaSettings {
        // peak overdraw layers that warn per platform; twice as many fail
        float overdrawMobile = 8.0f;
        float overdrawWeb = 12.0f;
        float overdrawDesktop = 20.0f;
        // estimated CPU milliseconds a frame (simulation and render prep) one effect may take
        float cpuMsMobile = 0.25f;
        float cpuMsWeb = 0.5f;
        float cpuMsDesktop = 1.0f;
        // % of the covered pixels clipped to white that warns
        float blownOutPercent = 25.0f;
        // a frame whose particles cover less than this % of the view counts as blank
        float invisiblePercent = 0.05f;
        // % of the live particles dying in one tick while still visible that warns
        float popPercent = 30.0f;
        int fixRounds = 2;

        bool operator==(const ParticleQaSettings& other) const;
        bool operator!=(const ParticleQaSettings& other) const { return !(*this == other); }
    };

    // One threshold: the settings registry, project.yaml, the Project Settings rows and a check's
    // audit all walk this table
    struct ParticleQaField {
        const char* key;      // particles.qa.<name>, the MCP key
        const char* yaml;     // its key under project.yaml's particleQa
        const char* label;
        const char* help;
        float ParticleQaSettings::*real;   // one of these two
        int ParticleQaSettings::*whole;
        double minimum;
        double maximum;
    };

    const std::vector<ParticleQaField>& particleQaFields();

}

#endif //PARTICLEQASETTINGS_H
