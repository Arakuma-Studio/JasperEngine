// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// S94 P2 (draw order and materials, docs/specs/S94-rendering-anim-gaps.md) on the GPU:
//   RenderBlendGpu: Blend Mode Custom's factors reach the pipeline (F9, AC9: Dst Color, Src
//     Color is a 2x multiply of the target);
//   RenderQueueGpu: a mesh whose submeshes differ in sort priority draws each priority in its
//     place (F6, AC6: an opaque hull at priority 1 with depth write off draws after an opaque
//     plane drawn later at priority 0, which erased it before);
//   MaterialPropertyBlockGpu: Property Overrides win over the material (F7, AC7) for a library
//     shader's parameter and texture and the built-in Lit's colour, a submesh's over the Mesh's;
//   RenderShadowGpu: a project shader's own depth program (@depth, <name>_depth.*) clips its
//     shadow (F10, AC10);
//   ShaderVariantGpu: @require vertexnormals feeds a_normal without the v_normal varying
//     (F13, AC13);
//   TextureReadyGpu: Texture::isReady (Lua Texture.ready) is true once the GPU texture exists
//     (F12, AC12).
// Pixels are read where a world point lands (glcore and Vulkan alike).

#include "Engine.h"
#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Mesh.h"
#include "object/Primitive.h"
#include "shader/ShaderManifest.h"
#include "shader/ShaderVariantCompiler.h"
#include "subsystem/RenderSystem.h"
#include "texture/Texture.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace DrawMaterialsGpuTest_cpp {

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    struct DrawPixel { int r = 0, g = 0, b = 0; };

    class DrawMaterialsScene : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};

        void SetUp() override{
            scene.setBackgroundColor(0.0f, 0.0f, 0.0f, 1.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 4.0f, 0.01f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            Engine::setScene(&scene);
        }

        void TearDown() override{
            Engine::removeScene(&scene);
            ShaderManifests::setForkReader(nullptr);
            ShaderManifests::clearForks();
        }

        bool settle(){
            Scene* current = &scene;
            return GpuEnvironment::runFrames([current](){ return current->getSystem<RenderSystem>()->isAllLoaded(); });
        }

        // the mean colour of a 5 x 5 block where a world point is drawn (0..255)
        DrawPixel at(const Vector3& world){
            Framebuffer& target = GpuEnvironment::target();
            const int width = (int)target.getWidth();
            const int height = (int)target.getHeight();
            std::vector<uint8_t> rgba;
            EXPECT_TRUE(readTexture(target.getRender().getColorTexture(), width, height, rgba));
            const CameraComponent& view = scene.getComponent<CameraComponent>(scene.getCamera());
            const Vector4 clip = view.viewProjectionMatrix * Vector4(world.x, world.y, world.z, 1.0f);
            const int cx = (int)std::lround((clip.x / clip.w * 0.5f + 0.5f) * (float)width);
            const int cy = (int)std::lround((0.5f - clip.y / clip.w * 0.5f) * (float)height);
            DrawPixel sum;
            int count = 0;
            for (int y = cy - 2; y <= cy + 2; y++){
                for (int x = cx - 2; x <= cx + 2; x++){
                    if (x < 0 || y < 0 || x >= width || y >= height || rgba.size() < ((size_t)y * width + x + 1) * 4)
                        continue;
                    const size_t i = ((size_t)y * width + x) * 4;
                    sum.r += rgba[i];
                    sum.g += rgba[i + 1];
                    sum.b += rgba[i + 2];
                    count++;
                }
            }
            if (count > 0){
                sum.r /= count;
                sum.g /= count;
                sum.b /= count;
            }
            return sum;
        }

        // a solid RGBA texture of its own id (TextureData points at the pixels: they live on)
        static Texture solidTexture(const std::string& id, uint8_t r, uint8_t g, uint8_t b){
            static std::map<std::string, std::vector<uint8_t>> kept;
            std::vector<uint8_t>& pixels = kept[id];
            pixels.clear();
            for (int i = 0; i < 4; i++){
                pixels.push_back(r);
                pixels.push_back(g);
                pixels.push_back(b);
                pixels.push_back(255);
            }
            Texture texture;
            texture.setData(id, TextureData(2, 2, (unsigned int)pixels.size(), ColorFormat::RGBA, 4, pixels.data()));
            return texture;
        }
    };

    // Two horizontal quads in one mesh, each its own submesh (offsets into one index buffer):
    // submesh 0 of half size `inner` at height y0, submesh 1 of half size `outer` at y1
    void makeTwoQuadMesh(MeshComponent& mesh, float inner, float y0, float outer, float y1){
        mesh.buffer.clear();
        mesh.buffer.addAttribute(AttributeType::POSITION, 3);
        mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
        mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
        Attribute* position = mesh.buffer.getAttribute(AttributeType::POSITION);
        Attribute* uv = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);
        Attribute* normal = mesh.buffer.getAttribute(AttributeType::NORMAL);
        for (const auto& [half, y] : {std::pair<float, float>{inner, y0}, std::pair<float, float>{outer, y1}}){
            mesh.buffer.addVector3(position, Vector3(-half, y, -half));
            mesh.buffer.addVector3(position, Vector3(-half, y, half));
            mesh.buffer.addVector3(position, Vector3(half, y, half));
            mesh.buffer.addVector3(position, Vector3(half, y, -half));
            for (const Vector2& corner : {Vector2(0, 0), Vector2(0, 1), Vector2(1, 1), Vector2(1, 0)})
                mesh.buffer.addVector2(uv, corner);
            for (int v = 0; v < 4; v++)
                mesh.buffer.addVector3(normal, Vector3(0.0f, 1.0f, 0.0f));
        }
        static const uint16_t indices[] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
        mesh.indices.clear();
        mesh.indices.setValues(0, mesh.indices.getAttribute(AttributeType::INDEX), 12, (char*)&indices[0], sizeof(uint16_t));
        mesh.indices.setRenderAttributes(false);
        mesh.numSubmeshes = 2;
        for (unsigned int s = 0; s < 2; s++){
            Attribute index;
            index.setBufferName("indices");
            index.setDataType(AttributeDataType::UNSIGNED_SHORT);
            index.setElements(1);
            index.setCount(6);
            index.setOffset(s * 6 * sizeof(uint16_t));
            index.setNormalized(false);
            mesh.submeshes[s].attributes[AttributeType::INDEX] = index;
            mesh.submeshes[s].primitiveType = PrimitiveType::TRIANGLES;
        }
        mesh.needReload = true;
    }

    class RenderBlendGpu : public DrawMaterialsScene {};
    class RenderQueueGpu : public DrawMaterialsScene {};
    class MaterialPropertyBlockGpu : public DrawMaterialsScene {};
    class RenderShadowGpu : public DrawMaterialsScene {};
    class ShaderVariantGpu : public DrawMaterialsScene {};
    class TextureReadyGpu : public DrawMaterialsScene {};

    // the library's sources, as a project fork would copy them
    std::string drawLibrarySource(const std::string& key){
        auto found = ShaderVariantCompiler::getLibrarySources().find(key);
        return found != ShaderVariantCompiler::getLibrarySources().end() ? found->second : std::string();
    }

}

