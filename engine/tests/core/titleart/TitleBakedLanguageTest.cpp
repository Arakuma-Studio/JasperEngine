// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A Baked title switched to another language on the headless device, every frame captured
// (docs/specs/S87-title-localisation-interchange.md AC7, R15, R27): the previous language draws
// until the new set's texture has loaded (no blank frame, no other picture in between),
// onLanguageReady fires on the first German frame, and the Japanese set is never requested
// (the texture data pool names the sets that were asked for). The sets are solid-colour PNGs so
// that every frame is identified by its centre pixel. Texture loads are asynchronous, as in a
// game: the frame that asks for the new set cannot show it, so the switch always spans a frame.

#include "Engine.h"
#include "Scene.h"
#include "component/TitleArtRendererComponent.h"
#include "object/Camera.h"
#include "object/TitleArt.h"
#include "object/ui/Image.h"
#include "pool/TextureDataPool.h"
#include "subsystem/RenderSystem.h"

#include "GpuEnvironment.h"
#include "ImageCompare.h"
#include "Readback.h"
#include "support/TestTempDir.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace jasperengine;
using namespace jasperengine::test;
namespace fs = std::filesystem;

namespace {

    const std::vector<uint8_t> EN = {255, 40, 40, 255}, DE = {40, 255, 40, 255}, JA = {40, 40, 255, 255};
    const std::vector<uint8_t> CLEAR = {255, 0, 255, 255};

    void writeSolidPng(const fs::path& path, const std::vector<uint8_t>& colour, int size = 64) {
        std::vector<uint8_t> rgba;
        for (int i = 0; i < size * size; i++) rgba.insert(rgba.end(), colour.begin(), colour.end());
        ASSERT_TRUE(writePng(path.string(), rgba, size, size));
    }

    void writeText(const fs::path& path, const std::string& text) {
        std::ofstream out(path, std::ios::binary);
        out << text;
    }

    std::string sidecar(const std::string& png, const std::string& languages = "") {
        return std::string("{\"format\": \"jasperengine.titleart.bakeinfo\", \"version\": 1, \"frame\": [0, 0, 64, 64], \"outputs\": [") +
               "{\"id\": \"main\", \"file\": \"" + png + "\", \"content\": \"all\", \"size\": [64, 64]}]" +
               (languages.empty() ? "" : ", \"languages\": " + languages) + "}";
    }

    class TitleBakedLanguageGpu : public ::testing::Test {
    protected:
        void SetUp() override {
            JASPER_REQUIRE_GPU();
            GpuEnvironment::resetView();
            scene.setBackgroundColor(1.0f, 0.0f, 1.0f, 1.0f);
            camera.setType(CameraType::CAMERA_ORTHO);
            camera.activate();
            Engine::setScene(&scene);
        }
        void TearDown() override { Engine::removeScene(&scene); }

        Scene scene;
        Camera camera{&scene};

        std::vector<uint8_t> centre() {
            Framebuffer& target = GpuEnvironment::target();
            const int width = int(target.getWidth()), height = int(target.getHeight());
            std::vector<uint8_t> rgba;
            EXPECT_TRUE(readTexture(target.getRender().getColorTexture(), width, height, rgba));
            const size_t at = (size_t)((height / 2) * width + width / 2) * 4;
            return std::vector<uint8_t>(rgba.begin() + at, rgba.begin() + at + 4);
        }

        static bool sameColour(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
            int worst = 0;
            for (size_t i = 0; i < 3; i++) worst = std::max(worst, std::abs((int)a[i] - (int)b[i]));
            return worst <= 16;
        }
    };

}

TEST_F(TitleBakedLanguageGpu, SwitchShowsThePreviousLanguageUntilTheNewSetIsLoaded) {
    struct AsyncScope {
        bool was = Engine::isAsyncLoading();
        AsyncScope() { Engine::setAsyncLoading(true); }
        ~AsyncScope() { Engine::setAsyncLoading(was); }
    } asyncScope;
    TestTempDir dir("title_baked_language");
    const fs::path titles = dir.path();
    writeSolidPng(titles / "logo.titleart.png", EN);
    writeSolidPng(titles / "logo.titleart.de.png", DE);
    writeSolidPng(titles / "logo.titleart.ja.png", JA);
    writeText(titles / "logo.titleart", "{}");
    writeText(titles / "logo.titleart.bake.json", sidecar("logo.titleart.png", R"({"source": "en", "sets": {"de": "logo.titleart.de.bake.json", "ja": "logo.titleart.ja.bake.json"}})"));
    writeText(titles / "logo.titleart.de.bake.json", sidecar("logo.titleart.de.png"));
    writeText(titles / "logo.titleart.ja.bake.json", sidecar("logo.titleart.ja.png"));

    Image logo(&scene);
    logo.setAnchorPreset(AnchorPreset::CENTER);
    logo.setSize(320, 180);
    TitleArtRendererComponent title;
    title.title = AssetRef((titles / "logo.titleart").string());
    title.renderMode = TitleArtRenderMode::BAKED;
    scene.addComponent<TitleArtRendererComponent>(logo.getEntity(), std::move(title));

    TitleArt art(&scene, logo.getEntity());
    int readyCount = 0;
    int readyFrame = -1;
    int frame = 0;
    art.getOnLanguageReady().add("test", [&](const std::string& code) {
        readyCount++;
        readyFrame = frame;
        EXPECT_EQ(code, "de");
    });

    ASSERT_TRUE(GpuEnvironment::runFrames([&]() { return sameColour(centre(), EN); }, 600)) << "the English set never drew";
    ASSERT_TRUE(sameColour(centre(), EN));

    art.setLanguage("de");
    int firstGerman = -1;
    for (frame = 0; frame < 1200 && firstGerman < 0; frame++) {
        GpuEnvironment::drawFrames(1);
        const std::vector<uint8_t> pixel = centre();
        if (sameColour(pixel, DE)) {
            firstGerman = frame;
            continue;
        }
        ASSERT_TRUE(sameColour(pixel, EN)) << "frame " << frame << " of the switch shows neither set: rgba " << (int)pixel[0] << " " << (int)pixel[1] << " "
                                     << (int)pixel[2] << " (no blank frame, no other language)";
    }
    ASSERT_GE(firstGerman, 0) << "the German set never showed";
    ASSERT_GE(firstGerman, 1) << "the frame that asks for the set cannot show it: the load is asynchronous";
    std::printf("[title baked language] the switch showed English for %d frame(s)\n", firstGerman);
    EXPECT_EQ(readyCount, 1) << "onLanguageReady fires once";
    EXPECT_EQ(readyFrame, firstGerman) << "onLanguageReady fires on the first German frame";
    EXPECT_EQ(art.getResolvedLanguage(), "de");
    EXPECT_TRUE(art.isLanguageReady());

    // the data pool holds what was asked for: German, never Japanese
    bool sawGerman = false;
    bool sawJapanese = false;
    for (const TextureResidentInfo& texture : TextureDataPool::getResidentTextures()) {
        sawGerman = sawGerman || texture.id.find("logo.titleart.de.png") != std::string::npos;
        sawJapanese = sawJapanese || texture.id.find("logo.titleart.ja.png") != std::string::npos;
    }
    EXPECT_TRUE(sawGerman) << "the pool holds the German set (the check sees requested textures)";
    EXPECT_FALSE(sawJapanese) << "the Japanese set was never requested";
    GpuEnvironment::drawFrames(2);
    EXPECT_TRUE(sameColour(centre(), DE));
}
