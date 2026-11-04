// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// origin/main's ParticleLegacyIdentityTest on S20: the seven create_particles presets of the
// version 1 (legacy) model and the editor's default (fixtures/particles-legacy/presets.yaml),
// on a moved, turned, scaled Points target (fire also on a Mesh target), migrated to S20
// effects the way a scene loads them (StreamMigrations: LegacyParticleConverter, D9) and played
// for 120 frames of 1/60 s, show what the old code drew (fixtures/particles-legacy/capture.json,
// recorded from it with srand(1)): the visible particles at frames 30, 60 and 120, and their mean
// colour and alpha. The old model drew from rand() and S20 from its own streams, so the
// particles themselves are not the same ones: their number and their colours (functions of
// their ages) are.

#include "Stream.h"
#include "profiler/Profiler.h"
#include "StreamMigrations.h"
#include "Scene.h"
#include "component/ParticleSystemComponent.h"
#include "component/Transform.h"
#include "memory/Arena.h"
#include "object/ParticleSystem.h"
#include "particles/ParticleRender.h"
#include "subsystem/ParticleSubsystem.h"
#include "subsystem/ParticleSystemState.h"
#include "util/Color.h"

#include "json.hpp"
#include "yaml-cpp/yaml.h"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

    fs::path fixtureDir(){
        return fs::path(JASPER_TEST_FIXTURES_DIR) / "particles-legacy";
    }

    struct Frame {
        size_t count = 0;
        double r = 0.0, g = 0.0, b = 0.0, a = 0.0;
    };

    // the legacy system on its target, migrated: the S20 component the target ends with
    ParticleSystemComponent migrated(const YAML::Node& preset, bool meshTarget){
        YAML::Node root = YAML::Load("{entities: []}");
        YAML::Node target;
        target["type"] = "Entity";
        target["entity"] = 1;
        target["name"] = "Target";
        target["components"]["transform"] = YAML::Load("{position: [1, 2, -3], scale: [2, 2, 2]}");
        if (meshTarget){
            target["components"]["mesh"] = YAML::Load("{submeshes: []}");
            target["components"]["instancedmesh"] = YAML::Load("{instances: []}");
        }else{
            target["components"]["points"] = YAML::Load("{points: []}");
        }
        YAML::Node system;
        system["type"] = "Entity";
        system["entity"] = 2;
        system["name"] = "System";
        system["components"]["action"] = YAML::Load("{target: 1, startTrigger: true}");
        system["components"]["particlesystem"] = YAML::Clone(preset);
        root["entities"].push_back(target);
        root["entities"].push_back(system);
        StreamMigrations::migrate(DocumentKind::Scene, root);
        return Stream::decodeParticleSystemComponent(root["entities"][0]["components"]["particlesystem"]);
    }

    std::vector<Frame> play(const ParticleSystemComponent& component){
        Scene scene;
        ParticleSystem system(&scene);
        Transform& transform = scene.getComponent<Transform>(system.getEntity());
        transform.position = Vector3(1.0f, 2.0f, -3.0f);
        transform.rotation = Quaternion(30.0f, Vector3(0.0f, 1.0f, 0.0f));
        transform.scale = Vector3(2.0f, 2.0f, 2.0f);
        transform.needUpdate = true;
        ParticleSystemComponent& ps = scene.getComponent<ParticleSystemComponent>(system.getEntity());
        ps.inlineEffect = component.inlineEffect;
        ps.playOnAwake = false;
        ps.playbackSpeed = component.playbackSpeed;
        ps.needReload = true;
        std::shared_ptr<ParticleSubsystem> subsystem = scene.getSystem<ParticleSubsystem>();
        subsystem->update(0.0);
        // the action started it (startTrigger)
        system.play();
        std::vector<Frame> frames;
        memory::Arena arena("LegacyParticlePresetsTest");
        for (int frame = 1; frame <= 120; frame++){
            // a camera sees it every frame (the render marks it; nothing renders here): S20's
            // culling does not pause it when the binary's frame counter is past its first frames
            subsystem->markVisible(system.getEntity());
            subsystem->update(1.0 / 60.0);
            if (frame != 30 && frame != 60 && frame != 120) continue;
            Frame f;
            const particles::ParticleSystemState* state = subsystem->getState(system.getEntity());
            if (state){
                const particles::PackResult pack = particles::ParticleRenderPrep::pack(state->runtime, particles::RenderView(), arena);
                f.count = pack.count;
                for (uint32_t i = 0; i < pack.count; i++){
                    const uint32_t c = pack.instances[i].color;
                    const Vector3 lin = Color::sRGBToLinear(Vector3((float)(c & 255u) / 255.0f, (float)((c >> 8) & 255u) / 255.0f,
                                                                    (float)((c >> 16) & 255u) / 255.0f));
                    f.r += lin.x; f.g += lin.y; f.b += lin.z;
                    f.a += (float)(c >> 24) / 255.0f;
                }
                arena.reset();
            }
            frames.push_back(f);
        }
        return frames;
    }

    Frame captured(const Json& particles){
        Frame f;
        f.count = particles.size();
        for (const Json& p : particles){
            f.r += p[3].get<double>();
            f.g += p[4].get<double>();
            f.b += p[5].get<double>();
            f.a += p[6].get<double>();
        }
        return f;
    }

}

TEST(LegacyParticlePresets, PresetsPlayAsTheOldModelDid){
    const YAML::Node presets = YAML::LoadFile((fixtureDir() / "presets.yaml").string());
    std::ifstream in(fixtureDir() / "capture.json");
    ASSERT_TRUE(in.good());
    const Json capture = Json::parse(in);
    struct Case { const char* name; const char* preset; bool mesh; };
    const Case cases[] = {{"default", "default", false}, {"fire", "fire", false}, {"smoke", "smoke", false}, {"sparks", "sparks", false},
                          {"explosion", "explosion", false}, {"snow", "snow", false}, {"rain", "rain", false}, {"magic", "magic", false},
                          {"fire_mesh", "fire", true}};
    for (const Case& c : cases){
        SCOPED_TRACE(c.name);
        ASSERT_TRUE(capture.contains(c.name));
        const std::vector<Frame> got = play(migrated(presets[c.preset], c.mesh));
        ASSERT_EQ(got.size(), 3u);
        const char* frameNames[] = {"30", "60", "120"};
        for (size_t k = 0; k < 3; k++){
            SCOPED_TRACE(std::string("frame ") + frameNames[k]);
            const Frame want = captured(capture[c.name][frameNames[k]]);
            const double tolerance = std::max(3.0, 0.2 * (double)want.count);
            EXPECT_NEAR((double)got[k].count, (double)want.count, tolerance) << "visible particles";
            if (want.count == 0 || got[k].count == 0) continue;
            const double n = (double)got[k].count, m = (double)want.count;
            EXPECT_NEAR(got[k].r / n, want.r / m, 0.12) << "mean red";
            EXPECT_NEAR(got[k].g / n, want.g / m, 0.12) << "mean green";
            EXPECT_NEAR(got[k].b / n, want.b / m, 0.12) << "mean blue";
            EXPECT_NEAR(got[k].a / n, want.a / m, 0.12) << "mean alpha";
        }
    }
}
