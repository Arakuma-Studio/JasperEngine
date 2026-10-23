// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "yaml-cpp/yaml.h"

#include <cstddef>

namespace jasperengine::editor {

    // The memory a YAML tree holds, estimated for the undo budget: 64 bytes per node plus
    // the length of every scalar. One walk, no copies; a null or undefined node is 0.
    size_t yamlFootprint(const YAML::Node& node);

}
