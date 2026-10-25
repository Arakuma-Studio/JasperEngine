// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Every component the reflection codec writes encodes and decodes as its hand-written codec did
// (docs/specs/S08-reflection.md AC3, AC4): for 500 seeded random instances (NaN, infinities and
// -0 included) the YAML text is the same byte for byte, and decoding that text over a random base
// (every other time with a random subset of its keys) gives the same values, floats compared
// bitwise. The hand-written codecs are kept verbatim in editor/tests/reflect/legacy.

#include "reflect/ReflectTestSupport.h"
#include "reflect/legacy/LegacyCodecs.h"

#include "reflect/Reflect.h"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <functional>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

// (a named namespace, tests included: a unity build merges the test sources)
namespace reflect_parity_test {

    constexpr int kSamples = 500;

    YAML::Node withoutKeys(const YAML::Node& map, RandomValues& random) {
        YAML::Node copy(YAML::NodeType::Map);
        if (!map.IsMap()) return map;
        for (auto it = map.begin(); it != map.end(); ++it) {
            if (random.chance(35)) continue;
            copy[it->first.Scalar()] = it->second;
        }
        return copy;
    }

    YAML::Node mapWithoutKey(const YAML::Node& map, const std::string& key) {
        YAML::Node copy(YAML::NodeType::Map);
        for (auto it = map.begin(); it != map.end(); ++it) {
            if (it->first.Scalar() != key) copy[it->first.Scalar()] = it->second;
        }
        return copy;
    }

    // The hand-written decode as S03 ran it (StreamLoad::decodeComponent): a key whose value threw
    // is left out and the decode runs again
    template<typename T, typename Decode>
    T legacyDecode(Decode decode, YAML::Node node, const T* base) {
        for (int attempt = 0; attempt < 16; attempt++) {
            try {
                return decode(node, base);
            } catch (const YAML::Exception&) {
                bool narrowed = false;
                for (auto it = node.begin(); it != node.end() && !narrowed; ++it) {
                    const std::string key = it->first.Scalar();
                    try {
                        (void)decode(mapWithoutKey(node, key), base);
                        node = mapWithoutKey(node, key);
                        narrowed = true;
                    } catch (const YAML::Exception&) {
                    }
                }
                if (!narrowed) break;
            }
        }
        return decode(YAML::Node(YAML::NodeType::Map), base);
    }

    template<typename T>
    void encodeMatches(ComponentType type, YAML::Node (*legacy)(const T&), uint32_t seed) {
        const reflect::ComponentDesc& desc = *reflect::find(type);
        ASSERT_TRUE(desc.codec == nullptr) << desc.cppName << " has its own codec";
        OwnedFramebuffers owned;
        for (int i = 0; i < kSamples; i++) {
            RandomValues random(seed + static_cast<uint32_t>(i));
            T comp{};
            owned.keep(&comp, type);
            random.fillStruct(desc.body, &comp);
            const std::string expected = YAML::Dump(legacy(comp));
            const std::string actual = YAML::Dump(reflect::encode(desc, &comp, reflect::EncodeContext{}));
            ASSERT_EQ(actual, expected) << desc.cppName << " sample " << i;
        }
    }

