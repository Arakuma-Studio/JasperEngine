// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Sprite normals phase 1 (docs/superpowers/specs/2026-10-27-sprite-normals-phase1-engine-design.md
// section 5.1): the tangent frame the 2D mesh generators (MeshSystem: sprite quad, sprite grid,
// MeshPolygon, Tilemap) write reads normal maps OpenGL style, the tangent where u grows and the
// bitangent (cross(N, T) * w, as mesh.vert and surface_vs.glsl build it) where v shrinks, up the
// image, with flipY on (world cameras) and off (UI cameras), mirrored tiles included.

#include <gtest/gtest.h>

#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/SpriteComponent.h"
#include "component/TilemapComponent.h"
#include "math/Rect.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "object/Camera.h"
#include "object/MeshPolygon.h"
#include "object/Sprite.h"
#include "object/Tilemap.h"
#include "subsystem/MeshSystem.h"

#include <cmath>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    struct Vertex {
        Vector3 position;
        Vector2 uv;
        Vector3 normal;
        float tangent[4];
    };

    std::vector<Vertex> readVertices(MeshComponent& mesh) {
        std::vector<Vertex> vertices;
        Attribute* position = mesh.buffer.getAttribute(AttributeType::POSITION);
        Attribute* texcoord = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);
        Attribute* normal = mesh.buffer.getAttribute(AttributeType::NORMAL);
        Attribute* tangent = mesh.buffer.getAttribute(AttributeType::TANGENT);
        if (!position || !texcoord || !normal || !tangent)
            return vertices;
        for (unsigned int i = 0; i < mesh.buffer.getCount(); i++) {
            Vertex v;
            v.position = Vector3(mesh.buffer.getFloat(position, i, 0), mesh.buffer.getFloat(position, i, 1), mesh.buffer.getFloat(position, i, 2));
            v.uv = Vector2(mesh.buffer.getFloat(texcoord, i, 0), mesh.buffer.getFloat(texcoord, i, 1));
            v.normal = Vector3(mesh.buffer.getFloat(normal, i, 0), mesh.buffer.getFloat(normal, i, 1), mesh.buffer.getFloat(normal, i, 2));
            for (int c = 0; c < 4; c++)
                v.tangent[c] = mesh.buffer.getFloat(tangent, i, c);
            vertices.push_back(v);
        }
        return vertices;
    }

    float signOf(float value) {
        return value < 0.0f ? -1.0f : 1.0f;
    }

    // The property on one group of vertices sharing a frame (a whole sprite or polygon, one tile):
    // du/dx and dv/dy from vertex pairs on the same row / column, then T.x == sign(du/dx) and
    // B.y == -sign(dv/dy) with B = cross(N, T) * w
    void expectOpenGLFrame(const std::vector<Vertex>& vertices, size_t first, size_t count, const std::string& what) {
        float duDx = 0.0f;
        float dvDy = 0.0f;
        for (size_t i = first; i < first + count; i++) {
            for (size_t j = first; j < first + count; j++) {
                const Vertex& a = vertices[i];
                const Vertex& b = vertices[j];
                const float dx = b.position.x - a.position.x;
                const float dy = b.position.y - a.position.y;
                if (std::fabs(dy) < 1e-4f && std::fabs(dx) > 1e-4f && duDx == 0.0f)
                    duDx = (b.uv.x - a.uv.x) / dx;
                if (std::fabs(dx) < 1e-4f && std::fabs(dy) > 1e-4f && dvDy == 0.0f)
                    dvDy = (b.uv.y - a.uv.y) / dy;
            }
        }
        ASSERT_NE(duDx, 0.0f) << what << ": u does not change along x";
        ASSERT_NE(dvDy, 0.0f) << what << ": v does not change along y";

        for (size_t i = first; i < first + count; i++) {
            const Vertex& v = vertices[i];
            EXPECT_FLOAT_EQ(v.normal.z, 1.0f) << what << " vertex " << i;
            EXPECT_EQ(v.tangent[1], 0.0f) << what << " vertex " << i;
            EXPECT_EQ(v.tangent[2], 0.0f) << what << " vertex " << i;
            EXPECT_EQ(v.tangent[0], signOf(duDx)) << what << " vertex " << i << ": the tangent points where u grows";
            const Vector3 n = v.normal;
            const Vector3 t(v.tangent[0], v.tangent[1], v.tangent[2]);
            const Vector3 bitangent = n.crossProduct(t) * v.tangent[3];
            EXPECT_EQ(bitangent.y, -signOf(dvDy)) << what << " vertex " << i << " (w " << v.tangent[3]
                                                  << "): the bitangent points where v shrinks, up the image";
        }
    }

    // a fresh scene per case with a world camera (the sprite reads the scene's camera), no
    // render system needed
    struct SpriteCase {
        Scene scene;
        Camera camera{&scene};
        Sprite sprite{&scene};

        SpriteCase(bool flipY, int grid) {
            camera.activate();
            sprite.setSize(64, 64);
            sprite.setFlipY(flipY);
            if (grid > 1) {
                MeshComponent& mesh = sprite.getComponent<MeshComponent>();
                mesh.lattice.wantsTessellation = true;
                mesh.lattice.tessellation = grid;
            }
        }
    };

}

