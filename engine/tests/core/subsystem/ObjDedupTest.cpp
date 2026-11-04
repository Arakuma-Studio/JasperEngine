// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// OBJ vertices welded and ordered as a cooked mesh (docs/specs/S53-model-import-cook.md R7, AC4):
// an OBJ face list gives one vertex per face corner (fixtures/gltf/s41/dedup_sphere.obj: a 12 x 8
// UV sphere, 576 corners over 117 distinct position, UV and normal triples, the shape of
// meshoptimizer's pirate.obj). The load keeps one vertex per distinct corner and draws the same
// triangles (ObjDedupGpuTest compares the image with the one drawn before the weld). CPU only.

#include "Engine.h"
#include "Scene.h"
#include "object/Model.h"

#include "GltfTriangleSoup.h"

#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // the OBJ's triangles as loadOBJ lays them out (V flipped: 1 - vt)
    std::vector<SoupTriangle> objTriangles(const std::string& path){
        std::ifstream in(path);
        std::vector<double> v, vt, vn;
        std::vector<SoupTriangle> out;
        std::string line;
        while (std::getline(in, line)){
            std::istringstream words(line);
            std::string kind;
            words >> kind;
            double a = 0, b = 0, c = 0;
            if (kind == "v"){ words >> a >> b >> c; v.insert(v.end(), {a, b, c}); }
            else if (kind == "vn"){ words >> a >> b >> c; vn.insert(vn.end(), {a, b, c}); }
            else if (kind == "vt"){ words >> a >> b; vt.insert(vt.end(), {a, 1.0 - b}); }
            else if (kind == "f"){
                SoupTriangle triangle;
                triangle.hasNormal = triangle.hasUv = true;
                for (int k = 0; k < 3; k++){
                    std::string corner;
                    words >> corner;
                    int iv = 0, it = 0, in = 0;
                    std::sscanf(corner.c_str(), "%d/%d/%d", &iv, &it, &in);
                    for (int i = 0; i < 3; i++){
                        triangle.corners[k].position[i] = v[(iv - 1) * 3 + i];
                        triangle.corners[k].normal[i] = vn[(in - 1) * 3 + i];
                    }
                    triangle.corners[k].uv[0] = vt[(it - 1) * 2];
                    triangle.corners[k].uv[1] = vt[(it - 1) * 2 + 1];
                }
                out.push_back(triangle);
            }
        }
        return out;
    }

    // the loaded mesh's triangles: its interleaved vertices (position, uv, normal, colour floats)
    // through its index buffer
    std::vector<SoupTriangle> meshTriangles(const MeshComponent& mesh){
        std::vector<SoupTriangle> out;
        const unsigned char* vertices = mesh.buffer.getData();
        const unsigned int stride = mesh.buffer.getStride();
        const unsigned char* indices = mesh.indices.getData();
        const unsigned int indexSize = mesh.indices.getStride();
        for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
            const auto found = mesh.submeshes[s].attributes.find(AttributeType::INDEX);
            if (found == mesh.submeshes[s].attributes.end())
                continue;
            const Attribute& index = found->second;
            for (unsigned int t = 0; t + 2 < index.getCount(); t += 3){
                SoupTriangle triangle;
                triangle.hasNormal = triangle.hasUv = true;
                for (int k = 0; k < 3; k++){
                    uint32_t i = 0;
                    std::memcpy(&i, indices + index.getOffset() + (size_t)(t + k) * indexSize, indexSize);
                    float f[8];
                    std::memcpy(f, vertices + (size_t)i * stride, sizeof(f));
                    for (int c = 0; c < 3; c++){
                        triangle.corners[k].position[c] = f[c];
                        triangle.corners[k].normal[c] = f[5 + c];
                    }
                    triangle.corners[k].uv[0] = f[3];
                    triangle.corners[k].uv[1] = f[4];
                }
                out.push_back(triangle);
            }
        }
        return out;
    }

}

TEST(ObjDedupTest, WeldsTheCornersOfTheFaces){
    Scene scene;
    Model model(&scene);
    const std::string path = s41Fixture("dedup_sphere.obj");
    ASSERT_TRUE(model.loadModel(path));
    const MeshComponent& mesh = scene.getComponent<MeshComponent>(model.getEntity());
    EXPECT_EQ(mesh.buffer.getCount(), 117u) << "one vertex per distinct corner, not per face corner (576)";
    EXPECT_EQ(mesh.buffer.getStride(), 48u);

    const std::vector<SoupTriangle> expected = objTriangles(path);
    ASSERT_EQ(expected.size(), 192u);
    SoupTolerance exact;
    exact.position = exact.uv = 1e-6;
    exact.normalDot = 0.99999;
    EXPECT_EQ(compareSoups(expected, meshTriangles(mesh), exact), "");
}
