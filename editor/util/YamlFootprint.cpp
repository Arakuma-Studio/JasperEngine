// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "YamlFootprint.h"

#include <vector>

using namespace jasperengine;

namespace {

    constexpr size_t NODE_BYTES = 64;

}

size_t editor::yamlFootprint(const YAML::Node& root){
    if (!root.IsDefined() || root.IsNull()){
        return 0;
    }
    size_t bytes = 0;
    // An explicit stack: scene encodes nest deeper than a recursion should go
    std::vector<YAML::Node> stack;
    stack.push_back(root);
    while (!stack.empty()){
        YAML::Node node = stack.back();
        stack.pop_back();
        bytes += NODE_BYTES;
        switch (node.Type()){
            case YAML::NodeType::Scalar:
                bytes += node.Scalar().size();
                break;
            case YAML::NodeType::Sequence:
                for (const YAML::Node& child : node){
                    stack.push_back(child);
                }
                break;
            case YAML::NodeType::Map:
                for (const auto& pair : node){
                    stack.push_back(pair.first);
                    stack.push_back(pair.second);
                }
                break;
            default:
                break;
        }
    }
    return bytes;
}
