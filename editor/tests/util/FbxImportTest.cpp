// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The FBX import (util/FbxImporter, util/GlbBuilder): an FBX is converted into a .glb that the
// glTF loader reads in its place. fixtures/rigged_cube.fbx is a handwritten ASCII FBX 7.4 with
// a textured material, a two-bone skin, a blend shape and an animation of both.

#include <gtest/gtest.h>

#include "util/FbxImporter.h"

#include "io/watch/SharedFileReader.h"
#include "ufbx.h"

#include "stb_image_write.h"
#include "tiny_gltf.h"

#include "support/PosixCompat.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace FbxImportTest_cpp {

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
    // (the same size, a few milliseconds after the first write: on Windows often in the same tick)
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

// A same-size write inside the tick of the clock that dated the first one (15.6 ms on Windows)
// leaves the size and the time as they were: the hash read just after the first write is not kept
TEST(FbxImport, ASameSizeEditInTheSameClockTickChangesTheSourceId) {
    FbxProject project("tick");
    const fs::file_time_type written = fs::last_write_time(project.fbx);
    const std::string first = FbxImporter::getSourceId(project.fbx);
    ASSERT_FALSE(first.empty());

    writeText(project.fbx, replaced(readText(project.fbx), "0.8,0.2,0.2", "0.2,0.8,0.2"));
    fs::last_write_time(project.fbx, written);
    EXPECT_NE(FbxImporter::getSourceId(project.fbx), first) << "the hash cached by size and time stood for other contents";
}

