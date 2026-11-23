// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Android launcher icons at every density (docs/specs/S98-editor-tooling-gaps.md REQ-19, AC17):
// from one Launcher Icon, the five mipmap densities and a round icon; from the adaptive layers,
// their 108 dp images and the mipmap-anydpi-v26 XML. Both routes: a source export's Gradle project
// and a prebuilt package's staging (AndroidPackager, which compiles what the helper writes).

#include "support/ExporterTestAccess.h"
#include "support/TestTempDir.h"

#include "Exporter.h"
#include "export/ExportFiles.h"
#include "Project.h"
#include "platform/AndroidIcons.h"
#include "platform/AndroidPackager.h"
#include "util/PngWriter.h"

#include "stb_image.h"

#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using jasperengine::test::TestTempDir;

namespace android_icons_test {

    const fs::path androidIconsRepo = JASPER_TEST_REPO_DIR;

    // an opaque square of one colour
    void writeSquare(const fs::path& file, int size, unsigned char r, unsigned char g, unsigned char b, unsigned char a = 255) {
        std::vector<unsigned char> pixels(static_cast<size_t>(size) * size * 4);
        for (size_t i = 0; i < pixels.size(); i += 4) {
            pixels[i] = r;
            pixels[i + 1] = g;
            pixels[i + 2] = b;
            pixels[i + 3] = a;
        }
        fs::create_directories(file.parent_path());
        ASSERT_TRUE(writePng(file, size, size, 4, pixels.data(), size * 4, io::Durability::Cache));
    }

    struct Png {
        int width = 0, height = 0;
        std::vector<unsigned char> rgba;
        unsigned char alphaAt(int x, int y) const { return rgba[(static_cast<size_t>(y) * width + x) * 4 + 3]; }
    };

    Png readPng(const fs::path& file) {
        Png png;
        int channels = 0;
        unsigned char* pixels = stbi_load(file.string().c_str(), &png.width, &png.height, &channels, 4);
        if (pixels) {
            png.rgba.assign(pixels, pixels + static_cast<size_t>(png.width) * png.height * 4);
            stbi_image_free(pixels);
        }
        return png;
    }

    // every density of `name` at its size, the round one cut to a circle
    void expectDensities(const fs::path& res, const std::string& name) {
        for (const platform::AndroidIconDensity& density : platform::androidIconDensities()) {
            const Png icon = readPng(res / density.folder / (name + ".png"));
            EXPECT_EQ(icon.width, density.legacyPx) << density.folder;
            EXPECT_EQ(icon.height, density.legacyPx) << density.folder;
            const Png round = readPng(res / density.folder / (name + "_round.png"));
            ASSERT_EQ(round.width, density.legacyPx) << density.folder;
            EXPECT_EQ(round.alphaAt(0, 0), 0) << density.folder << ": the corner is outside the circle";
            EXPECT_EQ(round.alphaAt(density.legacyPx / 2, density.legacyPx / 2), 255) << density.folder;
            if (!icon.rgba.empty()) {
                EXPECT_EQ(icon.alphaAt(0, 0), 255) << density.folder << ": the square one keeps its corner";
            }
        }
    }

    std::unique_ptr<Project> androidIconsProjectIn(const fs::path& root) {
        fs::create_directories(root / "assets");
        std::ofstream(root / "project.yaml") << "name: IconGame\nassetsDir: assets\nluaDir: assets\n";
        auto project = std::make_unique<Project>();
        project->setSkipProjectLock(true);
        EXPECT_TRUE(project->loadProject(root, false));
        return project;
    }

}

using namespace android_icons_test;

