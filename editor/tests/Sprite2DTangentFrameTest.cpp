// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Sprite normals phase 1 (docs/superpowers/specs/2026-10-27-sprite-normals-phase1-engine-design.md
// section 5.1): the tangent frame the 2D mesh generators (MeshSystem: sprite quad, sprite grid,
// MeshPolygon, Tilemap) write reads normal maps OpenGL style, the tangent where u grows and the
// bitangent (cross(N, T) * w, as mesh.vert and surface_vs.glsl build it) where v shrinks, up the
// image, with flipY on (world cameras) and off (UI cameras), mirrored tiles included (U1, U2).
// Library shaders read the material's normal map with @require normalmap (U3, U4); editor-made
// Light 2Ds start at height 64, loaded and script-made ones keep 0 (U5); Mesh::setNormalTexture
// (U6).

#include <gtest/gtest.h>
#include "object/ui/Polygon.h"

#include "AppSettings.h"
#include "Catalog.h"
#include "PhysicsTestScene.h"
#include "Project.h"
#include "Stream.h"
#include "command/CommandHandle.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/CreateEntityCmd.h"
#include "component/Light2DComponent.h"
#include "object/Light2D.h"
#include "object/Mesh.h"
#include "shader/ShaderLibraryMatrix.h"
#include "shader/ShaderManifest.h"
#include "subsystem/RenderSystem.h"
#include "util/ProjectUtils.h"
#include "yaml-cpp/yaml.h"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
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
#include "reflect/Reflect.h"

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

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


// ---- U3: @require normalmap

namespace {

    bool hasDefine(const std::vector<std::string>& defines, const std::string& define) {
        return std::find(defines.begin(), defines.end(), define) != defines.end();
    }

    bool reported(const ShaderManifest& manifest, const std::string& text) {
        for (const ShaderManifestDiagnostic& d : manifest.diagnostics) {
            if (d.message.find(text) != std::string::npos)
                return true;
        }
        return false;
    }

}

TEST(Sprite2DNormalMapManifest, RequireNormalMapParses) {
    ShaderManifest manifest;
    manifest.parse("// @shader mesh\n// @name \"T\"\n// @require uv1 color normalmap\n", "t.frag");
    manifest.checkComplete("t.frag", false);
    for (const ShaderManifestDiagnostic& d : manifest.diagnostics)
        EXPECT_FALSE(d.error) << d.line << ": " << d.message;
    const uint32_t require = manifest.resolveRequire({});
    EXPECT_EQ(require, uint32_t(ShaderRequire_UV1 | ShaderRequire_Color | ShaderRequire_NormalMap));
    EXPECT_TRUE(hasDefine(manifest.getRequireDefines(std::vector<std::string>()), "SURFACE_NORMAL_MAP"));
    const std::vector<std::string> names = ShaderManifest::getRequireNames(ShaderRequire_NormalMap);
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "normalmap");
    EXPECT_NE(manifest.getPossibleRequire() & ShaderRequire_NormalMap, 0u);
}

TEST(Sprite2DNormalMapManifest, FeatureGatesIt) {
    ShaderManifest manifest;
    manifest.parse("// @shader mesh\n// @name \"T\"\n// @feature LIT \"Lit\"\n// @require uv1\n"
                   "// @require normalmap feature=LIT\n", "t.frag");
    manifest.checkComplete("t.frag", false);
    EXPECT_FALSE(manifest.hasErrors());
    EXPECT_EQ(manifest.resolveRequire({}) & ShaderRequire_NormalMap, 0u);
    EXPECT_NE(manifest.resolveRequire({"LIT"}) & ShaderRequire_NormalMap, 0u);
    EXPECT_FALSE(hasDefine(manifest.getRequireDefines(std::vector<std::string>()), "SURFACE_NORMAL_MAP"));
    EXPECT_TRUE(hasDefine(manifest.getRequireDefines(std::vector<std::string>{"LIT"}), "SURFACE_NORMAL_MAP"));
    EXPECT_NE(manifest.getPossibleRequire() & ShaderRequire_NormalMap, 0u);
}