    template<typename T>
    void decodeMatches(ComponentType type, YAML::Node (*legacyEncode)(const T&), T (*legacy)(const YAML::Node&, const T*), uint32_t seed) {
        const reflect::ComponentDesc& desc = *reflect::find(type);
        OwnedFramebuffers owned;
        for (int i = 0; i < kSamples; i++) {
            RandomValues random(seed + static_cast<uint32_t>(i));
            T authored{};
            T base{};
            owned.keep(&authored, type);
            owned.keep(&base, type);
            random.fillStruct(desc.body, &authored);
            random.fillStruct(desc.body, &base);
            YAML::Node node = YAML::Load(YAML::Dump(legacyEncode(authored)));
            if (i % 2) node = withoutKeys(node, random);

            const T expected = legacyDecode<T>(legacy, node, &base);
            T actual = base;
            reflect::DecodeReport report;
            reflect::DecodeContext ctx;
            ctx.old = &base;
            reflect::decode(desc, node, &actual, ctx, report);
            owned.keep(&expected, type);
            owned.keep(&actual, type);

            std::string difference;
            ASSERT_TRUE(sameSavedValues(desc.body, &expected, &actual, difference))
                << desc.cppName << " sample " << i << ": " << difference << " differs, decoding\n" << YAML::Dump(node);
            ASSERT_EQ(YAML::Dump(legacyEncode(actual)), YAML::Dump(legacyEncode(expected))) << desc.cppName << " sample " << i;
        }
    }


#define PARITY(Name, Type, Encode, Decode) \
    TEST(ReflectParity, Name##EncodeMatchesLegacy) { \
        encodeMatches<Type>(ComponentType::Type, &LegacyCodecs::Encode, 1000u * static_cast<uint32_t>(ComponentType::Type)); \
    } \
    TEST(ReflectParity, Name##DecodeMatchesLegacy) { \
        decodeMatches<Type>(ComponentType::Type, &LegacyCodecs::Encode, &LegacyCodecs::Decode, \
                            7919u * static_cast<uint32_t>(ComponentType::Type) + 17u); \
    }

#define PARITY_COMPONENT(Name) PARITY(Name, Name##Component, encode##Name##Component, decode##Name##Component)

PARITY_COMPONENT(Light)
PARITY_COMPONENT(Light2D)
PARITY_COMPONENT(ShadowCaster2D)
PARITY_COMPONENT(Fog)
PARITY_COMPONENT(LocalVolumetricFog)
PARITY_COMPONENT(VolumetricClouds)
PARITY_COMPONENT(Mirror)
PARITY_COMPONENT(ReflectionProbe)
PARITY_COMPONENT(UIDocument)
PARITY_COMPONENT(Camera)
PARITY_COMPONENT(AudioSource)
PARITY_COMPONENT(Button)
PARITY_COMPONENT(Scrollbar)
PARITY_COMPONENT(Progressbar)
PARITY_COMPONENT(InputField)
PARITY_COMPONENT(Panel)
PARITY_COMPONENT(RectTransform)
PARITY_COMPONENT(LayoutGroup)
PARITY_COMPONENT(Text)
PARITY_COMPONENT(Image)
PARITY(Transform, Transform, encodeTransform, decodeTransform)
PARITY_COMPONENT(Joint2D)
PARITY_COMPONENT(Joint3D)
PARITY_COMPONENT(Action)
PARITY_COMPONENT(Tween)
PARITY_COMPONENT(PositionTween)
PARITY_COMPONENT(RotationTween)
PARITY_COMPONENT(ScaleTween)
PARITY_COMPONENT(ColorTween)
PARITY_COMPONENT(AlphaTween)
PARITY_COMPONENT(Animation)
PARITY_COMPONENT(Bone)

// PostProcessingSettings (R15): the scene's post-processing block written and read through its
// reflection fields gives the text and values of the hand-written codec
TEST(ReflectParity, PostProcessingMatchesLegacy) {
    const reflect::StructDesc& desc = reflect::postProcessingStruct();
    ASSERT_EQ(desc.fields.size(), PostProcessingSettings::getFields().size());
    EXPECT_LT(PostProcessingSettings::getFields().size(), reflect::postProcessingFieldCapacity());
    for (int i = 0; i < kSamples; i++) {
        RandomValues random(424242u + static_cast<uint32_t>(i));
        PostProcessingSettings settings;
        random.fillStruct(desc, &settings);
        const std::string expected = YAML::Dump(LegacyCodecs::encodePostProcessing(settings));
        ASSERT_EQ(YAML::Dump(Stream::encodePostProcessing(settings)), expected) << "sample " << i;

        YAML::Node node = YAML::Load(expected);
        if (i % 2 && node.IsMap()) node = withoutKeys(node, random);
        const PostProcessingSettings legacy = LegacyCodecs::decodePostProcessing(node);
        const PostProcessingSettings decoded = Stream::decodePostProcessing(node);
        ASSERT_EQ(YAML::Dump(LegacyCodecs::encodePostProcessing(decoded)), YAML::Dump(LegacyCodecs::encodePostProcessing(legacy)))
            << "sample " << i << ", decoding\n" << YAML::Dump(node);
    }
}

}
