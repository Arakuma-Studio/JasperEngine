// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NOISE_GOLDENCASES_H
#define NOISE_GOLDENCASES_H

// The one table of T2 R41 and what computes it: every node type, each basis in 2D and 3D, every preset
// at 64 texels, erosion, tiny and huge coordinates and each numeric rule of R6. Shared by the core
// tests and the standalone golden tool, built with the noise flags in both.

#include "noise/NoiseTypes.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine { namespace noise { namespace golden {

    struct Expected {
        const char* name;
        uint64_t hash;
    };

    struct Preset {
        std::string id;
        std::string json;
    };

    // name -> FNV-1a 64 of the case's floats, on this back end, in the table's order
    std::vector<std::pair<std::string, uint64_t>> computeCases(Backend backend, const std::vector<Preset>& presets);

    const Expected* expectedTable(size_t& count);
    // The table's own hash, which every report prints: two reports compared the same table
    uint64_t tableHash();

    // The one line a run reports per back end (R72)
    std::string reportLine(const char* target, const char* backend, size_t cases, bool pass);

    // The case names of node types, for NoiseNodes.EveryTypeHasAGoldenAndARange
    std::string nodeCaseName(const std::string& type);

}}}

#endif // NOISE_GOLDENCASES_H
