// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// `jasperengine-editor call` turns command-line tokens into tool arguments from the tool's JSON
// Schema (S31 R2, AC1: CallArgs) and validates them against the schema subset the McpSchema
// builders emit (R3, AC2: SchemaValidate). The schemas come from the builders themselves.

#include "cli/SchemaArgs.h"

#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/tools/McpTools.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

using namespace jasperengine::editor::cli;
namespace schema = jasperengine::editor::mcp::schema;

namespace {

    Json toolSchema() {
        using namespace schema;
        return object({
            {"name", string("")},
            {"count", integer("", 1, 100)},
            {"scale", number("")},
            {"select", boolean("")},
            {"mode", stringEnum("", {"replace", "merge"})},
            {"entity", entityRef()},
            {"position", vec3("")},
            {"color", color()},
            {"tags", array(string(""), "")},
            {"ids", array(Json{{"type", "integer"}}, "")},
            {"values", anyObject("")},
            {"create", object({{"name", string("")}, {"parent", entityRef()}})},
            {"rotation", quat()},
            {"value", any("")},
            {"screenshot", Json{{"anyOf", Json::array({Json{{"type", "boolean"}},
                                                       Json{{"type", "string"}, {"enum", {"game", "scene"}}}})}}},
            {"heightmap", Json{{"type", {"string", "object"}}, {"properties", {{"seed", integer("")}}},
                               {"additionalProperties", false}}}
        }, {"name"});
    }

    // The files @path values read in these tests
    std::string fakeFile(const std::string& path) {
        static const std::map<std::string, std::string> files = {
            {"args.json", R"({"name": "FromFile", "count": 3})"},
            {"name.txt", "Text Name\n"},
            {"pos.json", "[1, 2, 3]\n"},
            {"transform.json", R"({"position": [0, 1, 0]})"},
        };
        auto it = files.find(path);
        if (it == files.end()) throw UsageError("can't read @" + path);
        return it->second;
    }

    Json parse(const std::vector<std::string>& tokens) {
        return parseToolArgs(toolSchema(), tokens, fakeFile);
    }

    // The UsageError message of these tokens ("" when they parse)
    std::string usageError(const std::vector<std::string>& tokens) {
        try {
            parse(tokens);
        } catch (const UsageError& e) {
            return e.what();
        }
        return {};
    }

    bool hasError(const std::vector<std::string>& errors, const std::string& needle) {
        for (const std::string& error : errors) {
            if (error.find(needle) != std::string::npos) return true;
        }
        return false;
    }

}

// ---------------------------------------------------------------- CallArgs (AC1)

TEST(CallArgs, FlagsUseKebabOrSnakeCase) {
    const std::vector<ArgSpec> specs = argSpecs(schema::object({{"undo_group", schema::boolean("")}}));
    ASSERT_EQ(specs.size(), 1u);
    EXPECT_EQ(specs[0].flag, "--undo-group");
    const Json kebab = parseToolArgs(schema::object({{"max_width", schema::integer("")}}), std::vector<std::string>{"--max-width", "5"});
    const Json snake = parseToolArgs(schema::object({{"max_width", schema::integer("")}}), std::vector<std::string>{"--max_width=5"});
    EXPECT_EQ(kebab, (Json{{"max_width", 5}}));
    EXPECT_EQ(snake, kebab);
    EXPECT_EQ(snakeName("create-entity"), "create_entity");
}

TEST(CallArgs, StringsAreVerbatim) {
    EXPECT_EQ(parse({"--name", "Player 1"})["name"], "Player 1");
    EXPECT_EQ(parse({"--name=a=b"})["name"], "a=b");
    EXPECT_EQ(parse({"--name", "42"})["name"], "42");
}

TEST(CallArgs, IntegersAreStrictDecimal) {
    EXPECT_EQ(parse({"--count", "7"})["count"], 7);
    EXPECT_EQ(parse({"--count=-3"})["count"], -3);
    EXPECT_NE(usageError({"--count", "1.5"}).find("expects an integer"), std::string::npos);
    EXPECT_NE(usageError({"--count", "0x10"}), "");
    EXPECT_NE(usageError({"--count", "seven"}), "");
}

TEST(CallArgs, NumbersAreFinite) {
    EXPECT_DOUBLE_EQ(parse({"--scale", "1.5"})["scale"].get<double>(), 1.5);
    EXPECT_DOUBLE_EQ(parse({"--scale", "-2e3"})["scale"].get<double>(), -2000.0);
    EXPECT_NE(usageError({"--scale", "nan"}), "");
    EXPECT_NE(usageError({"--scale", "inf"}), "");
    EXPECT_NE(usageError({"--scale", "1e999"}), "");
}

