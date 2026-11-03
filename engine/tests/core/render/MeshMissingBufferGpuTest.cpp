// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// A submesh whose attribute names a buffer the mesh does not have (a flattened model's preview, past
// the mesh's limit of external buffers) has nothing to draw with: bound without it, its pipeline
// leaves a vertex input undefined on Vulkan and fails to be created on Metal, where its load was
// then tried again every frame, with its errors (15,000 a second, which the log's summary carried
// into a later test). It is reported once, not drawn, and not tried again until it is reloaded.

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
#include "object/Primitive.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::test;

TEST(MeshMissingBufferGpu, IsReportedOnceNotDrawnAndNotTriedAgain){
    JASPER_REQUIRE_GPU();
    Scene scene;
    Camera camera(&scene);
    camera.setType(CameraType::CAMERA_PERSPECTIVE);
    camera.activate();
    Primitive plane(&scene);
    plane.createPlane(1.0f, 1.0f);
    MeshComponent& mesh = scene.getComponent<MeshComponent>(plane.getEntity());
    mesh.submeshes[0].attributes[AttributeType::TEXCOORD1] = Attribute(AttributeDataType::FLOAT, "no-such-buffer", 2, 0, 4, false, false);
    Engine::setScene(&scene);

    const LogCapture capture;
    ASSERT_TRUE(GpuEnvironment::runFrames(nullptr, 4));
    EXPECT_FALSE(mesh.loaded);
    EXPECT_TRUE(mesh.loadCalled);
    EXPECT_EQ(capture.count(D_LOG_ERROR, "no-such-buffer"), 1u) << capture.describeErrors();

    GpuEnvironment::drawFrames(20);
    EXPECT_FALSE(mesh.loaded);
    EXPECT_EQ(capture.count(D_LOG_ERROR, "no-such-buffer"), 1u) << capture.describeErrors();
    EXPECT_FALSE(capture.contains("leaves vertex inputs"));

    Engine::removeScene(&scene);
    // (what it reported on purpose is not this test's failure, nor a later one's)
    LogCapture::markTestStart();
}
