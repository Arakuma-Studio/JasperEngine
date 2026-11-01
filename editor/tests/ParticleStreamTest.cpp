// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The scene encoding of Particle Systems (particles spec 6.7, 9): version 1 writes exactly the
// keys it wrote before version 2 existed (a re-encode of a decode is byte-identical); version
// 2 writes `version: 2` and only the non-default module fields, and round-trips every field of
// the field table; MinMaxCurve / MinMaxGradient forms; the exported build's code compiles.

#include <gtest/gtest.h>

#include "Stream.h"
#include "Factory.h"
#include "Catalog.h"
#include "Scene.h"
#include "component/Transform.h"
#include "particles/ParticleFields.h"

#include "yaml-cpp/yaml.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // a value of each field that differs from its default
    void setNonDefault(const particles::FieldInfo& field, ParticleSystemComponent& c, int salt) {
        void* value = field.get(c);
        switch (field.type) {
            case particles::FieldType::Bool: *static_cast<bool*>(value) = !*static_cast<bool*>(value); break;
            case particles::FieldType::Int: *static_cast<int*>(value) = 3 + salt; break;
            case particles::FieldType::UInt: *static_cast<unsigned int*>(value) = 77u + (unsigned int)salt; break;
            case particles::FieldType::Float: *static_cast<float*>(value) = 0.25f + 0.125f * (float)salt; break;
            case particles::FieldType::Vector2: *static_cast<Vector2*>(value) = Vector2(0.5f, 2.5f + salt); break;
            case particles::FieldType::Vector3: *static_cast<Vector3*>(value) = Vector3(0.5f, -1.5f, 2.0f + salt); break;
            case particles::FieldType::Curve:
                switch (salt % 3) {
                    case 0: *static_cast<MinMaxCurve*>(value) = MinMaxCurve::between(0.5f, 1.5f + salt); break;
                    case 1: *static_cast<MinMaxCurve*>(value) = MinMaxCurve::fromCurve(AnimationCurve::easeInOut(0.0f, 0.25f, 1.0f, 2.0f), 1.5f); break;
                    default: *static_cast<MinMaxCurve*>(value) = MinMaxCurve::between(AnimationCurve::constant(0.5f), AnimationCurve::linear(0.0f, 1.0f, 1.0f, 3.0f), 2.0f); break;
                }
                break;
            case particles::FieldType::Gradient:
                switch (salt % 4) {
                    case 0: *static_cast<MinMaxGradient*>(value) = MinMaxGradient::fromColor(Vector4(0.25f, 0.5f, 1.0f, 0.75f)); break;
                    case 1: *static_cast<MinMaxGradient*>(value) = MinMaxGradient::between(Vector4(1, 0, 0, 1), Vector4(0, 0.2f, 0.4f, 0.5f)); break;
                    case 2: *static_cast<MinMaxGradient*>(value) = MinMaxGradient::between(Gradient::twoColors(Vector4(1, 0, 0, 1), Vector4(0, 1, 0, 0)), Gradient()); break;
                    default: {
                        MinMaxGradient g = MinMaxGradient::fromGradient(Gradient::twoColors(Vector4(0.1f, 0.2f, 0.3f, 1), Vector4(1, 1, 1, 0)));
                        g.mode = ParticleSystemGradientMode::RandomColor;
                        *static_cast<MinMaxGradient*>(value) = g;
                    }
                }
                break;
            case particles::FieldType::Enum:
                *static_cast<int*>(value) = (int)field.enumNames->size() - 1 == *static_cast<int*>(value) ? 0 : (int)field.enumNames->size() - 1;
                break;
            case particles::FieldType::Bursts: {
                ParticleEmissionBurst a, b;
                a.time = 0.5f;
                a.count = MinMaxCurve::between(3.0f, 8.0f);
                a.cycles = 0;
                a.interval = 0.25f;
                a.probability = 0.5f;
                b.time = 2.0f;
                *static_cast<std::vector<ParticleEmissionBurst>*>(value) = {a, b};
                break;
            }
            case particles::FieldType::IntList: *static_cast<std::vector<int>*>(value) = {3, 1, 4}; break;
            case particles::FieldType::String: *static_cast<std::string*>(value) = "cube"; break;
        }
    }

    bool sameField(const particles::FieldInfo& field, const ParticleSystemComponent& a, const ParticleSystemComponent& b) {
        ParticleSystemComponent copy = b;
        particles::copyField(field, a, copy);
        // copying a's value over b's leaves b's encoding unchanged when they are equal
        return YAML::Dump(Stream::encodeParticleSystemComponent(copy)) == YAML::Dump(Stream::encodeParticleSystemComponent(b));
    }

}