// What was read from a file last written well before stays cached until its size or time changes:
// a big FBX is not read again on every check
TEST(FbxImport, AnOldFilesHashIsReadOnceUntilItsStampChanges) {
    FbxProject project("settled");
    const fs::file_time_type old = fs::file_time_type::clock::now() - std::chrono::hours(1);
    fs::last_write_time(project.fbx, old);
    const std::string first = FbxImporter::getSourceId(project.fbx);
    ASSERT_FALSE(first.empty());

    // other contents under the same stamp: only a read of the file could see them
    writeText(project.fbx, replaced(readText(project.fbx), "0.8,0.2,0.2", "0.2,0.8,0.2"));
    fs::last_write_time(project.fbx, old);
    EXPECT_EQ(FbxImporter::getSourceId(project.fbx), first) << "the hash of a file unchanged for an hour was read again";

    fs::last_write_time(project.fbx, old + std::chrono::seconds(1));
    EXPECT_NE(FbxImporter::getSourceId(project.fbx), first);
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
            // read as the engine reads the import (MeshSystem::readWholeFile through File): shared for
            // deletion on Windows, so the publish replaces the file while it is read. std::ifstream never
            // shares it: a reader reopening it back to back would hold every replace off, past the retries
            std::vector<uint8_t> bytes;
            jasperengine::SharedFileReader::readAll(project.glb, bytes);
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

// ---------------------------------------------------------------- spec S94 (P7: model import)

namespace {

    // the fixture without one Objects block (its header line up to the tab-indented closing brace)
    std::string withoutBlock(std::string text, const std::string& header) {
        const size_t at = text.find("\t" + header);
        EXPECT_NE(at, std::string::npos) << header;
        if (at == std::string::npos) return text;
        const size_t end = text.find("\n\t}\n", at);
        text.erase(at, end + 4 - at);
        return text;
    }

    // the fixture without the connections that name any of `ids` (C: "OO",child,parent[, "property"])
    std::string withoutConnections(const std::string& text, const std::vector<std::string>& ids) {
        std::string out;
        std::istringstream lines(text);
        std::string line;
        while (std::getline(lines, line)) {
            bool drop = false;
            const size_t at = line.find("C: \"O");
            if (at != std::string::npos) {
                std::vector<std::string> fields;
                std::istringstream parts(line.substr(at));
                std::string field;
                while (std::getline(parts, field, ',')) fields.push_back(field);
                for (const std::string& id : ids) {
                    drop = drop || (fields.size() > 2 && (fields[1] == id || fields[2] == id));
                }
            }
            if (!drop) out += line + "\n";
        }
        return out;
    }

    std::string withObjects(const std::string& text, const std::string& objects, const std::string& connections) {
        std::string out = replaced(text, "\n}\nConnections:  {\n", "\n" + objects + "}\nConnections:  {\n" + connections);
        return out;
    }

    void writeMeta(const fs::path& fbx, const std::string& modelImporter) {
        writeText(fs::path(fbx.string() + ".meta"), "import:\n  modelImporter:\n    version: 1\n" + modelImporter);
    }

    // a translation curve on Root (Y at -3 m all take long), over the fixture's take
    std::string withRootTranslationTake(const std::string& text) {
        return withObjects(text,
            "\tAnimationCurveNode: 806, \"AnimCurveNode::T\", \"\" {\n\t\tProperties70:  {\n"
            "\t\t\tP: \"d|X\", \"Number\", \"\", \"A\",0\n\t\t\tP: \"d|Y\", \"Number\", \"\", \"A\",-1\n\t\t\tP: \"d|Z\", \"Number\", \"\", \"A\",0\n"
            "\t\t}\n\t}\n"
            "\tAnimationCurve: 807, \"AnimCurve::\", \"\" {\n\t\tDefault: 0\n\t\tKeyVer: 4009\n"
            "\t\tKeyTime: *2 {\n\t\t\ta: 0,46186158000\n\t\t}\n\t\tKeyValueFloat: *2 {\n\t\t\ta: -3,-3\n\t\t}\n"
            "\t\tKeyAttrFlags: *1 {\n\t\t\ta: 4\n\t\t}\n\t\tKeyAttrDataFloat: *4 {\n\t\t\ta: 0,0,0,0\n\t\t}\n"
            "\t\tKeyAttrRefCount: *1 {\n\t\t\ta: 2\n\t\t}\n\t}\n",
            "\tC: \"OO\",806,801\n\tC: \"OP\",806,400, \"Lcl Translation\"\n\tC: \"OP\",807,806, \"d|Y\"\n");
    }

    std::vector<float> accessorFloats(const tinygltf::Model& model, int index) {
        const tinygltf::Accessor& accessor = model.accessors[static_cast<size_t>(index)];
        const tinygltf::BufferView& view = model.bufferViews[static_cast<size_t>(accessor.bufferView)];
        const size_t components = static_cast<size_t>(tinygltf::GetNumComponentsInType(static_cast<uint32_t>(accessor.type)));
        std::vector<float> out(accessor.count * components);
        std::memcpy(out.data(), model.buffers[static_cast<size_t>(view.buffer)].data.data() + view.byteOffset + accessor.byteOffset,
            out.size() * sizeof(float));
        return out;
    }

    // a glTF node's local matrix (column-major), from its TRS
    std::array<double, 16> nodeMatrix(const tinygltf::Node& node) {
        const double tx = node.translation.empty() ? 0 : node.translation[0], ty = node.translation.empty() ? 0 : node.translation[1],
                     tz = node.translation.empty() ? 0 : node.translation[2];
        const double qx = node.rotation.empty() ? 0 : node.rotation[0], qy = node.rotation.empty() ? 0 : node.rotation[1],
                     qz = node.rotation.empty() ? 0 : node.rotation[2], qw = node.rotation.empty() ? 1 : node.rotation[3];
        const double sx = node.scale.empty() ? 1 : node.scale[0], sy = node.scale.empty() ? 1 : node.scale[1], sz = node.scale.empty() ? 1 : node.scale[2];
        const double r[9] = {1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy + qz * qw), 2 * (qx * qz - qy * qw),
                             2 * (qx * qy - qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz + qx * qw),
                             2 * (qx * qz + qy * qw), 2 * (qy * qz - qx * qw), 1 - 2 * (qx * qx + qy * qy)};
        return {r[0] * sx, r[1] * sx, r[2] * sx, 0, r[3] * sy, r[4] * sy, r[5] * sy, 0, r[6] * sz, r[7] * sz, r[8] * sz, 0, tx, ty, tz, 1};
    }

    std::array<double, 16> multiply(const std::array<double, 16>& a, const std::array<double, 16>& b) {
        std::array<double, 16> out{};
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                for (int k = 0; k < 4; k++) out[c * 4 + r] += a[k * 4 + r] * b[c * 4 + k];
        return out;
    }

} // namespace