// red before REQ-19: one res/drawable/ic_launcher.png, the template's own mipmaps beside it
TEST(AndroidIcons, WritesEveryDensityAndRound) {
    TestTempDir dir("android-icons");
    auto project = androidIconsProjectIn(dir / "game");
    writeSquare(dir / "game" / "icon.png", 1024, 200, 40, 40);
    project->getAndroidProjectSettings().launcherIcon = "icon.png";

    // the source export: the template's mipmap-*dpi/ic_launcher.png replaced, roundIcon declared
    const fs::path target = dir / "export";
    std::string error;
    ASSERT_TRUE(exportfiles::mirrorTree(androidIconsRepo / "engine" / "workspaces" / "androidstudio", target / "workspaces" / "androidstudio", true, error)) << error;
    const fs::path java = fs::path("platform") / "android" / "java" / "com" / "jaspergameengine" / "jasperengine" / "MainActivity.java";
    fs::create_directories((target / java).parent_path());
    fs::copy_file(androidIconsRepo / "engine" / java, target / java);
    Exporter exporter;
    ASSERT_TRUE(ExporterTestAccess::writeAndroidProjectSettings(exporter, project.get(), target, error)) << error;
    const fs::path res = target / "workspaces" / "androidstudio" / "app" / "src" / "main" / "res";
    expectDensities(res, "ic_launcher");
    EXPECT_FALSE(fs::exists(res / "drawable" / "ic_launcher.png"));
    const std::string manifest = TestTempDir::read(target / "workspaces" / "androidstudio" / "app" / "src" / "main" / "AndroidManifest.xml");
    EXPECT_NE(manifest.find("android:icon=\"@mipmap/ic_launcher\""), std::string::npos) << manifest;
    EXPECT_NE(manifest.find("android:roundIcon=\"@mipmap/ic_launcher_round\""), std::string::npos) << manifest;
    // the template's default icon is gone at every density: each one is the project's red
    const Png xxxhdpi = readPng(res / "mipmap-xxxhdpi" / "ic_launcher.png");
    ASSERT_FALSE(xxxhdpi.rgba.empty());
    EXPECT_EQ(xxxhdpi.rgba[0], 200);
    EXPECT_EQ(xxxhdpi.rgba[1], 40);

    // the prebuilt package: its own names (the player's resources keep ic_launcher), the manifest's
    // round icon an attribute of the runtime template's {{ICON}}
    const fs::path staging = dir / "staging" / "res";
    platform::AndroidIcons icons;
    ASSERT_TRUE(platform::writeAndroidLauncherIcons(project->getAndroidProjectSettings(), project->getProjectPath(), staging, "ic_launcher_game",
                                                    icons, error)) << error;
    expectDensities(staging, "ic_launcher_game");
    EXPECT_EQ(icons.files.size(), 10u);
    platform::AndroidPackage package;
    package.settings = project->getAndroidProjectSettings();
    std::string packaged;
    ASSERT_TRUE(platform::writeAndroidManifest(package, TestTempDir::read(androidIconsRepo / "engine" / "platform" / "android" / "player" / "AndroidManifest.template.xml"),
                                               packaged, error)) << error;
    EXPECT_NE(packaged.find("android:icon=\"@mipmap/ic_launcher_game\""), std::string::npos) << packaged;
    EXPECT_NE(packaged.find("android:roundIcon=\"@mipmap/ic_launcher_game_round\""), std::string::npos) << packaged;
}

// the adaptive layers at 108 dp, with the legacy and round icons composed from them when no
// Launcher Icon is set
TEST(AndroidIcons, AdaptiveLayersAtEveryDensity) {
    TestTempDir dir("android-icons-adaptive");
    writeSquare(dir / "fg.png", 432, 255, 255, 255, 0);
    writeSquare(dir / "bg.png", 512, 0, 0, 255);
    AndroidProjectSettings settings;
    settings.adaptiveIconForeground = "fg.png";
    settings.adaptiveIconBackground = "bg.png";
    platform::AndroidIcons icons;
    std::string error;
    ASSERT_TRUE(platform::writeAndroidLauncherIcons(settings, dir.path(), dir / "res", "ic_launcher", icons, error)) << error;
    for (const platform::AndroidIconDensity& density : platform::androidIconDensities()) {
        for (const char* layer : {"_foreground.png", "_background.png"}) {
            const Png png = readPng(dir / "res" / density.folder / (std::string("ic_launcher") + layer));
            EXPECT_EQ(png.width, density.adaptivePx) << density.folder << layer;
            EXPECT_EQ(png.height, density.adaptivePx) << density.folder << layer;
        }
    }
    expectDensities(dir / "res", "ic_launcher");
    // the legacy icon is the background seen through the transparent foreground
    const Png legacy = readPng(dir / "res" / "mipmap-mdpi" / "ic_launcher.png");
    ASSERT_FALSE(legacy.rgba.empty());
    EXPECT_EQ(legacy.rgba[2], 255);
    for (const char* xml : {"ic_launcher.xml", "ic_launcher_round.xml"}) {
        const std::string text = TestTempDir::read(dir / "res" / "mipmap-anydpi-v26" / xml);
        EXPECT_NE(text.find("@mipmap/ic_launcher_foreground"), std::string::npos) << text;
        EXPECT_NE(text.find("@mipmap/ic_launcher_background"), std::string::npos) << text;
    }
    EXPECT_EQ(icons.roundIcon, "@mipmap/ic_launcher_round");

    // the adaptive icon cleared: an earlier export's XML would still win on Android 8+
    settings.adaptiveIconForeground.clear();
    settings.launcherIcon = "bg.png";
    ASSERT_TRUE(platform::writeAndroidLauncherIcons(settings, dir.path(), dir / "res", "ic_launcher", icons, error)) << error;
    EXPECT_FALSE(fs::exists(dir / "res" / "mipmap-anydpi-v26" / "ic_launcher.xml"));
}