// F9, AC9: Blend Dst Color, Src Color (Unity's "multiply x2"): the target becomes 2 x src x dst
TEST_F(RenderBlendGpu, CustomDstColorSrcColor){
    JASPER_REQUIRE_GPU();
    // the target the quad blends over: a grey floor drawn first (opaque, farther)
    Primitive floor(&scene);
    floor.createPlane(8.0f, 8.0f);
    floor.setPosition(0.0f, -2.0f, 0.0f);
    floor.setReceiveLights(false);
    // dark: a 2x multiply over it differs from the quad's own colour (over 0.5 it would not)
    floor.setColor(0.25f, 0.25f, 0.25f);
    Primitive quad(&scene);
    quad.createPlane(2.0f, 2.0f);
    quad.setReceiveLights(false);
    quad.setColor(0.6f, 0.4f, 0.8f);
    ASSERT_TRUE(settle());
    const DrawPixel dst = at(Vector3(3.0f, -2.0f, 0.0f));
    const DrawPixel src = at(Vector3(0.0f, 0.0f, 0.0f));

    Material material = quad.getMaterial();
    material.blendMode = MaterialBlendMode::CUSTOM;
    material.blendSrc = MaterialBlendFactor::DST_COLOR;
    material.blendDst = MaterialBlendFactor::SRC_COLOR;
    material.blendSrcAlpha = MaterialBlendFactor::ZERO;
    material.blendDstAlpha = MaterialBlendFactor::ONE;
    quad.setMaterial(material);
    ASSERT_TRUE(settle());
    const DrawPixel blended = at(Vector3(0.0f, 0.0f, 0.0f));
    EXPECT_LT(dst.r, 100) << "the target is dark";
    auto expected = [](int s, int d){ return std::min(255, (int)std::lround(2.0 * s * d / 255.0)); };
    EXPECT_NEAR(blended.r, expected(src.r, dst.r), 3) << "src " << src.r << " dst " << dst.r;
    EXPECT_NEAR(blended.g, expected(src.g, dst.g), 3) << "src " << src.g << " dst " << dst.g;
    EXPECT_NEAR(blended.b, expected(src.b, dst.b), 3) << "src " << src.b << " dst " << dst.b;
    EXPECT_NO_ENGINE_ERRORS();
}

