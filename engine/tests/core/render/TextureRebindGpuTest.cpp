// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// An image whose pixels are replaced after it was bound is bound again by whatever holds it
// (TexturePool::getRebindEpoch, Texture::isRenderOutdated; meshes: TextureLateBindGpuTest): a UI
// image, Points, a sky (its drawing and the lighting made of it), a terrain's height and blend
// maps. Each case draws what holds a file from the start with the pixels it must end up with (the
// reference), then what holds a file of other pixels, and takes the new pixels of that file as the
// hot reload does (reload::reloadTextures). What holds it must end up as the reference. The rest of
// a progressive load (a cooked file's levels past its tail, read after the tail was bound) is the
// same replacement, through a real cooked load. glcore and Vulkan.

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "io/FileData.h"
#include "jobs/JobSystem.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Points.h"
#include "object/Primitive.h"
#include "object/Terrain.h"
#include "object/environment/Skybox.h"
#include "object/ui/Image.h"
#include "pool/TextureDataPool.h"
#include "pool/TexturePool.h"
#include "render/SystemRender.h"
#include "render/TextureRender.h"
#include "subsystem/RenderSystem.h"
#include "texture/CookedTextures.h"
#include "texture/Ktx2.h"
#include "texture/Material.h"
#include "texture/Texture.h"
#include "texture/TextureData.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"
#include "Readback.h"
#include "TempDir.h"

#include "stb_image_write.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    struct Rgba { uint8_t r, g, b, a; };
    constexpr Rgba RED{230, 40, 30, 255};
    constexpr Rgba BLUE{30, 60, 230, 255};
    constexpr Rgba GREY_BRIGHT{240, 240, 240, 255};
    constexpr Rgba GREY_DARK{40, 40, 40, 255};
    constexpr Rgba WHITE{255, 255, 255, 255};
    constexpr Rgba BLACK{0, 0, 0, 255};

    std::vector<uint8_t> readTarget(){
        Framebuffer& target = GpuEnvironment::target();
        std::vector<uint8_t> rgba;
        readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba);
        return rgba;
    }

    // mean over the pixels of the largest channel difference (0..255)
    double meanDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b){
        if (a.size() != b.size() || a.empty())
            return 255.0;
        double sum = 0.0;
        const size_t pixels = a.size() / 4;
        for (size_t i = 0; i < pixels; i++){
            int largest = 0;
            for (int c = 0; c < 3; c++)
                largest = std::max(largest, std::abs((int)a[i * 4 + c] - (int)b[i * 4 + c]));
            sum += largest;
        }
        return sum / (double)pixels;
    }

    void writePng(const std::string& path, Rgba color){
        std::vector<uint8_t> pixels(16 * 16 * 4);
        for (size_t i = 0; i < pixels.size(); i += 4){
            pixels[i] = color.r;
            pixels[i + 1] = color.g;
            pixels[i + 2] = color.b;
            pixels[i + 3] = color.a;
        }
        ASSERT_TRUE(stbi_write_png(path.c_str(), 16, 16, 4, pixels.data(), 16 * 4)) << path;
    }

    // the file's new pixels as the hot reload takes them (reload::reloadTextures): decoded, in the
    // pool, the GPU texture stale; false when nothing loaded reads the file
    bool reloadFile(const std::string& path, Rgba color){
        writePng(path, color);
        bool found = false;
        for (const std::string& id : TextureDataPool::findIdsUsingFile(FileData::getSystemPath(path))){
            TextureSource source;
            if (!TextureDataPool::getSource(id, source))
                continue;
            TextureDataPool::replace(id, TextureDataPool::shareDecoded(TextureDataPool::decode(id, source)));
            TexturePool::invalidate(id);
            found = true;
        }
        return found;
    }

    // a scene with a perspective camera looking at the origin from above and in front, set as the
    // engine's scene while it lives
    struct Stage {
        Scene scene;
        Camera camera{&scene};

        Stage(){
            scene.setBackgroundColor(0.1f, 0.1f, 0.12f, 1.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 2.2f, 2.6f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            Engine::setScene(&scene);
        }

        ~Stage(){
            Engine::removeScene(&scene);
        }
    };

    // what a case keeps alive while it draws
    using Keep = std::vector<std::shared_ptr<void>>;

    class TextureRebindGpu : public ::testing::Test {
    protected:
        TempDir dir;
        Scene* scene = nullptr;

        std::string png(const std::string& name, Rgba color){
            const std::string path = dir.file(name + ".png").generic_string();
            writePng(path, color);
            return path;
        }

        bool settled(){
            Scene* current = scene;
            const bool loaded = GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded() && TextureDataPool::getLoadingCount() == 0;
            }, 600);
            GpuEnvironment::drawFrames(3);
            return loaded;
        }

        // the reference with the final pixels from the start; then the holder of a file of other
        // pixels, its file reloaded after it was drawn
        void expectRebinds(const char* what, Rgba first, Rgba final, const std::function<void(Stage&, const std::string& path, Rgba color, Keep& keep)>& make,
                           const std::function<void(Scene&)>& atTheEnd = nullptr){
            std::vector<uint8_t> reference;
            {
                Stage stage;
                scene = &stage.scene;
                Keep keep;
                make(stage, dir.file(std::string(what) + "-final.png").generic_string(), final, keep);
                ASSERT_FALSE(HasFatalFailure());
                ASSERT_TRUE(settled()) << what << ": the reference never finished loading";
                reference = readTarget();
            }

            Stage stage;
            scene = &stage.scene;
            Keep keep;
            const std::string path = dir.file(std::string(what) + "-first.png").generic_string();
            make(stage, path, first, keep);
            ASSERT_FALSE(HasFatalFailure());
            ASSERT_TRUE(settled()) << what << ": the first image never finished loading";
            const std::vector<uint8_t> before = readTarget();
            ASSERT_GT(meanDifference(before, reference), 1.0) << what << ": the two images draw alike (the case shows nothing)";

            ASSERT_TRUE(reloadFile(path, final)) << what << ": nothing loaded reads " << path;
            ASSERT_TRUE(settled()) << what << ": the new image never finished loading";
            const std::vector<uint8_t> after = readTarget();
            EXPECT_LT(meanDifference(after, reference), 1.0) << what << ": it still draws the old image (mean difference to the old one "
                                                             << meanDifference(after, before) << ")";
            if (atTheEnd)
                atTheEnd(stage.scene);
            EXPECT_NO_ENGINE_ERRORS();
        }
    };

}

