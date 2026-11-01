// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Particle Systems of version 1 (the legacy initializer / modifier model) play exactly as they
// did before the module model existed (particles spec 5.1, 9): the seven create_particles
// presets and the editor's default (fixtures/particles-legacy/presets.yaml), simulated for 120
// frames of 1/60 s with srand(1) on a Points target (fire also on a Mesh target, the instanced
// path), give the particle positions, colours and sizes of fixtures/particles-legacy/
// capture.json, recorded from the code before version 2. JASPER_CAPTURE_PARTICLES_LEGACY=1
// rewrites the capture instead (only ever from that code).

#include <gtest/gtest.h>

#include "Stream.h"
#include "Scene.h"
#include "subsystem/ActionSystem.h"
#include "component/ActionComponent.h"
#include "component/InstancedMeshComponent.h"
#include "component/MeshComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/PointsComponent.h"
#include "component/Transform.h"

#include "json.hpp"
#include "yaml-cpp/yaml.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace jasperengine;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

    fs::path fixtureDir() {
        return fs::path(JASPER_EDITOR_TEST_FIXTURES) / "particles-legacy";
    }

    // 5 significant decimals: the capture is text, the comparison has a tolerance anyway
    double rounded(float value) {
        return std::round((double)value * 100000.0) / 100000.0;
    }

    // the visible particles of the target after `frames` frames, recorded at frames 30, 60 and 120
    Json simulate(const YAML::Node& componentNode, bool meshTarget) {
        Scene scene;

        Entity target = scene.createEntity();
        Transform transform;
        // a moved, turned, scaled emitter: the world-space conversions take part
        transform.position = Vector3(1.0f, 2.0f, -3.0f);
        transform.rotation = Quaternion(30.0f, Vector3(0.0f, 1.0f, 0.0f));
        transform.scale = Vector3(2.0f, 2.0f, 2.0f);
        scene.addComponent<Transform>(target, transform);
        if (meshTarget) {
            scene.addComponent<MeshComponent>(target, MeshComponent());
        } else {
            scene.addComponent<PointsComponent>(target, PointsComponent());
        }

        Entity system = scene.createEntity();
        ActionComponent action;
        action.target = target;
        action.startTrigger = true;
        scene.addComponent<ActionComponent>(system, action);
        scene.addComponent<ParticleSystemComponent>(system, editor::Stream::decodeParticleSystemComponent(componentNode));

        std::srand(1);
        Json frames = Json::object();
        for (int frame = 1; frame <= 120; frame++) {
            scene.getSystem<ActionSystem>()->update(1.0 / 60.0);
            if (frame != 30 && frame != 60 && frame != 120) continue;

            Json particles = Json::array();
            if (meshTarget) {
                const InstancedMeshComponent* instmesh = scene.findComponent<InstancedMeshComponent>(target);
                if (instmesh) {
                    for (const InstanceData& instance : instmesh->instances) {
                        if (!instance.visible) continue;
                        particles.push_back({rounded(instance.position.x), rounded(instance.position.y), rounded(instance.position.z),
                                             rounded(instance.color.x), rounded(instance.color.y), rounded(instance.color.z), rounded(instance.color.w),
                                             rounded(instance.scale.x), rounded(instance.scale.y), rounded(instance.scale.z)});
                    }
                }
            } else {
                const PointsComponent& points = scene.getComponent<PointsComponent>(target);
                for (const PointData& point : points.points) {
                    if (!point.visible) continue;
                    particles.push_back({rounded(point.position.x), rounded(point.position.y), rounded(point.position.z),
                                         rounded(point.color.x), rounded(point.color.y), rounded(point.color.z), rounded(point.color.w),
                                         rounded(point.size), rounded(point.rotation)});
                }
            }
            frames[std::to_string(frame)] = particles;
        }
        return frames;
    }

    struct LegacyCase {
        std::string name;
        std::string preset;
        bool meshTarget;
    };

    std::vector<LegacyCase> legacyCases() {
        std::vector<LegacyCase> cases;
        for (const char* preset : {"default", "fire", "smoke", "sparks", "explosion", "snow", "rain", "magic"}) {
            cases.push_back({preset, preset, false});
        }
        cases.push_back({"fire_mesh", "fire", true});
        return cases;
    }

}

TEST(ParticleLegacyIdentity, PresetsPlayAsBeforeTheModuleModel) {
    const YAML::Node presets = YAML::LoadFile((fixtureDir() / "presets.yaml").string());
    const fs::path capturePath = fixtureDir() / "capture.json";

    if (std::getenv("JASPER_CAPTURE_PARTICLES_LEGACY")) {
        Json capture = Json::object();
        for (const LegacyCase& item : legacyCases()) {
            capture[item.name] = simulate(presets[item.preset], item.meshTarget);
        }
        std::ofstream out(capturePath);
        out << capture.dump() << "\n";
        GTEST_SKIP() << "capture written to " << capturePath.string();
    }

    std::ifstream in(capturePath);
    ASSERT_TRUE(in.good()) << capturePath.string();
    const Json capture = Json::parse(in);

    for (const LegacyCase& item : legacyCases()) {
        SCOPED_TRACE(item.name);
        ASSERT_TRUE(capture.contains(item.name));
        const Json result = simulate(presets[item.preset], item.meshTarget);
        const Json& expected = capture[item.name];
        for (const char* frame : {"30", "60", "120"}) {
            SCOPED_TRACE(std::string("frame ") + frame);
            const Json& want = expected[frame];
            const Json& got = result[frame];
            ASSERT_EQ(got.size(), want.size()) << "visible particles";
            for (size_t p = 0; p < want.size(); p++) {
                ASSERT_EQ(got[p].size(), want[p].size());
                for (size_t v = 0; v < want[p].size(); v++) {
                    EXPECT_NEAR(got[p][v].get<double>(), want[p][v].get<double>(), 2e-4) << "particle " << p << " value " << v;
                }
            }
        }
    }
}