TEST(CallArgs, BooleansAreTriState) {
    EXPECT_FALSE(parse({"--name", "x"}).contains("select"));
    EXPECT_EQ(parse({"--select"})["select"], true);
    EXPECT_EQ(parse({"--select=false"})["select"], false);
    EXPECT_EQ(parse({"--select", "off"})["select"], false);
    EXPECT_EQ(parse({"--select", "YES"})["select"], true);
    EXPECT_EQ(parse({"--select=0"})["select"], false);
    EXPECT_EQ(parse({"--no-select"})["select"], false);
    EXPECT_EQ(parse({"--select", "--name", "x"})["select"], true);
    EXPECT_NE(usageError({"--select", "maybe"}).find("--select expects true/false"), std::string::npos);
    EXPECT_NE(usageError({"--select=maybe"}), "");
    EXPECT_NE(usageError({"--no-name"}).find("unknown flag"), std::string::npos);
}

TEST(CallArgs, EnumsListTheirValues) {
    EXPECT_EQ(parse({"--mode", "merge"})["mode"], "merge");
    const std::string error = usageError({"--mode", "mix"});
    EXPECT_NE(error.find("replace, merge"), std::string::npos) << error;
}

TEST(CallArgs, RefsAreIdsNamesOrQuotedNames) {
    EXPECT_EQ(parse({"--entity", "42"})["entity"], 42);
    EXPECT_TRUE(parse({"--entity", "42"})["entity"].is_number_integer());
    EXPECT_EQ(parse({"--entity", "\"42\""})["entity"], "42");
    EXPECT_EQ(parse({"--entity", "Player/Gun"})["entity"], "Player/Gun");
    EXPECT_EQ(parse({"--entity", "Enemy#2"})["entity"], "Enemy#2");
    EXPECT_NE(usageError({"--entity", "\"unterminated"}), "");
}

TEST(CallArgs, ScalarArrays) {
    EXPECT_EQ(parse({"--position", "1,2,3"})["position"], (Json{1.0, 2.0, 3.0}));
    EXPECT_EQ(parse({"--position", "[1, 2.5, 3]"})["position"], (Json{1, 2.5, 3}));
    EXPECT_EQ(parse({"--tags", "a", "--tags", "b,c"})["tags"], (Json{"a", "b", "c"}));
    EXPECT_EQ(parse({"--ids", "4,5"})["ids"], (Json{4, 5}));
    EXPECT_NE(usageError({"--ids", "4,x"}), "");
    // the length is the validator's: 1,2 for a vec3 parses, then fails validation
    const Json args = parse({"--name", "x", "--position", "1,2"});
    EXPECT_TRUE(hasError(validate(toolSchema(), args, true), "position: expected 3 items, got 2"));
}

TEST(CallArgs, FreeObjectsTakeJsonOrKeyValue) {
    const Json args = parse({"--values", "transform={\"position\":[0,1,0]}", "--values", "label=Hello", "--values", "n=3"});
    EXPECT_EQ(args["values"]["transform"], (Json{{"position", {0, 1, 0}}}));
    EXPECT_EQ(args["values"]["label"], "Hello");
    EXPECT_EQ(args["values"]["n"], 3);
    EXPECT_EQ(parse({"--values", "{\"a\": 1}"})["values"], (Json{{"a", 1}}));
    EXPECT_EQ(parse({"--values", "t=@transform.json"})["values"]["t"], (Json{{"position", {0, 1, 0}}}));
    EXPECT_NE(usageError({"--values", "novalue"}), "");
}

TEST(CallArgs, DeclaredObjectsAnyOfAndAny) {
    EXPECT_EQ(parse({"--create", "{\"name\": \"A\"}"})["create"], (Json{{"name", "A"}}));
    EXPECT_NE(usageError({"--create", "name=A"}).find("takes JSON"), std::string::npos);
    EXPECT_EQ(parse({"--rotation", "{\"euler\": [0, 90, 0]}"})["rotation"], (Json{{"euler", {0, 90, 0}}}));
    EXPECT_EQ(parse({"--value", "[1, 2]"})["value"], (Json{1, 2}));
    EXPECT_EQ(parse({"--value", "plain words"})["value"], "plain words");   // any: not JSON = a string
    EXPECT_EQ(parse({"--screenshot"})["screenshot"], true);                 // anyOf with a boolean branch
    EXPECT_EQ(parse({"--screenshot", "scene"})["screenshot"], "scene");
    EXPECT_EQ(parse({"--heightmap", "assets/h.png"})["heightmap"], "assets/h.png");
    EXPECT_EQ(parse({"--heightmap", "{\"seed\": 4}"})["heightmap"], (Json{{"seed", 4}}));
}

