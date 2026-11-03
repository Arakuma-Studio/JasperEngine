// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The registry of component descriptions (docs/specs/S08-reflection.md AC1, AC2, R18, R20)

#include "reflect/Reflect.h"
#include "reflect/ReflectIndex.h"

#include "component/Joint2DComponent.h"
#include "mcp/McpJson.h"

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <set>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using Json = jasperengine::editor::mcp::Json;

// (a named namespace, tests included: a unity build merges the test sources)
namespace reflect_registry_test {

    std::string hex(const char* text) {
        static const char* digits = "0123456789abcdef";
        std::string out;
        for (const unsigned char* c = reinterpret_cast<const unsigned char*>(text); c && *c; c++) {
            out += digits[*c >> 4];
            out += digits[*c & 15];
        }
        return out;
    }

    std::string lowered(std::string text) {
        for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }


// AC1: a description per ComponentType, its names as the Catalog before reflection gave them
TEST(ReflectRegistry, CoversAllComponentTypes) {
    std::ifstream in(fs::path(JASPER_TEST_REPO_DIR) / "editor" / "tests" / "reflect" / "catalog_snapshot.json", std::ios::binary);
    ASSERT_TRUE(in.good());
    const Json snapshot = Json::parse(in);
    const Json& components = snapshot["components"];
    ASSERT_EQ(components.size(), static_cast<size_t>(LAST_COMPONENT_TYPE) + 1);
    ASSERT_EQ(reflect::all().size(), components.size());
    for (const Json& expected : components) {
        const ComponentType type = static_cast<ComponentType>(expected["type"].get<int>());
        const reflect::ComponentDesc* desc = reflect::find(type);
        ASSERT_NE(desc, nullptr) << expected["name"];
        EXPECT_EQ(desc->type, type);
        EXPECT_EQ(desc->cppName, expected["name"].get<std::string>());
        EXPECT_EQ(desc->key, expected["key"].get<std::string>());
        EXPECT_EQ(desc->displayName, expected["display_name"].get<std::string>());
        EXPECT_EQ(hex(desc->icon), expected["icon"].get<std::string>()) << desc->cppName;
        EXPECT_EQ(desc->hasEnabled, expected["has_enabled"].get<bool>()) << desc->cppName;
        EXPECT_EQ(desc->structuralUpdateFlags, expected["structural_update_flags"].get<uint64_t>()) << desc->cppName;
        // by key, C++ name and case
        EXPECT_EQ(reflect::findByKey(desc->key), desc);
        EXPECT_EQ(reflect::findByKey(desc->cppName), desc);
        EXPECT_EQ(reflect::findByKey(lowered(desc->cppName)), desc);
    }
    EXPECT_EQ(reflect::findByKey("NoSuchComponent"), nullptr);
}

// AC2: names, keys and aliases unique; no alias names a live field; enums round-trip with their
// fallback a member; every saved field has a codec; every proxy inside its owner
TEST(ReflectRegistry, SelfCheck) {
    const std::vector<std::string> problems = reflect::selfCheck();
    std::string text;
    for (const std::string& problem : problems) text += "\n  " + problem;
    EXPECT_TRUE(problems.empty()) << text;
}

// R20: the name indexes are small and built fast
TEST(ReflectRegistry, NameIndexesAreSmall) {
    const auto start = std::chrono::steady_clock::now();
    const size_t structs = reflect::buildNameIndexes();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("[bench] name indexes: %zu structs, %zu bytes, built in %.3f ms\n", structs, reflect::nameIndexBytes(), ms);
    EXPECT_GT(structs, static_cast<size_t>(LAST_COMPONENT_TYPE));
    EXPECT_LE(reflect::nameIndexBytes(), 256u * 1024u);
}

// The orders the registry keeps: every component once in each
TEST(ReflectRegistry, OrdersListEveryComponent) {
    for (auto order : {reflect::serializationOrder(), reflect::listingOrder(), reflect::all()}) {
        std::set<const reflect::ComponentDesc*> seen(order.begin(), order.end());
        EXPECT_EQ(seen.size(), static_cast<size_t>(LAST_COMPONENT_TYPE) + 1);
    }
    EXPECT_EQ(reflect::serializationOrder().front()->type, ComponentType::Transform);
}

}

// a joint's default break force is infinite: as 0 (what formatFloat makes of it) every joint of an
// exported game breaks at its first step, which a golden made with the bug does not show
TEST(ReflectRegistry, GeneratedCodeKeepsAnInfiniteBreakForce) {
    const reflect::ComponentDesc* desc = reflect::find(ComponentType::Joint2DComponent);
    ASSERT_NE(desc, nullptr);
    Joint2DComponent joint;
    reflect::CodegenContext ctx;
    ctx.variable = "joint2d";
    std::string code = reflect::cppAssignments(*desc, &joint, ctx);
    EXPECT_NE(code.find("joint2d.breakForce = std::numeric_limits<float>::infinity();"), std::string::npos) << code;
    EXPECT_NE(code.find("joint2d.breakTorque = std::numeric_limits<float>::infinity();"), std::string::npos) << code;
    joint.breakForce = 5.0f;
    code = reflect::cppAssignments(*desc, &joint, ctx);
    EXPECT_NE(code.find("joint2d.breakForce = 5.000000f;"), std::string::npos) << code;
}
