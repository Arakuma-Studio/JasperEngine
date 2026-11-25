// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The FBX import's warnings (S98 REQ-34, REQ-35; AC30, AC31): one Console line per model that
// counts them, the whole list kept as the model's import report, and Material Creation Mode None,
// which makes no material and so looks for no texture. The model is a handwritten ASCII FBX 7.4:
// a box whose five materials each name a diffuse texture, three of them missing and two present
// with a tiling.

#include <gtest/gtest.h>

#include "model/ModelImportSettings.h"
#include "util/FbxImporter.h"

#include "TempDir.h"

#include "stb_image_write.h"
#include "tiny_gltf.h"
#include "yaml-cpp/yaml.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    const char* const kTextureFiles[] = {"missing_wall.psd", "missing_floor.png", "missing_roof.tga", "tile_a.png", "tile_b.png"};

    std::string hallFbx() {
        std::string objects;
        std::string connections = "\tC: \"OO\",200,0\n\tC: \"OO\",100,200\n";
        for (int i = 0; i < 5; i++) {
            const std::string id = std::to_string(i);
            const bool tiled = i >= 3;
            objects += "\tMaterial: 30" + id + ", \"Material::M" + id + "\", \"\" {\n\t\tVersion: 102\n\t\tShadingModel: \"phong\"\n"
                       "\t\tMultiLayer: 0\n\t\tProperties70:  {\n\t\t\tP: \"DiffuseColor\", \"Color\", \"\", \"A\",0.8,0.8,0.8\n\t\t}\n\t}\n";
            objects += "\tTexture: 31" + id + ", \"Texture::T" + id + "\", \"\" {\n\t\tType: \"TextureVideoClip\"\n\t\tVersion: 202\n"
                       "\t\tTextureName: \"Texture::T" + id + "\"\n"
                       "\t\tFileName: \"textures/" + kTextureFiles[i] + "\"\n\t\tRelativeFilename: \"textures/" + kTextureFiles[i] + "\"\n"
                       "\t\tModelUVTranslation: 0,0\n\t\tModelUVScaling: " + (tiled ? "2,2" : "1,1") + "\n";
            if (tiled) objects += "\t\tProperties70:  {\n\t\t\tP: \"Scaling\", \"Vector\", \"\", \"A\",2,2,1\n\t\t}\n";
            objects += "\t}\n";
            connections += "\tC: \"OO\",30" + id + ",200\n\tC: \"OP\",31" + id + ",30" + id + ", \"DiffuseColor\"\n";
        }
        return std::string("; FBX 7.4.0 project file\n"
            "FBXHeaderExtension:  {\n\tFBXHeaderVersion: 1003\n\tFBXVersion: 7400\n\tCreator: \"JasperEngine tests\"\n}\n"
            "GlobalSettings:  {\n\tVersion: 1000\n\tProperties70:  {\n"
            "\t\tP: \"UpAxis\", \"int\", \"Integer\", \"\",1\n\t\tP: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n"
            "\t\tP: \"FrontAxis\", \"int\", \"Integer\", \"\",2\n\t\tP: \"FrontAxisSign\", \"int\", \"Integer\", \"\",1\n"
            "\t\tP: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n\t\tP: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n"
            "\t\tP: \"UnitScaleFactor\", \"double\", \"Number\", \"\",100\n\t}\n}\n"
            "Objects:  {\n"
            "\tGeometry: 100, \"Geometry::Box\", \"Mesh\" {\n"
            "\t\tVertices: *24 {\n\t\t\ta: -1,-1,-1,1,-1,-1,1,1,-1,-1,1,-1,-1,-1,1,1,-1,1,1,1,1,-1,1,1\n\t\t}\n"
            "\t\tPolygonVertexIndex: *24 {\n\t\t\ta: 0,3,2,-2,4,5,6,-8,0,1,5,-5,3,7,6,-3,0,4,7,-4,1,2,6,-6\n\t\t}\n"
            "\t\tGeometryVersion: 124\n"
            "\t\tLayerElementUV: 0 {\n\t\t\tVersion: 101\n\t\t\tName: \"UVMap\"\n\t\t\tMappingInformationType: \"ByPolygonVertex\"\n"
            "\t\t\tReferenceInformationType: \"IndexToDirect\"\n\t\t\tUV: *8 {\n\t\t\t\ta: 0,0,1,0,1,1,0,1\n\t\t\t}\n"
            "\t\t\tUVIndex: *24 {\n\t\t\t\ta: 0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3\n\t\t\t}\n\t\t}\n"
            "\t\tLayerElementMaterial: 0 {\n\t\t\tVersion: 101\n\t\t\tName: \"\"\n\t\t\tMappingInformationType: \"ByPolygon\"\n"
            "\t\t\tReferenceInformationType: \"IndexToDirect\"\n\t\t\tMaterials: *6 {\n\t\t\t\ta: 0,1,2,3,4,4\n\t\t\t}\n\t\t}\n"
            "\t\tLayer: 0 {\n\t\t\tVersion: 100\n"
            "\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementUV\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n"
            "\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementMaterial\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n\t\t}\n\t}\n"
            "\tModel: 200, \"Model::Hall\", \"Mesh\" {\n\t\tVersion: 232\n\t\tShading: T\n\t\tCulling: \"CullingOff\"\n\t}\n") +
            objects + "}\nConnections:  {\n" + connections + "}\n";
    }

    void writeText(const fs::path& path, const std::string& text) {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
    }

    void writePng(const fs::path& path) {
        fs::create_directories(path.parent_path());
        std::vector<unsigned char> rgba(4 * 4 * 4, 200);
        ASSERT_TRUE(stbi_write_png(path.string().c_str(), 4, 4, 4, rgba.data(), 16));
    }

    struct Hall {
        jasperengine::test::TempDir dir;
        fs::path assets = dir.path() / "assets";
        fs::path fbx = assets / "models" / "Hall.fbx";
        fs::path glb = dir.path() / ".jasperengine" / "imported" / "models" / "Hall.fbx.glb";

        Hall() {
            writeText(fbx, hallFbx());
            writePng(assets / "models" / "textures" / "tile_a.png");
            writePng(assets / "models" / "textures" / "tile_b.png");
        }

        size_t count(const std::vector<std::string>& warnings, const std::string& prefix) const {
            size_t n = 0;
            for (const std::string& warning : warnings) n += warning.rfind(prefix, 0) == 0 ? 1 : 0;
            return n;
        }

        tinygltf::Model load() const {
            tinygltf::Model model;
            tinygltf::TinyGLTF loader;
            loader.SetImagesAsIs(true);
            std::string err, warn;
            EXPECT_TRUE(loader.LoadBinaryFromFile(&model, &err, &warn, glb.string())) << err;
            return model;
        }
    };

}

