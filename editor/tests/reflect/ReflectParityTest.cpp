// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Every component the reflection codec writes encodes and decodes as its hand-written codec did
// (docs/specs/S08-reflection.md AC3, AC4): for 500 seeded random instances (NaN, infinities and
// -0 included) the YAML text is the same byte for byte, and decoding that text over a random base
// (every other time with a random subset of its keys) gives the same values, floats compared
// bitwise. The hand-written codecs are kept verbatim in editor/tests/reflect/legacy.
//
// editor/tests/reflect/parity_golden freezes what those codecs give for these samples: the first
// samples in full, and a digest of all of them.

#include "reflect/ReflectTestSupport.h"
#include "reflect/legacy/LegacyCodecs.h"

#include "reflect/Reflect.h"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

// (a named namespace, tests included: a unity build merges the test sources)
namespace reflect_parity_test {

    constexpr int kSamples = 500;
    // the samples a golden file keeps in full (the rest, in its digest only)
    constexpr int kFullSamples = 8;

    // One golden file: "digest <hex>", then "--- <sample>" before each sample kept in full.
    // JASPER_TEST_UPDATE_GOLDEN=1 writes it from the samples checked.
    class FrozenParity {
    public:
        explicit FrozenParity(const std::string& name):
            path(std::filesystem::path(JASPER_TEST_REPO_DIR) / "editor" / "tests" / "reflect" / "parity_golden" / (name + ".yaml")),
            updating(std::getenv("JASPER_TEST_UPDATE_GOLDEN") != nullptr) {
            if (!updating) read();
        }

        // what the samples were made from: in the digest only
        void note(const std::string& text) {
            for (const char c : text) digest = (digest ^ static_cast<unsigned char>(c)) * 1099511628211ull;
            digest = (digest ^ 0xffu) * 1099511628211ull;
        }

        // false (and a failure with the difference) when a sample kept in full is not the text
        bool check(int sample, const std::string& text) {
            note(text);
            if (sample >= kFullSamples) return true;
            if (updating) {
                samples[sample] = text;
                return true;
            }
            const auto kept = samples.find(sample);
            if (kept == samples.end()) {
                ADD_FAILURE() << path << " has no sample " << sample << " (JASPER_TEST_UPDATE_GOLDEN=1 writes it)";
                return false;
            }
            EXPECT_EQ(text, kept->second) << path << " sample " << sample;
            return text == kept->second;
        }

        // the digest of every sample, or the file written
        void finish() {
            char hex[32];
            std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(digest));
            if (updating) {
                std::filesystem::create_directories(path.parent_path());
                std::ofstream out(path, std::ios::binary);
                out << "# Frozen by ReflectParity (editor/tests/reflect/ReflectParityTest.cpp); JASPER_TEST_UPDATE_GOLDEN=1 rewrites it.\n";
                out << "digest " << hex << "\n";
                for (const auto& [sample, text] : samples) out << "--- " << sample << "\n" << text << "\n";
                return;
            }
            EXPECT_EQ(hex, frozenDigest) << path << ": a sample differs from the frozen data";
        }