TEST(FbxImport, ClipOnlyFile) {
    // Unity's Model@Clip.fbx: the skeleton and a take, no geometry (spec S94 F39)
    std::string text = readText(fixture);
    for (const char* block : {"Geometry: 100,", "Geometry: 702,", "Model: 200,", "Material: 300,", "Texture: 310,", "Deformer: 600,",
                              "Deformer: 601,", "Deformer: 602,", "Deformer: 700,", "Deformer: 701,", "AnimationCurveNode: 804,",
                              "AnimationCurve: 805,"}) {
        text = withoutBlock(text, block);
    }
    text = withoutConnections(text, {"100", "702", "200", "300", "310", "600", "601", "602", "700", "701", "804", "805"});
    FbxProject project("cliponly", text);
    const FbxImporter::Result result = FbxImporter::import(project.fbx, project.glb, project.assets);
    ASSERT_TRUE(result.success) << result.error;
    tinygltf::Model model;
    ASSERT_TRUE(project.load(model));
    EXPECT_TRUE(model.meshes.empty());
    EXPECT_GE(nodeNamed(model, "Root"), 0);
    const int bone = nodeNamed(model, "Bone");
    ASSERT_GE(bone, 0);
    ASSERT_EQ(model.animations.size(), 1u);
    EXPECT_EQ(model.animations[0].name, "Take 001");
    bool rotation = false;
    for (const tinygltf::AnimationChannel& channel : model.animations[0].channels) {
        rotation |= channel.target_node == bone && channel.target_path == "rotation";
    }
    EXPECT_TRUE(rotation);

    // neither geometry nor a take still fails
    FbxProject empty("cliponly-empty", withoutConnections(withoutBlock(withoutBlock(withoutBlock(withoutBlock(text,
        "AnimationStack: 800,"), "AnimationLayer: 801,"), "AnimationCurveNode: 802,"), "AnimationCurve: 803,"), {"800", "801", "802", "803"}));
    const FbxImporter::Result failed = FbxImporter::import(empty.fbx, empty.glb, empty.assets);
    EXPECT_FALSE(failed.success);
    EXPECT_NE(failed.error.find("no triangle meshes"), std::string::npos) << failed.error;
}

TEST(FbxImport, RestPoseFirstFrame) {
    FbxProject project("restpose", withRootTranslationTake(readText(fixture)));
    ASSERT_TRUE(FbxImporter::import(project.fbx, project.glb, project.assets).success);
    tinygltf::Model nodeValues;
    ASSERT_TRUE(project.load(nodeValues));
    ASSERT_EQ(translationOf(nodeValues, "Root").size(), 3u);
    EXPECT_NEAR(translationOf(nodeValues, "Root")[1], -1.0, 1e-6) << "Node Values keeps the node's own translation";
    const std::string defaultSource = FbxImporter::getSourceId(project.fbx);

    // First Frame: the take's first frame is the nodes' pose; the setting is part of the import
    writeMeta(project.fbx, "    restPose: first_frame\n");
    EXPECT_EQ(FbxImporter::getState(project.fbx, project.glb, project.assets), FbxImporter::State::Stale);
    EXPECT_NE(FbxImporter::getSourceId(project.fbx), defaultSource);
    const FbxImporter::Result result = FbxImporter::import(project.fbx, project.glb, project.assets);
    ASSERT_TRUE(result.success) << result.error;
    EXPECT_EQ(FbxImporter::getState(project.fbx, project.glb, project.assets), FbxImporter::State::UpToDate);
    tinygltf::Model firstFrame;
    ASSERT_TRUE(project.load(firstFrame));
    EXPECT_NEAR(translationOf(firstFrame, "Root")[1], -3.0, 1e-5);
    EXPECT_EQ(FbxImporter::getImportId(firstFrame), FbxImporter::getSourceId(project.fbx));
    // the bind pose is kept: the inverse bind matrices are the file's
    ASSERT_EQ(firstFrame.skins.size(), 1u);
    EXPECT_EQ(accessorFloats(firstFrame, firstFrame.skins[0].inverseBindMatrices),
              accessorFloats(nodeValues, nodeValues.skins[0].inverseBindMatrices));

    // a take the file does not have: the first one, with a warning
    writeMeta(project.fbx, "    restPose: first_frame\n    restPoseTake: Run\n");
    const FbxImporter::Result unknown = FbxImporter::import(project.fbx, project.glb, project.assets);
    ASSERT_TRUE(unknown.success) << unknown.error;
    EXPECT_NE(std::find_if(unknown.warnings.begin(), unknown.warnings.end(),
        [](const std::string& w) { return w.find("'Run'") != std::string::npos; }), unknown.warnings.end());
}

