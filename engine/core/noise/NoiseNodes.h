// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NOISE_NOISENODES_H
#define NOISE_NOISENODES_H

// The noise graph kind (T2 R9, R15): its 29 node types, ports and parameters, and what only this kind
// checks. A shipped node's defaults and outputs are frozen (R14): NoiseNodes.TableIsFrozen pins them.

#include "Export.h"
#include "graph/GraphRegistry.h"

#include <string>
#include <vector>

namespace jasperengine { namespace noise {

    inline constexpr const char* kNoiseKindId = "noise";
    inline constexpr int kNoiseKindVersion = 1;

    // The descriptor registerNoiseKind() registers. Migrations go in its list, ascending.
    JASPERENGINE_API graph::GraphKindDesc makeNoiseKindDesc();

    // Type traits the compiler and validator share
    JASPERENGINE_API bool isGridOnlyType(const std::string& typeId);
    JASPERENGINE_API bool isBakeOnlyType(const std::string& typeId);

    // no-output, duplicate-output, octave-limit, unknown-variable, curve-keys, the usage codes of
    // metadata.noise.usage, and erosion-budget at resolution (0: metadata.noise.resolution, else 1024)
    JASPERENGINE_API void appendKindIssues(const graph::Graph& graph, int resolution, std::vector<graph::Issue>& issues);
    // erosion-budget alone (R26): whether droplets * lifetime and texels * iterations fit
    JASPERENGINE_API void appendErosionBudgetIssues(const graph::Graph& graph, int resolution, std::vector<graph::Issue>& issues);

    // The parameter's value in a node, or its type's default when absent or of another kind
    JASPERENGINE_API double paramNumber(const graph::Node& node, const char* name, double fallback);
    JASPERENGINE_API std::string paramString(const graph::Node& node, const char* name, const char* fallback);
    JASPERENGINE_API bool paramBool(const graph::Node& node, const char* name, bool fallback);

}}

#endif // NOISE_NOISENODES_H
