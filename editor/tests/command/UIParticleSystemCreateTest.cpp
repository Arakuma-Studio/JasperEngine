// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// GameObject > UI > Particle System on S20 (ui-scaling-text; docs/specs/S20-particles.md,
// Upstream absorption): one GameObject whose Particle System plays at Engine.timeScale 0 (its
// own Unscaled Time), simulates in local space and draws the soft round image; origin/main's
// sparkle in UI units (rising on a y-down screen) through the old system's converter; one undo
// step.

#include "Project.h"
#include "command/CommandHandle.h"
#include "command/type/CreateEntityCmd.h"
#include "component/ParticleSystemComponent.h"
#include "component/PointsComponent.h"
#include "component/Transform.h"
#include "particles/LegacyParticleConverter.h"
#include "support/TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    class UIParticleSystemCreateTest : public ::testing::Test {
    protected:
        jasperengine::test::TestTempDir dir{"ui-particle-system"};
        std::unique_ptr<Project> project;
        SceneProject* scene = nullptr;

        void SetUp() override{
            fs::copy(fs::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/prefab-runtime", dir.path(),
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            project = std::make_unique<Project>();
            project->setSkipProjectLock(true);
            ASSERT_TRUE(project->loadProject(dir.path(), false));
            for (SceneProject& candidate : project->getScenes()){
                if (!candidate.filepath.empty() && !candidate.scene){
                    project->loadScene(candidate.filepath, false, false, true);
                }
            }
            for (SceneProject& candidate : project->getScenes()){
                if (candidate.scene && candidate.name == "Main Scene") scene = &candidate;
            }
            ASSERT_NE(scene, nullptr);
            project->setSelectedSceneId(scene->id);
        }

        void TearDown() override{
            if (project){
                for (SceneProject& candidate : project->getScenes()){
                    CommandHandle::remove(candidate.id);
                }
                project->releaseScenes();
            }
            project.reset();
        }

        CommandHistory* history(){ return CommandHandle::get(scene->id); }
    };

}

TEST_F(UIParticleSystemCreateTest, OneUnscaledLocalSystemWithTheSoftImage){
    CreateEntityCmd* create = new CreateEntityCmd(project.get(), scene->id, "Sparkle", EntityCreationType::UI_PARTICLE_SYSTEM);
    ASSERT_TRUE(history()->addCommandNoMerge(create));
    const Entity entity = create->getEntity();
    ASSERT_NE(entity, NULL_ENTITY);
    Scene* s = scene->scene;

    // one GameObject: it renders itself (no Points emitter, no child with an action)
    ASSERT_TRUE(s->findComponent<Transform>(entity));
    ASSERT_TRUE(s->findComponent<ParticleSystemComponent>(entity));
    EXPECT_FALSE(s->findComponent<PointsComponent>(entity));
    auto transforms = s->getComponentArray<Transform>();
    ASSERT_TRUE(transforms->hasEntity(entity));
    EXPECT_EQ(s->findBranchLastIndex(entity), transforms->getIndex(entity)) << "no child";

    const ParticleSystemComponent& particles = s->getComponent<ParticleSystemComponent>(entity);
    EXPECT_TRUE(particles.useUnscaledTime) << "UI particles play in a paused game";
    EXPECT_TRUE(particles.playOnAwake);
    EXPECT_TRUE(particles.effect.empty()) << "an inline effect";
    const particles::ParticleEffectDocument& effect = particles.inlineEffect;
    EXPECT_EQ(effect.simulationSpace, particles::SimulationSpace::Local);
    EXPECT_EQ(effect.maxParticles, 100u);

    // origin/main's fields, read back through the converter's old view
    const LegacyParticleSystem legacy = particles::LegacyParticleConverter::extract(effect);
    EXPECT_EQ(legacy.rate, 25);
    EXPECT_TRUE(legacy.localSpace);
    EXPECT_FLOAT_EQ(legacy.lifeInitializer.minLife, 0.8f);
    EXPECT_FLOAT_EQ(legacy.lifeInitializer.maxLife, 1.6f);
    EXPECT_FLOAT_EQ(legacy.sizeInitializer.minSize, 16.0f);
    EXPECT_FLOAT_EQ(legacy.sizeInitializer.maxSize, 28.0f);
    // y down: it starts upwards on screen (negative y) and falls back
    EXPECT_LT(legacy.velocityInitializer.maxVelocity.y, 0.0f);
    EXPECT_GT(legacy.accelerationInitializer.minAcceleration.y, 0.0f);

    // the soft round image, made in the project
    ASSERT_FALSE(effect.renderer.texture.empty());
    EXPECT_NE(effect.renderer.texture.find("particle_soft.png"), std::string::npos) << effect.renderer.texture;
    EXPECT_TRUE(fs::exists(project->getAssetsPath() / "ui" / "particle_soft.png"));
    EXPECT_EQ(effect.renderer.untexturedShape, particles::UntexturedShape::Disc);

    ASSERT_TRUE(history()->undo());
    EXPECT_FALSE(s->isEntityCreated(entity) && s->findComponent<ParticleSystemComponent>(entity));
}