TEST(CallArgs, AtFileValues) {
    EXPECT_EQ(parse({"--name", "@name.txt"})["name"], "Text Name");
    EXPECT_EQ(parse({"--position", "@pos.json"})["position"], (Json{1, 2, 3}));
    EXPECT_EQ(parse({"@args.json"}), (Json{{"name", "FromFile"}, {"count", 3}}));
    EXPECT_NE(usageError({"--name", "@missing.txt"}).find("can't read @missing.txt"), std::string::npos);
}

TEST(CallArgs, OneJsonObjectOrFlags) {
    EXPECT_EQ(parse({"{\"name\": \"A\", \"count\": 2}"}), (Json{{"name", "A"}, {"count", 2}}));
    EXPECT_EQ(parse({}), Json::object());
    EXPECT_NE(usageError({"{\"name\": \"A\"}", "--count", "2"}).find("not both"), std::string::npos);
    EXPECT_NE(usageError({"--count", "2", "{\"name\": \"A\"}"}), "");
    EXPECT_NE(usageError({"[1, 2]"}), "");
    EXPECT_NE(usageError({"{not json"}), "");
    EXPECT_NE(usageError({"stray"}).find("unexpected argument"), std::string::npos);
}

TEST(CallArgs, UnknownFlagsSuggestAndList) {
    const std::string error = usageError({"--nmae", "x"});
    EXPECT_NE(error.find("unknown flag --nmae"), std::string::npos) << error;
    EXPECT_NE(error.find("did you mean --name?"), std::string::npos) << error;
    EXPECT_NE(error.find("--position"), std::string::npos) << error;
    EXPECT_EQ(usageError({"--zzzzzzzz", "x"}).find("did you mean"), std::string::npos);
    EXPECT_NE(usageError({"--count"}).find("needs a value"), std::string::npos);
    EXPECT_NE(usageError({"--name", "a", "--name", "b"}).find("given twice"), std::string::npos);
}

TEST(CallArgs, EditDistance) {
    EXPECT_EQ(editDistance("kitten", "sitting"), 3u);
    EXPECT_EQ(editDistance("", "abc"), 3u);
    EXPECT_EQ(editDistance("same", "same"), 0u);
    EXPECT_EQ(closeMatches("positon", {"position", "parent", "pos"}).front(), "position");
}

// ---------------------------------------------------------------- SchemaValidate (AC2)

TEST(SchemaValidate, Type) {
    EXPECT_TRUE(validate(Json{{"type", "integer"}}, 3).empty());
    EXPECT_TRUE(validate(Json{{"type", "integer"}}, 3.0).empty());
    EXPECT_FALSE(validate(Json{{"type", "integer"}}, 3.5).empty());
    EXPECT_FALSE(validate(Json{{"type", "string"}}, 3).empty());
    EXPECT_TRUE(validate(Json{{"type", {"integer", "string"}}}, "Player").empty());
    EXPECT_TRUE(validate(Json{{"type", {"integer", "string"}}}, 4).empty());
    EXPECT_TRUE(hasError(validate(Json{{"type", {"integer", "string"}}}, true), "expected integer or string, got boolean"));
    EXPECT_TRUE(validate(Json::object(), Json{{"any", 1}}).empty());
}

TEST(SchemaValidate, Enum) {
    const Json s = schema::stringEnum("", {"a", "b"});
    EXPECT_TRUE(validate(s, "a").empty());
    EXPECT_TRUE(hasError(validate(s, "c"), "must be one of a, b"));
}

