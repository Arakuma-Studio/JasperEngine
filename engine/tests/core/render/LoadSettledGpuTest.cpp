// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A scene with an asset it cannot load still finishes loading: RenderSystem::isAllLoaded() (the
// engine's "Engine ready:" line, a --after-ready run, the loading-screen progress) counts a mesh
// with nothing to load as settled, the mesh of a model whose file is missing included. Before,
// the empty mesh was pending forever and a shipped game's --after-ready run waited until its
// --ready-timeout (exit 6) for a scene that had long finished (docs/specs/S30-shipping.md).

#include "GpuEnvironment.h"

#include "Engine.h"
#include "Scene.h"
#include "component/ModelComponent.h"
#include "object/Camera.h"
#include "object/Mesh.h"
#include "object/Model.h"
#include "object/Primitive.h"
#include "subsystem/RenderSystem.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class LoadSettledGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};

        void SetUp() override{
            camera.activate();
            Engine::setScene(&scene);
        }

        void TearDown() override{
            Engine::removeScene(&scene);
        }

        std::shared_ptr<RenderSystem> render(){ return scene.getSystem<RenderSystem>(); }
    };

}

TEST_F(LoadSettledGpu, AMissingModelAndAnEmptyMeshDoNotKeepTheSceneLoading){
    JASPER_REQUIRE_GPU();
    // a model as a scene names it (the cooked or the YAML scene): its file is not there
    Model missing(&scene);
    ModelComponent& model = missing.getComponent<ModelComponent>();
    model.filename = "load-settled-missing-model.gltf";
    model.needUpdateModel = true;
    // a mesh with no geometry and no model (one a script fills later, or none at all)
    Mesh empty(&scene);
    // and one that does load, so the scene has something to wait for
    Primitive plane(&scene);
    plane.createPlane(1.0f, 1.0f);

    const std::shared_ptr<RenderSystem> system = render();
    ASSERT_TRUE(GpuEnvironment::runFrames([system](){ return system->isAllLoaded(); }, 120))
        << "still waiting for: " << [&](){
               std::string all;
               for (const std::string& part : system->describeNotLoaded()) all += part + "; ";
               return all;
           }();
    EXPECT_TRUE(system->describeNotLoaded().empty());
    EXPECT_FALSE(model.needUpdateModel) << "the failed load is over, not retried";
}
