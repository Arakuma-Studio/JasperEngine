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


    namespace metadata_validation {

        using namespace jasperengine::editor::reflect;

        // ownCodec: the component writes its YAML with a codec of its own, the fields need none
        void checkStructMetadataForRegistryTest(const std::string& owner, const StructDesc& desc, std::vector<std::string>& problems, std::set<const StructDesc*>& seen,
                         bool ownCodec) {
            if (!seen.insert(&desc).second) return;
            std::set<std::string> names;
            std::set<std::string> keys;
            for (const FieldDesc& field : desc.fields) {
                const std::string where = owner + "." + field.name;
                if (!names.insert(field.name).second) problems.push_back(where + ": the name is used twice");
                if (isSaved(field) && !keys.insert(field.yamlKey).second) problems.push_back(where + ": the YAML key " + field.yamlKey + " is used twice");
                if (!field.ops) {
                    problems.push_back(where + ": no TypeOps");
                    continue;
                }
                if (!ownCodec && isSaved(field) && !field.ops->toYaml && !field.ops->scalar && !field.ops->encodeKey && !field.ops->fromYaml && !field.ops->decodeKey) {
                    problems.push_back(where + ": saved but its type has no YAML codec");
                }
                if (field.kind == Kind::Enum && !field.enumDesc) problems.push_back(where + ": an enum without an EnumDesc");
                if (field.kind == Kind::Struct && !field.structDesc) problems.push_back(where + ": a struct without a StructDesc");
                if (field.kind == Kind::Array && (!field.arrayOps || (!field.elementOps && !field.structDesc))) {
                    problems.push_back(where + ": an array without element ops");
                }
                if ((field.flags & Proxy) && desc.defaults) {
                    // a proxy resolves inside its owner (the default instance's storage)
                    const char* begin = static_cast<const char*>(desc.defaults());
                    const char* ref = static_cast<const char*>(field.ref(const_cast<void*>(desc.defaults())));
                    if (!desc.ops || ref < begin || ref + field.ops->size > begin + desc.ops->size) problems.push_back(where + ": a proxy outside its owner");
                }
                if (field.structDesc) checkStructMetadataForRegistryTest(owner + "." + field.name, *field.structDesc, problems, seen, ownCodec);
            }
            // an alias never names a live field
            for (const FieldDesc& field : desc.fields) {
                for (const char* alias : field.aliases) {
                    if (names.count(alias)) problems.push_back(owner + "." + field.name + ": the alias " + alias + " is a field's name");
                    if (keys.count(alias)) problems.push_back(owner + "." + field.name + ": the alias " + alias + " is a field's YAML key");
                }
            }
            std::set<std::string> aliases;
            for (const FieldDesc& field : desc.fields) {
                for (const char* alias : field.aliases) {
                    if (!aliases.insert(alias).second) problems.push_back(owner + ": the alias " + std::string(alias) + " is used twice");
                }
            }
        }

        void checkEnumMetadataForRegistryTest(const EnumDesc& desc, std::vector<std::string>& problems) {
            const std::string owner = desc.cppType;
            std::set<std::string> tokens;
            std::set<int> values;
            for (const EnumItem& item : desc.items) {
                if (!tokens.insert(item.token).second) problems.push_back(owner + ": the token " + item.token + " is used twice");
                if (item.mcpToken && std::string_view(item.mcpToken) != item.token && !tokens.insert(item.mcpToken).second) {
                    problems.push_back(owner + ": the MCP token " + std::string(item.mcpToken) + " is used twice");
                }
                if (!values.insert(item.value).second) problems.push_back(owner + ": the value " + std::to_string(item.value) + " is listed twice");
            }
            for (const EnumItem& item : desc.items) {
                for (const char* alias : item.aliases) {
                    if (!tokens.insert(alias).second) problems.push_back(owner + ": the alias " + std::string(alias) + " is a token");
                }
                // token -> value -> token
                const EnumItem* found = enumItemByToken(desc, item.token);
                if (!found || found->value != item.value || std::string_view(enumToken(desc, found->value)) != item.token) {
                    problems.push_back(owner + ": " + item.token + " does not round-trip");
                }
                if (item.mcpToken) {
                    const EnumItem* byMcp = enumItemByToken(desc, item.mcpToken);
                    if (!byMcp || byMcp->value != item.value) problems.push_back(owner + ": the MCP token " + std::string(item.mcpToken) + " does not round-trip");
                }
            }
            if (!enumItem(desc, desc.fallback)) problems.push_back(owner + ": the fallback is not a value");
            for (int value : desc.uiOrder) {
                if (!enumItem(desc, value)) problems.push_back(owner + ": the UI order lists " + std::to_string(value) + ", not a value");
            }
            if (!desc.uiOrder.empty() && desc.uiOrder.size() != desc.items.size()) problems.push_back(owner + ": the UI order misses values");
        }

        std::vector<std::string> registryMetadataProblemsForTest() {
            std::vector<std::string> problems;
            std::set<std::string> keys;
            std::set<const StructDesc*> seen;
            const auto components = reflect::all();
            for (size_t i = 0; i < components.size(); i++) {
                const ComponentDesc& desc = *components[i];
                if (static_cast<size_t>(desc.type) != i) problems.push_back(std::string(desc.cppName) + ": listed at the place of another type");
                if (!keys.insert(desc.key).second) problems.push_back(std::string(desc.cppName) + ": the key " + desc.key + " is used twice");
                for (const char* alias : desc.keyAliases) {
                    if (!keys.insert(alias).second) problems.push_back(std::string(desc.cppName) + ": the key alias " + alias + " is used twice");
                }
                if (!desc.find || !desc.add || !desc.componentId) problems.push_back(std::string(desc.cppName) + ": no storage access");
                if (desc.codec && (!desc.codec->encode || !desc.codec->decode)) problems.push_back(std::string(desc.cppName) + ": an incomplete codec");
                if (!desc.codegen && !desc.codegenVar) problems.push_back(std::string(desc.cppName) + ": no exported code");
                checkStructMetadataForRegistryTest(desc.cppName, desc.body, problems, seen, desc.codec != nullptr);
                for (const char* name : desc.codegenOrder) {
                    if (!findSegment(desc.body, name)) problems.push_back(std::string(desc.cppName) + ": the codegen order lists " + name + ", not a field");
                }
            }
            for (const auto order : {reflect::serializationOrder(), reflect::listingOrder()}) {
                std::set<const ComponentDesc*> listed(order.begin(), order.end());
                if (listed.size() != components.size()) problems.push_back("an order list misses or repeats a component");
            }
            for (const EnumDesc* desc : reflect::enumTables()) checkEnumMetadataForRegistryTest(*desc, problems);
            return problems;
        }
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
    const std::vector<std::string> problems = metadata_validation::registryMetadataProblemsForTest();
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
// exported game breaks at its first step, which a golden made with the bug does not show. It stays at the
// component's default, and a finite one is written
TEST(ReflectRegistry, GeneratedCodeLeavesAnInfiniteBreakForceAtTheDefault) {
    const reflect::ComponentDesc* desc = reflect::find(ComponentType::Joint2DComponent);
    ASSERT_NE(desc, nullptr);
    Joint2DComponent joint;
    reflect::CodegenContext ctx;
    ctx.variable = "joint2d";
    std::string code = reflect::cppAssignments(*desc, &joint, ctx);
    EXPECT_EQ(code.find("joint2d.breakForce"), std::string::npos) << code;
    EXPECT_EQ(code.find("joint2d.breakTorque"), std::string::npos) << code;
    joint.breakForce = 5.0f;
    code = reflect::cppAssignments(*desc, &joint, ctx);
    EXPECT_NE(code.find("joint2d.breakForce = 5.000000f;"), std::string::npos) << code;
}