TEST(FbxImporterWarnings, OneLinePerModel) {
    Hall hall;
    const FbxImporter::Result result = FbxImporter::import(hall.fbx, hall.glb, hall.assets, true);
    ASSERT_TRUE(result.success) << result.error;
    EXPECT_EQ(hall.count(result.warnings, "Texture not found: "), 3u);
    EXPECT_EQ(hall.count(result.warnings, "Texture tiling/offset is not imported: "), 2u);

    // the Console's one line: counts, two names then "+N"
    const std::string line = FbxImporter::summarizeWarnings("Hall.fbx", result.warnings);
    EXPECT_EQ(line.find('\n'), std::string::npos) << line;
    EXPECT_EQ(line.rfind("Hall.fbx: 3 textures not found (", 0), 0u) << line;
    EXPECT_NE(line.find(", +1)"), std::string::npos) << line;
    EXPECT_NE(line.find("tiling/offset not imported on 2 textures"), std::string::npos) << line;
    EXPECT_EQ(FbxImporter::summarizeWarnings("Hall.fbx", {}), "");

    // the import report holds all of them, as the import said them
    EXPECT_EQ(FbxImporter::getImportWarnings(hall.glb), result.warnings);
    EXPECT_GE(result.warnings.size(), 5u);
}

TEST(FbxImporterWarnings, MaterialCreationNoneWarnsNothing) {
    Hall hall;
    ASSERT_TRUE(FbxImporter::import(hall.fbx, hall.glb, hall.assets, true).success);
    EXPECT_EQ(FbxImporter::getState(hall.fbx, hall.glb, hall.assets), FbxImporter::State::UpToDate);
    EXPECT_EQ(hall.load().materials.size(), 5u);

    // Material Creation Mode None in the model's sidecar: the import is stale, then makes none
    writeText(fs::path(hall.fbx.string() + ".meta"), "import:\n  modelImporter:\n    version: 1\n    materialCreationMode: none\n");
    EXPECT_EQ(FbxImporter::getState(hall.fbx, hall.glb, hall.assets), FbxImporter::State::Stale) << "the setting changes the import";
    const FbxImporter::Result result = FbxImporter::import(hall.fbx, hall.glb, hall.assets, false);
    ASSERT_TRUE(result.success) << result.error;
    EXPECT_TRUE(result.warnings.empty()) << result.warnings.front();
    EXPECT_TRUE(FbxImporter::getImportWarnings(hall.glb).empty());
    EXPECT_EQ(FbxImporter::getState(hall.fbx, hall.glb, hall.assets), FbxImporter::State::UpToDate);
    const tinygltf::Model model = hall.load();
    EXPECT_TRUE(model.materials.empty());
    EXPECT_TRUE(model.images.empty());
    ASSERT_FALSE(model.meshes.empty());
    for (const tinygltf::Primitive& primitive : model.meshes[0].primitives) {
        EXPECT_LT(primitive.material, 0) << "every submesh draws with the default material";
    }
    EXPECT_GE(model.meshes[0].primitives.size(), 5u) << "the submeshes stay";

    // Import again: materials come back
    writeText(fs::path(hall.fbx.string() + ".meta"), "import:\n  modelImporter:\n    version: 1\n");
    EXPECT_EQ(FbxImporter::getState(hall.fbx, hall.glb, hall.assets), FbxImporter::State::Stale);
}

