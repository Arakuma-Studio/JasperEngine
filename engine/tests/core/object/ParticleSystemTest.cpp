// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// An old script's ParticleSystem(scene, e) on the GameObject an old system was migrated from
// (docs/specs/S94-rendering-anim-gaps.md F2): it addresses the moved system through the
// component's legacyFrom, also once that GameObject has no action naming the system any more.

#include "Scene.h"
#include "component/ParticleSystemComponent.h"
#include "object/ParticleSystem.h"
#include "subsystem/ParticleSubsystem.h"

#include "LogCapture.h"

#include <gtest/gtest.h>

namespace ParticleSystemTest_cpp {

using namespace jasperengine;
using namespace jasperengine::test;

TEST(ParticleSystemTest, OldSystemGameObjectResolvesThroughLegacyFrom){
    Scene scene;
    ParticleSystem moved(&scene);
    moved.setPlayOnAwake(false);
    ASSERT_TRUE(moved.setInlineEffect(R"({"version": 1, "duration": 1, "looping": false, "maxParticles": 10,
        "modules": [{"type": "main", "startLifetime": 100, "startSpeed": 0},
                    {"type": "emission", "rateOverTime": 0, "bursts": [{"time": 0, "count": 4}]},
                    {"type": "shape", "enabled": false}]})"));
    // the old controller, its action gone (removed by hand after the migration)
    const Entity old = scene.createEntity();
    scene.setEntityName(old, "FxConfettiSystem");
    moved.getComponent<ParticleSystemComponent>().legacyFrom = old;

    LogCapture log;
    ParticleSystem wrapper(&scene, old);
    EXPECT_EQ(wrapper.getEntity(), moved.getEntity());
    wrapper.play();
    // until its first tick has run (a frame may carry its tick to the next)
    for (int i = 0; i < 10 && moved.getTime() <= 0.0f; i++)
        scene.getSystem<ParticleSubsystem>()->update(1.0 / 60.0);
    EXPECT_EQ(moved.getParticleCount(), 4u) << "the wrapper plays the moved system";

    ParticleSystem again(&scene, old);
    EXPECT_EQ(again.getEntity(), moved.getEntity());
    EXPECT_EQ(log.count(D_LOG_WARN, "ParticleSystem(scene, entity) of the old system's GameObject 'FxConfettiSystem'"), 1u)
        << "one deprecation note for the system";

    // a GameObject nothing was migrated from stays itself
    const Entity unrelated = scene.createEntity();
    EXPECT_EQ(ParticleSystem(&scene, unrelated).getEntity(), unrelated);
}

}  // namespace ParticleSystemTest_cpp
