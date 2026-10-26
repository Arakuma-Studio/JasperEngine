// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Records the old ActionSystem particle simulation's trajectories of the migration fixtures
// (LegacyParticleFixtures.h) at 1/60 s steps, the reference ParticleMigration compares the new
// runtime with (S20 AC13). With JASPER_PARTICLE_RECORD_DIR set it writes <dir>/<name>.csv.

#include "Scene.h"
#include "subsystem/ActionSystem.h"
#include "subsystem/MeshSystem.h"
#include "util/Angle.h"

#include "LegacyParticleFixtures.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace jasperengine;
using namespace particletest;

TEST(LegacyParticleTrajectories, Record){
    const char* dir = std::getenv("JASPER_PARTICLE_RECORD_DIR");
    for (size_t f = 0; f < legacyFixtures().size(); f++){
        const LegacyFixture& fx = legacyFixtures()[f];
        Scene scene;
        const Entity target = scene.createEntity();
        scene.addComponent<Transform>(target);
        Transform& t = scene.getComponent<Transform>(target);
        t.position = fx.position;
        t.rotation = fx.rotation;
        t.scale = fx.scale;
        if (fx.mesh){
            scene.addComponent<MeshComponent>(target);
        }else{
            scene.addComponent<PointsComponent>(target);
            PointsComponent& points = scene.getComponent<PointsComponent>(target);
            for (const Rect& r : fx.frames){
                points.framesRect[points.numFramesRect].rect = r;
                points.numFramesRect++;
            }
        }
        const Entity system = scene.createEntity();
        ActionComponent action;
        action.target = target;
        action.startTrigger = true;
        scene.addComponent<ActionComponent>(system, action);
        scene.addComponent<ParticleSystemComponent>(system);
        configureLegacy(f, scene.getComponent<ParticleSystemComponent>(system));

        std::string csv = std::string(legacyCsvHeader()) + "\n";
        ActionSystem* actions = scene.getSystem<ActionSystem>().get();
        size_t rows = 0;
        for (int frame = 0; frame < kLegacyFrames; frame++){
            actions->update(kLegacyStep);
            const ParticleSystemComponent& ps = scene.getComponent<ParticleSystemComponent>(system);
            for (size_t slot = 0; slot < ps.particles.size(); slot++){
                const ParticleData& p = ps.particles[slot];
                if (p.life <= p.time)
                    continue;
                Vector4 color(1, 1, 1, 1);
                Vector3 size(1, 1, 1);
                Quaternion q;
                float rot = 0.0f;
                Rect rect(0, 0, 1, 1);
                if (fx.mesh){
                    const InstanceData& in = scene.getComponent<InstancedMeshComponent>(target).instances[slot];
                    color = in.color;
                    size = p.scale;
                    q = p.rotation;
                    rect = in.textureRect;
                }else{
                    const PointData& pt = scene.getComponent<PointsComponent>(target).points[slot];
                    color = pt.color;
                    size = Vector3(pt.size, pt.size, pt.size);
                    rot = pt.rotation;
                    rect = pt.textureRect;
                }
                char line[512];
                std::snprintf(line, sizeof(line), "%d,%zu,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",
                              frame, slot, p.time, p.position.x, p.position.y, p.position.z, color.x, color.y, color.z, color.w,
                              size.x, size.y, size.z, q.x, q.y, q.z, q.w, rot, rect.getX(), rect.getY(), rect.getWidth(), rect.getHeight());
                csv += line;
                rows++;
            }
        }
        EXPECT_GT(rows, 0u) << fx.name;
        if (dir){
            const std::string path = std::string(dir) + "/" + fx.name + ".csv";
            FILE* out = std::fopen(path.c_str(), "w");
            ASSERT_NE(out, nullptr) << path;
            std::fwrite(csv.data(), 1, csv.size(), out);
            std::fclose(out);
        }
    }
}