TEST_F(TextureRebindGpu, AUiImageBindsANewImage){
    JASPER_REQUIRE_GPU();
    expectRebinds("ui", RED, BLUE, [](Stage& stage, const std::string& path, Rgba color, Keep& keep){
        writePng(path, color);
        stage.camera.setType(CameraType::CAMERA_UI);
        stage.camera.setPosition(0.0f, 0.0f, 1.0f);
        auto image = std::make_shared<Image>(&stage.scene);
        image->setTexture(path);
        image->setAnchorPreset(AnchorPreset::CENTER);
        image->setSize(256, 256);
        keep.push_back(image);
    });
}

TEST_F(TextureRebindGpu, PointsBindANewImage){
    JASPER_REQUIRE_GPU();
    expectRebinds("points", RED, BLUE, [](Stage& stage, const std::string& path, Rgba color, Keep& keep){
        writePng(path, color);
        auto points = std::make_shared<Points>(&stage.scene);
        points->setTexture(path);
        points->addPoint(Vector3(0.0f, 0.0f, 0.0f), Vector4(1.0f, 1.0f, 1.0f, 1.0f), 200.0f);
        keep.push_back(points);
    });
}

TEST_F(TextureRebindGpu, ASkyBindsANewImage){
    JASPER_REQUIRE_GPU();
    expectRebinds("sky", RED, BLUE, [](Stage& stage, const std::string& path, Rgba color, Keep& keep){
        writePng(path, color);
        auto sky = std::make_shared<Skybox>(&stage.scene);
        sky->setTextures(path, path, path, path, path, path);
        keep.push_back(sky);
    });
}

