// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A mesh of more submeshes and buffers than it holds at first: its file keeps them all (spec S45 R3).

#include "Stream.h"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;

TEST(StreamMeshCapacity, AMeshPastItsFirstCapacitiesKeepsEverySubmeshAndBuffer){
    const unsigned int submeshes = MAX_SUBMESHES + 5;
    const unsigned int buffers = MAX_EXTERNAL_BUFFERS + 5;
    MeshComponent mesh;
    mesh.numSubmeshes = submeshes;
    for (unsigned int s = 0; s < submeshes; s++)
        mesh.submeshes[s].vertexCount = 100 + s;
    mesh.numExternalBuffers = buffers;
    for (unsigned int b = 0; b < buffers; b++)
        mesh.eBuffers[b].setName("buffer" + std::to_string(b));

    const YAML::Node node = YAML::Load(YAML::Dump(Stream::encodeMeshComponent(mesh)));
    const MeshComponent decoded = Stream::decodeMeshComponent(node);

    ASSERT_EQ(decoded.numSubmeshes, submeshes);
    ASSERT_EQ(decoded.numExternalBuffers, buffers);
    for (unsigned int s = 0; s < submeshes; s++)
        EXPECT_EQ(decoded.submeshes[s].vertexCount, 100 + s) << s;
    for (unsigned int b = 0; b < buffers; b++)
        EXPECT_EQ(decoded.eBuffers[b].getName(), "buffer" + std::to_string(b)) << b;
}
