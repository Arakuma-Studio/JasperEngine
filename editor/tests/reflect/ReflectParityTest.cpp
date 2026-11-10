// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Every component the reflection codec writes encodes and decodes as its hand-written codec did
// (docs/specs/S08-reflection.md AC3, AC4): for 500 seeded random instances (NaN, infinities and
// -0 included) the YAML text, and the text of what decoding it over a random base gives (every
// other time with a random subset of its keys), is the frozen data in
// editor/tests/reflect/parity_golden. The hand-written codecs wrote that data and are gone
// (docs/specs/S73-code-cleanups.md R2). A field added to a table adds its key there:
// JASPER_TEST_UPDATE_GOLDEN=1 rewrites the files, and no other line of a sample moves.

#include "reflect/ReflectTestSupport.h"
#include "JasperEngine.h"
#include "Stream.h"
#include "spritenormals/SpriteNormalsField.h"
#include "component/ActionComponent.h"
#include "component/AlphaTweenComponent.h"
#include "component/AnimationComponent.h"
#include "component/BoneComponent.h"
#include "component/ButtonComponent.h"
#include "component/CameraComponent.h"
#include "component/ColorTweenComponent.h"
#include "component/FluidSimulationComponent.h"
#include "component/FogComponent.h"
#include "component/ImageComponent.h"
#include "component/InputFieldComponent.h"
#include "component/Joint2DComponent.h"
#include "component/Joint3DComponent.h"
#include "component/KeyframeTracksComponent.h"
#include "component/LatticeFollowerComponent.h"
#include "component/LayoutGroupComponent.h"
#include "component/Light2DComponent.h"
#include "component/LocalVolumetricFogComponent.h"
#include "component/MirrorComponent.h"
#include "component/PanelComponent.h"
#include "component/PositionTweenComponent.h"
#include "component/ProgressbarComponent.h"
#include "component/RectTransformComponent.h"
#include "component/ReflectionProbeComponent.h"
#include "component/RotateTracksComponent.h"
#include "component/RotationTweenComponent.h"
#include "component/ScaleTracksComponent.h"
#include "component/ScaleTweenComponent.h"
#include "component/ScrollbarComponent.h"
#include "component/ShadowCaster2DComponent.h"
#include "component/TextComponent.h"
#include "component/Transform.h"
#include "component/TranslateTracksComponent.h"
#include "component/TweenComponent.h"
#include "component/UIDocumentComponent.h"
#include "component/VolumetricCloudsComponent.h"
#include "particles/ParticleModules.h"

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

    template<typename T>
    void encodeMatches(ComponentType type, uint32_t seed, const std::string& name) {
        const reflect::ComponentDesc& desc = *reflect::find(type);
        ASSERT_TRUE(desc.codec == nullptr) << desc.cppName << " has its own codec";
        FrozenParity frozen(name + ".encode");
        OwnedFramebuffers owned;
        for (int i = 0; i < kSamples; i++) {
            RandomValues random(seed + static_cast<uint32_t>(i));
            T comp{};
            owned.keep(&comp);
            random.fillStruct(desc.body, &comp);
            ASSERT_TRUE(frozen.check(i, YAML::Dump(reflect::encode(desc, &comp, reflect::EncodeContext{})))) << desc.cppName;
        }
        frozen.finish();
    }

    template<typename T>
    void decodeMatches(ComponentType type, uint32_t seed, const std::string& name) {
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
            const std::string authoredText = YAML::Dump(reflect::encode(desc, &authored, reflect::EncodeContext{}));
            frozen.note(authoredText);
            YAML::Node node = YAML::Load(authoredText);
            if (i % 2) node = withoutKeys(node, random);

            T actual = base;
            reflect::DecodeReport report;
            reflect::DecodeContext ctx;
            ctx.old = &base;
            reflect::decode(desc, node, &actual, ctx, report);
            owned.keep(&actual);
            ASSERT_TRUE(frozen.check(i, YAML::Dump(reflect::encode(desc, &actual, reflect::EncodeContext{}))))
                << desc.cppName << ", decoding\n" << YAML::Dump(node);
        }
        frozen.finish();
    }


    // The samples hold denormals: they compare against the frozen text only while the main thread
    // keeps them, which SoLoud's mix now leaves as it found (engine/libs/soloud/CMakeLists.txt)
    class ReflectParity : public ::testing::Test {};