TEST(FbxImport, RestPoseSkipsTakesThatAnimateNothing) {
    // CLEIVER's file: empty takes before the one that animates (no clip is made of them), so the
    // default take is the first clip, the one the Inspector shows
    std::string text = replaced(withRootTranslationTake(readText(fixture)), "\tAnimationStack: 800,",
        "\tAnimationStack: 790, \"AnimStack::Empty\", \"\" {\n\t}\n\tAnimationLayer: 791, \"AnimLayer::BaseLayer\", \"\" {\n\t}\n"
        "\tAnimationStack: 800,");
    text = replaced(text, "\tC: \"OO\",801,800\n", "\tC: \"OO\",791,790\n\tC: \"OO\",801,800\n");
    FbxProject project("restpose-empty-take", text);
    writeMeta(project.fbx, "    restPose: first_frame\n");
    const FbxImporter::Result result = FbxImporter::import(project.fbx, project.glb, project.assets);
    ASSERT_TRUE(result.success) << result.error;
    tinygltf::Model model;
    ASSERT_TRUE(project.load(model));
    ASSERT_EQ(model.animations.size(), 1u);
    EXPECT_EQ(model.animations[0].name, "Take 001");
    ASSERT_EQ(translationOf(model, "Root").size(), 3u);
    EXPECT_NEAR(translationOf(model, "Root")[1], -3.0, 1e-5) << "the first clip's first frame, not the empty first take's";
}

TEST(FbxImport, SubmeshOrderFollowsMaterialSlots) {
    FbxImporter::setDefaultUsedMaterialsOrder(false);
    // three slots: 0 unused, then the faces' materials 1 and 2, the first face on slot 1 (B's
    // eyes/body case: the empty slot shifted every later submesh by one)
    std::string text = readText(fixture);
    text = replaced(text, "\t\t\tMappingInformationType: \"AllSame\"\n\t\t\tReferenceInformationType: \"IndexToDirect\"\n\t\t\tMaterials: *1 {\n\t\t\t\ta: 0\n",
        "\t\t\tMappingInformationType: \"ByPolygon\"\n\t\t\tReferenceInformationType: \"IndexToDirect\"\n\t\t\tMaterials: *6 {\n\t\t\t\ta: 1,1,2,2,1,2\n");
    text = withObjects(text,
        "\tMaterial: 301, \"Material::Body\", \"\" {\n\t\tVersion: 102\n\t\tShadingModel: \"phong\"\n\t}\n"
        "\tMaterial: 302, \"Material::Eyes\", \"\" {\n\t\tVersion: 102\n\t\tShadingModel: \"phong\"\n\t}\n",
        "");
    text = replaced(text, "\tC: \"OO\",300,200\n", "\tC: \"OO\",300,200\n\tC: \"OO\",301,200\n\tC: \"OO\",302,200\n");
    FbxProject project("submeshes", text);
    const FbxImporter::Result result = FbxImporter::import(project.fbx, project.glb, project.assets);
    ASSERT_TRUE(result.success) << result.error;
    tinygltf::Model model;
    ASSERT_TRUE(project.load(model));
    ASSERT_EQ(model.meshes.size(), 1u);
    const std::vector<tinygltf::Primitive>& primitives = model.meshes[0].primitives;
    ASSERT_EQ(primitives.size(), 3u) << "the empty slot 0 keeps its index";
    const char* names[] = {"Checker", "Body", "Eyes"};
    for (int i = 0; i < 3; i++) {
        ASSERT_TRUE(primitives[i].extras.Has("fbxMaterialSlot")) << i;
        EXPECT_EQ(primitives[i].extras.Get("fbxMaterialSlot").GetNumberAsInt(), i);
        EXPECT_EQ(primitives[i].extras.Get("fbxMaterial").Get<std::string>(), names[i]);
        ASSERT_GE(primitives[i].material, 0);
        EXPECT_EQ(model.materials[static_cast<size_t>(primitives[i].material)].name, names[i]);
    }
    EXPECT_TRUE(primitives[0].extras.Has("fbxEmptySlot"));
    EXPECT_EQ(model.accessors[static_cast<size_t>(primitives[0].indices)].count, 3u) << "one triangle that draws nothing";
    EXPECT_EQ(model.accessors[static_cast<size_t>(primitives[1].indices)].count, 18u);
    EXPECT_EQ(model.accessors[static_cast<size_t>(primitives[2].indices)].count, 18u);
    EXPECT_FALSE(primitives[1].extras.Has("fbxEmptySlot"));

    // a project made before (FBX Submesh Order Used Materials, set by its migration) keeps the old
    // order, the used slots only, so its saved submesh values stay put; the order is part of the
    // import, so switching it reimports
    const std::string slotsSource = FbxImporter::getSourceId(project.fbx);
    FbxImporter::setDefaultUsedMaterialsOrder(true);
    struct RestoreOrder {
        ~RestoreOrder() { FbxImporter::setDefaultUsedMaterialsOrder(false); }
    } restoreOrder;
    EXPECT_EQ(FbxImporter::getState(project.fbx, project.glb, project.assets), FbxImporter::State::Stale);
    EXPECT_NE(FbxImporter::getSourceId(project.fbx), slotsSource);
    const FbxImporter::Result legacy = FbxImporter::import(project.fbx, project.glb, project.assets);
    ASSERT_TRUE(legacy.success) << legacy.error;
    EXPECT_EQ(FbxImporter::getState(project.fbx, project.glb, project.assets), FbxImporter::State::UpToDate);
    tinygltf::Model used;
    ASSERT_TRUE(project.load(used));
    ASSERT_EQ(used.meshes.size(), 1u);
    const std::vector<tinygltf::Primitive>& usedPrimitives = used.meshes[0].primitives;
    ASSERT_EQ(usedPrimitives.size(), 2u) << "the empty slot 0 makes no submesh";
    const char* usedNames[] = {"Body", "Eyes"};
    for (int i = 0; i < 2; i++) {
        ASSERT_GE(usedPrimitives[i].material, 0);
        EXPECT_EQ(used.materials[static_cast<size_t>(usedPrimitives[i].material)].name, usedNames[i]);
        EXPECT_FALSE(usedPrimitives[i].extras.Has("fbxEmptySlot"));
    }
    EXPECT_EQ(FbxImporter::getImportId(used), FbxImporter::getSourceId(project.fbx));
}

