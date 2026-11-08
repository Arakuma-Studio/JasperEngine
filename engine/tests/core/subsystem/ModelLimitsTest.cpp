// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The limits of a model that are real (spec S65 R4): the error names the model, the count and the
// limit. CPU only: no draw runs here.

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "object/Model.h"

#include "LogCapture.h"
#include "TempDir.h"

#include <gtest/gtest.h>

#include <fstream>
#include <string>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // A triangle (POSITION: view 0, indices: view 1) and its `targets` morph targets over the same
    // positions
    std::string writeModel(TempDir& dir, const std::string& name, int targets){
        const float positions[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        const uint16_t indices[3] = {0, 1, 2};
        const std::string binName = name + ".bin";
        {
            std::ofstream bin(dir.file(binName), std::ios::binary);
            bin.write(reinterpret_cast<const char*>(positions), sizeof(positions));
            bin.write(reinterpret_cast<const char*>(indices), sizeof(indices));
        }
        std::string morphs;
        for (int t = 0; t < targets; t++)
            morphs += std::string(t ? "," : "") + R"({"POSITION": 0})";
        const std::string gltf = name + ".gltf";
        std::ofstream(dir.file(gltf)) << R"({"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
"meshes": [{"name": "Face", "primitives": [{"attributes": {"POSITION": 0}, "indices": 1)"
            << (targets ? R"(, "targets": [)" + morphs + "]" : std::string()) << R"(}]}],
"buffers": [{"uri": ")" << binName << R"(", "byteLength": )" << sizeof(positions) + sizeof(indices) << R"(}],
"bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": )" << sizeof(positions) << R"(, "target": 34962},
                {"buffer": 0, "byteOffset": )" << sizeof(positions) << R"(, "byteLength": )" << sizeof(indices) << R"(, "target": 34963}],
"accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
              {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}]})";
        return dir.file(gltf).generic_string();
    }

}

// R4: the blend shapes over the shader's inputs: the mesh draws without them, and says so once
TEST(ModelLimits, TooManyBlendShapesNameTheModelTheCountAndTheLimit){
    TempDir dir;
    const std::string file = writeModel(dir, "many-shapes", MAX_MORPHTARGETS + 3);
    const LogCapture log;
    Scene scene;
    Model model(&scene);
    EXPECT_TRUE(model.loadModel(file));
    const std::string expected = "Model " + file + ": mesh 'Face' has " + std::to_string(MAX_MORPHTARGETS + 3) +
                                 " blend shapes, more than the maximum allowed (" + std::to_string(MAX_MORPHTARGETS) + ")";
    EXPECT_EQ(log.count(D_LOG_ERROR, expected), 1u) << log.describeErrors();
}

TEST(ModelLimits, AModelWithinThemLogsNoError){
    TempDir dir;
    const std::string file = writeModel(dir, "within", MAX_MORPHTARGETS);
    const LogCapture log;
    Scene scene;
    Model model(&scene);
    EXPECT_TRUE(model.loadModel(file));
    EXPECT_TRUE(log.errors().empty()) << log.describeErrors();
}
