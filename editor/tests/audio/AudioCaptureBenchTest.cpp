// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What the deterministic pump and a capture cost a frame (docs/specs/S35-agent-audio.md R27, R28; AC13): 32 voices through 4 groups
// with a reverb, fixed 60 fps at 48 kHz. Printed always, enforced by perfSlack() on a benchmark run.

#include <gtest/gtest.h>

#include "../ScriptDataTestSupport.h"
#include "../support/EditorTestSupport.h"

#include "Scene.h"
#include "audio/AudioCapture.h"
#include "audio/AudioMixer.h"
#include "object/sound/AudioSource.h"
#include "pool/SoundPool.h"
#include "subsystem/AudioSystem.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
namespace fs = std::filesystem;

namespace {

    const char* const benchMixer = R"({
  "format": "jasperengine.mixer", "version": 1, "updateMode": "scaled",
  "groups": [
    {"id": "00000001", "name": "Master", "parent": null, "volume": 0},
    {"id": "9b51c0de", "name": "Music", "parent": "00000001", "volume": 0, "effects": [{"id": "c4f00d01", "type": "reverb"}]},
    {"id": "9b51c0df", "name": "SFX", "parent": "00000001", "volume": -6},
    {"id": "9b51c0e0", "name": "Voice", "parent": "00000001", "volume": -3}
  ]
})";

    class AudioCaptureBench : public scriptdata::ScriptDataTest {
    protected:
        Scene* scene = nullptr;
        bool wasAsync = false;
        std::vector<std::unique_ptr<AudioSource>> sources;

        void SetUp() override {
            ScriptDataTest::SetUp();
            wasAsync = Engine::isAsyncLoading();
            Engine::setAsyncLoading(false);
            SoundPool::clear();
            const fs::path file = root / "assets" / "loop.wav";
            fs::create_directories(file.parent_path());
            std::ofstream out(file, std::ios::binary);
            const uint32_t samples = 48000;
            auto u32 = [&out](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
            auto u16 = [&out](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
            out.write("RIFF", 4);
            u32(36 + samples * 2);
            out.write("WAVEfmt ", 8);
            u32(16);
            u16(1);
            u16(1);
            u32(48000);
            u32(96000);
            u16(2);
            u16(16);
            out.write("data", 4);
            u32(samples * 2);
            for (uint32_t i = 0; i < samples; i++) u16((uint16_t)(int16_t)std::lround(0.05 * std::sin(2.0 * M_PI * 440.0 * i / 48000.0) * 32767.0));
            out.close();
            scene = new Scene();
            Engine::setScene(scene);
        }

        void TearDown() override {
            sources.clear();
            Engine::removeScene(scene);
            delete scene;
            scene = nullptr;
            AudioSystem::setBackend(AudioBackend::Auto);
            AudioSystem::close();
            AudioMixer::unload();
            SoundPool::clear();
            Engine::setAsyncLoading(wasAsync);
            ScriptDataTest::TearDown();
        }

        void start(AudioBackend backend) {
            sources.clear();
            AudioSystem::close();
            AudioSystem::setBackend(backend);
            AudioConfig config;
            config.maxRealVoices = 64;
            AudioSystem::configure(config);
            std::string error;
            ASSERT_TRUE(AudioMixer::loadFromText(benchMixer, "bench.mixer", &error)) << error;
            const char* const groups[] = {"Music", "SFX", "Voice", "Master"};
            for (int i = 0; i < 32; i++) {
                sources.push_back(std::make_unique<AudioSource>(scene));
                sources.back()->loadSound("loop.wav");
                sources.back()->setOutputGroup(groups[i % 4]);
                sources.back()->setLooping(true);
                sources.back()->play();
            }
            AudioMixer::update(1.0 / 60.0, 1.0 / 60.0);
            scene->getSystem<AudioSystem>()->update(1.0 / 60.0);
        }

        // the mean ms of a frame's mix
        double measure(bool deterministic) {
            const auto begin = std::chrono::steady_clock::now();
            const int frames = 600;
            for (int k = 0; k < frames; k++) {
                if (deterministic) AudioSystem::pumpDeterministic(1.0 / 60.0);
                else AudioSystem::mixManually(1.0 / 60.0);
            }
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count() / frames;
        }
    };

}

TEST_F(AudioCaptureBench, PumpAndCaptureBudgets) {
    start(AudioBackend::Null);
    const double null = measure(false);

    start(AudioBackend::Deterministic);
    const double pump = measure(true);

    start(AudioBackend::Deterministic);
    AudioCaptureOptions options;
    options.root = root / "captures";
    options.scene = "bench";
    options.maxSeconds = 3600;
    std::string error;
    ASSERT_TRUE(AudioCapture::begin(options, &error)) << error;
    const double captured = measure(true);
    AudioCapture::end();
    const double capture = captured - pump;

    std::printf("[bench] 32 voices, 4 groups, a reverb: Null pump %.3f ms a frame, deterministic pump %.3f ms (%+.1f %%), capture on top %.3f ms\n", null, pump, (pump / null - 1.0) * 100.0, capture);
    const double slack = editor::test::perfSlack();
    if (slack > 0.0) {
        EXPECT_LE(pump, 0.6 * slack) << "R27";
        EXPECT_LE(pump, null * 1.05 * slack) << "R27: the same work as the Null pump";
        EXPECT_LE(capture, 0.4 * slack) << "R28";
    }
}