#define PARITY(Name, Type) \
    TEST_F(ReflectParity, Name##EncodeMatchesFrozen) { \
        encodeMatches<Type>(ComponentType::Type, 1000u * static_cast<uint32_t>(ComponentType::Type), #Name); \
    } \
    TEST_F(ReflectParity, Name##DecodeMatchesFrozen) { \
        decodeMatches<Type>(ComponentType::Type, 7919u * static_cast<uint32_t>(ComponentType::Type) + 17u, #Name); \
    }

#define PARITY_COMPONENT(Name) PARITY(Name, Name##Component)

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
TEST_F(ReflectParity, FluidBackColorAwayFromItsDefault) {
    FluidSimulationComponent fluid;
    fluid.backColor = Vector3(0.0f, 9845.456f, 7931.2134f);
    const YAML::Node encoded = reflect::encodeValue(ComponentType::FluidSimulationComponent, &fluid);
    EXPECT_TRUE(encoded["backColor"].IsDefined()) << YAML::Dump(encoded);
    FrozenParity frozen("FluidBackColor.encode");
    EXPECT_TRUE(frozen.check(0, YAML::Dump(encoded)));
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
PARITY(Transform, Transform)
// a document (a scene or prefab file) leaves out the derived world values (S29 R20); every other
// encode writes them
TEST_F(ReflectParity, TransformDocumentEncodeLeavesOutDerivedValues) {
    Transform transform;
    transform.position = Vector3(1, 2, 3);
    transform.worldPosition = Vector3(4, 5, 6);
    YAML::Node whole = reflect::encodeValue(ComponentType::Transform, &transform);
    FrozenParity frozen("TransformWhole.encode");
    EXPECT_TRUE(frozen.check(0, YAML::Dump(whole)));
    frozen.finish();
    YAML::Node written;
    {
        reflect::DocumentEncodeScope document;
        written = reflect::encodeValue(ComponentType::Transform, &transform);
    }
    for (const char* key : {"worldPosition", "worldRotation", "worldScale", "localMatrix", "modelMatrix", "normalMatrix"}) {
        EXPECT_FALSE(written[key].IsDefined()) << key;
        whole.remove(key);
    }
    EXPECT_EQ(YAML::Dump(written), YAML::Dump(whole));
}
// (Joint2D and Joint3D: upstream's joint codecs since SYNC#4, a key a file leaves out decoding to
// the old joint's value, editor/reflect/components/TransformPhysicsReflect.cpp kJoint2DCodec /
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
// reflection fields gives the text and values the hand-written codec gave
TEST_F(ReflectParity, PostProcessingMatchesFrozen) {
    const reflect::StructDesc& desc = reflect::postProcessingStruct();
    ASSERT_EQ(desc.fields.size(), PostProcessingSettings::getFields().size());
    EXPECT_LT(PostProcessingSettings::getFields().size(), reflect::postProcessingFieldCapacity());
    FrozenParity encoded("PostProcessing.encode");
    FrozenParity decodedFrozen("PostProcessing.decode");
    for (int i = 0; i < kSamples; i++) {
        RandomValues random(424242u + static_cast<uint32_t>(i));
        PostProcessingSettings settings;
        random.fillStruct(desc, &settings);
        const std::string text = YAML::Dump(Stream::encodePostProcessing(settings));
        ASSERT_TRUE(encoded.check(i, text));

        YAML::Node node = YAML::Load(text);
        if (i % 2 && node.IsMap()) node = withoutKeys(node, random);
        const PostProcessingSettings decoded = Stream::decodePostProcessing(node);
        ASSERT_TRUE(decodedFrozen.check(i, YAML::Dump(Stream::encodePostProcessing(decoded)))) << "decoding\n" << YAML::Dump(node);
    }
    encoded.finish();
    decodedFrozen.finish();
}

}