// S98 REQ-44: Project Settings > Mesh > Material Creation Mode is the mode of the FBX models whose
// sidecar sets none; a model's own mode wins over it
TEST(FbxImporterWarnings, ProjectMaterialCreationModeIsTheDefault) {
    Hall hall;
    ASSERT_TRUE(FbxImporter::import(hall.fbx, hall.glb, hall.assets, true).success);
    EXPECT_EQ(hall.load().materials.size(), 5u);

    struct RestoreDefault {
        ~RestoreDefault() { FbxImporter::setDefaultCreatesMaterials(true); }
    } restore;
    FbxImporter::setDefaultCreatesMaterials(false);
    EXPECT_EQ(FbxImporter::getState(hall.fbx, hall.glb, hall.assets), FbxImporter::State::Stale) << "the project's mode changes the import";
    const FbxImporter::Result result = FbxImporter::import(hall.fbx, hall.glb, hall.assets, false);
    ASSERT_TRUE(result.success) << result.error;
    EXPECT_TRUE(result.warnings.empty()) << result.warnings.front();
    EXPECT_TRUE(hall.load().materials.empty());

    // the model's own Import wins
    writeText(fs::path(hall.fbx.string() + ".meta"), "import:\n  modelImporter:\n    version: 1\n    materialCreationMode: import\n");
    EXPECT_EQ(FbxImporter::getState(hall.fbx, hall.glb, hall.assets), FbxImporter::State::Stale);
    ASSERT_TRUE(FbxImporter::import(hall.fbx, hall.glb, hall.assets, false).success);
    EXPECT_EQ(hall.load().materials.size(), 5u);

    // project.yaml's mesh: writes the mode only when it is None, and reads it back
    ModelProjectSettings settings;
    EXPECT_FALSE(encodeModelProjectSettings(settings)["materialCreationMode"]);
    settings.materialCreationMode = MaterialCreationMode::None;
    const YAML::Node encoded = encodeModelProjectSettings(settings);
    ASSERT_TRUE(encoded["materialCreationMode"]);
    EXPECT_EQ(encoded["materialCreationMode"].Scalar(), "none");
    ModelProjectSettings decoded;
    decodeModelProjectSettings(encoded, decoded);
    EXPECT_EQ(decoded.materialCreationMode, MaterialCreationMode::None);
}
