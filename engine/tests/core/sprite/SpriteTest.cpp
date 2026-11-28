// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// S94 F29, F30 (AC30, AC29's geometry): a Sprite's Size sets its quad's world size in every draw
// mode (a legacy quad, a sheet frame, a sheet frame Sliced with its corners unscaled), the pivot at
// the same fraction of it; Flip X mirrors the quad about the pivot with its texels, the vertices
// still left to right (the triangles keep their winding) and the tangent turned. No GPU: the
// MeshSystem builds the buffers.

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/SpriteComponent.h"
#include "object/Camera.h"
#include "object/Sprite.h"
#include "sprite/SpriteSheet.h"
#include "sprite/SpriteSheetPool.h"
#include "subsystem/MeshSystem.h"
#include "texture/TextureData.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace SpriteTest_cpp {

using namespace jasperengine;

namespace {

    const std::vector<uint8_t>& spriteTestPixels(){
        static const std::vector<uint8_t> pixels(16 * 8 * 4, 255);
        return pixels;
    }

    TextureData spriteTestTexture(){
        return TextureData(16, 8, 16 * 8 * 4, ColorFormat::RGBA, 4, (void*)spriteTestPixels().data());
    }

    // a sheet of one 16 x 8 frame at 16 pixels per unit, bordered 4 pixels all round
    const std::string& bordered(){
        static const std::string path = "s94-sprite-test://bordered";
        static bool registered = false;
        if (!registered){
            registered = true;
            SpriteSheetData data;
            data.pixelsPerUnit = 16.0f;
            data.pages.push_back({"s94|sprite|page", 16, 8});
            SpriteFrame frame;
            frame.name = "f0";
            frame.rect = Rect(0.0f, 0.0f, 16.0f, 8.0f);
            frame.sourceSize = Vector2(16.0f, 8.0f);
            frame.border = Vector4(4.0f, 2.0f, 4.0f, 2.0f);
            data.frames.push_back(frame);
            auto sheet = std::make_shared<const SpriteSheetData>(data);
            SpriteSheetPool::registerSource([](const std::string& p){ return p == path; }, [sheet](const std::string&){ return sheet; });
        }
        return path;
    }

    struct SpriteScene {
        Scene scene;
        Camera camera{&scene};
        std::unique_ptr<Sprite> sprite;

        SpriteScene(){
            camera.setType(CameraType::CAMERA_ORTHO);
            camera.activate();
            sprite = std::make_unique<Sprite>(&scene);
        }

        void legacy(PivotPreset pivot){
            sprite->setTexture("s94|sprite|white", spriteTestTexture());
            sprite->setSize(16, 8);
            sprite->setPivotPreset(pivot);
        }

        void sheet(SpriteDrawMode mode){
            sprite->setSheet(bordered());
            sprite->setFrame(0);
            sprite->setPivotPreset(PivotPreset::BOTTOM_LEFT);
            sprite->setDrawMode(mode);
        }

        const MeshComponent& build(){
            scene.getSystem<MeshSystem>()->update(0);
            return scene.getComponent<MeshComponent>(sprite->getEntity());
        }

        std::vector<float> column(AttributeType type, int lane){
            build();
            Buffer& buffer = scene.getComponent<MeshComponent>(sprite->getEntity()).buffer;
            Attribute* attribute = buffer.getAttribute(type);
            std::vector<float> out;
            for (unsigned int i = 0; attribute && i < attribute->getCount(); i++){
                if (type == AttributeType::TEXCOORD1){
                    const Vector2 v = buffer.getVector2(attribute, i);
                    out.push_back(lane == 0 ? v.x : v.y);
                }else if (type == AttributeType::TANGENT){
                    const Vector4 v = buffer.getVector4(attribute, i);
                    out.push_back(lane == 0 ? v.x : v.w);
                }else{
                    const Vector3 v = buffer.getVector3(attribute, i);
                    out.push_back(lane == 0 ? v.x : v.y);
                }
            }
            return out;
        }

        // the lattice's distinct x positions, left to right
        std::vector<float> columns(){
            std::vector<float> xs = column(AttributeType::POSITION, 0);
            std::sort(xs.begin(), xs.end());
            xs.erase(std::unique(xs.begin(), xs.end(), [](float a, float b){ return std::abs(a - b) < 1e-5f; }), xs.end());
            return xs;
        }
    };

    void expectBounds(const AABB& box, float x0, float y0, float x1, float y1){
        EXPECT_NEAR(box.getMinimum().x, x0, 1e-5f);
        EXPECT_NEAR(box.getMinimum().y, y0, 1e-5f);
        EXPECT_NEAR(box.getMaximum().x, x1, 1e-5f);
        EXPECT_NEAR(box.getMaximum().y, y1, 1e-5f);
    }

}