// The sky lights what is under it: a surface takes the new image's lighting too
TEST_F(TextureRebindGpu, ASkysLightingTakesANewImage){
    JASPER_REQUIRE_GPU();
    Entity surface = NULL_ENTITY;
    expectRebinds("sky-lighting", GREY_DARK, GREY_BRIGHT, [&surface](Stage& stage, const std::string& path, Rgba color, Keep& keep){
        writePng(path, color);
        auto sky = std::make_shared<Skybox>(&stage.scene);
        sky->setTextures(path, path, path, path, path, path);
        keep.push_back(sky);
        auto plane = std::make_shared<Primitive>(&stage.scene);
        plane->createPlane(3.0f, 3.0f);
        Material material;
        material.baseColorFactor = Vector4(0.9f, 0.9f, 0.9f, 1.0f);
        material.metallicFactor = 0.0f;
        material.roughnessFactor = 0.8f;
        plane->setMaterial(material);
        stage.scene.getComponent<MeshComponent>(plane->getEntity()).receiveIBL = true;
        surface = plane->getEntity();
        keep.push_back(plane);
    }, [&surface](Scene& scene){
        // (ShaderPool::getMeshProperties: 'Ibl')
        EXPECT_NE(scene.getComponent<MeshComponent>(surface).submeshes[0].shaderProperties & (1u << 19), 0u) << "the sky does not light the surface: the case shows nothing";
    });
}

// A cook of six files swapped in (TextureCookQueue::collectSwaps): the same image, the GPU's format,
// nothing the CPU reads. The sky draws it; the maps made of its source keep lighting the surface.
TEST_F(TextureRebindGpu, ASkysLightingStaysWhenACookComesInThatTheCpuCannotRead){
    JASPER_REQUIRE_GPU();
    const std::string path = dir.file("sky-cook.png").generic_string();
    writePng(path, GREY_BRIGHT);
    Stage stage;
    scene = &stage.scene;
    Skybox sky(&stage.scene);
    sky.setTextures(path, path, path, path, path, path);
    Primitive plane(&stage.scene);
    plane.createPlane(3.0f, 3.0f);
    Material material;
    material.baseColorFactor = Vector4(0.9f, 0.9f, 0.9f, 1.0f);
    material.metallicFactor = 0.0f;
    material.roughnessFactor = 0.8f;
    plane.setMaterial(material);
    stage.scene.getComponent<MeshComponent>(plane.getEntity()).receiveIBL = true;
    ASSERT_TRUE(settled());
    const std::vector<uint8_t> before = readTarget();

    // the same pixels as a cook's: BC1 blocks of the one colour, nothing plain to read
    if (!SystemRender::getTextureFormatSupport()(GpuTextureFormat::BC1))
        GTEST_SKIP() << "the device samples no BC1";
    std::vector<std::string> ids = TextureDataPool::findIdsUsingFile(FileData::getSystemPath(path));
    ASSERT_FALSE(ids.empty());
    const uint16_t grey565 = (uint16_t)(((GREY_BRIGHT.r >> 3) << 11) | ((GREY_BRIGHT.g >> 2) << 5) | (GREY_BRIGHT.b >> 3));
    std::vector<uint8_t> blocks(4 * 4 * 8, 0);
    for (size_t b = 0; b < blocks.size(); b += 8){
        blocks[b] = blocks[b + 2] = (uint8_t)(grey565 & 0xFF);
        blocks[b + 1] = blocks[b + 3] = (uint8_t)(grey565 >> 8);
    }
    auto faces = std::make_shared<std::array<TextureData, 6>>();
    for (int f = 0; f < 6; f++)
        faces->at(f).setLevels(GpuTextureFormat::BC1, true, 1, 16, 16, {blocks});
    TextureDataPool::replace(ids[0], faces);
    TexturePool::invalidate(ids[0]);
    ASSERT_TRUE(settled());
    // (the sky's own pixels are BC1's rounding of the colour: the surface's are what is compared, its middle)
    const std::vector<uint8_t> after = readTarget();
    const int width = (int)GpuEnvironment::target().getWidth();
    const int height = (int)GpuEnvironment::target().getHeight();
    double surface = 0.0;
    for (int y = height / 2 - 8; y < height / 2 + 8; y++){
        for (int x = width / 2 - 8; x < width / 2 + 8; x++)
            surface += std::abs((int)after[(size_t)(y * width + x) * 4] - (int)before[(size_t)(y * width + x) * 4]);
    }
    EXPECT_LT(surface / (16.0 * 16.0), 1.0) << "the surface lost its lighting with the cook";
    EXPECT_NE(stage.scene.getComponent<MeshComponent>(plane.getEntity()).submeshes[0].shaderProperties & (1u << 19), 0u) << "the sky does not light the surface anymore";
    EXPECT_NO_ENGINE_ERRORS();
}