TEST(Sprite2DNormalMapManifest, Errors) {
    ShaderManifest unknown;
    unknown.parse("// @shader mesh\n// @name \"T\"\n// @require normalmaps\n", "t.frag");
    // (the list grows: d33b22638 and b2beef162 added inputs after normalmap)
    EXPECT_TRUE(reported(unknown, "unknown input 'normalmaps'")) << "the unknown-input error names the misspelling";
    EXPECT_TRUE(reported(unknown, ", normalmap,")) << "the unknown-input error lists normalmap";

    ShaderManifest notMesh;
    notMesh.parse("// @shader ui\n// @name \"T\"\n// @require uv1 normalmap\n", "t.frag");
    notMesh.checkComplete("t.frag", false);
    EXPECT_TRUE(reported(notMesh, "@require normalmap needs @shader mesh"));

    ShaderManifest reserved;
    reserved.parse("// @shader mesh\n// @name \"T\"\n// @require uv1 normalmap\n// @texture u_normalTexture normal \"N\"\n", "t.frag");
    reserved.checkComplete("t.frag", false);
    EXPECT_TRUE(reported(reserved, "'u_normalTexture' is reserved with @require normalmap"));
}

TEST(Sprite2DNormalMapManifest, LibrarySpriteRequiresIt) {
    const std::shared_ptr<const ShaderManifest> sprite = ShaderManifests::get("library:sprite");
    ASSERT_TRUE(sprite);
    EXPECT_NE(sprite->resolveRequire({}) & ShaderRequire_NormalMap, 0u);
    EXPECT_NE(sprite->resolveRequire({"OUTLINE"}) & ShaderRequire_NormalMap, 0u);
}

// the variant samples the material's normal map exactly under 2D lights, with a normal texture,
// on a mesh with normals, tangents and uv1 (RenderSystem::getSubmeshShaderVariant)
TEST(Sprite2DNormalMapManifest, VariantSamplesTheMaterialNormalMap) {
    const uint64_t nmp = shaderPropertyBit(7);
    SubmeshShaderInputs inputs;
    inputs.texCoord1 = true;
    inputs.normal = true;
    inputs.tangent = true;
    inputs.vertexColor4 = true;
    inputs.textureRect = true;
    inputs.baseColorTexture = true;
    inputs.normalTexture = true;
    inputs.manifest = ShaderManifests::get("library:sprite");
    ASSERT_TRUE(inputs.manifest);
    MeshShaderEnvironment lights2D;
    lights2D.lights2D = true;

    const SubmeshShaderVariant lit = RenderSystem::getSubmeshShaderVariant(lights2D, inputs);
    EXPECT_NE(lit.properties & nmp, 0u) << "under 2D lights with a normal texture";

    EXPECT_EQ(RenderSystem::getSubmeshShaderVariant(MeshShaderEnvironment(), inputs).properties & nmp, 0u) << "no 2D lights";
    SubmeshShaderInputs without = inputs;
    without.normalTexture = false;
    const SubmeshShaderVariant plain = RenderSystem::getSubmeshShaderVariant(lights2D, without);
    EXPECT_EQ(plain.properties & nmp, 0u) << "no normal texture";
    without = inputs;
    without.tangent = false;
    EXPECT_EQ(RenderSystem::getSubmeshShaderVariant(lights2D, without).properties & nmp, 0u) << "no tangents";
    without = inputs;
    without.receiveLights = false;
    EXPECT_EQ(RenderSystem::getSubmeshShaderVariant(lights2D, without).properties & nmp, 0u) << "not receiving lights";

    // a library shader without @require normalmap: as before
    SubmeshShaderInputs simpleLit = inputs;
    simpleLit.manifest = ShaderManifests::get("library:simple_lit");
    ASSERT_TRUE(simpleLit.manifest);
    EXPECT_EQ(RenderSystem::getSubmeshShaderVariant(lights2D, simpleLit).properties & nmp, 0u) << "library:simple_lit";
}

// ---- the shader library matrix (shaders --library) compiles library:sprite's normal-map path

