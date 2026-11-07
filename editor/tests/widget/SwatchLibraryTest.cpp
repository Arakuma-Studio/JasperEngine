// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The swatch libraries (docs/specs/S27-editor-tools.md R5, AC14)

#include "widget/SwatchLibrary.h"

#include "support/TestTempDir.h"

#include <gtest/gtest.h>

#include <fstream>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;
using jasperengine::test::TestTempDir;

TEST(SwatchLibrary, Io) {
    TestTempDir dir("swatches");
    const fs::path user = swatches::userDirectory(dir.path() / "settings");
    const fs::path project = swatches::projectDirectory(dir.path() / "Game");
    EXPECT_EQ(project, dir.path() / "Game" / "EditorSettings" / "Swatches");

    // no user library yet: "Default" is seeded and written
    std::vector<std::string> errors;
    std::vector<SwatchLibrary> all = swatches::loadAll(user, project, &errors);
    EXPECT_TRUE(errors.empty());
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].name, "Default");
    EXPECT_EQ(all[0].swatches.size(), 19u);   // black, white, grey and 16 hues
    EXPECT_TRUE(fs::exists(user / "Default.colors"));

    // a project library: create, add, persist
    SwatchLibrary palette;
    std::string error;
    ASSERT_TRUE(swatches::createLibrary(project, LibraryScope::Project, "Level Art", palette, error)) << error;
    EXPECT_FALSE(swatches::createLibrary(project, LibraryScope::Project, "Level Art", palette, error));
    ASSERT_TRUE(swatches::add(palette, {"Sky", Vector4(0.1f, 0.4f, 0.9f, 1.0f), false}, error));
    ASSERT_TRUE(swatches::add(palette, {"Lava", Vector4(6.0f, 1.0f, 0.0f, 1.0f), true}, error));
    palette.gradients.push_back({"Sunset", Gradient::twoColors(Vector3(1, 0.5f, 0), Vector3(0.2f, 0, 0.5f))});
    ASSERT_TRUE(swatches::write(palette, error)) << error;
    all = swatches::loadAll(user, project);
    ASSERT_EQ(all.size(), 2u);
    const SwatchLibrary& read = all[1];
    EXPECT_EQ(read.scope, LibraryScope::Project);
    ASSERT_EQ(read.swatches.size(), 2u);
    EXPECT_EQ(read.swatches[1], (Swatch{"Lava", Vector4(6.0f, 1.0f, 0.0f, 1.0f), true}));
    ASSERT_EQ(read.gradients.size(), 1u);
    EXPECT_EQ(read.gradients[0].gradient, palette.gradients[0].gradient);

    // edits
    SwatchLibrary edited = read;
    ASSERT_TRUE(swatches::move(edited, 1, -1, error));
    EXPECT_EQ(edited.swatches[0].name, "Lava");
    EXPECT_FALSE(swatches::move(edited, 0, -1, error));
    ASSERT_TRUE(swatches::rename(edited, 0, "Magma", error));
    ASSERT_TRUE(swatches::replace(edited, 1, Vector4(0, 0, 0, 1), false, error));
    ASSERT_TRUE(swatches::remove(edited, 0, error));
    EXPECT_FALSE(swatches::remove(edited, 5, error));
    EXPECT_EQ(edited.swatches.size(), 1u);
    // full at 76
    SwatchLibrary full;
    full.name = "Full";
    for (size_t i = 0; i < SwatchLibrary::kMaxSwatches; i++) ASSERT_TRUE(swatches::add(full, {}, error));
    EXPECT_FALSE(swatches::add(full, {}, error));
    // rename moves the file
    ASSERT_TRUE(swatches::renameLibrary(edited, "Night", error)) << error;
    EXPECT_TRUE(fs::exists(project / "Night.colors"));
    EXPECT_FALSE(fs::exists(project / "Level Art.colors"));
    ASSERT_TRUE(swatches::deleteLibrary(edited, error));
    EXPECT_FALSE(fs::exists(project / "Night.colors"));
}

TEST(SwatchLibrary, UnknownVersionIsReadOnly) {
    TestTempDir dir("swatches-version");
    const fs::path file = dir.path() / "Future.colors";
    {
        std::ofstream out(file);
        out << R"({"version": 9, "name": "Future", "swatches": [{"name": "A", "linear": [1, 0, 0, 1], "hdr": false}]})";
    }
    SwatchLibrary library;
    std::string error;
    ASSERT_TRUE(swatches::read(file, LibraryScope::User, library, error)) << error;
    EXPECT_TRUE(library.readOnly);
    EXPECT_FALSE(library.warning.empty());
    EXPECT_EQ(library.swatches.size(), 1u);
    EXPECT_FALSE(swatches::add(library, {}, error));
    EXPECT_FALSE(swatches::write(library, error));
    // malformed files are reported
    {
        std::ofstream out(file);
        out << R"({"version": 1, "swatches": [{"linear": [1, 0]}]})";
    }
    EXPECT_FALSE(swatches::read(file, LibraryScope::User, library, error));
    EXPECT_NE(error.find("Future.colors"), std::string::npos);
}

TEST(SwatchLibrary, FileNames) {
    EXPECT_EQ(swatches::libraryPath("/x", "My: Colors?").filename().string(), "My_ Colors_.colors");
    EXPECT_EQ(swatches::libraryPath("/x", "..").filename().string(), "_.colors");
}

// S90 R26: a preset saved from a graph's 32-key ramp comes back with its 32 keys (the particles' 8
// are a field's limit, not the library's)
TEST(SwatchLibrary, AGradientPresetKeepsAGraphRampsKeys) {
    TestTempDir dir("swatches-ramp");
    const fs::path project = swatches::projectDirectory(dir.path() / "Game");
    SwatchLibrary library;
    std::string error;
    ASSERT_TRUE(swatches::createLibrary(project, LibraryScope::Project, "Ramps", library, error)) << error;
    Gradient ramp;
    for (int i = 0; i < 32; i++) {
        const float t = (float)i / 31.0f;
        ramp.colorKeys.push_back({t, Vector3(t, 1.0f - t, 0.5f)});
        ramp.alphaKeys.push_back({t, 1.0f - t * 0.5f});
    }
    library.gradients.push_back({"Ramp 32", ramp});
    ASSERT_TRUE(swatches::write(library, error)) << error;
    SwatchLibrary read;
    ASSERT_TRUE(swatches::read(project / "Ramps.colors", LibraryScope::Project, read, error)) << error;
    ASSERT_EQ(read.gradients.size(), 1u);
    EXPECT_EQ(read.gradients[0].gradient.colorKeys.size(), 32u);
    EXPECT_EQ(read.gradients[0].gradient.alphaKeys.size(), 32u);
    EXPECT_EQ(read.gradients[0].gradient, ramp);
}