    private:
        void read() {
            std::ifstream in(path, std::ios::binary);
            ASSERT_TRUE(in.good()) << path << " is missing (JASPER_TEST_UPDATE_GOLDEN=1 writes it)";
            std::string line;
            std::string* current = nullptr;
            while (std::getline(in, line)) {
                // (a checkout that writes CRLF: no sample holds a carriage return of its own, YAML escapes it)
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.rfind("digest ", 0) == 0 && !current) {
                    frozenDigest = line.substr(7);
                } else if (line.rfind("--- ", 0) == 0) {
                    current = &samples[std::atoi(line.c_str() + 4)];
                } else if (current) {
                    if (!current->empty()) *current += "\n";
                    *current += line;
                }
            }
        }

        std::filesystem::path path;
        bool updating;
        std::map<int, std::string> samples;
        std::string frozenDigest;
        uint64_t digest = 14695981039346656037ull;
    };

    YAML::Node withoutKeys(const YAML::Node& map, RandomValues& random) {
        YAML::Node copy(YAML::NodeType::Map);
        if (!map.IsMap()) return map;
        for (auto it = map.begin(); it != map.end(); ++it) {
            if (random.chance(35)) continue;
            copy[it->first.Scalar()] = it->second;
        }
        return copy;
    }

    // The hand-written decode as S03 ran it (StreamLoad::decodeComponent): each key whose value
    // threw is left out (S03 finds them one by one from the error's position; here, the keys that
    // throw on their own) and the decode runs again
    template<typename T, typename Decode>
    T legacyDecode(Decode decode, YAML::Node node, const T* base) {
        try {
            return decode(node, base);
        } catch (const YAML::Exception&) {
        }
        YAML::Node kept(YAML::NodeType::Map);
        for (auto it = node.begin(); it != node.end(); ++it) {
            YAML::Node single(YAML::NodeType::Map);
            single[it->first.Scalar()] = it->second;
            try {
                (void)decode(single, base);
                kept[it->first.Scalar()] = it->second;
            } catch (const YAML::Exception&) {
            }
        }
        try {
            return decode(kept, base);
        } catch (const YAML::Exception&) {
        }
        return decode(YAML::Node(YAML::NodeType::Map), base);
    }

    template<typename T>
    void encodeMatches(ComponentType type, YAML::Node (*legacy)(const T&), uint32_t seed, const std::string& name) {
        const reflect::ComponentDesc& desc = *reflect::find(type);
        ASSERT_TRUE(desc.codec == nullptr) << desc.cppName << " has its own codec";
        FrozenParity frozen(name + ".encode");
        OwnedFramebuffers owned;
        for (int i = 0; i < kSamples; i++) {
            RandomValues random(seed + static_cast<uint32_t>(i));
            T comp{};
            owned.keep(&comp);
            random.fillStruct(desc.body, &comp);
            const std::string expected = YAML::Dump(legacy(comp));
            const std::string actual = YAML::Dump(reflect::encode(desc, &comp, reflect::EncodeContext{}));
            ASSERT_EQ(actual, expected) << desc.cppName << " sample " << i;
            ASSERT_TRUE(frozen.check(i, expected)) << desc.cppName;
        }
        frozen.finish();
    }

    template<typename T>
    void decodeMatches(ComponentType type, YAML::Node (*legacyEncode)(const T&), T (*legacy)(const YAML::Node&, const T*), uint32_t seed, const std::string& name) {
        const reflect::ComponentDesc& desc = *reflect::find(type);
        FrozenParity frozen(name + ".decode");
        OwnedFramebuffers owned;
        for (int i = 0; i < kSamples; i++) {
            RandomValues random(seed + static_cast<uint32_t>(i));
            T authored{};
            T base{};
            owned.keep(&authored);
            owned.keep(&base);
            random.fillStruct(desc.body, &authored);
            random.fillStruct(desc.body, &base);
            const std::string authoredText = YAML::Dump(legacyEncode(authored));
            ASSERT_EQ(YAML::Dump(reflect::encode(desc, &authored, reflect::EncodeContext{})), authoredText) << desc.cppName << " sample " << i;
            frozen.note(authoredText);
            YAML::Node node = YAML::Load(authoredText);
            if (i % 2) node = withoutKeys(node, random);

            const T expected = legacyDecode<T>(legacy, node, &base);
            T actual = base;
            reflect::DecodeReport report;
            reflect::DecodeContext ctx;
            ctx.old = &base;
            reflect::decode(desc, node, &actual, ctx, report);
            owned.keep(&expected);
            owned.keep(&actual);

            std::string difference;
            ASSERT_TRUE(sameSavedValues(desc.body, &expected, &actual, difference))
                << desc.cppName << " sample " << i << ": " << difference << " differs, decoding\n" << YAML::Dump(node);
            const std::string expectedText = YAML::Dump(legacyEncode(expected));
            ASSERT_EQ(YAML::Dump(legacyEncode(actual)), expectedText) << desc.cppName << " sample " << i;
            ASSERT_EQ(YAML::Dump(reflect::encode(desc, &actual, reflect::EncodeContext{})), expectedText) << desc.cppName << " sample " << i;
            ASSERT_TRUE(frozen.check(i, expectedText)) << desc.cppName;
        }
        frozen.finish();
    }


