// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Project Settings > Input in exported builds (input spec §7.5): the generated main.cpp loads the
// project's actions with InputActions::loadJson in a raw string literal, only when the project
// has an input node (else the engine keeps Unity's defaults), with a delimiter the JSON lacks.

#include <gtest/gtest.h>

#include "Generator.h"
#include "input/InputActions.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    fs::path tempProject(const std::string& name) {
        const fs::path dir = fs::temp_directory_path() / ("jasper-generator-input-test-" + name + "-" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir / ".jasperengine" / "generated");
        return dir;
    }

    std::string readFile(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    // what the C++ compiler reads from the adjacent raw string literals of `source`
    std::string rawLiteralValue(const std::string& source) {
        std::string value;
        size_t at = 0;
        while ((at = source.find("R\"", at)) != std::string::npos) {
            const size_t open = source.find('(', at);
            const std::string delimiter = source.substr(at + 2, open - at - 2);
            const std::string close = ")" + delimiter + "\"";
            const size_t end = source.find(close, open);
            if (end == std::string::npos) return "<unterminated>";
            value += source.substr(open + 1, end - open - 1);
            at = end + close.size();
        }
        return value;
    }

}

TEST(GeneratorInputTest, NoLineForTheDefaults) {
    EXPECT_EQ(Generator::inputActionsInitSource(""), "");
}

TEST(GeneratorInputTest, LoadsTheProjectJson) {
    const std::string json = InputActions::actionsToJson(InputActions::defaults());
    const std::string line = Generator::inputActionsInitSource(json);
    EXPECT_EQ(line, "    InputActions::loadJson(R\"jinput(" + json + ")jinput\", nullptr);\n");
    EXPECT_EQ(rawLiteralValue(line), json);
}

TEST(GeneratorInputTest, TheDelimiterIsAbsentFromTheJson) {
    std::vector<InputAction> actions = InputActions::defaults();
    // a JSON string that ends like the literal's end (its quote is the string's)
    actions[0].description = "breaks a naive literal )jinput";
    actions[1].description = "and the next delimiter )jinput1";
    const std::string json = InputActions::actionsToJson(actions);
    const std::string line = Generator::inputActionsInitSource(json);
    EXPECT_NE(line.find("R\"jinput2("), std::string::npos) << line;
    EXPECT_EQ(rawLiteralValue(line), json);
}

TEST(GeneratorInputTest, ALongDefinitionIsSplitIntoLiteralsMsvcTakes) {
    std::vector<InputAction> actions;
    for (int i = 0; i < 200; i++) {
        InputAction action;
        action.name = "Action " + std::to_string(i);
        action.description = std::string(40, 'x');
        action.bindings.push_back(InputActions::makeBinding(InputBindingKind::Buttons));
        actions.push_back(action);
    }
    const std::string json = InputActions::actionsToJson(actions);
    ASSERT_GT(json.size(), 16000u);
    const std::string line = Generator::inputActionsInitSource(json);
    EXPECT_EQ(rawLiteralValue(line), json);
    size_t pieces = 0;
    for (size_t at = line.find("R\""); at != std::string::npos; at = line.find("R\"", at + 1)) pieces++;
    EXPECT_GE(pieces, 3u);
}

TEST(GeneratorInputTest, PiecesEndOnCharacterBoundaries) {
    // review fix: a fixed 8000-byte cut could split a UTF-8 character between two literals
    std::vector<InputAction> actions;
    for (int i = 0; i < 120; i++) {
        InputAction action;
        action.name = "Acci\xc3\xb3n " + std::to_string(i);
        // two- and four-byte characters everywhere, so some fall on a cut
        for (int k = 0; k < 20; k++) action.description += "\xc3\xa9\xf0\x9f\x98\x80";
        actions.push_back(action);
    }
    const std::string json = InputActions::actionsToJson(actions);
    ASSERT_GT(json.size(), 16000u);
    const std::string line = Generator::inputActionsInitSource(json);
    EXPECT_EQ(rawLiteralValue(line), json);
    size_t at = 0;
    int pieces = 0;
    while ((at = line.find("R\"jinput(", at)) != std::string::npos) {
        const size_t open = at + 9;
        const size_t end = line.find(")jinput\"", open);
        ASSERT_NE(end, std::string::npos);
        const std::string piece = line.substr(open, end - open);
        ASSERT_FALSE(piece.empty());
        // no piece starts with a continuation byte or ends inside a character
        EXPECT_NE(static_cast<unsigned char>(piece.front()) & 0xC0, 0x80) << "piece " << pieces;
        size_t lead = piece.size() - 1;
        while (lead > 0 && (static_cast<unsigned char>(piece[lead]) & 0xC0) == 0x80) lead--;
        const unsigned char c = static_cast<unsigned char>(piece[lead]);
        const size_t length = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
        EXPECT_EQ(piece.size() - lead, length) << "piece " << pieces;
        pieces++;
        at = end;
    }
    EXPECT_GE(pieces, 3);
}

TEST(GeneratorInputTest, MainCppHasTheLineOnlyWhenTheProjectHasAnInputNode) {
    const fs::path project = tempProject("main");
    const fs::path internal = project / ".jasperengine";
    const std::vector<SceneBuildInfo> scenes = {SceneBuildInfo{1, "Level", {1}, {1}, true}};

    Generator generator;
    generator.configure(scenes, "test", {}, {}, project, internal, project / "assets", project / "lua", {}, 17);
    EXPECT_EQ(readFile(internal / "generated" / "main.cpp").find("InputActions::loadJson"), std::string::npos);

    std::vector<InputAction> actions = InputActions::defaults();
    InputAction dash;
    dash.name = "Dash";
    InputBinding shift = InputActions::makeBinding(InputBindingKind::Buttons);
    shift.positive.push_back({InputDevice::Key, D_KEY_LEFT_SHIFT});
    dash.bindings.push_back(shift);
    actions.push_back(dash);
    const std::string json = InputActions::actionsToJson(actions);
    generator.configure(scenes, "test", {}, {}, project, internal, project / "assets", project / "lua", {}, 17,
                        Scaling::FITWIDTH, TextureStrategy::NONE, 1280, 720, true, WindowSettings(), LoadingSettings(),
                        TagsAndLayers(), 4, json);
    const std::string mainCpp = readFile(internal / "generated" / "main.cpp");
    const size_t at = mainCpp.find("InputActions::loadJson(");
    ASSERT_NE(at, std::string::npos);
    EXPECT_EQ(rawLiteralValue(mainCpp.substr(at, mainCpp.find(";\n", at) - at)), json);
    // before the first scene loads
    EXPECT_LT(at, mainCpp.find("SceneManager::loadScene"));
    // main.cpp includes only JasperEngine.h: InputActions must be declared through it
    EXPECT_NE(mainCpp.find("#include \"JasperEngine.h\""), std::string::npos);
    const std::string umbrella = readFile(fs::path(JASPER_EDITOR_TEST_FIXTURES) / ".." / ".." / ".." / "engine" / "core" / "JasperEngine.h");
    EXPECT_NE(umbrella.find("#include \"input/InputActions.h\""), std::string::npos);

    // what the build reads back is the project's definition
    std::vector<InputAction> parsed;
    std::string error;
    ASSERT_TRUE(InputActions::parseActionsJson(json, parsed, &error)) << error;
    EXPECT_EQ(parsed.back().name, "Dash");

    fs::remove_all(project);
}
