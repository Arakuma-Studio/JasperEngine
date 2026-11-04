// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What a mesh holds past its inline capacities, in a build without the editor, and the limits that
// remain real (spec S45 R3, R4). CPU only: no draw runs here.

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "object/Model.h"
#include "serialization/CookCodecs.h"
#include "serialization/RuntimeFields.h"
#include "util/Fnv.h"

#include "LogCapture.h"
#include "TempDir.h"

#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <string>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // A triangle (POSITION: view 0, indices: view 1), its `targets` morph targets over the same
    // positions and, with `joints`, a skin of that many nodes
    std::string writeModel(TempDir& dir, const std::string& name, int targets, int joints){
        const float positions[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        const uint16_t indices[3] = {0, 1, 2};
        const std::string binName = name + ".bin";
        {
            std::ofstream bin(dir.file(binName), std::ios::binary);
            bin.write(reinterpret_cast<const char*>(positions), sizeof(positions));
            bin.write(reinterpret_cast<const char*>(indices), sizeof(indices));
        }
        std::string morphs, nodes = R"({"mesh": 0)", jointList;
        for (int t = 0; t < targets; t++)
            morphs += std::string(t ? "," : "") + R"({"POSITION": 0})";
        if (joints > 0)
            nodes += R"(, "skin": 0)";
        nodes += "}";
        for (int j = 0; j < joints; j++){
            nodes += R"(, {"name": "Joint )" + std::to_string(j) + R"("})";
            jointList += std::string(j ? "," : "") + std::to_string(j + 1);
        }
        const std::string gltf = name + ".gltf";
        std::ofstream(dir.file(gltf)) << R"({"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [)" << nodes << R"(],
"meshes": [{"name": "Face", "primitives": [{"attributes": {"POSITION": 0}, "indices": 1)"
            << (targets ? R"(, "targets": [)" + morphs + "]" : std::string()) << R"(}]}],)"
            << (joints ? R"("skins": [{"joints": [)" + jointList + "]}]," : std::string()) << R"(
"buffers": [{"uri": ")" << binName << R"(", "byteLength": )" << sizeof(positions) + sizeof(indices) << R"(}],
"bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": )" << sizeof(positions) << R"(, "target": 34962},
                {"buffer": 0, "byteOffset": )" << sizeof(positions) << R"(, "byteLength": )" << sizeof(indices) << R"(, "target": 34963}],
"accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
              {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}]})";
        return dir.file(gltf).generic_string();
    }

}

// the cooked form of a mesh past its inline capacities is read whole by a player
TEST(ModelLimits, ACookedMeshPastTheInlineCapacitiesLosesNothing){
    const unsigned int submeshes = MAX_SUBMESHES + 5;
    const unsigned int buffers = MAX_EXTERNAL_BUFFERS + 5;
    // (on the heap, as the exported scene code keeps them: they are large)
    auto written = std::make_unique<MeshComponent>();
    written->numSubmeshes = submeshes;
    for (unsigned int s = 0; s < submeshes; s++)
        written->submeshes[s].vertexCount = 100 + s;
    written->numExternalBuffers = buffers;
    for (unsigned int b = 0; b < buffers; b++)
        written->eBuffers[b].setName("buffer" + std::to_string(b));

    const cook::ComponentHooks* hooks = cook::componentHooks(fnv1a32("mesh"));
    ASSERT_NE(hooks, nullptr);
    cook::StringTable strings;
    cook::Writer writer(strings, [](Entity entity) { return (uint32_t)entity; });
    hooks->writeExtra(writer, written.get(), cook::WriteContext{});

    cook::Reader reader(writer.data(), strings.strings(), [](uint32_t entity) { return (Entity)entity; });
    auto read = std::make_unique<MeshComponent>();
    hooks->readExtra(reader, read.get(), cook::LoadContext{});
    ASSERT_FALSE(reader.failed());
    EXPECT_EQ(read->numExternalBuffers, buffers);
    for (unsigned int s = 0; s < submeshes; s++)
        EXPECT_EQ(read->submeshes[s].vertexCount, 100 + s);
    for (unsigned int b = 0; b < buffers; b++)
        EXPECT_EQ(read->eBuffers[b].getName(), "buffer" + std::to_string(b));
}

// R4: a skin over the bones a mesh holds does not load: the error names the model, the count and the limit
TEST(ModelLimits, TooManyBonesNameTheModelTheCountAndTheLimit){
    TempDir dir;
    const std::string file = writeModel(dir, "many-bones", 0, MAX_BONES + 2);
    const LogCapture log;
    Scene scene;
    Model model(&scene);
    EXPECT_FALSE(model.loadModel(file));
    const std::string expected = "Model " + file + " has " + std::to_string(MAX_BONES + 2) +
                                 " bones, more than the maximum allowed (" + std::to_string(MAX_BONES) + ")";
    EXPECT_EQ(log.count(D_LOG_ERROR, expected), 1u) << log.describeErrors();
}

// R4: the blend shapes over the shader's inputs: the mesh draws without them, and says so once
TEST(ModelLimits, TooManyBlendShapesNameTheModelTheCountAndTheLimit){
    TempDir dir;
    const std::string file = writeModel(dir, "many-shapes", MAX_MORPHTARGETS + 3, 0);
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
    const std::string file = writeModel(dir, "within", MAX_MORPHTARGETS, MAX_BONES);
    const LogCapture log;
    Scene scene;
    Model model(&scene);
    EXPECT_TRUE(model.loadModel(file));
    EXPECT_TRUE(log.errors().empty()) << log.describeErrors();
}
