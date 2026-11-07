// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The C++ the editor generates for a project's scenes (Generator, Factory): a scene's source and
// symbols are named by its name and id, so a scene called "main" leaves main.cpp alone and two
// scenes whose names map to one identifier keep their own files.

#include <gtest/gtest.h>
#include "component/AlphaTweenComponent.h"
#include "component/Transform.h"
#include "component/TweenComponent.h"
#include "script/ScriptProperty.h"

#include "Factory.h"
#include "Generator.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/CameraComponent.h"
#include "component/SpriteComponent.h"
#include "component/TerrainComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "object/Camera.h"
#include "object/Mesh.h"
#include "object/Sprite.h"
#include "object/Terrain.h"
#include "texture/Texture.h"

#include <unistd.h>

#include "Scene.h"
#include "component/CanvasGroupComponent.h"
#include <cstdlib>
#include <sstream>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
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
    const std::string first = Factory::sceneToFileName(11, "main");
    const std::string second = Factory::sceneToFileName(22, "Main");

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
    // one written by an older editor (under the scene's name) does not count. On a
    // case-insensitive filesystem (macOS, Windows) Main.cpp is main.cpp itself, the clash the
    // id-named sources avoid: such a file cannot be written there without replacing main.cpp
    if (!fs::exists(generated / "MAIN.CPP")) {
        std::ofstream(generated / "Main.cpp") << Factory::sceneSourceHeader << "\n";
        EXPECT_FALSE(generator.hasSceneSource(22, "Main", internal));
    }

    generator.clearSceneSource(11, "main", internal);
    EXPECT_FALSE(fs::exists(generated / (first + ".cpp")));
    EXPECT_EQ(readFile(generated / "main.cpp"), mainCpp);

    fs::remove_all(project);
}

