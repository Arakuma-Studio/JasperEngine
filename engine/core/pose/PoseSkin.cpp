// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pose/PoseSkin.h"

#include <limits>

namespace jasperengine::pose {

    size_t PoseSkin::sampleStride(size_t totalVertices){
        if (totalVertices <= MAX_VERTICES){
            return 1;
        }
        return (totalVertices + MAX_VERTICES - 1) / MAX_VERTICES;
    }

    double PoseSkin::lowest(const std::vector<Affine3d>& world) const{
        // only the up row (model +Y) of World_j * InvBind_j is needed
        const size_t boneCount = std::min(world.size(), inverseBind.size());
        std::vector<std::array<double, 4>> upRows(boneCount);
        for (size_t j = 0; j < boneCount; j++){
            const Affine3d& w = world[j];
            const Matrix4& ib = inverseBind[j];
            for (int c = 0; c < 4; c++){
                double sum = 0.0;
                for (int k = 0; k < 3; k++){
                    sum += w.m[1][k] * static_cast<double>(ib.get(c, k));
                }
                sum += w.t.y * static_cast<double>(ib.get(c, 3));
                upRows[j][c] = sum;
            }
        }

        double lowest = std::numeric_limits<double>::infinity();
        for (const PoseSkinVertex& v : vertices){
            double y = 0.0;
            bool any = false;
            for (int i = 0; i < 4; i++){
                const int bone = v.bones[i];
                if (bone < 0 || static_cast<size_t>(bone) >= boneCount || v.weights[i] == 0.0f){
                    continue;
                }
                const std::array<double, 4>& r = upRows[bone];
                y += v.weights[i] * (r[0] * v.position.x + r[1] * v.position.y + r[2] * v.position.z + r[3]);
                any = true;
            }
            if (any && y < lowest){
                lowest = y;
            }
        }
        return lowest;
    }

    float PoseSkin::lowest(const std::vector<Matrix4>& worldMatrices) const{
        std::vector<Affine3d> world(worldMatrices.size());
        for (size_t j = 0; j < worldMatrices.size(); j++){
            const Matrix4& m = worldMatrices[j];
            for (int r = 0; r < 3; r++){
                for (int c = 0; c < 3; c++){
                    world[j].m[r][c] = m.get(c, r);
                }
            }
            world[j].t = Vec3d(m.get(3, 0), m.get(3, 1), m.get(3, 2));
        }
        return static_cast<float>(lowest(world));
    }

}
