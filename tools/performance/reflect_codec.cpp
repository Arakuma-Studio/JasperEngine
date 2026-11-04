// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The reflection codec against the hand-written codecs it replaced (docs/specs/S08-reflection.md
// R17, AC14): encode and decode of a synthetic scene (Transform + Sprite + Light + AudioSource +
// Rigidbody2D on every GameObject), median of 5 runs each. Built into jasperengine-editor-tests
// (editor/tests/EditorTests.cmake) as ReflectCodecBench.*: 1,000 GameObjects by default,
// JASPER_BENCH_ENTITIES=10000 for the acceptance size; JASPER_TEST_PERF_SLACK holds each to +5% of
// the hand-written codecs' frozen numbers. Prints "[bench] reflect codec".

#include "support/EditorTestSupport.h"
#include "Stream.h"
#include "profiler/GpuMemory.h"

#include "reflect/Reflect.h"

#include "component/AudioSourceComponent.h"
#include "component/LightComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/SpriteComponent.h"
#include "component/Transform.h"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

// (a named namespace, tests included: a unity build merges the test sources)
namespace reflect_codec_bench {

    // The hand-written codecs' medians for this scene, ms per GameObject, measured last before they
    // were deleted (docs/specs/S73-code-cleanups.md R2): 10,000 GameObjects on build server B
    // (RelWithDebInfo). A machine that is not B gives JASPER_TEST_PERF_SLACK its ratio to it.
    constexpr double kFrozenEncodeMs = 3557.07 / 10000.0;
    constexpr double kFrozenDecodeMs = 801.96 / 10000.0;

    struct Entry {
        Transform transform;
        SpriteComponent sprite;
        LightComponent light;
        AudioSourceComponent audio;
        Rigidbody2DComponent body;
    };

    std::vector<Entry> scene(size_t count) {
        std::vector<Entry> entries(count);
        for (size_t i = 0; i < count; i++) {
            Entry& entry = entries[i];
            const float f = static_cast<float>(i);
            entry.transform.position = Vector3(f, f * 0.5f, -f);
            entry.transform.scale = Vector3(1.0f + f * 0.001f, 1.0f, 1.0f);
            entry.sprite.width = 32 + static_cast<unsigned int>(i % 7);
            entry.sprite.height = 32;
            entry.light.intensity = 1.0f + f * 0.01f;
            entry.light.range = 10.0f + f;
            entry.audio.volume = 0.5f;
            entry.audio.filename = "sounds/hit.wav";
            entry.body.type = (i % 2) ? BodyType::DYNAMIC : BodyType::STATIC;
        }
        return entries;
    }

    double medianMs(const std::function<void()>& run) {
        std::vector<double> times;
        for (int i = 0; i < 5; i++) {
            const auto start = std::chrono::steady_clock::now();
            run();
            times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
        std::sort(times.begin(), times.end());
        return times[2];
    }

    size_t entityCount() {
        const char* value = std::getenv("JASPER_BENCH_ENTITIES");
        return value ? static_cast<size_t>(std::strtoul(value, nullptr, 10)) : 1000;
    }

    TEST(ReflectCodecBench, EncodeAndDecodeAgainstTheFrozenCodecs) {
        const std::vector<Entry> entries = scene(entityCount());
        const reflect::ComponentDesc& transform = *reflect::find(ComponentType::Transform);
        const reflect::ComponentDesc& sprite = *reflect::find(ComponentType::SpriteComponent);
        const reflect::ComponentDesc& light = *reflect::find(ComponentType::LightComponent);
        const reflect::ComponentDesc& audio = *reflect::find(ComponentType::AudioSourceComponent);
        const reflect::ComponentDesc& body = *reflect::find(ComponentType::Rigidbody2DComponent);

        std::vector<YAML::Node> encoded(entries.size());
        const double reflectEncode = medianMs([&]() {
            for (size_t i = 0; i < entries.size(); i++) {
                YAML::Node node;
                node["transform"] = reflect::encode(transform, &entries[i].transform, reflect::EncodeContext{});
                node["sprite"] = reflect::encode(sprite, &entries[i].sprite, reflect::EncodeContext{});
                node["light"] = reflect::encode(light, &entries[i].light, reflect::EncodeContext{});
                node["audiosource"] = reflect::encode(audio, &entries[i].audio, reflect::EncodeContext{});
                node["rigidbody2d"] = reflect::encode(body, &entries[i].body, reflect::EncodeContext{});
                encoded[i] = node;
            }
        });
        const double reflectDecode = medianMs([&]() {
            for (const YAML::Node& node : encoded) {
                Entry entry;
                reflect::DecodeReport report;
                reflect::decode(transform, node["transform"], &entry.transform, reflect::DecodeContext{}, report);
                reflect::decode(sprite, node["sprite"], &entry.sprite, reflect::DecodeContext{}, report);
                reflect::decode(light, node["light"], &entry.light, reflect::DecodeContext{}, report);
                reflect::decode(audio, node["audiosource"], &entry.audio, reflect::DecodeContext{}, report);
                reflect::decode(body, node["rigidbody2d"], &entry.body, reflect::DecodeContext{}, report);
            }
        });
        const double frozenEncode = kFrozenEncodeMs * static_cast<double>(entries.size());
        const double frozenDecode = kFrozenDecodeMs * static_cast<double>(entries.size());
        std::printf("[bench] reflect codec, %zu GameObjects: encode %.2f ms (frozen %.2f ms, %+.1f%%), decode %.2f ms (frozen %.2f ms, %+.1f%%)\n",
                    entries.size(), reflectEncode, frozenEncode, 100.0 * (reflectEncode / frozenEncode - 1.0), reflectDecode, frozenDecode,
                    100.0 * (reflectDecode / frozenDecode - 1.0));
        if (const double slack = editor::test::perfSlack(); slack > 0.0) {
            EXPECT_LE(reflectEncode, frozenEncode * 1.05 * slack);
            EXPECT_LE(reflectDecode, frozenDecode * 1.05 * slack);
        }
    }

}
