// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PROCEDURALMATERIALKIND_H
#define PROCEDURALMATERIALKIND_H

// The `.procmat` graph kind (S90 R1-R3, S23): node types come from the library's descriptors, plus the
// two the host owns, material_output and buffer. A file stores values, never code: every parameter is
// checked against its declared type and range before it gets near GLSL, and text only ever selects from
// a closed list.

#include "NodeLibrary.h"

#include "graph/GraphRegistry.h"

#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor::procmat {

    inline constexpr const char* PROCEDURAL_MATERIAL_KIND = "procedural_material";
    inline constexpr const char* PROCEDURAL_MATERIAL_EXTENSION = ".procmat";
    inline constexpr const char* MATERIAL_OUTPUT_NODE = "material_output";
    inline constexpr const char* BUFFER_NODE = "buffer";
    // material_output's one parameter: how steep the Normal made from Height is (1: Material Maker's
    // Normal Map at its default strength)
    inline constexpr const char* MATERIAL_NORMAL_STRENGTH = "normal_strength";
    // a generic node's copies of its inputs, as far as a file can ask
    inline constexpr int GENERIC_SIZE_MAX = 8;
    // R26: a procedural ramp holds up to 32 keys
    inline constexpr size_t RAMP_KEYS_MAX = 32;
    // a shape's points and an image's side: each point is a uniform vec4, each image a constant table
    inline constexpr size_t SHAPE_POINTS_MAX = 64;
    inline constexpr int PIXELS_SIDE_MAX = 64;

    graph::GraphKindDesc makeProceduralMaterialKind(const std::shared_ptr<const NodeLibrary>& library);
    // Registers the kind with the library embedded in the editor, once
    void registerProceduralMaterialKind();

    // The problems of one node's stored values against its type; empty when it may reach the generator.
    // `checkRange` adds out-of-range values, which a planner must refuse and the validator leaves to S23.
    std::vector<std::string> checkNodeParameters(const Json& descriptor, const graph::Node& node, bool checkRange);

    // A node's stored values as the generator reads them: names resolved to enum indices
    Json generatorParameters(const Json& descriptor, const graph::Node& node);
    int genericSizeOf(const graph::Node& node);
    // The file's name of an enum value, unique within its parameter
    std::vector<std::string> enumValueIds(const Json& parameter);
    // The parameters of the host's Buffer node, as a descriptor
    const Json& hostBufferDescriptor();
    // The ports a node of this type has at that generic size
    std::vector<graph::Port> nodePorts(const Json& descriptor, int genericSize);

}

#endif