TEST(Sprite2DNormalMapManifest, LibraryMatrixHasANormalMappedSprite) {
    const std::shared_ptr<const ShaderManifest> sprite = ShaderManifests::get("library:sprite");
    ASSERT_TRUE(sprite);
    const uint64_t nmp = 1ull << 7;   // 'Nmp'
    int normalMapped = 0;
    for (const ShaderLibraryMatrix::Variant& v : ShaderLibraryMatrix::buildVariants("sprite", *sprite)) {
        if (v.type != ShaderType::MESH || v.label.find("normal-mapped") == std::string::npos) continue;
        EXPECT_NE(v.properties & nmp, 0u) << v.label << ": samples the normal map";
        normalMapped++;
    }
    EXPECT_GE(normalMapped, 2) << "the normal-mapped sprite cases (no features, all features)";
}

// ---- U4: Catalog::shaderReadsMaterialNormalMap

TEST(Sprite2DNormalMapManifest, CatalogShaderReadsMaterialNormalMap) {
    EXPECT_TRUE(Catalog::shaderReadsMaterialNormalMap(ShaderManifests::get("library:sprite").get(), {}));
    EXPECT_TRUE(Catalog::shaderReadsMaterialNormalMap(ShaderManifests::get("library:sprite").get(), {"OUTLINE"}));
    EXPECT_FALSE(Catalog::shaderReadsMaterialNormalMap(ShaderManifests::get("library:simple_lit").get(), {}));
    EXPECT_FALSE(Catalog::shaderReadsMaterialNormalMap(nullptr, {})) << "the built-in shader";

    ShaderManifest gated;
    gated.parse("// @shader mesh\n// @name \"T\"\n// @feature LIT \"Lit\"\n// @require uv1\n"
                "// @require normalmap feature=LIT\n", "t.frag");
    EXPECT_FALSE(Catalog::shaderReadsMaterialNormalMap(&gated, {}));
    EXPECT_TRUE(Catalog::shaderReadsMaterialNormalMap(&gated, {"LIT"}));
}

// ---- U5: the height of new Light 2Ds

namespace {

    struct Headless {
        bool ready = false;
        std::unique_ptr<Project> project;
    };

