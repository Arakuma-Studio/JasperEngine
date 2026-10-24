// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "YamlFootprint.h"

using namespace jasperengine;

namespace {

    constexpr size_t NODE_BYTES = 64;
    // A sequence or map longer than this is measured on this many children spread over it and
    // scaled to its length: a scene encode is thousands of alike GameObjects, and an estimate
    // within the budget's tolerance costs a fraction of a full walk
    constexpr size_t SAMPLES = 16;

    size_t footprint(const YAML::Node& node);

    // Every child, or SAMPLES of them scaled to all, of a sequence (pairs: false) or map
    template<typename Visit>
    size_t children(const YAML::Node& node, Visit visit){
        const size_t count = node.size();
        const size_t stride = count > SAMPLES ? count / SAMPLES : 1;
        size_t bytes = 0;
        size_t visited = 0;
        size_t position = 0;
        for (YAML::const_iterator it = node.begin(); it != node.end(); ++it, ++position){
            if (position % stride != 0){
                continue;
            }
            bytes += visit(it);
            visited++;
        }
        if (visited == 0){
            return 0;
        }
        return stride == 1 ? bytes : bytes / visited * count;
    }

    size_t footprint(const YAML::Node& node){
        switch (node.Type()){
            case YAML::NodeType::Scalar:
                return NODE_BYTES + node.Scalar().size();
            case YAML::NodeType::Sequence:
                return NODE_BYTES + children(node, [](const YAML::const_iterator& it){ return footprint(*it); });
            case YAML::NodeType::Map:
                return NODE_BYTES + children(node, [](const YAML::const_iterator& it){
                    return footprint(it->first) + footprint(it->second);
                });
            default:
                return NODE_BYTES;
        }
    }

}

size_t editor::yamlFootprint(const YAML::Node& root){
    if (!root.IsDefined() || root.IsNull()){
        return 0;
    }
    return footprint(root);
}

bool editor::yamlEqual(const YAML::Node& a, const YAML::Node& b){
    if (a.Type() != b.Type()){
        return false;
    }
    switch (a.Type()){
        case YAML::NodeType::Scalar:
            return a.Scalar() == b.Scalar();
        case YAML::NodeType::Sequence: {
            if (a.size() != b.size()){
                return false;
            }
            YAML::const_iterator other = b.begin();
            for (YAML::const_iterator it = a.begin(); it != a.end(); ++it, ++other){
                if (!yamlEqual(*it, *other)) return false;
            }
            return true;
        }
        case YAML::NodeType::Map: {
            if (a.size() != b.size()){
                return false;
            }
            for (YAML::const_iterator it = a.begin(); it != a.end(); ++it){
                const YAML::Node other = b[it->first.Scalar()];
                if (!other || !yamlEqual(it->second, other)) return false;
            }
            return true;
        }
        default:
            return true;
    }
}