TEST(FbxImport, SkinnedGeometricTransform) {
    // CLEIVER's case (R22): the skinned mesh has a geometric translation and its parents a
    // pre-rotation and a non-unit scale. The import's bind pose must skin where ufbx does.
    std::string text = readText(fixture);
    text = replaced(text, "\tModel: 200, \"Model::Cube\", \"Mesh\" {\n\t\tVersion: 232\n\t\tProperties70:  {\n",
        "\tModel: 200, \"Model::Cube\", \"Mesh\" {\n\t\tVersion: 232\n\t\tProperties70:  {\n"
        "\t\t\tP: \"GeometricTranslation\", \"Vector3D\", \"Vector\", \"\",0.5,0.25,0\n");
    text = withObjects(text,
        "\tModel: 210, \"Model::Rig\", \"Null\" {\n\t\tVersion: 232\n\t\tProperties70:  {\n"
        "\t\t\tP: \"RotationActive\", \"bool\", \"\", \"\",1\n\t\t\tP: \"PreRotation\", \"Vector3D\", \"Vector\", \"\",-90,0,0\n"
        "\t\t\tP: \"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\",0.5,0.5,0.5\n\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",0,2.8,0\n"
        "\t\t}\n\t\tShading: T\n\t\tCulling: \"CullingOff\"\n\t}\n",
        "\tC: \"OO\",210,0\n");
    text = replaced(text, "\tC: \"OO\",200,0\n", "\tC: \"OO\",200,210\n");
    text = replaced(text, "\tC: \"OO\",400,0\n", "\tC: \"OO\",400,210\n");
    FbxProject project("geometric", text);
    const FbxImporter::Result result = FbxImporter::import(project.fbx, project.glb, project.assets);
    ASSERT_TRUE(result.success) << result.error;
    tinygltf::Model model;
    ASSERT_TRUE(project.load(model));

    // ufbx's own evaluation of the skin at the take's start, loaded as the importer loads
    ufbx_load_opts options = {};
    options.target_axes = ufbx_axes_right_handed_y_up;
    options.target_unit_meters = 1.0;
    options.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    options.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;
    options.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_COMPENSATE;
    options.pivot_handling = UFBX_PIVOT_HANDLING_ADJUST_TO_PIVOT;
    options.geometry_transform_helper_name.data = "GeometryTransformHelper";
    options.geometry_transform_helper_name.length = SIZE_MAX;
    options.scale_helper_name.data = "ScaleHelper";
    options.scale_helper_name.length = SIZE_MAX;
    options.clean_skin_weights = true;
    ufbx_error error;
    ufbx_scene* scene = ufbx_load_file(project.fbx.string().c_str(), &options, &error);
    ASSERT_NE(scene, nullptr);
    ufbx_evaluate_opts evaluate = {};
    evaluate.evaluate_skinning = true;
    ufbx_scene* posed = ufbx_evaluate_scene(scene, scene->anim, 0.0, &evaluate, &error);
    ASSERT_NE(posed, nullptr);
    std::vector<std::array<double, 3>> expected;
    for (const ufbx_mesh* mesh : posed->meshes) {
        const ufbx_matrix toWorld = mesh->skinned_is_local && mesh->instances.count > 0 ? mesh->instances.data[0]->geometry_to_world
                                                                                       : ufbx_identity_matrix;
        for (size_t i = 0; i < mesh->num_indices; i++) {
            const ufbx_vec3 p = ufbx_transform_position(&toWorld, ufbx_get_vertex_vec3(&mesh->skinned_position, i));
            expected.push_back({p.x, p.y, p.z});
        }
    }
    ufbx_free_scene(posed);
    ufbx_free_scene(scene);
    ASSERT_FALSE(expected.empty());

    // the glb's: each joint's global matrix times its inverse bind, weighted
    std::vector<std::array<double, 16>> globals(model.nodes.size());
    std::vector<int> parent(model.nodes.size(), -1);
    for (size_t n = 0; n < model.nodes.size(); n++)
        for (int child : model.nodes[n].children) parent[static_cast<size_t>(child)] = static_cast<int>(n);
    for (size_t n = 0; n < model.nodes.size(); n++) {
        std::array<double, 16> m = nodeMatrix(model.nodes[n]);
        for (int p = parent[n]; p >= 0; p = parent[static_cast<size_t>(p)]) m = multiply(nodeMatrix(model.nodes[static_cast<size_t>(p)]), m);
        globals[n] = m;
    }
    ASSERT_EQ(model.skins.size(), 1u);
    const tinygltf::Skin& skin = model.skins[0];
    const std::vector<float> inverseBinds = accessorFloats(model, skin.inverseBindMatrices);
    const tinygltf::Primitive& primitive = model.meshes[0].primitives[0];
    const std::vector<float> positions = accessorFloats(model, primitive.attributes.at("POSITION"));
    const std::vector<float> weights = accessorFloats(model, primitive.attributes.at("WEIGHTS_0"));
    const tinygltf::Accessor& jointAccessor = model.accessors[static_cast<size_t>(primitive.attributes.at("JOINTS_0"))];
    const tinygltf::BufferView& jointView = model.bufferViews[static_cast<size_t>(jointAccessor.bufferView)];
    const uint16_t* joints = reinterpret_cast<const uint16_t*>(model.buffers[0].data.data() + jointView.byteOffset + jointAccessor.byteOffset);
    double worst = 0.0;
    for (size_t v = 0; v < positions.size() / 3; v++) {
        double skinned[3] = {0, 0, 0};
        for (int k = 0; k < 4; k++) {
            const float w = weights[v * 4 + static_cast<size_t>(k)];
            if (w <= 0.0f) continue;
            const uint16_t joint = joints[v * 4 + static_cast<size_t>(k)];
            std::array<double, 16> ibm{};
            for (int i = 0; i < 16; i++) ibm[static_cast<size_t>(i)] = inverseBinds[static_cast<size_t>(joint) * 16 + static_cast<size_t>(i)];
            const std::array<double, 16> m = multiply(globals[static_cast<size_t>(skin.joints[joint])], ibm);
            for (int r = 0; r < 3; r++)
                skinned[r] += w * (m[0 * 4 + r] * positions[v * 3] + m[1 * 4 + r] * positions[v * 3 + 1] + m[2 * 4 + r] * positions[v * 3 + 2] + m[12 + r]);
        }
        double nearest = 1e30;
        for (const auto& e : expected) {
            nearest = std::min(nearest, std::sqrt((e[0] - skinned[0]) * (e[0] - skinned[0]) + (e[1] - skinned[1]) * (e[1] - skinned[1]) +
                                                  (e[2] - skinned[2]) * (e[2] - skinned[2])));
        }
        worst = std::max(worst, nearest);
    }
    EXPECT_LT(worst, 1e-4) << "the bind pose skins away from ufbx's evaluation";
}

}  // namespace FbxImportTest_cpp