#define PARITY(Name, Type, Encode, Decode) \
    TEST(ReflectParity, Name##EncodeMatchesLegacy) { \
        encodeMatches<Type>(ComponentType::Type, &LegacyCodecs::Encode, 1000u * static_cast<uint32_t>(ComponentType::Type), #Name); \
    } \
    TEST(ReflectParity, Name##DecodeMatchesLegacy) { \
        decodeMatches<Type>(ComponentType::Type, &LegacyCodecs::Encode, &LegacyCodecs::Decode, \
                            7919u * static_cast<uint32_t>(ComponentType::Type) + 17u, #Name); \
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
PARITY_COMPONENT(FluidSimulation)
PARITY_COMPONENT(LatticeFollower)

// the fluid's back colour, written by the codec it replaced only away from its default and finite
TEST(ReflectParity, FluidBackColorAwayFromItsDefault) {
    FluidSimulationComponent fluid;
    fluid.backColor = Vector3(0.0f, 9845.456f, 7931.2134f);
    const YAML::Node legacy = LegacyCodecs::encodeFluidSimulationComponent(fluid);
    EXPECT_TRUE(legacy["backColor"].IsDefined()) << YAML::Dump(legacy);
    EXPECT_EQ(YAML::Dump(reflect::encodeValue(ComponentType::FluidSimulationComponent, &fluid)), YAML::Dump(legacy));
    FrozenParity frozen("FluidBackColor.encode");
    EXPECT_TRUE(frozen.check(0, YAML::Dump(legacy)));
    frozen.finish();
    fluid.backColor.x = -INFINITY;
    EXPECT_FALSE(reflect::encodeValue(ComponentType::FluidSimulationComponent, &fluid)["backColor"].IsDefined());
}
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
// a document (a scene or prefab file) leaves out the derived world values (S29 R20); every other
// encode is the legacy text
TEST(ReflectParity, TransformDocumentEncodeLeavesOutDerivedValues) {
    Transform transform;
    transform.position = Vector3(1, 2, 3);
    transform.worldPosition = Vector3(4, 5, 6);
    YAML::Node legacy = LegacyCodecs::encodeTransform(transform);
    EXPECT_EQ(YAML::Dump(reflect::encodeValue(ComponentType::Transform, &transform)), YAML::Dump(legacy));
    FrozenParity frozen("TransformWhole.encode");
    EXPECT_TRUE(frozen.check(0, YAML::Dump(legacy)));
    frozen.finish();
    YAML::Node written;
    {
        reflect::DocumentEncodeScope document;
        written = reflect::encodeValue(ComponentType::Transform, &transform);
    }
    for (const char* key : {"worldPosition", "worldRotation", "worldScale", "localMatrix", "modelMatrix", "normalMatrix"}) {
        EXPECT_FALSE(written[key].IsDefined()) << key;
        legacy.remove(key);
    }
    EXPECT_EQ(YAML::Dump(written), YAML::Dump(legacy));
}
// (Joint2D and Joint3D: upstream's joint codecs since SYNC#4, a key a file leaves out decoding to
// the legacy joint's value, editor/reflect/components/TransformPhysicsReflect.cpp kJoint2DCodec /
// kJoint3DCodec; their round trips: CookedScene, ReflectFixtures.AllComponentsScene)
PARITY_COMPONENT(Action)
PARITY_COMPONENT(Tween)
PARITY_COMPONENT(PositionTween)
PARITY_COMPONENT(RotationTween)
PARITY_COMPONENT(ScaleTween)
PARITY_COMPONENT(ColorTween)
PARITY_COMPONENT(AlphaTween)
PARITY_COMPONENT(Animation)
PARITY_COMPONENT(Bone)
PARITY_COMPONENT(KeyframeTracks)
PARITY_COMPONENT(TranslateTracks)
PARITY_COMPONENT(RotateTracks)
PARITY_COMPONENT(ScaleTracks)

// PostProcessingSettings (R15): the scene's post-processing block written and read through its
// reflection fields gives the text and values of the hand-written codec
TEST(ReflectParity, PostProcessingMatchesLegacy) {
    const reflect::StructDesc& desc = reflect::postProcessingStruct();
    ASSERT_EQ(desc.fields.size(), PostProcessingSettings::getFields().size());
    EXPECT_LT(PostProcessingSettings::getFields().size(), reflect::postProcessingFieldCapacity());
    FrozenParity encoded("PostProcessing.encode");
    FrozenParity decodedFrozen("PostProcessing.decode");
    for (int i = 0; i < kSamples; i++) {
        RandomValues random(424242u + static_cast<uint32_t>(i));
        PostProcessingSettings settings;
        random.fillStruct(desc, &settings);
        const std::string expected = YAML::Dump(LegacyCodecs::encodePostProcessing(settings));
        ASSERT_EQ(YAML::Dump(Stream::encodePostProcessing(settings)), expected) << "sample " << i;
        ASSERT_TRUE(encoded.check(i, expected));

        YAML::Node node = YAML::Load(expected);
        if (i % 2 && node.IsMap()) node = withoutKeys(node, random);
        const PostProcessingSettings legacy = LegacyCodecs::decodePostProcessing(node);
        const PostProcessingSettings decoded = Stream::decodePostProcessing(node);
        ASSERT_EQ(YAML::Dump(LegacyCodecs::encodePostProcessing(decoded)), YAML::Dump(LegacyCodecs::encodePostProcessing(legacy)))
            << "sample " << i << ", decoding\n" << YAML::Dump(node);
        ASSERT_EQ(YAML::Dump(Stream::encodePostProcessing(decoded)), YAML::Dump(LegacyCodecs::encodePostProcessing(legacy))) << "sample " << i;
        ASSERT_TRUE(decodedFrozen.check(i, YAML::Dump(LegacyCodecs::encodePostProcessing(legacy))));
    }
    encoded.finish();
    decodedFrozen.finish();
}

}