// F6, AC6: the hull (submesh 1, priority 1, depth write off) draws after the floor drawn later
// at priority 0; equal priorities keep the mesh one entry
TEST_F(RenderQueueGpu, SubmeshPriorityHullDrawsAfterOpaques){
    JASPER_REQUIRE_GPU();
    Mesh character(&scene);
    MeshComponent& mesh = character.getComponent<MeshComponent>();
    makeTwoQuadMesh(mesh, 0.3f, 1.0f, 0.8f, 1.2f);
    character.setReceiveLights(false);
    Material body;
    body.baseColorFactor = Vector4(0.0f, 0.0f, 1.0f, 1.0f);
    body.alphaMode = MaterialAlphaMode::ALPHA_OPAQUE;
    Material hull = body;
    hull.baseColorFactor = Vector4(1.0f, 0.0f, 0.0f, 1.0f);
    hull.blendMode = MaterialBlendMode::NONE;
    hull.depthWrite = MaterialDepthWrite::DISABLED;
    character.setMaterial(0, body);
    character.setMaterial(1, hull);
    // farther: a later distance band, drawn after the character at priority 0
    Primitive floor(&scene);
    floor.createPlane(8.0f, 8.0f);
    floor.setPosition(0.0f, -4.0f, 0.0f);
    floor.setReceiveLights(false);
    floor.setColor(0.0f, 1.0f, 0.0f);
    ASSERT_TRUE(settle());
    // equal priorities: one entry, the hull erased by the floor
    EXPECT_FALSE(mesh.splitPriorities);

    hull.sortPriority = 1;
    character.setMaterial(1, hull);
    ASSERT_TRUE(settle());
    EXPECT_TRUE(mesh.splitPriorities);
    const DrawPixel hullOnly = at(Vector3(0.6f, 1.2f, 0.0f));
    EXPECT_GT(hullOnly.r, 200) << "the hull is erased by the floor drawn after it";
    EXPECT_LT(hullOnly.g, 40);
    const DrawPixel bodyPixel = at(Vector3(0.0f, 1.2f, 0.0f));
    EXPECT_GT(bodyPixel.r, 200) << "the hull (no depth test failure: it is in front) covers the body";
    // past the hull's footprint on the screen
    const DrawPixel floorPixel = at(Vector3(2.6f, -4.0f, 0.0f));
    EXPECT_GT(floorPixel.g, 200);
    EXPECT_NO_ENGINE_ERRORS();
}

