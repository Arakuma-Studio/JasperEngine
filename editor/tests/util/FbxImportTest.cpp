// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The FBX import (util/FbxImporter, util/GlbBuilder): an FBX is converted into a .glb that the
// glTF loader reads in its place. fixtures/rigged_cube.fbx is a handwritten ASCII FBX 7.4 with
// a textured material, a two-bone skin, a blend shape and an animation of both.

#include <gtest/gtest.h>

#include "util/FbxImporter.h"

#include "stb_image_write.h"
#include "tiny_gltf.h"

#include "support/PosixCompat.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    const fs::path fixture = fs::path(JASPER_TEST_FIXTURES_DIR) / "rigged_cube.fbx";

    std::string readText(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    void writeText(const fs::path& path, const std::string& text) {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
    }

    std::string replaced(std::string text, const std::string& from, const std::string& to) {
        const size_t at = text.find(from);
        EXPECT_NE(at, std::string::npos) << from;
        if (at != std::string::npos) text.replace(at, from.size(), to);
        return text;
    }

    void writeChecker(const fs::path& path, unsigned char shade) {
        fs::create_directories(path.parent_path());
        std::vector<unsigned char> rgba(4 * 4 * 4);
        for (size_t i = 0; i < rgba.size(); i += 4) {
            const bool odd = ((i / 4) + (i / 16)) % 2;
            rgba[i] = odd ? shade : 0;
            rgba[i + 1] = odd ? shade : 0;
            rgba[i + 2] = odd ? shade : 0;
            rgba[i + 3] = 255;
        }
        ASSERT_TRUE(stbi_write_png(path.string().c_str(), 4, 4, 4, rgba.data(), 16));
    }

    // A project as the editor lays it out: the FBX in the assets, its import in the cache
    struct FbxProject {
        fs::path root;
        fs::path assets;
        fs::path fbx;
        fs::path glb;

        explicit FbxProject(const std::string& name, const std::string& fbxText = readText(fixture)) {
            root = fs::temp_directory_path() / ("jasper-fbx-test-" + name + "-" + std::to_string(::getpid()));
            fs::remove_all(root);
            assets = root / "assets";
            fbx = assets / "models" / "rigged_cube.fbx";
            glb = root / ".jasperengine" / "imported" / "models" / "rigged_cube.fbx.glb";
            writeText(fbx, fbxText);
            writeChecker(assets / "models" / "textures" / "checker.png", 255);
        }
        ~FbxProject() {
            std::error_code ec;
            fs::remove_all(root, ec);
        }

        // Read as MeshSystem::loadGLTFFile reads it: image URIs relative to the FBX
        bool load(tinygltf::Model& model) const {
            std::ifstream file(glb, std::ios::binary);
            const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            tinygltf::TinyGLTF loader;
            loader.SetImagesAsIs(true);
            std::string err, warn;
            const bool loaded = loader.LoadBinaryFromMemory(&model, &err, &warn, bytes.data(),
                static_cast<unsigned int>(bytes.size()), fbx.parent_path().string());
            EXPECT_TRUE(loaded) << err;
            return loaded;
        }
    };

    int nodeNamed(const tinygltf::Model& model, const std::string& name) {
        for (size_t i = 0; i < model.nodes.size(); i++) {
            if (model.nodes[i].name == name) return static_cast<int>(i);
        }
        return -1;
    }

    std::vector<double> translationOf(const tinygltf::Model& model, const std::string& name) {
        const int node = nodeNamed(model, name);
        if (node < 0) return {};
        const std::vector<double>& t = model.nodes[node].translation;
        return t.empty() ? std::vector<double>{0, 0, 0} : t;
    }

} // namespace

TEST(FbxImport, RecognizesFbxPathsWhateverTheCase) {
    EXPECT_TRUE(FbxImporter::isFbxFile("models/hero.fbx"));
    EXPECT_TRUE(FbxImporter::isFbxFile("models/HERO.FBX"));
    EXPECT_FALSE(FbxImporter::isFbxFile("models/hero.glb"));
    EXPECT_FALSE(FbxImporter::isFbxFile("models/hero.fbx.glb"));
}

