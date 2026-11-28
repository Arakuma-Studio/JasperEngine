// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Tolerant .mtl texture paths (spec S94 F46, R23): a .mtl written on Windows names its textures
// with backslashes, a drive letter or an absolute path. The OBJ load finds them relative to the
// .mtl, then by file name beside it, and logs one warning per texture with the path it used.

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "object/Model.h"

#include "LogCapture.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>

namespace MeshSystemObjTest_cpp {

using namespace jasperengine;
using namespace jasperengine::test;
namespace fs = std::filesystem;

TEST(MeshSystemObjTest, MtlWindowsPaths){
    const fs::path dir = fs::temp_directory_path() / ("jasperengine-mtl-paths-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(dir);
    fs::create_directories(dir / "tex");
    std::ofstream(dir / "flag.obj") << "mtllib flag.mtl\n"
                                       "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nvt 1 1\nvn 0 0 1\n"
                                       "usemtl Cloth\nf 1/1/1 2/2/1 3/3/1\nusemtl Pole\nf 2/2/1 4/4/1 3/3/1\n";
    std::ofstream(dir / "flag.mtl") << "newmtl Cloth\nKd 1 1 1\nmap_Kd C:\\art\\tex\\a.png\n"
                                       "newmtl Pole\nKd 1 1 1\nmap_Kd tex\\b.png\n";
    // a 1x1 PNG is not needed: the path is what is checked, the files only have to exist
    std::ofstream(dir / "a.png") << "png";
    std::ofstream(dir / "tex" / "b.png") << "png";

    LogCapture log;
    Scene scene;
    Model model(&scene);
    ASSERT_TRUE(model.loadModel((dir / "flag.obj").string()));
    const MeshComponent& mesh = scene.getComponent<MeshComponent>(model.getEntity());
    ASSERT_EQ(mesh.numSubmeshes, 2u);
    const std::string base = (dir / "").string();
    // the drive path's file name beside the .mtl; backslashes are folders
    const std::set<std::string> paths = {mesh.submeshes[0].material.baseColorTexture.getPath(), mesh.submeshes[1].material.baseColorTexture.getPath()};
    EXPECT_EQ(paths, (std::set<std::string>{base + "a.png", base + "tex/b.png"}));
    EXPECT_EQ(log.count(D_LOG_WARN, "C:\\art\\tex\\a.png"), 1u);
    EXPECT_EQ(log.count(D_LOG_WARN, "tex\\b.png"), 1u);
    fs::remove_all(dir);
}

}  // namespace MeshSystemObjTest_cpp