TEST(SchemaValidate, RequiredPropertiesAdditional) {
    const Json s = toolSchema();
    EXPECT_TRUE(validate(s, Json{{"name", "x"}}, true).empty());
    EXPECT_TRUE(hasError(validate(s, Json::object(), true), "name: required"));
    EXPECT_TRUE(hasError(validate(s, Json{{"name", "x"}, {"nmae", 1}}, true), "nmae: unknown key (did you mean name?)"));
    // the top level is open unless closedTop (the listing leaves additionalProperties out)
    Json listed = s;
    listed.erase("additionalProperties");
    EXPECT_TRUE(validate(listed, Json{{"name", "x"}, {"extra", 1}}).empty());
    EXPECT_FALSE(validate(listed, Json{{"name", "x"}, {"extra", 1}}, true).empty());
    // nested closed objects, and free-form ones
    EXPECT_TRUE(hasError(validate(s, Json{{"name", "x"}, {"create", {{"nam", "A"}}}}, true), "create.nam: unknown key"));
    EXPECT_TRUE(validate(s, Json{{"name", "x"}, {"values", {{"anything", {{"goes", 1}}}}}}, true).empty());
    EXPECT_TRUE(hasError(validate(s, Json{{"name", "x"}, {"create", {{"name", 5}}}}, true), "create.name: expected string"));
}

TEST(SchemaValidate, ItemsMinMaxItems) {
    const Json vec = schema::vec3("");
    EXPECT_TRUE(validate(vec, Json{1, 2, 3}).empty());
    EXPECT_TRUE(hasError(validate(vec, Json{1, 2}), "expected 3 items, got 2"));
    EXPECT_TRUE(hasError(validate(vec, Json{1, "y", 3}), "[1]: expected number"));
    const Json col = schema::color();
    EXPECT_TRUE(validate(col, Json{1, 0, 0}).empty());
    EXPECT_TRUE(validate(col, Json{1, 0, 0, 1}).empty());
    EXPECT_TRUE(hasError(validate(col, Json{1, 0}), "at least 3"));
    EXPECT_TRUE(hasError(validate(col, Json{1, 0, 0, 1, 1}), "at most 4"));
}

TEST(SchemaValidate, MinimumMaximum) {
    const Json s = schema::integer("", 1, 100);
    EXPECT_TRUE(validate(s, 1).empty());
    EXPECT_TRUE(validate(s, 100).empty());
    EXPECT_TRUE(hasError(validate(s, 0), "below the minimum 1"));
    EXPECT_TRUE(hasError(validate(s, 101), "above the maximum 100"));
}

TEST(SchemaValidate, AnyOf) {
    const Json q = schema::quat();
    EXPECT_TRUE(validate(q, Json{0, 0, 0, 1}).empty());
    EXPECT_TRUE(validate(q, Json{{"euler", {0, 90, 0}}}).empty());
    EXPECT_TRUE(hasError(validate(q, Json{0, 0, 1}), "matches none of the allowed forms"));
    EXPECT_FALSE(validate(q, Json{{"euler", {0, 90, 0}}, {"w", 1}}).empty());
}

TEST(SchemaValidate, DescribeType) {
    EXPECT_EQ(describeType(schema::vec3("")), "number[3]");
    EXPECT_EQ(describeType(schema::integer("", 1, 100)), "integer 1..100");
    EXPECT_EQ(describeType(schema::stringEnum("", {"a", "b"})), "one of: a | b");
    EXPECT_EQ(describeType(schema::entityRef()), "ref (id, name or path)");
    EXPECT_EQ(describeType(schema::anyObject("")), "object (JSON or KEY=VALUE)");
    EXPECT_EQ(describeType(schema::color()), "number[3..4]");
}

// Every tool the editor registers: its flags classify, and an empty call validates as "required"
// errors only (the CLI can convert and check every real schema, not just the ones above)
TEST(SchemaValidate, EveryRegisteredToolSchema) {
    jasperengine::editor::mcp::McpRegistry registry;
    jasperengine::editor::mcp::buildToolRegistry(registry);
    ASSERT_GT(registry.tools().size(), 100u);
    for (const auto& tool : registry.tools()) {
        const Json listed = jasperengine::editor::mcp::McpRegistry::toolToJson(tool)["inputSchema"];
        for (const ArgSpec& spec : argSpecs(listed)) {
            EXPECT_EQ(spec.flag.find('_'), std::string::npos) << tool.name;
            EXPECT_FALSE(describeType(spec.schema).empty()) << tool.name << " " << spec.key;
        }
        const std::vector<std::string> errors = validate(listed, Json::object(), true);
        for (const Json& key : listed.value("required", Json::array())) {
            EXPECT_TRUE(std::find(errors.begin(), errors.end(), key.get<std::string>() + ": required") != errors.end())
                << tool.name << " " << key;
        }
        for (const std::string& error : errors) {
            EXPECT_NE(error.find(": required"), std::string::npos) << tool.name << ": " << error;
        }
    }
}