TEST(FbxImport, ConvertsMeshMaterialSkinMorphsAndAnimation) {
    FbxProject project("convert");
    EXPECT_EQ(FbxImporter::getState(project.fbx, project.glb, project.assets), FbxImporter::State::Missing);

    const FbxImporter::Result result = FbxImporter::import(project.fbx, project.glb, project.assets);
    ASSERT_TRUE(result.success) << result.error;
    ASSERT_TRUE(fs::exists(project.glb));
    EXPECT_EQ(FbxImporter::getState(project.fbx, project.glb, project.assets), FbxImporter::State::UpToDate);

    tinygltf::Model model;
    ASSERT_TRUE(project.load(model));

    // Node hierarchy: the FBX root, the mesh node and the two bones under it
    EXPECT_EQ(model.nodes.size(), 4u);
    const int cube = nodeNamed(model, "Cube");
    const int rootBone = nodeNamed(model, "Root");
    const int bone = nodeNamed(model, "Bone");
    ASSERT_GE(cube, 0);
    ASSERT_GE(rootBone, 0);
    ASSERT_GE(bone, 0);
    EXPECT_NE(std::find(model.nodes[rootBone].children.begin(), model.nodes[rootBone].children.end(), bone),
        model.nodes[rootBone].children.end());
    ASSERT_EQ(model.scenes.size(), 1u);

    // One mesh, one primitive: 6 quads as 12 triangles over 24 corners (each face has its own normal)
    ASSERT_EQ(model.meshes.size(), 1u);
    ASSERT_EQ(model.nodes[cube].mesh, 0);
    ASSERT_EQ(model.meshes[0].primitives.size(), 1u);
    const tinygltf::Primitive& primitive = model.meshes[0].primitives[0];
    ASSERT_TRUE(primitive.attributes.count("POSITION"));
    const tinygltf::Accessor& positions = model.accessors[primitive.attributes.at("POSITION")];
    EXPECT_EQ(positions.count, 24u);
    EXPECT_EQ(model.accessors[primitive.indices].count, 36u);
    ASSERT_EQ(positions.minValues.size(), 3u);
    for (int c = 0; c < 3; c++) {
        EXPECT_NEAR(positions.minValues[c], -1.0, 1e-5);
        EXPECT_NEAR(positions.maxValues[c], 1.0, 1e-5);
    }
    EXPECT_TRUE(primitive.attributes.count("NORMAL"));
    EXPECT_TRUE(primitive.attributes.count("TEXCOORD_0"));

    // Skin: both clusters' bones are joints, every corner weighted
    ASSERT_EQ(model.skins.size(), 1u);
    EXPECT_EQ(model.nodes[cube].skin, 0);
    std::vector<int> joints = model.skins[0].joints;
    std::sort(joints.begin(), joints.end());
    std::vector<int> bones = {rootBone, bone};
    std::sort(bones.begin(), bones.end());
    EXPECT_EQ(joints, bones);
    EXPECT_TRUE(primitive.attributes.count("JOINTS_0"));
    EXPECT_TRUE(primitive.attributes.count("WEIGHTS_0"));
    EXPECT_GE(model.skins[0].inverseBindMatrices, 0);

    // Blend shape: one morph target with its name
    ASSERT_EQ(primitive.targets.size(), 1u);
    EXPECT_TRUE(primitive.targets[0].count("POSITION"));
    ASSERT_TRUE(model.meshes[0].extras.Has("targetNames"));
    EXPECT_EQ(model.meshes[0].extras.Get("targetNames").Get(0).Get<std::string>(), "Bulge");

    // Material: the diffuse texture, linked by a URI relative to the FBX since it is in the assets
    ASSERT_EQ(model.materials.size(), 1u);
    EXPECT_EQ(model.materials[0].name, "Checker");
    const int texture = model.materials[0].pbrMetallicRoughness.baseColorTexture.index;
    ASSERT_GE(texture, 0);
    const tinygltf::Image& image = model.images[model.textures[texture].source];
    EXPECT_EQ(image.uri, "textures/checker.png");

    // Animation: the bone's rotation and the blend shape's weight
    ASSERT_EQ(model.animations.size(), 1u);
    EXPECT_EQ(model.animations[0].name, "Take 001");
    bool rotation = false;
    bool weights = false;
    for (const tinygltf::AnimationChannel& channel : model.animations[0].channels) {
        rotation |= channel.target_node == bone && channel.target_path == "rotation";
        weights |= channel.target_node == cube && channel.target_path == "weights";
    }
    EXPECT_TRUE(rotation);
    EXPECT_TRUE(weights);

    // The marker ties the parse to the FBX it came from
    EXPECT_FALSE(FbxImporter::getSourceId(project.fbx).empty());
    EXPECT_EQ(FbxImporter::getImportId(model), FbxImporter::getSourceId(project.fbx));
    EXPECT_TRUE(FbxImporter::isCurrentImport(model, project.glb));
    EXPECT_TRUE(FbxImporter::findMissingLinkedFiles(project.fbx, project.glb).empty());
}

