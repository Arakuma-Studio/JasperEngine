// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// tools/fixtures/reflection-parity/AllComponents.scene (docs/specs/S08-reflection.md AC5, AC6,
// AC11): one GameObject per component type, every Catalog property of each set away from its
// default. JASPER_REFLECT_WRITE_FIXTURE=1 writes it; it was written with the editor's codecs
// before S08, and the editor's re-save of it before S08 is the golden S08 must match. Otherwise
// the test checks it loads cleanly and holds every component type.

#include "support/EditorTestSupport.h"
#include "component/TextComponent.h"

#include "Catalog.h"
#include "Stream.h"
#include "mcp/McpValues.h"
#include "util/ProjectUtils.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <random>
#include <set>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

// (a named namespace, tests included: a unity build merges the test sources)
namespace all_components_fixture_test {

    fs::path fixturePath() {
        return fs::path(JASPER_TEST_REPO_DIR) / "tools" / "fixtures" / "reflection-parity" / "AllComponents.scene";
    }

    // Counts, sizes and indices the components' runtime trusts: left as they are
    bool kept(const std::string& name) {
        static const char* const names[] = {"numSubmeshes", "numColliders", "framesSize", "framesTimeSize", "numBoxes", "frameIndex",
            "frameTimeIndex", "spriteFrameCount", "reserveTiles", "maxPoints", "maxLines", "maxParticles", "maxPerUpdate", "maxInstances",
            "vertexCount", "levels", "rootGridSize", "resolution", "mapResolution", "numShadowCascades", "index", "model", "cursorIndex",
            "selectionAnchor", "containerBoxIndex", "textureWidth", "textureHeight", "framebufferWidth", "framebufferHeight", "fontSize",
            "maxTextSize", "minBufferCount", "minIndicesCount", "colorWrite", "sortPriority", "customShader", "customDepthShader",
            "shader", "length", "playingTime", "width", "height", "heightMap", "filename", "actionFrameCount", "numFramesRect", "numTilesRect",
            "numTiles", "numVertices", "numIndices", "resolutionX", "resolutionY", "resolutionZ", "simResolution", "dyeResolution",
            "bloomResolution", "sunraysResolution", "subdivisions", "seed"};
        const size_t dot = name.rfind('.');
        const std::string leaf = dot == std::string::npos ? name : name.substr(dot + 1);
        for (const char* item : names) {
            if (leaf == item) return true;
        }
        return name.find("stencil") != std::string::npos || name.find("extraPass") != std::string::npos;
    }

    void randomize(std::mt19937& rng, ComponentType type, const std::string& name, const PropertyData& property) {
        if (!property.ref || kept(name)) return;
        auto number = [&rng]() { return std::uniform_int_distribution<int>(1, 90)(rng) / 10.0f; };
        auto flip = [&rng]() { return std::uniform_int_distribution<int>(0, 1)(rng) == 1; };
        switch (property.type) {
            case PropertyType::Bool: *static_cast<bool*>(property.ref) = !*static_cast<bool*>(property.ref); break;
            case PropertyType::Int: *static_cast<int*>(property.ref) = std::uniform_int_distribution<int>(1, 7)(rng); break;
            case PropertyType::UInt: *static_cast<unsigned int*>(property.ref) = static_cast<unsigned int>(std::uniform_int_distribution<int>(1, 7)(rng)); break;
            case PropertyType::Float: *static_cast<float*>(property.ref) = number(); break;
            case PropertyType::Double: *static_cast<double*>(property.ref) = number(); break;
            case PropertyType::Vector2: *static_cast<Vector2*>(property.ref) = Vector2(number(), number()); break;
            case PropertyType::Vector3: *static_cast<Vector3*>(property.ref) = Vector3(number(), number(), number()); break;
            case PropertyType::Vector4: *static_cast<Vector4*>(property.ref) = Vector4(number() / 9.0f, number() / 9.0f, number() / 9.0f, 1.0f); break;
            case PropertyType::Quat: *static_cast<Quaternion*>(property.ref) = Quaternion(number() * 10.0f, Vector3(0, 1, 0)); break;
            case PropertyType::String: *static_cast<std::string*>(property.ref) = flip() ? "Reflection parity" : "text: [1, 2]"; break;
            case PropertyType::Texture: static_cast<Texture*>(property.ref)->setPath("textures/parity.png"); break;
            case PropertyType::Font: (*static_cast<FontArray*>(property.ref))[0] = "fonts/Parity.ttf"; break;
            case PropertyType::Ease: *static_cast<Ease*>(property.ref) = Ease(EaseType::CUBIC_IN_OUT); break;
            case PropertyType::Enum:
                if (const mcp::McpValues::EnumOptions* options = mcp::McpValues::enumOptions(type, name)) {
                    if (!options->empty()) *static_cast<int*>(property.ref) = options->back().value;
                }
                break;
            default:
                break;
        }
    }

    std::string generate() {
        std::mt19937 rng(2026);
        std::string text = "formatVersion: 1\nid: 2026080800\nname: AllComponents\nscene:\n  backgroundColor: [0, 0, 0, 1]\nsceneType: scene_3d\n"
                           "mainCamera: 0\nentities:\n";
        for (int t = 0; t <= static_cast<int>(LAST_COMPONENT_TYPE); t++) {
            text += "  - type: Entity\n    entity: " + std::to_string(1001 + t) + "\n    name: " + Catalog::getComponentName(static_cast<ComponentType>(t)) +
                    "\n    components:\n      transform:\n        position: [" + std::to_string(t) + ", 0, 0]\n";
        }
        DecodedScene decoded = decodeSceneText(text, "AllComponents.scene");
        Scene* scene = decoded.scene->scene;
        for (int t = 0; t <= static_cast<int>(LAST_COMPONENT_TYPE); t++) {
            const ComponentType type = static_cast<ComponentType>(t);
            const Entity entity = static_cast<Entity>(1001 + t);
            if (type != ComponentType::Transform) {
                ProjectUtils::addEntityComponent(scene, entity, type, decoded.scene->entities, YAML::Node());
            }
            for (const auto& [name, property] : Catalog::findEntityProperties(scene, entity, type)) {
                randomize(rng, type, name, property);
            }
        }
        return encodeSceneText(decoded);
    }


TEST(ReflectFixtures, AllComponentsScene) {
    if (std::getenv("JASPER_REFLECT_WRITE_FIXTURE")) {
        const std::string text = generate();
        std::ofstream out(fixturePath(), std::ios::binary);
        out << text;
    }
    ASSERT_TRUE(fs::exists(fixturePath()));
    DecodedScene decoded = decodeSceneFile(fixturePath());
    std::string issues;
    for (const LoadIssue& issue : decoded.report.issues) {
        if (issue.fate != LoadIssueFate::Info) issues += "\n  " + issue.consoleLine();
    }
    EXPECT_TRUE(issues.empty()) << issues;
    std::set<ComponentType> present;
    for (Entity entity : decoded.scene->entities) {
        for (ComponentType type : Catalog::findComponents(decoded.scene->scene, entity)) present.insert(type);
    }
    EXPECT_EQ(present.size(), static_cast<size_t>(LAST_COMPONENT_TYPE) + 1) << "a component type is missing from the fixture";
    // a second round trip is stable
    const std::string first = encodeSceneText(decoded);
    DecodedScene again = decodeSceneText(first, "AllComponents.scene");
    EXPECT_EQ(encodeSceneText(again), first);
}

}