TEST(ParticleStream, VersionOneIsByteIdentical) {
    const YAML::Node presets = YAML::LoadFile((std::filesystem::path(JASPER_EDITOR_TEST_FIXTURES) / "particles-legacy" / "presets.yaml").string());
    for (const auto& item : presets) {
        SCOPED_TRACE(item.first.as<std::string>());
        const ParticleSystemComponent decoded = Stream::decodeParticleSystemComponent(item.second);
        EXPECT_EQ(decoded.version, 1);
        const std::string once = YAML::Dump(Stream::encodeParticleSystemComponent(decoded));
        const std::string twice = YAML::Dump(Stream::encodeParticleSystemComponent(Stream::decodeParticleSystemComponent(YAML::Load(once))));
        EXPECT_EQ(once, twice);
        // no key of version 2 in a version 1 file
        const YAML::Node node = YAML::Load(once);
        EXPECT_FALSE(node["version"]);
        EXPECT_FALSE(node["main"]);
        EXPECT_FALSE(node["useUnscaledTime"]);
        EXPECT_TRUE(node["lifeInitializer"]);
    }
    // the base's key set, in order (the legacy encode is untouched)
    const YAML::Node node = Stream::encodeParticleSystemComponent(ParticleSystemComponent());
    std::vector<std::string> keys;
    for (const auto& item : node) keys.push_back(item.first.as<std::string>());
    const std::vector<std::string> expected = {"enabled", "maxParticles", "emitter", "loop", "localSpace", "rate", "maxPerUpdate", "bursts",
        "lifeInitializer", "positionInitializer", "positionModifier", "velocityInitializer", "velocityModifier",
        "accelerationInitializer", "accelerationModifier", "colorInitializer", "colorModifier", "colorGradient",
        "alphaInitializer", "alphaModifier", "sizeInitializer", "sizeModifier", "spriteInitializer", "spriteModifier",
        "rotationInitializer", "rotationModifier", "scaleInitializer", "scaleModifier"};
    EXPECT_EQ(keys, expected);
}

TEST(ParticleStream, VersionTwoWritesOnlyWhatDiffers) {
    ParticleSystemComponent ps;
    ps.version = 2;
    ps.main.startLifetime = MinMaxCurve::between(0.8f, 1.0f);
    const YAML::Node node = Stream::encodeParticleSystemComponent(ps);
    EXPECT_EQ(node["version"].as<int>(), 2);
    ASSERT_TRUE(node["main"]);
    EXPECT_EQ(node["main"].size(), 1u);
    EXPECT_EQ(node["main"]["startLifetime"]["mode"].as<std::string>(), "two_constants");
    EXPECT_FLOAT_EQ(node["main"]["startLifetime"]["min"].as<float>(), 0.8f);
    EXPECT_FALSE(node["emission"]) << "an all-default module is not written";
    EXPECT_FALSE(node["lifeInitializer"]) << "no legacy keys";
    // a constant is a bare number
    ps.main.startSpeed = MinMaxCurve(2.5f);
    EXPECT_FLOAT_EQ(Stream::encodeParticleSystemComponent(ps)["main"]["startSpeed"].as<float>(), 2.5f);
}

TEST(ParticleStream, VersionTwoRoundTripsEveryField) {
    int salt = 0;
    for (const particles::FieldInfo& field : particles::fields()) {
        SCOPED_TRACE(field.path());
        ParticleSystemComponent ps;
        ps.version = 2;
        setNonDefault(field, ps, salt++);
        ASSERT_FALSE(particles::isDefault(field, ps));
        const YAML::Node node = Stream::encodeParticleSystemComponent(ps);
        ASSERT_TRUE(node[field.module]) << YAML::Dump(node);
        ASSERT_TRUE(node[field.module][field.name]) << YAML::Dump(node);
        const ParticleSystemComponent back = Stream::decodeParticleSystemComponent(YAML::Load(YAML::Dump(node)));
        EXPECT_EQ(back.version, 2);
        EXPECT_FALSE(particles::isDefault(field, back));
        EXPECT_TRUE(sameField(field, ps, back)) << YAML::Dump(node);
    }
}