namespace {
    // a terrain under the camera: its first layer red, its second blue, weighted by its blend map
    std::shared_ptr<Terrain> makeTerrain(Stage& stage, const std::string& heightMap, const std::string& blendMap, const std::string& red, const std::string& blue){
        auto terrain = std::make_shared<Terrain>(&stage.scene);
        terrain->setSize(4.0f);
        terrain->setMaxHeight(1.5f);
        terrain->setHeightMap(heightMap);
        terrain->setBlendMap(blendMap);
        terrain->setTextureLayer(0, red);
        terrain->setTextureLayer(1, blue);
        return terrain;
    }
}

TEST_F(TextureRebindGpu, ATerrainsHeightMapBindsANewImage){
    JASPER_REQUIRE_GPU();
    const std::string blendMap = png("terrain-height-blend", Rgba{255, 0, 0, 255});
    const std::string red = png("terrain-height-red", RED);
    const std::string blue = png("terrain-height-blue", BLUE);
    expectRebinds("terrain-height", BLACK, WHITE, [&](Stage& stage, const std::string& path, Rgba color, Keep& keep){
        writePng(path, color);
        keep.push_back(makeTerrain(stage, path, blendMap, red, blue));
    });
}

TEST_F(TextureRebindGpu, ATerrainsBlendMapBindsANewImage){
    JASPER_REQUIRE_GPU();
    const std::string heightMap = png("terrain-blend-height", BLACK);
    const std::string red = png("terrain-blend-red", RED);
    const std::string blue = png("terrain-blend-blue", BLUE);
    expectRebinds("terrain-blend", Rgba{255, 0, 0, 255}, Rgba{0, 255, 0, 255}, [&](Stage& stage, const std::string& path, Rgba color, Keep& keep){
        writePng(path, color);
        keep.push_back(makeTerrain(stage, heightMap, path, red, blue));
    });
}

namespace {
    // a cook of `size` px and its whole chain, as the raw writer stores it: the levels of at most
    // 64 px (the tail a progressive load reads first) one colour, the rest another
    std::vector<uint8_t> progressiveKtx2(int size, Rgba top, Rgba tail, uint64_t& tailBytes){
        std::vector<std::vector<uint8_t>> levels;
        for (int s = size; ; s /= 2){
            const Rgba color = s <= 64 ? tail : top;
            std::vector<uint8_t> level((size_t)s * s * 4);
            for (size_t i = 0; i < level.size(); i += 4){
                level[i] = color.r;
                level[i + 1] = color.g;
                level[i + 2] = color.b;
                level[i + 3] = color.a;
            }
            levels.push_back(std::move(level));
            if (s == 1)
                break;
        }
        Ktx2::WriteDesc desc;
        desc.format = GpuTextureFormat::RGBA8;
        desc.srgb = true;
        desc.width = desc.height = size;
        for (const std::vector<uint8_t>& level : levels)
            desc.levels.push_back(std::span<const uint8_t>(level));
        std::vector<uint8_t> out;
        std::string error;
        EXPECT_TRUE(Ktx2::write(desc, out, error)) << error;
        // as TextureCooker::tailBytes
        Ktx2Info info;
        tailBytes = 0;
        EXPECT_TRUE(Ktx2::parseInfo(out, out.size(), info, error)) << error;
        for (uint32_t level = 0; level < info.levelCount; level++){
            if (std::max(info.levelWidth(level), info.levelHeight(level)) <= 64)
                tailBytes = std::max(tailBytes, info.levels[level].byteOffset + info.levels[level].byteLength);
        }
        return out;
    }

