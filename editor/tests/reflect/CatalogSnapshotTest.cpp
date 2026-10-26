// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What Catalog says of every component type (docs/specs/S08-reflection.md AC1): its names, icon,
// enable checkbox and structural flags, and its properties (name, type, update flags, whether it
// has a default), for a default component, without one (list_component_types) and over every
// component of the fixture scenes (indices folded: "submeshes[N].material"); the YAML keys its
// decode knows, and the options MCP offers for each of its enum properties. The snapshot in
// editor/tests/reflect/catalog_snapshot.json was taken from the Catalog before reflection
// (JASPER_TEST_UPDATE_GOLDEN=1 rewrites it); the Catalog on reflection must say the same.

#include "support/EditorTestSupport.h"

#include "Catalog.h"
#include "StreamSchema.h"
#include "mcp/McpJson.h"
#include "mcp/McpValues.h"
#include "util/ProjectUtils.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <vector>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;
using Json = jasperengine::editor::mcp::Json;

// (a named namespace, tests included: a unity build merges the test sources)
namespace catalog_snapshot_test {

    fs::path snapshotPath() {
        return fs::path(JASPER_TEST_REPO_DIR) / "editor" / "tests" / "reflect" / "catalog_snapshot.json";
    }

    // "submeshes[3].material[1]" -> "submeshes[N].material[N]"
    std::string folded(const std::string& name) {
        std::string out;
        for (size_t i = 0; i < name.size(); i++) {
            if (name[i] == '[' && i + 1 < name.size() && std::isdigit(static_cast<unsigned char>(name[i + 1]))) {
                out += "[N";
                i++;
                while (i < name.size() && std::isdigit(static_cast<unsigned char>(name[i]))) i++;
                i--;
                continue;
            }
            out += name[i];
        }
        return out;
    }

    Json describe(const PropertyData& property) {
        return Json::array({static_cast<int>(property.type), property.updateFlags, property.def != nullptr});
    }

    Json properties(const std::map<std::string, PropertyData>& map) {
        Json out = Json::object();
        for (const auto& [name, property] : map) out[name] = describe(property);
        return out;
    }

    std::string hex(const char* text) {
        static const char* digits = "0123456789abcdef";
        std::string out;
        for (const unsigned char* c = reinterpret_cast<const unsigned char*>(text); c && *c; c++) {
            out += digits[*c >> 4];
            out += digits[*c & 15];
        }
        return out;
    }