// F7, AC7: two meshes with the same material draw their own override colours; a library
// texture override; the built-in Lit's colour, a submesh's over the Mesh's
TEST_F(MaterialPropertyBlockGpu, OverrideWinsOverMaterial){
    JASPER_REQUIRE_GPU();
    Material shared;
    shared.shader = "library:unlit";
    shared.shaderParams = {{"baseColor", Vector4(1.0f, 1.0f, 1.0f, 1.0f)}};
    Primitive a(&scene), b(&scene), c(&scene), d(&scene);
    const float xs[] = {-2.4f, -0.8f, 0.8f, 2.4f};
    Primitive* quads[] = {&a, &b, &c, &d};
    for (int i = 0; i < 4; i++){
        quads[i]->createPlane(1.2f, 1.2f);
        quads[i]->setPosition(xs[i], 0.0f, 0.0f);
        quads[i]->setReceiveLights(false);
    }
    a.setMaterial(shared);
    b.setMaterial(shared);
    c.setMaterial(shared);
    d.setColor(1.0f, 1.0f, 1.0f);

    ShaderPropertyOverrides red;
    red.values = {{"baseColor", Vector4(1.0f, 0.0f, 0.0f, 1.0f)}};
    ShaderPropertyOverrides green;
    green.values = {{"baseColor", Vector4(0.0f, 1.0f, 0.0f, 1.0f)}};
    a.setPropertyBlock(red);
    b.setPropertyBlock(green);
    // c: the material's colour, its base map overridden (blue)
    ShaderPropertyOverrides map;
    map.textures = {{"u_baseTexture", solidTexture("p2-override-blue", 0, 0, 255)}};
    c.setPropertyBlock(map);
    // d (built-in Lit): the Mesh's colour, then its submesh's wins
    ShaderPropertyOverrides lit;
    lit.values = {{"baseColorFactor", Vector4(1.0f, 0.0f, 1.0f, 1.0f)}};
    d.setPropertyBlock(lit);
    ASSERT_TRUE(settle());

    const DrawPixel pa = at(Vector3(xs[0], 0.0f, 0.0f));
    const DrawPixel pb = at(Vector3(xs[1], 0.0f, 0.0f));
    const DrawPixel pc = at(Vector3(xs[2], 0.0f, 0.0f));
    const DrawPixel pd = at(Vector3(xs[3], 0.0f, 0.0f));
    EXPECT_GT(pa.r, 200);
    EXPECT_LT(pa.g, 40);
    EXPECT_LT(pb.r, 40);
    EXPECT_GT(pb.g, 200);
    EXPECT_LT(pc.r, 40);
    EXPECT_GT(pc.b, 200);
    EXPECT_GT(pd.r, 200);
    EXPECT_LT(pd.g, 40);
    EXPECT_GT(pd.b, 200);

    ShaderPropertyOverrides submeshYellow;
    submeshYellow.values = {{"baseColorFactor", Vector4(1.0f, 1.0f, 0.0f, 1.0f)}};
    d.setPropertyBlock(submeshYellow, 0);
    // a cleared block: the material again
    a.setPropertyBlock(ShaderPropertyOverrides());
    ASSERT_TRUE(settle());
    const DrawPixel yellow = at(Vector3(xs[3], 0.0f, 0.0f));
    EXPECT_GT(yellow.r, 200);
    EXPECT_GT(yellow.g, 200);
    EXPECT_LT(yellow.b, 40);
    const DrawPixel white = at(Vector3(xs[0], 0.0f, 0.0f));
    EXPECT_GT(white.g, 200);
    EXPECT_GT(white.b, 200);
    EXPECT_NO_ENGINE_ERRORS();
}

namespace {

    // shaders/HalfClip: library:unlit's sources as a project shader with its own depth program,
    // which leaves out the half of the quad at u > 0.5
    bool readHalfClip(const std::string& path, std::string& content){
        if (path == "shaders/HalfClip.frag"){
            content = drawLibrarySource("library/unlit.frag");
            return !content.empty();
        }
        if (path == "shaders/HalfClip.vert"){
            content = drawLibrarySource("library/surface.vert");
            return !content.empty();
        }
        if (path == "shaders/HalfClip_depth.vert"){
            content = "#version 450\n#define SURFACE_DEPTH_VARYINGS\n#include \"includes/surface_depth_vs.glsl\"\n";
            return true;
        }
        if (path == "shaders/HalfClip_depth.frag"){
            content = "#version 450\n#define SURFACE_DEPTH_DISCARD\n#include \"includes/surface_depth_fs.glsl\"\n"
                      "bool surfaceDepthDiscard(vec2 uv, vec4 color){ return uv.x > 0.5; }\n";
            return true;
        }
        return false;
    }

}