    Material materialOfFile(const std::string& path){
        Material material;
        material.baseColorTexture = Texture(path);
        material.baseColorFactor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        material.lighting = MaterialLighting::NONE;
        return material;
    }
}

// A real cooked load: the plane's image comes from a cooked file read in two stages, the tail
// (bound and drawn) and later the rest, which must reach the plane
TEST_F(TextureRebindGpu, TheRestOfAProgressiveLoadReachesTheMeshDrawingItsTail){
    JASPER_REQUIRE_GPU();
    uint64_t tailBytes = 0;
    const std::vector<uint8_t> file = progressiveKtx2(1024, RED, BLUE, tailBytes);
    ASSERT_GT(tailBytes, 0u);
    ASSERT_LT(tailBytes, file.size());
    const std::string cooked = dir.file("progressive.ktx2").generic_string();
    std::ofstream(cooked, std::ios::binary).write(reinterpret_cast<const char*>(file.data()), (std::streamsize)file.size());

    const auto entryOf = [&](uint64_t tail){
        CookedTextureEntry entry;
        entry.file = cooked;
        entry.width = entry.height = 1024;
        entry.format = "RGBA8";
        entry.levels = 11;
        entry.bytes = file.size();
        entry.tailBytes = tail;
        return entry;
    };
    // (a load that reads in the background, as the editor and a player's do)
    const bool asyncLoading = Engine::isAsyncLoading();
    Engine::setAsyncLoading(true);
    std::map<std::string, CookedTextureEntry> entries = {
        // one read of the whole file: what the image is in the end
        {"bf17/whole.png", entryOf(0)},
        // the tail, then the rest
        {"bf17/progressive.png", entryOf(tailBytes)},
    };
    std::map<std::string, CookedTextureEntry>* table = &entries;
    CookedTextures::setProvider([table](const std::string& path, CookedTextureEntry& out){
        auto it = table->find(path);
        if (it == table->end())
            return false;
        out = it->second;
        return true;
    }, "desktop");

    std::vector<uint8_t> reference;
    {
        Stage stage;
        scene = &stage.scene;
        Primitive plane(&stage.scene);
        plane.createPlane(3.0f, 3.0f);
        plane.setMaterial(materialOfFile("bf17/whole.png"));
        ASSERT_TRUE(settled());
        reference = readTarget();
    }

    Stage stage;
    scene = &stage.scene;
    Primitive plane(&stage.scene);
    plane.createPlane(3.0f, 3.0f);
    plane.setMaterial(materialOfFile("bf17/progressive.png"));
    // the mesh bound the tail (the frame it was uploaded), the rest came after it: the one GPU
    // texture was made again in place (TexturePool::get)
    ASSERT_TRUE(settled());
    const std::vector<uint8_t> after = readTarget();
    std::shared_ptr<TextureRender> render = TexturePool::get("bf17/progressive.png");
    ASSERT_NE(render, nullptr) << "the mesh never made the GPU texture of its image";
    EXPECT_GE(render->getRevision(), 1u) << "the load was not progressive: the case shows nothing";
    EXPECT_EQ(render->getLevelCount(), 11);
    EXPECT_LT(meanDifference(after, reference), 1.0) << "the mesh still draws the tail";

    CookedTextures::setProvider(nullptr, "");
    Engine::setAsyncLoading(asyncLoading);
    EXPECT_NO_ENGINE_ERRORS();
}