    Json snapshot() {
        Json components = Json::array();
        Scene scene;
        std::vector<Entity> entities;
        for (int t = 0; t <= static_cast<int>(LAST_COMPONENT_TYPE); t++) {
            const ComponentType type = static_cast<ComponentType>(t);
            Json item;
            item["type"] = t;
            item["name"] = Catalog::getComponentName(type);
            item["key"] = Catalog::getComponentName(type, true);
            item["display_name"] = Catalog::getComponentDisplayName(type);
            item["icon"] = hex(Catalog::getComponentIcon(type));
            item["has_enabled"] = Catalog::hasEnabledProperty(type);
            item["structural_update_flags"] = Catalog::getComponentStructuralUpdateFlags(type);
            item["type_of_name"] = static_cast<int>(Catalog::getComponentType(Catalog::getComponentName(type)));
            item["type_of_key"] = static_cast<int>(Catalog::getComponentType(Catalog::getComponentName(type, true)));
            item["properties_without_component"] = properties(Catalog::getProperties(type, nullptr));

            const Entity entity = scene.createEntity();
            entities.push_back(entity);
            ProjectUtils::addEntityComponent(&scene, entity, type, entities, YAML::Node());
            const std::map<std::string, PropertyData> ofDefault = Catalog::findEntityProperties(&scene, entity, type);
            item["properties_of_default"] = properties(ofDefault);

            std::vector<std::string> keys = StreamSchema::knownFieldKeys(type);
            std::sort(keys.begin(), keys.end());
            item["known_keys"] = keys;
            Json options = Json::object();
            for (const auto& [name, property] : ofDefault) {
                if (property.type != PropertyType::Enum) continue;
                const mcp::McpValues::EnumOptions* list = mcp::McpValues::enumOptions(type, name);
                Json entries = Json::array();
                if (list) {
                    for (const mcp::McpValues::EnumOption& option : *list) entries.push_back(Json::array({option.value, option.name, option.label}));
                }
                options[name] = entries;
            }
            item["enum_options"] = options;
            components.push_back(item);
        }
        releaseCameraFramebuffers(&scene);

        // every property of the fixture scenes' components, indices folded, with how often it was seen
        std::map<std::string, Json> seen;
        std::vector<fs::path> scenes;
        for (const auto& entry : fs::recursive_directory_iterator(fs::path(JASPER_TEST_REPO_DIR) / "tools" / "fixtures")) {
            if (entry.is_regular_file() && entry.path().extension() == ".scene") scenes.push_back(entry.path());
        }
        std::sort(scenes.begin(), scenes.end());
        for (const fs::path& file : scenes) {
            DecodedScene decoded = decodeSceneFile(file);
            Scene* registry = decoded.scene->scene;
            for (Entity entity : decoded.scene->entities) {
                if (!registry->isEntityCreated(entity)) continue;
                for (ComponentType type : Catalog::findComponents(registry, entity)) {
                    Json& byType = seen[Catalog::getComponentName(type)];
                    if (byType.is_null()) byType = Json::object();
                    for (const auto& [name, property] : Catalog::findEntityProperties(registry, entity, type)) {
                        byType[folded(name)] = describe(property);
                    }
                }
            }
        }
        Json fixtures = Json::object();
        for (auto& [name, byType] : seen) fixtures[name] = byType;

        // the YAML names of the components' enums: what the scene files write and read
        Json streamEnums = Json::object();
        auto table = [&streamEnums](const auto& enumTable) {
            streamEnums[enumTable.typeName()] = Json{{"names", enumTable.names()}, {"fallback", enumTable.toString(enumTable.fallback())}};
        };
        table(StreamSchema::actionState());
        table(StreamSchema::anchorPreset());
        table(StreamSchema::audioSourceAttenuation());
        table(StreamSchema::audioSourceState());
        table(StreamSchema::bodyType());
        table(StreamSchema::cameraType());
        table(StreamSchema::collider2DType());
        table(StreamSchema::collider3DType());
        table(StreamSchema::cullingMode());
        table(StreamSchema::easeType());
        table(StreamSchema::atmosphereQualityOverride());
        table(StreamSchema::cloudsPreset());
        table(StreamSchema::cloudsRendering());
        table(StreamSchema::fogColorMode());
        table(StreamSchema::fogType());
        table(StreamSchema::fogVolumeFalloff());
        table(StreamSchema::fogVolumeShape());
        table(StreamSchema::joint2DType());
        table(StreamSchema::joint3DType());
        table(StreamSchema::layoutGroupType());
        table(StreamSchema::lightShading());
        table(StreamSchema::lightType());
        table(StreamSchema::materialAlphaMode());
        table(StreamSchema::materialBlendMode());
        table(StreamSchema::materialCullMode());
        table(StreamSchema::materialDepthTest());
        table(StreamSchema::materialDepthWrite());
        table(StreamSchema::materialLighting());
        table(StreamSchema::materialPassOrder());
        table(StreamSchema::materialStencilCompare());
        table(StreamSchema::materialStencilOp());
        table(StreamSchema::particleEmitterShape());
        table(StreamSchema::pivotPreset());
        table(StreamSchema::primitiveType());
        table(StreamSchema::progressbarType());
        table(StreamSchema::rigidbodyMotionQuality());
        table(StreamSchema::scriptType());
        table(StreamSchema::scrollbarType());
        table(StreamSchema::shadowCaster2DShape());
        table(StreamSchema::shadowUpdateMode());
        table(StreamSchema::cascadeSplitMode());
        table(StreamSchema::textureAlphaBorder());
        table(StreamSchema::textureFilter());
        table(StreamSchema::textureWrap());
        table(StreamSchema::uiDocumentRenderMode());
        table(StreamSchema::uiEventState());
        table(StreamSchema::windingOrder());

        Json ease = Json::array();
        for (const mcp::McpValues::EnumOption& option : mcp::McpValues::easeOptions()) ease.push_back(Json::array({option.value, option.name, option.label}));

        return Json{{"components", components}, {"fixture_properties", fixtures}, {"ease_options", ease}, {"stream_enums", streamEnums}};
    }

    void listDifferences(const Json& expected, const Json& actual, const std::string& path, std::vector<std::string>& out) {
        if (out.size() > 40) return;
        if (expected.is_object() && actual.is_object()) {
            for (auto it = expected.begin(); it != expected.end(); ++it) {
                if (!actual.contains(it.key())) out.push_back(path + "/" + it.key() + ": missing now");
                else listDifferences(it.value(), actual[it.key()], path + "/" + it.key(), out);
            }
            for (auto it = actual.begin(); it != actual.end(); ++it) {
                if (!expected.contains(it.key())) out.push_back(path + "/" + it.key() + ": new " + it.value().dump());
            }
            return;
        }
        if (expected.is_array() && actual.is_array() && expected.size() == actual.size() && !expected.empty() && expected[0].is_object()) {
            for (size_t i = 0; i < expected.size(); i++) listDifferences(expected[i], actual[i], path + "/" + std::to_string(i), out);
            return;
        }
        if (expected != actual) out.push_back(path + ": " + expected.dump() + " -> " + actual.dump());
    }


TEST(ReflectRegistry, CatalogSnapshot) {
    const Json now = snapshot();
    if (std::getenv("JASPER_TEST_UPDATE_GOLDEN")) {
        std::ofstream out(snapshotPath(), std::ios::binary);
        out << now.dump(1) << "\n";
    }
    std::ifstream in(snapshotPath(), std::ios::binary);
    ASSERT_TRUE(in.good()) << "no snapshot " << snapshotPath();
    const Json expected = Json::parse(in);
    std::vector<std::string> differences;
    listDifferences(expected, now, "", differences);
    // the differences docs/specs/S08-reflection.md (Deviations) accepts: Animation's
    // actionFrameCount, a count that read a dangling pointer, is no longer listed
    std::erase_if(differences, [](const std::string& line) { return line.ends_with("/actionFrameCount: missing now"); });
    std::string text;
    for (const std::string& line : differences) text += "\n  " + line;
    EXPECT_TRUE(differences.empty()) << "Catalog differs from the snapshot taken before reflection:" << text;
}

}