    Headless& headless() {
        static Headless h = []() {
            Headless out;
            physicstest::installTestSystem();
            Engine::setAsyncLoading(false);
            const fs::path root = fs::temp_directory_path() / ("jasper-light2d-height-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            const fs::path dir = root / "project";
            fs::create_directories(dir / ".jasperengine");
            fs::create_directories(dir / "assets");
            std::ofstream(dir / "project.yaml") << "name: light2dheight\nassetsDir: assets\n";
            AppSettings::initialize();
            out.project.reset(new Project());
            out.ready = out.project->loadProject(dir, false);
            return out;
        }();
        return h;
    }

    uint32_t newScene2D() {
        Project* p = headless().project.get();
        if (SceneProject* current = p->getSelectedScene()) current->isModified = false;
        p->createNewScene("Lights", SceneType::SCENE_2D);
        return p->getSelectedSceneId();
    }

    Scene* sceneOf(uint32_t sceneId) { return headless().project->getScene(sceneId)->scene; }

}

TEST(Sprite2DLightHeight, CreatedLight2DIsAt64) {
    ASSERT_TRUE(headless().ready);
    const uint32_t sceneId = newScene2D();
    CreateEntityCmd* create = new CreateEntityCmd(headless().project.get(), sceneId, "Light 2D", EntityCreationType::POINT_LIGHT_2D);
    create->setQuiet(true);
    CommandHandle::get(sceneId)->addCommandNoMerge(create);
    const Entity entity = create->getEntity();
    ASSERT_TRUE(sceneOf(sceneId)->findComponent<Light2DComponent>(entity));
    EXPECT_EQ(sceneOf(sceneId)->getComponent<Light2DComponent>(entity).height, 64.0f);
    EXPECT_EQ(sceneOf(sceneId)->getComponent<Light2DComponent>(entity).range, 200.0f);

    ASSERT_TRUE(CommandHandle::get(sceneId)->undo());
    EXPECT_FALSE(sceneOf(sceneId)->isEntityCreated(entity)) << "undo removes the light";
    ASSERT_TRUE(CommandHandle::get(sceneId)->redo());
    const Entity redone = create->getEntity();
    ASSERT_TRUE(sceneOf(sceneId)->findComponent<Light2DComponent>(redone));
    EXPECT_EQ(sceneOf(sceneId)->getComponent<Light2DComponent>(redone).height, 64.0f) << "redo";
}

TEST(Sprite2DLightHeight, AddedLight2DIsAt64) {
    ASSERT_TRUE(headless().ready);
    const uint32_t sceneId = newScene2D();
    CreateEntityCmd* create = new CreateEntityCmd(headless().project.get(), sceneId, "Empty", EntityCreationType::GAMEOBJECT);
    create->setQuiet(true);
    CommandHandle::get(sceneId)->addCommandNoMerge(create);
    const Entity entity = create->getEntity();

    CommandHandle::get(sceneId)->addCommandNoMerge(new AddComponentCmd(headless().project.get(), sceneId, entity, ComponentType::Light2DComponent));
    ASSERT_TRUE(sceneOf(sceneId)->findComponent<Light2DComponent>(entity));
    EXPECT_EQ(sceneOf(sceneId)->getComponent<Light2DComponent>(entity).height, 64.0f);

    ASSERT_TRUE(CommandHandle::get(sceneId)->undo());
    EXPECT_FALSE(sceneOf(sceneId)->findComponent<Light2DComponent>(entity)) << "undo removes the component";
    ASSERT_TRUE(CommandHandle::get(sceneId)->redo());
    ASSERT_TRUE(sceneOf(sceneId)->findComponent<Light2DComponent>(entity));
    EXPECT_EQ(sceneOf(sceneId)->getComponent<Light2DComponent>(entity).height, 64.0f) << "redo";
}

namespace {
    // a scene's Light2D node read as the scene loader reads it (the S08 table: reflect::decode)
    Light2DComponent decodeLight2D(const YAML::Node& node) {
        Light2DComponent light;
        reflect::DecodeReport report;
        reflect::decode(*reflect::find(ComponentType::Light2DComponent), node, &light, reflect::DecodeContext{}, report);
        return light;
    }
}

TEST(Sprite2DLightHeight, LoadedAndWrappedLightsKeepTheirHeight) {
    // a scene's Light2D without height: the struct default
    YAML::Node node = YAML::Load("intensity: 2\nrange: 300\n");
    const Light2DComponent loaded = decodeLight2D(node);
    EXPECT_EQ(loaded.height, 0.0f);
    EXPECT_EQ(loaded.intensity, 2.0f);
    // with one, its value
    EXPECT_EQ(decodeLight2D(YAML::Load("height: 12\n")).height, 12.0f);

    // the loader's path (ProjectUtils::addEntityComponent) and a script wrapping that light keep
    // 0; a light the C++ / Lua constructor makes new starts at 64, as the editor's (S78 R16)
    Scene scene;
    const Entity entity = scene.createEntity();
    std::vector<Entity> entities{entity};
    ProjectUtils::addEntityComponent(&scene, entity, ComponentType::Light2DComponent, entities);
    ASSERT_TRUE(scene.findComponent<Light2DComponent>(entity));
    EXPECT_EQ(scene.getComponent<Light2DComponent>(entity).height, 0.0f);
    EXPECT_EQ(Light2D(&scene, entity).getHeight(), 0.0f);
    Light2D light(&scene);
    EXPECT_EQ(light.getHeight(), 64.0f);
}

// ---- U6: Mesh::setNormalTexture

TEST(Sprite2DNormalTexture, SetAndGet) {
    Scene scene;
    Mesh mesh(&scene);
    MeshComponent& component = mesh.getComponent<MeshComponent>();
    component.numSubmeshes = 2;
    component.loaded = true;

    EXPECT_EQ(mesh.getNormalTexture(), "");
    mesh.setNormalTexture("hero_normal.png");
    EXPECT_EQ(mesh.getNormalTexture(), "hero_normal.png");
    EXPECT_EQ(mesh.getNormalTexture(0), "hero_normal.png");
    EXPECT_FALSE(component.submeshes[0].material.normalTexture.empty());
    EXPECT_TRUE(component.submeshes[0].needUpdateTexture);
    EXPECT_TRUE(component.needReload) << "gaining a normal map changes the variant";

    // another path: only the texture
    component.needReload = false;
    component.submeshes[0].needUpdateTexture = false;
    mesh.setNormalTexture("hero_normal_b.png");
    EXPECT_EQ(mesh.getNormalTexture(), "hero_normal_b.png");
    EXPECT_TRUE(component.submeshes[0].needUpdateTexture);
    EXPECT_FALSE(component.needReload) << "another path keeps the variant";

    // submesh 1
    mesh.setNormalTexture(1, "tiles_normal.png");
    EXPECT_EQ(mesh.getNormalTexture(1), "tiles_normal.png");
    EXPECT_EQ(mesh.getNormalTexture(), "hero_normal_b.png");
    EXPECT_TRUE(component.needReload);

    // "" clears it
    component.needReload = false;
    mesh.setNormalTexture("");
    EXPECT_EQ(mesh.getNormalTexture(), "");
    EXPECT_TRUE(component.submeshes[0].material.normalTexture.empty());
    EXPECT_TRUE(component.needReload) << "losing it changes the variant";
    component.needReload = false;
    mesh.setNormalTexture("");
    EXPECT_FALSE(component.needReload) << "clearing nothing changes nothing";

    // out of range: an error, nothing changes
    component.needReload = false;
    mesh.setNormalTexture(2, "x.png");
    EXPECT_FALSE(component.needReload);
    EXPECT_EQ(mesh.getNormalTexture(2), "");
}

TEST(Sprite2DNormalTexture, FreshSpriteAndData) {
    Scene scene;
    Sprite sprite(&scene);
    // submesh 0 before the sprite is built
    sprite.setNormalTexture("dome.png");
    EXPECT_EQ(sprite.getNormalTexture(), "dome.png");

    static unsigned char pixels[4] = {128, 128, 255, 255};
    sprite.setNormalTexture("flat|normal", TextureData(1, 1, 4, ColorFormat::RGBA, 4, pixels));
    EXPECT_EQ(sprite.getNormalTexture(), "flat|normal") << "a normal map from data reads its id";
}

// ---- fix round 1: the facing of mirrored draws (RenderSystem::isMirroredDraw)

TEST(Sprite2DMirroredDraw, TruthTable) {
    MeshComponent plain;
    const Matrix4 identity;
    const Matrix4 mirrorX = Matrix4::scaleMatrix(Vector3(-1.0f, 1.0f, 1.0f));
    const Matrix4 mirrorXY = Matrix4::scaleMatrix(Vector3(-1.0f, -1.0f, 1.0f));
    EXPECT_FALSE(RenderSystem::isMirroredDraw(plain, identity));
    EXPECT_TRUE(RenderSystem::isMirroredDraw(plain, mirrorX));
    EXPECT_FALSE(RenderSystem::isMirroredDraw(plain, mirrorXY)) << "two mirrors are a rotation";
    EXPECT_TRUE(RenderSystem::isMirroredDraw(plain, Matrix4::translateMatrix(Vector3(5.0f, 0.0f, 0.0f)) * mirrorX));

    // a winding order that compensates the entity's mirror (glTF import)
    MeshComponent compensated;
    compensated.windingOrder = WindingOrder::CW;
    compensated.mirroredWinding = true;
    EXPECT_FALSE(RenderSystem::isMirroredDraw(compensated, mirrorX));
    EXPECT_TRUE(RenderSystem::isMirroredDraw(compensated, identity));

    // a winding order set by hand (no compensation): only the transform counts
    MeshComponent clockwise;
    clockwise.windingOrder = WindingOrder::CW;
    EXPECT_FALSE(RenderSystem::isMirroredDraw(clockwise, identity));
    EXPECT_TRUE(RenderSystem::isMirroredDraw(clockwise, mirrorX));
}
