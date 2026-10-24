// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The C++ the editor generates for a project's scenes (Generator, Factory): a scene's source and
// symbols are named by its name and id, so a scene called "main" leaves main.cpp alone and two
// scenes whose names map to one identifier keep their own files.

#include <gtest/gtest.h>

#include "Factory.h"
#include "Generator.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
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
        const fs::path dir = fs::temp_directory_path() / ("jasper-generated-scenes-test-" + name + "-" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir / ".jasperengine" / "generated");
        return dir;
    }

    std::string readFile(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    std::string lower(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    SceneBuildInfo stack(uint32_t id, const std::string& name, bool isMain = false) {
        return SceneBuildInfo{id, name, {id}, {id}, isMain};
    }

    // The body of the generated function `signature` in `source`, up to its closing brace
    std::string functionBody(const std::string& source, const std::string& signature) {
        const size_t start = source.find(signature);
        if (start == std::string::npos) return "";
        const size_t end = source.find("\n}\n", start);
        return source.substr(start, end == std::string::npos ? std::string::npos : end - start);
    }

}

TEST(GeneratedScenes, FileNamesNeverClashWithTheProjectSourcesOrEachOther) {
    const std::vector<std::string> names = {
        Factory::sceneToFileName(1, "main"),
        Factory::sceneToFileName(2, "Main"),
        Factory::sceneToFileName(3, "scripts"),
        Factory::sceneToFileName(4, "scene scripts"),
        Factory::sceneToFileName(5, "scene_scripts"),
        Factory::sceneToFileName(6, "prefab level"),
        Factory::sceneToFileName(7, "My Scene"),
        Factory::sceneToFileName(8, "My-Scene"),
        Factory::sceneToFileName(9, ""),
    };
    const std::vector<std::string> reserved = {"main", "scene_scripts", lower(Factory::prefabToFileName("level.prefab"))};
    for (size_t i = 0; i < names.size(); i++) {
        for (const std::string& name : reserved) {
            EXPECT_NE(lower(names[i]), name);
        }
        for (size_t j = i + 1; j < names.size(); j++) {
            EXPECT_NE(lower(names[i]), lower(names[j])) << names[i] << " and " << names[j];
        }
        // a C identifier: the file name is also the scene's function names
        ASSERT_FALSE(names[i].empty());
        EXPECT_TRUE(std::isalpha(static_cast<unsigned char>(names[i][0])) || names[i][0] == '_') << names[i];
        EXPECT_TRUE(std::all_of(names[i].begin(), names[i].end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; })) << names[i];
    }
    // the same scene keeps its name
    EXPECT_EQ(Factory::sceneToFileName(42, "My Scene"), Factory::sceneToFileName(42, "My Scene"));
}

TEST(GeneratedScenes, SceneNamedMainLeavesMainCppAlone) {
    const fs::path project = tempProject("main");
    const fs::path internal = project / ".jasperengine";

    // two scenes whose names are one identifier, one of them "main"
    const std::vector<SceneBuildInfo> scenes = {stack(11, "main", true), stack(22, "Main")};
    Generator generator;
    generator.configure(scenes, "test", {}, {}, project, internal, project / "assets", project / "lua", {}, 17);

    const fs::path generated = internal / "generated";
    const std::string mainCpp = readFile(generated / "main.cpp");
    ASSERT_FALSE(mainCpp.empty());
    EXPECT_NE(mainCpp.find("JASPERENGINE_INIT void init()"), std::string::npos);

    const std::string first = Factory::sceneToFileName(11, "main");
    const std::string second = Factory::sceneToFileName(22, "Main");
    for (const std::string& id : {first, second}) {
        EXPECT_NE(mainCpp.find("void create_" + id + "(Scene* scene);"), std::string::npos) << id;
        EXPECT_NE(mainCpp.find("SceneResources resources_" + id + "();"), std::string::npos) << id;
        EXPECT_NE(mainCpp.find("static Scene* _" + id + " = nullptr;"), std::string::npos) << id;
        EXPECT_NE(mainCpp.find("void load_" + id + "() {"), std::string::npos) << id;
        EXPECT_NE(mainCpp.find("void add_" + id + "() {"), std::string::npos) << id;
        EXPECT_NE(mainCpp.find("create_" + id + "(_" + id + ");"), std::string::npos) << id;
        EXPECT_NE(mainCpp.find(", load_" + id + ", add_" + id + ", {"), std::string::npos) << id;
    }
    // the old names (by name alone) are gone
    EXPECT_EQ(mainCpp.find("void create_main("), std::string::npos);
    EXPECT_EQ(mainCpp.find("void create_Main("), std::string::npos);

    // the project's CMake compiles both scene sources, neither of them main.cpp
    const std::string cmake = readFile(project / "CMakeLists.txt");
    ASSERT_FALSE(cmake.empty());
    EXPECT_NE(cmake.find("/generated/" + first + ".cpp"), std::string::npos);
    EXPECT_NE(cmake.find("/generated/" + second + ".cpp"), std::string::npos);

    // a scene source that is missing counts as missing, under its own name only
    EXPECT_FALSE(generator.hasSceneSource(11, "main", internal));
    std::ofstream(generated / (first + ".cpp")) << Factory::sceneSourceHeader << "\n";
    EXPECT_TRUE(generator.hasSceneSource(11, "main", internal));
    EXPECT_FALSE(generator.hasSceneSource(22, "Main", internal));
    // one written by an older editor (under the scene's name) does not count
    std::ofstream(generated / "Main.cpp") << Factory::sceneSourceHeader << "\n";
    EXPECT_FALSE(generator.hasSceneSource(22, "Main", internal));

    generator.clearSceneSource(11, "main", internal);
    EXPECT_FALSE(fs::exists(generated / (first + ".cpp")));
    EXPECT_EQ(readFile(generated / "main.cpp"), mainCpp);

    fs::remove_all(project);
}