TEST(Sprite2DTangentFrame, SpriteQuad) {
    for (bool flipY : {true, false}) {
        SpriteCase c(flipY, 1);
        ASSERT_TRUE(c.sprite.createSprite());
        const std::vector<Vertex> vertices = readVertices(c.sprite.getComponent<MeshComponent>());
        ASSERT_EQ(vertices.size(), 4u);
        expectOpenGLFrame(vertices, 0, vertices.size(), std::string("sprite quad, flipY ") + (flipY ? "on" : "off"));
    }
}

TEST(Sprite2DTangentFrame, SpriteGrid) {
    for (bool flipY : {true, false}) {
        SpriteCase c(flipY, 4);
        ASSERT_TRUE(c.sprite.createSprite());
        const std::vector<Vertex> vertices = readVertices(c.sprite.getComponent<MeshComponent>());
        ASSERT_EQ(vertices.size(), 25u) << "a 4 x 4 grid";
        expectOpenGLFrame(vertices, 0, vertices.size(), std::string("sprite grid, flipY ") + (flipY ? "on" : "off"));
    }
}

TEST(Sprite2DTangentFrame, Polygon) {
    for (bool flipY : {true, false}) {
        Scene scene;
        MeshPolygon polygon(&scene);
        polygon.addVertex(Vector3(0.0f, 0.0f, 0.0f));
        polygon.addVertex(Vector3(64.0f, 0.0f, 0.0f));
        polygon.addVertex(Vector3(0.0f, 64.0f, 0.0f));
        polygon.addVertex(Vector3(64.0f, 64.0f, 0.0f));
        polygon.setFlipY(flipY);
        ASSERT_TRUE(polygon.createPolygon());
        const std::vector<Vertex> vertices = readVertices(polygon.getComponent<MeshComponent>());
        ASSERT_EQ(vertices.size(), 4u);
        expectOpenGLFrame(vertices, 0, vertices.size(), std::string("polygon square, flipY ") + (flipY ? "on" : "off"));
    }
}

TEST(Sprite2DTangentFrame, TilemapWithMirroredTile) {
    for (bool flipY : {true, false}) {
        Scene scene;
        Tilemap tilemap(&scene);
        TilemapComponent& component = tilemap.getComponent<TilemapComponent>();
        component.automaticFlipY = false;
        component.flipY = flipY;
        // a plain tile and one whose rect has a negative width (the tile mirrored)
        tilemap.addRect(0, "plain", Rect(0.0f, 0.0f, 0.5f, 0.5f));
        tilemap.addRect(1, "mirrored", Rect(1.0f, 0.0f, -0.5f, 0.5f));
        tilemap.addTile(0, Vector2(0.0f, 0.0f), 64.0f, 64.0f);
        tilemap.addTile(1, Vector2(64.0f, 0.0f), 64.0f, 64.0f);
        ASSERT_TRUE(tilemap.createTilemap());
        const std::vector<Vertex> vertices = readVertices(tilemap.getComponent<MeshComponent>());
        ASSERT_EQ(vertices.size(), 8u);
        const std::string flip = std::string(", flipY ") + (flipY ? "on" : "off");
        expectOpenGLFrame(vertices, 0, 4, "plain tile" + flip);
        expectOpenGLFrame(vertices, 4, 4, "mirrored tile" + flip);
    }
}

TEST(Sprite2DTangentFrame, Handedness) {
    EXPECT_EQ(MeshSystem::flatTangentHandedness(true), 1.0f);
    EXPECT_EQ(MeshSystem::flatTangentHandedness(false), -1.0f);
}