TEST(SpriteTest, SizeSetsWorldQuad){
    {
        SCOPED_TRACE("legacy Simple");
        SpriteScene s;
        s.legacy(PivotPreset::BOTTOM_LEFT);
        expectBounds(s.build().aabb, 0.0f, 0.0f, 16.0f, 8.0f);
        s.sprite->setQuadSize(Vector2(1.5f, 0.75f));
        expectBounds(s.build().aabb, 0.0f, 0.0f, 1.5f, 0.75f);
        EXPECT_EQ(s.sprite->getWorldSize(), Vector2(1.5f, 0.75f));
        // the pivot at the same fraction of the Size
        s.sprite->setPivotPreset(PivotPreset::CENTER);
        expectBounds(s.build().aabb, -0.75f, -0.375f, 0.75f, 0.375f);
        // an axis at 0 keeps its pixels over the Pixels Per Unit
        s.sprite->setQuadSize(Vector2(1.5f, 0.0f));
        expectBounds(s.build().aabb, -0.75f, -4.0f, 0.75f, 4.0f);
    }
    {
        SCOPED_TRACE("sheet Simple");
        SpriteScene s;
        s.sheet(SpriteDrawMode::SIMPLE);
        expectBounds(s.build().aabb, 0.0f, 0.0f, 1.0f, 0.5f);
        s.sprite->setQuadSize(Vector2(1.5f, 0.75f));
        expectBounds(s.build().aabb, 0.0f, 0.0f, 1.5f, 0.75f);
    }
    {
        SCOPED_TRACE("sheet Sliced");
        SpriteScene s;
        s.sheet(SpriteDrawMode::SLICED);
        s.sprite->setQuadSize(Vector2(1.5f, 0.75f));
        expectBounds(s.build().aabb, 0.0f, 0.0f, 1.5f, 0.75f);
        // the corners keep their 4 pixels at 16 per unit
        const std::vector<float> xs = s.columns();
        ASSERT_EQ(xs.size(), 4u);
        EXPECT_NEAR(xs[1] - xs[0], 0.25f, 1e-5f);
        EXPECT_NEAR(xs[3] - xs[2], 0.25f, 1e-5f);
    }
    // a negative Size is refused
    SpriteScene s;
    s.legacy(PivotPreset::BOTTOM_LEFT);
    s.sprite->setQuadSize(Vector2(-1.0f, 1.0f));
    EXPECT_EQ(s.sprite->getQuadSize(), Vector2(0.0f, 0.0f));
}

TEST(SpriteTest, FlipXMirrorsAboutThePivot){
    SpriteScene s;
    s.legacy(PivotPreset::BOTTOM_LEFT);
    const std::vector<float> xs = s.column(AttributeType::POSITION, 0);
    const std::vector<float> us = s.column(AttributeType::TEXCOORD1, 0);
    const std::vector<uint16_t> indices = [&](){
        const MeshComponent& mesh = s.build();
        const uint16_t* data = (const uint16_t*)mesh.indices.getData();
        return std::vector<uint16_t>(data, data + mesh.indices.getCount());
    }();

    s.sprite->setFlipX(true);
    expectBounds(s.build().aabb, -16.0f, 0.0f, 0.0f, 8.0f);
    const std::vector<float> flippedX = s.column(AttributeType::POSITION, 0);
    const std::vector<float> flippedU = s.column(AttributeType::TEXCOORD1, 0);
    ASSERT_EQ(flippedX.size(), xs.size());
    for (size_t i = 0; i < xs.size(); i++){
        // each vertex keeps its side (left stays left: the same winding), mirrored about x 0, with
        // the texels of the other side
        EXPECT_FLOAT_EQ(flippedX[i], xs[i] - 16.0f);
        EXPECT_FLOAT_EQ(flippedU[i], 1.0f - us[i]);
    }
    const MeshComponent& mesh = s.build();
    const uint16_t* data = (const uint16_t*)mesh.indices.getData();
    EXPECT_EQ(std::vector<uint16_t>(data, data + mesh.indices.getCount()), indices);
    // u runs along -X: the tangent turns, the handedness with it (the bitangent still up the image)
    for (float x : s.column(AttributeType::TANGENT, 0)) EXPECT_FLOAT_EQ(x, -1.0f);

    SpriteScene sliced;
    sliced.sheet(SpriteDrawMode::SLICED);
    sliced.sprite->setQuadSize(Vector2(1.5f, 0.75f));
    sliced.sprite->setFlipX(true);
    expectBounds(sliced.build().aabb, -1.5f, 0.0f, 0.0f, 0.75f);
    const std::vector<float> columns = sliced.columns();
    ASSERT_EQ(columns.size(), 4u);
    EXPECT_NEAR(columns[1] - columns[0], 0.25f, 1e-5f);
}

}  // namespace SpriteTest_cpp