TEST(ParticleStream, MinMaxForms) {
    // a curve given as a number, two constants, a curve
    EXPECT_FLOAT_EQ(Stream::decodeMinMaxCurve(YAML::Load("3.5")).evaluate(0, 0), 3.5f);
    const MinMaxCurve two = Stream::decodeMinMaxCurve(YAML::Load("{mode: two_constants, min: 1, max: 4}"));
    EXPECT_EQ(two.mode, ParticleSystemCurveMode::TwoConstants);
    EXPECT_FLOAT_EQ(two.evaluate(0, 0.5f), 2.5f);
    const MinMaxCurve curve = Stream::decodeMinMaxCurve(YAML::Load("{mode: curve, multiplier: 2, curve: {keys: [[0, 0], [1, 1]]}}"));
    EXPECT_EQ(curve.mode, ParticleSystemCurveMode::Curve);
    EXPECT_NEAR(curve.evaluate(0.5f, 0), 1.0f, 1e-4f);
    // a gradient given as one colour (sRGB hex with alpha)
    const MinMaxGradient color = Stream::decodeMinMaxGradient(YAML::Load("'#FF000080'"));
    EXPECT_EQ(color.mode, ParticleSystemGradientMode::Color);
    EXPECT_NEAR(color.color.x, 1.0f, 1e-6f);
    EXPECT_NEAR(color.color.w, 128.0f / 255.0f, 1e-6f);
    EXPECT_EQ(Stream::encodeMinMaxGradient(color).as<std::string>(), "#FF000080");
}

// The exported build's code for a version 2 system compiles against the engine headers
TEST(ParticleStream, FactoryCodeCompiles) {
#if defined(JASPER_EDITOR_TEST_ENGINE_FLAGS) && defined(JASPER_EDITOR_TEST_CXX)
    std::ifstream flagsFile(JASPER_EDITOR_TEST_ENGINE_FLAGS);
    if (!flagsFile) {
        GTEST_SKIP() << "no engine flags file " << JASPER_EDITOR_TEST_ENGINE_FLAGS;
    }
    std::string flags, line;
    bool defines = false;
    while (std::getline(flagsFile, line)) {
        if (line.empty()) continue;
        if (line == "#defines") { defines = true; continue; }
        flags += (defines ? " -D'" : " -isystem '") + line + "'";
    }

    Scene scene;
    const Entity entity = scene.createUserEntity();
    scene.addComponent<Transform>(entity, {});
    ParticleSystemComponent ps;
    ps.version = 2;
    int salt = 0;
    for (const particles::FieldInfo& field : particles::fields()) setNonDefault(field, ps, salt++);
    scene.addComponent<ParticleSystemComponent>(entity, ps);
    const std::string code = Factory::createComponent(4, &scene, entity, ComponentType::ParticleSystemComponent, "", "scene", "entity");
    EXPECT_NE(code.find("particles.version = 2;"), std::string::npos);
    EXPECT_NE(code.find("particles.main.startLifetime"), std::string::npos);
    EXPECT_EQ(code.find("lifeInitializer"), std::string::npos) << "no legacy fields";

    std::ostringstream source;
    source << "#include \"Scene.h\"\n#include \"component/ParticleSystemComponent.h\"\nusing namespace jasperengine;\n"
           << "void generatedParticles(Scene* scene, Entity entity){\n{\n" << code << "}\n}\n";
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("jasper-particles-factory-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / "generated.cpp";
    std::ofstream(file) << source.str();
    const std::string command = std::string("'") + JASPER_EDITOR_TEST_CXX + "' -std=gnu++17 -fsyntax-only" + flags +
                                " '" + file.string() + "' > '" + (dir / "log.txt").string() + "' 2>&1";
    const int status = std::system(command.c_str());
    std::ifstream log(dir / "log.txt");
    const std::string output((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
    EXPECT_EQ(status, 0) << output << "\n" << source.str();
    std::filesystem::remove_all(dir);
#else
    GTEST_SKIP() << "built without the engine flags";
#endif
}
