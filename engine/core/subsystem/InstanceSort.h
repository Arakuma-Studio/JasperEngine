// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JASPERENGINE_INSTANCE_SORT_H
#define JASPERENGINE_INSTANCE_SORT_H

#include "math/Matrix4.h"
#include <algorithm>
#include <vector>

namespace jasperengine::detail {

// Keep the index sort (including equal-depth ordering) shared by instances and
// their bounds. Project each position once instead of twice per comparison.
template <typename Instance>
std::vector<unsigned int> instanceDepthOrder(const std::vector<Instance>& instances,
        const Matrix4& modelMatrix, const Vector3& cameraDirection) {
    std::vector<unsigned int> order(instances.size());
    std::vector<float> depths(instances.size());
    for (unsigned int i = 0; i < order.size(); ++i) {
        order[i] = i;
        const Matrix4& matrix = instances[i].instanceMatrix;
        const Vector3 position(matrix[3][0], matrix[3][1], matrix[3][2]);
        depths[i] = (modelMatrix * position).dotProduct(cameraDirection);
    }
    std::sort(order.begin(), order.end(), [&](unsigned int a, unsigned int b) {
        return depths[a] < depths[b];
    });
    return order;
}

}

#endif