TEST(FbxImport, ConvertsUnitsAndAxes) {
    // Centimeters: the 2-unit cube is 2 cm, the bones sit 1 cm down
    {
        const std::string text = replaced(replaced(readText(fixture),
            "P: \"UnitScaleFactor\", \"double\", \"Number\", \"\",100", "P: \"UnitScaleFactor\", \"double\", \"Number\", \"\",1"),
            "P: \"OriginalUnitScaleFactor\", \"double\", \"Number\", \"\",100", "P: \"OriginalUnitScaleFactor\", \"double\", \"Number\", \"\",1");
        FbxProject project("units", text);
        const FbxImporter::Result result = FbxImporter::import(project.fbx, project.glb, project.assets);
        ASSERT_TRUE(result.success) << result.error;
        tinygltf::Model model;
        ASSERT_TRUE(project.load(model));
        const tinygltf::Accessor& positions = model.accessors[model.meshes[0].primitives[0].attributes.at("POSITION")];
        for (int c = 0; c < 3; c++) {
            EXPECT_NEAR(positions.maxValues[c], 0.01, 1e-6);
        }
        const std::vector<double> root = translationOf(model, "Root");
        ASSERT_EQ(root.size(), 3u);
        EXPECT_NEAR(root[1], -0.01, 1e-6);
    }
    // Z up, as Blender writes: the Root bone 1 m down Z ends up 1 m down Y
    {
        std::string text = readText(fixture);
        text = replaced(text, "P: \"UpAxis\", \"int\", \"Integer\", \"\",1", "P: \"UpAxis\", \"int\", \"Integer\", \"\",2");
        text = replaced(text, "P: \"FrontAxis\", \"int\", \"Integer\", \"\",2", "P: \"FrontAxis\", \"int\", \"Integer\", \"\",1");
        text = replaced(text, "P: \"FrontAxisSign\", \"int\", \"Integer\", \"\",1", "P: \"FrontAxisSign\", \"int\", \"Integer\", \"\",-1");
        text = replaced(text, "P: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",0,-1,0",
            "P: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",0,0,-1");
        FbxProject project("axes", text);
        const FbxImporter::Result result = FbxImporter::import(project.fbx, project.glb, project.assets);
        ASSERT_TRUE(result.success) << result.error;
        tinygltf::Model model;
        ASSERT_TRUE(project.load(model));
        // The conversion sits on the nodes or on the root; either way the world position is Y up
        std::vector<double> world = translationOf(model, "Root");
        ASSERT_EQ(world.size(), 3u);
        const int rootNode = model.scenes[0].nodes[0];
        const tinygltf::Node& sceneRoot = model.nodes[rootNode];
        if (!sceneRoot.rotation.empty() || !sceneRoot.matrix.empty() || !sceneRoot.scale.empty()) {
            ADD_FAILURE() << "the axis conversion was left on the root node instead of the geometry and transforms";
        }
        EXPECT_NEAR(world[0], 0.0, 1e-6);
        EXPECT_NEAR(world[1], -1.0, 1e-6);
        EXPECT_NEAR(world[2], 0.0, 1e-6);
    }
}

TEST(FbxImport, ReimportsWhenTheFbxOrATextureChanges) {
    FbxProject project("stale");
    ASSERT_TRUE(FbxImporter::import(project.fbx, project.glb, project.assets).success);
    ASSERT_EQ(FbxImporter::getState(project.fbx, project.glb, project.assets), FbxImporter::State::UpToDate);
    const std::string firstSource = FbxImporter::getSourceId(project.fbx);

    // A texture edit (contents, not just the time: hashes survive a git clone)
    writeChecker(project.assets / "models" / "textures" / "checker.png", 128);
    EXPECT_EQ(FbxImporter::getState(project.fbx, project.glb, project.assets), FbxImporter::State::Stale);
    ASSERT_TRUE(FbxImporter::import(project.fbx, project.glb, project.assets).success);
    EXPECT_EQ(FbxImporter::getState(project.fbx, project.glb, project.assets), FbxImporter::State::UpToDate);

    // An FBX edit changes its source id, so scenes saved with the old parts are told apart
    writeText(project.fbx, replaced(readText(project.fbx), "0.8,0.2,0.2", "0.2,0.8,0.2"));
    EXPECT_EQ(FbxImporter::getState(project.fbx, project.glb, project.assets), FbxImporter::State::Stale);
    EXPECT_NE(FbxImporter::getSourceId(project.fbx), firstSource);
    tinygltf::Model old;
    ASSERT_TRUE(project.load(old));
    ASSERT_TRUE(FbxImporter::import(project.fbx, project.glb, project.assets).success);
    EXPECT_FALSE(FbxImporter::isCurrentImport(old, project.glb));
    tinygltf::Model current;
    ASSERT_TRUE(project.load(current));
    EXPECT_TRUE(FbxImporter::isCurrentImport(current, project.glb));
    EXPECT_EQ(FbxImporter::getImportId(current), FbxImporter::getSourceId(project.fbx));

    // A deleted texture keeps the last import, which reports the link it lost
    fs::remove(project.assets / "models" / "textures" / "checker.png");
    EXPECT_EQ(FbxImporter::getState(project.fbx, project.glb, project.assets), FbxImporter::State::UpToDate);
    const std::vector<fs::path> missing = FbxImporter::findMissingLinkedFiles(project.fbx, project.glb);
    ASSERT_EQ(missing.size(), 1u);
    EXPECT_EQ(missing[0].filename(), "checker.png");

    // A forced import redoes an up-to-date one; a texture that is nowhere is a warning
    const FbxImporter::Result forced = FbxImporter::import(project.fbx, project.glb, project.assets, true);
    ASSERT_TRUE(forced.success) << forced.error;
    EXPECT_NE(std::find_if(forced.warnings.begin(), forced.warnings.end(),
        [](const std::string& w) { return w.find("checker.png") != std::string::npos; }), forced.warnings.end());
}

TEST(FbxImport, ConcurrentImportsNeverShowAHalfWrittenFile) {
    // The editor imports from the main thread, the background check, Play startup and the
    // thumbnail worker; readers load the .glb meanwhile
    FbxProject project("concurrent");
    ASSERT_TRUE(FbxImporter::import(project.fbx, project.glb, project.assets).success);
    std::atomic<bool> done{false};
    std::atomic<int> failures{0};
    std::atomic<int> badReads{0};
    std::vector<std::thread> writers;
    for (int w = 0; w < 4; w++) {
        writers.emplace_back([&]() {
            for (int i = 0; i < 5; i++) {
                if (!FbxImporter::import(project.fbx, project.glb, project.assets, true).success) failures++;
            }
        });
    }
    std::thread reader([&]() {
        while (!done) {
            std::ifstream file(project.glb, std::ios::binary);
            const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            tinygltf::TinyGLTF loader;
            loader.SetImagesAsIs(true);
            tinygltf::Model model;
            std::string err, warn;
            if (bytes.empty() || !loader.LoadBinaryFromMemory(&model, &err, &warn, bytes.data(),
                    static_cast<unsigned int>(bytes.size()), project.fbx.parent_path().string())) {
                badReads++;
            }
        }
    });
    for (std::thread& writer : writers) writer.join();
    done = true;
    reader.join();
    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(badReads.load(), 0);
    EXPECT_EQ(FbxImporter::getState(project.fbx, project.glb, project.assets), FbxImporter::State::UpToDate);
    for (const fs::directory_entry& entry : fs::directory_iterator(project.glb.parent_path())) {
        EXPECT_EQ(entry.path().filename(), "rigged_cube.fbx.glb") << "left behind: " << entry.path();
    }
}

TEST(FbxImport, WritesAsideUnderItsOwnName) {
    // What another writer (a second editor on the project, a crash) left at the old shared name
    FbxProject project("aside");
    fs::path shared = project.glb;
    shared += ".tmp";
    fs::create_directories(shared / "busy");
    const FbxImporter::Result result = FbxImporter::import(project.fbx, project.glb, project.assets);
    EXPECT_TRUE(result.success) << result.error;
    EXPECT_TRUE(fs::is_directory(shared / "busy")) << "someone else's file is left alone";
    EXPECT_EQ(FbxImporter::getState(project.fbx, project.glb, project.assets), FbxImporter::State::UpToDate);
}

TEST(FbxImport, ReportsFilesItCannotConvert) {
    FbxProject project("broken", "; FBX 7.4.0 project file\nthis is not an FBX {\n");
    const FbxImporter::Result result = FbxImporter::import(project.fbx, project.glb, project.assets);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error.empty());
    EXPECT_FALSE(fs::exists(project.glb));
    EXPECT_EQ(FbxImporter::getState(project.fbx, project.glb, project.assets), FbxImporter::State::Missing);
}