// The first draw that asks for a missing file makes the texture read as empty() (test_reflection.py:
// a volumetric clouds' weather map came and went from the generated scene with the frames before the
// save): the code of a scene does not follow what was drawn
TEST(GeneratedScenes, ATexturePathStaysWhenItsLoadFailed) {
    Texture texture;
    texture.setPath("no-such-texture.png");
    const std::string before = Factory::formatTexture(8, texture, "clouds.weatherMap", fs::path());
    EXPECT_NE(before.find("clouds.weatherMap.setPath(\"no-such-texture.png\")"), std::string::npos) << before;
    for (int attempt = 0; attempt < 2000 && !texture.empty(); attempt++) {
        texture.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(texture.empty());
    EXPECT_EQ(Factory::formatTexture(8, texture, "clouds.weatherMap", fs::path()), before);
}

// A Sprite's frames are authored data, as the scene file holds them. They were normalised once the
// texture's size was known, so the scene's code followed whether a frame had loaded the texture
// before the save. The game normalises them when it shows a frame (MeshSystem::setSpriteFrameRect).
TEST(GeneratedScenes, ASpriteFrameIsGeneratedAsAuthoredWhateverItsTextureLoaded) {
    Scene scene;
    Sprite sprite(&scene);
    sprite.setTexture("no-such-sheet.png");
    sprite.addFrame("right", 32.0f, 0.0f, 32.0f, 16.0f);
    MeshComponent& mesh = scene.getComponent<MeshComponent>(sprite.getEntity());

    const std::string unloaded = Factory::createSpriteComponent(8, &scene, sprite.getEntity());
    EXPECT_NE(unloaded.find("sprite.framesRect[0].rect = Rect(32"), std::string::npos) << unloaded;

    std::vector<unsigned char> pixels(64 * 32 * 4, 255);
    mesh.submeshes[0].material.baseColorTexture.setData(
        "sheet", TextureData(64, 32, (unsigned int)pixels.size(), ColorFormat::RGBA, 4, pixels.data()));
    ASSERT_EQ(mesh.submeshes[0].material.baseColorTexture.getWidth(), 64u);
    EXPECT_EQ(Factory::createSpriteComponent(8, &scene, sprite.getEntity()), unloaded);
}

// A Sprite makes its quad when it runs, as a Tilemap, Mesh Polygon or Terrain make theirs. The scene
// file never held that geometry; the code held it when a frame had made it before the save.
TEST(GeneratedScenes, AGeneratedMeshGeneratesTheSameBeforeAndAfterItsGeometryIsMade) {
    Scene scene;
    Sprite sprite(&scene);
    MeshComponent& mesh = scene.getComponent<MeshComponent>(sprite.getEntity());
    mesh.numSubmeshes = 1;
    const std::string before = Factory::createMeshComponent(8, &scene, sprite.getEntity(), fs::path());

    const float vertices[3] = {1.0f, 2.0f, 3.0f};
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.importData(vertices, sizeof(vertices));
    mesh.vertexCount = 4;
    mesh.submeshes[0].vertexCount = 6;
    mesh.submeshes[0].attributes[AttributeType::POSITION] = Attribute();
    EXPECT_EQ(Factory::createMeshComponent(8, &scene, sprite.getEntity(), fs::path()), before);
}

// A plain mesh keeps its data, and a load sets its vertexCount (RenderSystem::loadMesh), which the
// file does not hold either
TEST(GeneratedScenes, AnInlineMeshGeneratesItsDataButNoVertexCount) {
    Scene scene;
    Mesh object(&scene);
    MeshComponent& mesh = scene.getComponent<MeshComponent>(object.getEntity());
    mesh.numSubmeshes = 1;
    const float vertices[3] = {1.0f, 2.0f, 3.0f};
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.importData(vertices, sizeof(vertices));
    mesh.vertexCount = 4;
    const std::string code = Factory::createMeshComponent(8, &scene, object.getEntity(), fs::path());
    EXPECT_NE(code.find("mesh.buffer.importData"), std::string::npos) << code;
    EXPECT_EQ(code.find("mesh.vertexCount"), std::string::npos) << code;
}

// An auto-resizing camera's clips and aspect are the canvas's: the size of the view that drew last,
// for any camera of the scene and not only its Default Camera
TEST(GeneratedScenes, ACameraSizedByTheCanvasGeneratesNoSize) {
    Scene scene;
    Camera camera(&scene);
    CameraComponent& component = scene.getComponent<CameraComponent>(camera.getEntity());
    const std::string code = [&]() {
        component.autoResize = true;
        component.renderToTexture = false;
        component.rightClip = 1280.0f;
        component.aspect = 1.9f;
        return Factory::createComponent(8, &scene, camera.getEntity(), ComponentType::CameraComponent, fs::path());
    }();
    for (const char* field : {"leftClip", "rightClip", "bottomClip", "topClip", "aspect"}) {
        EXPECT_EQ(code.find(std::string("camera.") + field), std::string::npos) << field << ": " << code;
    }
    EXPECT_NE(code.find("camera.yfov"), std::string::npos) << code;

    component.autoResize = false;
    const std::string fixedSize = Factory::createComponent(8, &scene, camera.getEntity(), ComponentType::CameraComponent, fs::path());
    EXPECT_NE(fixedSize.find("camera.rightClip = 1280"), std::string::npos) << fixedSize;
}

// A body's runtime flags are not authored: a frame clears them, a load and the game start with them on
TEST(GeneratedScenes, ARigidbodyGeneratesNoRuntimeFlags) {
    Scene scene;
    Entity entity = scene.createEntity();
    Rigidbody2DComponent body2d;
    body2d.needReloadBody = false;
    body2d.needUpdateColliders = false;
    body2d.newBody = false;
    scene.addComponent<Rigidbody2DComponent>(entity, body2d);
    const std::string code = Factory::createComponent(8, &scene, entity, ComponentType::Rigidbody2DComponent, fs::path());
    EXPECT_NE(code.find("Rigidbody2DComponent rigidbody2d;"), std::string::npos) << code;
    EXPECT_EQ(code.find("needReloadBody"), std::string::npos) << code;
    EXPECT_EQ(code.find("needUpdateColliders"), std::string::npos) << code;
    EXPECT_EQ(code.find("newBody"), std::string::npos) << code;
}

// A Terrain's Auto Ranges are its camera's far clip
TEST(GeneratedScenes, ATerrainUnderAutoRangesGeneratesNoRanges) {
    Scene scene;
    Terrain terrain(&scene);
    TerrainComponent& component = scene.getComponent<TerrainComponent>(terrain.getEntity());
    component.ranges = {8.0f, 4000.0f};
    component.autoSetRanges = true;
    EXPECT_EQ(Factory::createComponent(8, &scene, terrain.getEntity(), ComponentType::TerrainComponent, fs::path()).find("terrain.ranges"), std::string::npos);
    component.autoSetRanges = false;
    EXPECT_NE(Factory::createComponent(8, &scene, terrain.getEntity(), ComponentType::TerrainComponent, fs::path()).find("terrain.ranges"), std::string::npos);
}

// A terrain makes its geometry when it runs, so its mesh's vertices are not generated. It asked its
// height map's empty(), which turns true once a load failed: the scene's code took the vertices of
// the last load as bytes, whether or not a frame had tried the file before the save.
TEST(GeneratedScenes, ATerrainWhoseHeightMapFailedToLoadGeneratesNoVertices) {
    Scene scene;
    Terrain terrain(&scene);
    terrain.setHeightMap("no-such-height-map.png");
    MeshComponent& mesh = scene.getComponent<MeshComponent>(terrain.getEntity());
    const float vertices[3] = {1.0f, 2.0f, 3.0f};
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.importData(vertices, sizeof(vertices));
    ASSERT_GT(mesh.buffer.getSize(), 0u);

    const std::string before = Factory::createMeshComponent(8, &scene, terrain.getEntity(), fs::path());
    EXPECT_EQ(before.find("mesh.buffer.importData"), std::string::npos) << before;

    Texture& heightMap = scene.getComponent<TerrainComponent>(terrain.getEntity()).heightMap;
    for (int attempt = 0; attempt < 2000 && !heightMap.empty(); attempt++) {
        heightMap.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(heightMap.empty());
    EXPECT_EQ(Factory::createMeshComponent(8, &scene, terrain.getEntity(), fs::path()), before);
}

// A Lua script's string property is generated as an assignment to a ScriptPropertyValue. Since
// the variant holds AssetRef too (S04), constructible from a C string like std::string, a bare
// string literal is ambiguous between the two (GCC 11: "no match for operator="): every exported
// scene with a string property failed to compile (found by tools/ci/upgrade_from_main). The value
// is written as a std::string.
TEST(GeneratedScenes, StringScriptPropertyIsAStdString) {
    const std::string code = Factory::formatScriptPropertyValue(ScriptPropertyValue(std::string("Level1")));
    EXPECT_EQ(code.rfind("std::string(", 0), 0u) << code;
    EXPECT_NE(code.find("\"Level1\""), std::string::npos) << code;
    // the other alternatives keep their own forms
    EXPECT_EQ(Factory::formatScriptPropertyValue(ScriptPropertyValue(6)), "6");
}

// ui-canvas-group (spec 10): the Factory code of a Canvas Group, an AlphaTween aimed at it and
// an unscaled Tween compiles against the engine headers (what an exported build compiles)
TEST(GeneratedScenes, CanvasGroupFactoryCodeCompiles){
#if defined(JASPER_EDITOR_TEST_ENGINE_FLAGS) && defined(JASPER_EDITOR_TEST_CXX)
    std::ifstream flagsFile(JASPER_EDITOR_TEST_ENGINE_FLAGS);
    if (!flagsFile){
        GTEST_SKIP() << "no engine flags file " << JASPER_EDITOR_TEST_ENGINE_FLAGS;
    }
    std::string flags, line;
    bool defines = false;
    while (std::getline(flagsFile, line)){
        if (line.empty()) continue;
        if (line == "#defines"){ defines = true; continue; }
        flags += (defines ? " -D'" : " -isystem '") + line + "'";
    }

    Scene scene;
    const Entity entity = scene.createUserEntity();
    scene.addComponent<Transform>(entity, {});
    CanvasGroupComponent group;
    group.alpha = 0.25f;
    group.interactable = false;
    group.renderMode = CanvasGroupRenderMode::FLATTEN;
    scene.addComponent<CanvasGroupComponent>(entity, group);
    TweenComponent tween;
    tween.useUnscaledTime = true;
    scene.addComponent<TweenComponent>(entity, tween);
    AlphaTweenComponent alpha{0.0f, 1.0f};
    alpha.alphaTarget = AlphaTweenTarget::CANVAS_GROUP;
    scene.addComponent<AlphaTweenComponent>(entity, alpha);

    const std::string groupCode = Factory::createComponent(4, &scene, entity, ComponentType::CanvasGroupComponent, "", "scene", "entity");
    const std::string tweenCode = Factory::createComponent(4, &scene, entity, ComponentType::TweenComponent, "", "scene", "entity");
    const std::string alphaCode = Factory::createComponent(4, &scene, entity, ComponentType::AlphaTweenComponent, "", "scene", "entity");
    EXPECT_NE(groupCode.find("CanvasGroupRenderMode::FLATTEN"), std::string::npos) << groupCode;
    EXPECT_NE(tweenCode.find("useUnscaledTime = true"), std::string::npos) << tweenCode;
    EXPECT_NE(alphaCode.find("AlphaTweenTarget::CANVAS_GROUP"), std::string::npos) << alphaCode;

    std::ostringstream source;
    source << "#include \"Scene.h\"\n#include \"component/Transform.h\"\nusing namespace jasperengine;\n"
           << "void generatedGroup(Scene* scene, Entity entity){\n"
           << "{\n" << groupCode << "}\n{\n" << tweenCode << "}\n{\n" << alphaCode << "}\n}\n";
    const fs::path dir = fs::temp_directory_path() / ("jasper-canvas-group-factory-" + std::to_string(::getpid()));
    fs::create_directories(dir);
    const fs::path file = dir / "generated.cpp";
    std::ofstream(file) << source.str();
    const std::string command = std::string("'") + JASPER_EDITOR_TEST_CXX + "' -std=gnu++20 -fsyntax-only" + flags +
                                " '" + file.string() + "' > '" + (dir / "log.txt").string() + "' 2>&1";
    const int status = std::system(command.c_str());
    std::ifstream log(dir / "log.txt");
    const std::string output((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
    EXPECT_EQ(status, 0) << output << "\n" << source.str();
    fs::remove_all(dir);
#else
    GTEST_SKIP() << "built without the engine flags";
#endif
}