// F10, AC10: the occluder's shadow on the floor is cut where its own depth program discards
TEST_F(RenderShadowGpu, ProjectShaderDepthProgramClipsShadow){
    JASPER_REQUIRE_GPU();
    ShaderManifests::setForkReader(readHalfClip);
    ShaderManifests::clearForks();
    Primitive floor(&scene);
    floor.createPlane(10.0f, 10.0f);
    floor.setPosition(0.0f, -1.0f, 0.0f);
    floor.setColor(0.8f, 0.8f, 0.8f);
    Primitive occluder(&scene);
    occluder.createPlane(2.0f, 2.0f);
    occluder.setPosition(0.0f, 1.0f, 0.0f);
    Material material = occluder.getMaterial();
    material.shader = "shaders/HalfClip";
    occluder.setMaterial(material);
    Light sun(&scene);
    sun.setType(LightType::DIRECTIONAL);
    sun.setDirection(0.0f, -1.0f, 0.0f);
    sun.setIntensity(2.0f);
    sun.setShadows(true);
    // seen from the side, below the occluder: the floor's lit and shadowed parts
    camera.setPosition(0.0f, 2.0f, 6.0f);
    camera.setTarget(0.0f, -1.0f, 0.0f);
    ASSERT_TRUE(settle());

    // the plane's u runs along x: the half at u <= 0.5 (x < 0) casts, the other does not
    const DrawPixel shadowed = at(Vector3(-0.5f, -1.0f, 0.0f));
    const DrawPixel clipped = at(Vector3(0.5f, -1.0f, 0.0f));
    const DrawPixel open = at(Vector3(3.5f, -1.0f, 0.0f));
    EXPECT_LT(shadowed.r + 30, open.r) << "the kept half casts no shadow";
    EXPECT_NEAR(clipped.r, open.r, 12) << "the discarded half still casts a shadow (the depth program is not the shader's)";
    EXPECT_NO_ENGINE_ERRORS();
}

namespace {

    // shaders/VertexNormal: lights in its vertex stage from a_normal (@require vertexnormals),
    // its fragment stage reads no v_normal
    bool readVertexNormal(const std::string& path, std::string& content){
        if (path == "shaders/VertexNormal.vert"){
            content = "#version 450\n#define SURFACE_VERTEX\n#include \"includes/surface_vs.glsl\"\n"
                      "out vec3 v_light;\n"
                      "void surfaceVertex(vec3 worldPosition, vec3 worldNormal, vec4 color, vec2 uv){ v_light = worldNormal * 0.5 + 0.5; }\n";
            return true;
        }
        if (path == "shaders/VertexNormal.frag"){
            content = "#version 450\n// @shader mesh\n// @require vertexnormals\n// @state blend=opaque\n"
                      "in vec3 v_light;\n"
                      "out vec4 g_finalColor;\n"
                      "#include \"includes/output.glsl\"\n"
                      "void main(){ g_finalColor = finalizeColor(vec4(v_light, 1.0)); }\n";
            return true;
        }
        return false;
    }

}

// F13, AC13: the box's side faces the camera (normal +Z): the colour is that normal, not the +Y
// stand-in a mesh without normals gets; it builds with no v_normal read in the fragment stage
TEST_F(ShaderVariantGpu, VertexNormalsWithoutVarying){
    JASPER_REQUIRE_GPU();
    ShaderManifests::setForkReader(readVertexNormal);
    ShaderManifests::clearForks();
    std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get("shaders/VertexNormal");
    ASSERT_TRUE(manifest && manifest->present);
    EXPECT_FALSE(manifest->hasErrors());
    EXPECT_EQ(manifest->require & ShaderRequire_Normals, 0u);
    Primitive box(&scene);
    box.createBox(2.0f, 2.0f, 2.0f);
    box.setReceiveLights(false);
    Material material = box.getMaterial();
    material.shader = "shaders/VertexNormal";
    box.setMaterial(material);
    camera.setPosition(0.0f, 0.0f, 6.0f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(settle());
    const DrawPixel face = at(Vector3(0.0f, 0.0f, 1.0f));
    // (0.5, 0.5, 1) linear: blue the largest; the +Y stand-in would make green the largest
    EXPECT_GT(face.b, face.g + 30) << "r " << face.r << " g " << face.g << " b " << face.b;
    const MeshComponent& mesh = box.getComponent<MeshComponent>();
    ASSERT_TRUE(mesh.submeshes[0].shader);
    EXPECT_NE(mesh.submeshes[0].shader->shaderData.getAttrIndex(AttributeType::NORMAL), -1) << "no a_normal input";
    EXPECT_NO_ENGINE_ERRORS();
}

// F12, AC12: not ready before its GPU texture is made, ready after load() and an upload
TEST_F(TextureReadyGpu, ReadyAfterLoad){
    JASPER_REQUIRE_GPU();
    Texture texture = solidTexture("p2-ready-texture", 10, 20, 30);
    EXPECT_FALSE(texture.isReady());
    texture.load();
    EXPECT_FALSE(texture.isReady()) << "load() decodes; the GPU texture is made by a draw or getRender";
    ASSERT_NE(texture.getRender(), nullptr);
    EXPECT_TRUE(texture.isReady());
    texture.destroy();
    EXPECT_NO_ENGINE_ERRORS();
}

}  // namespace DrawMaterialsGpuTest_cpp
