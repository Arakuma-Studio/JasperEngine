// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "yaml-cpp/yaml.h"

#include <cstddef>

namespace jasperengine::editor {

    // The memory a YAML tree holds, estimated for the undo budget: 64 bytes per node plus
    // the length of every scalar. Long sequences and maps are sampled (16 children spread
    // over them, scaled to their length), so the cost stays small next to the encode that
    // made the tree; a null or undefined node is 0.
    size_t yamlFootprint(const YAML::Node& node);

}
