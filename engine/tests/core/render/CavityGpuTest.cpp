// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S100 Cavity on the headless device (docs/specs/S100-cavity.md R1-R8, AC1-AC8): convex edges
// get lighter, concave creases darker, by one multiply draw inside the colour pass. Most tests draw
// the same frame with Cavity off and on and assert on the per-pixel difference (on minus off) at
// points projected from the scene, so the property each criterion names is explicit and holds on
// every backend: an edge brightens, a face centre stays, a seam between two objects stays, a smooth
// sphere shows no triangle lines. The numeric scenes use unlit materials, so a pixel moves only by
// Cavity. A few goldens of a rock, a smooth sphere and a bevelled cube pin the look
// (engine/tests/core/golden/cavity_*.png; JASPER_TEST_UPDATE_GOLDEN=1 writes them on glcore and
// Vulkan matches them).

#include "Engine.h"
#include "Scene.h"
#include "SceneSettings.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Mesh.h"
#include "object/Primitive.h"
#include "object/ui/Image.h"
#include "pool/ShaderPool.h"
#include "render/RenderStats.h"
#include "render/SystemRender.h"
#include "shader/ShaderUniforms.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"
#include "texture/Material.h"
#include "texture/TextureData.h"

#include "GpuEnvironment.h"
#include "ImageCompare.h"
#include "LogCapture.h"
#include "Readback.h"
#include "TerrainGpuFixture.h"
#include "support/AllocationCounter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace CavityGpuTest_cpp {

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // how far (levels of 255, the mean of R, G and B) Cavity must move a pixel to count as a ridge
    // or a valley, and how far a pixel it must leave alone may move
    constexpr double CAVITY_VISIBLE = 6.0;
    constexpr double CAVITY_NEUTRAL = 2.0;

    struct CavityFrame {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> rgba;

        bool valid() const{
            return width > 0 && height > 0 && rgba.size() == (size_t)width * height * 4;
        }
        const uint8_t* at(int x, int y) const{
            x = std::clamp(x, 0, width - 1);
            y = std::clamp(y, 0, height - 1);
            return &rgba[((size_t)y * width + x) * 4];
        }
        double luma(int x, int y) const{
            const uint8_t* p = at(x, y);
            return ((double)p[0] + (double)p[1] + (double)p[2]) / 3.0;
        }
    };

    CavityFrame cavityReadTarget(){
        Framebuffer& target = GpuEnvironment::target();
        CavityFrame frame;
        frame.width = (int)target.getWidth();
        frame.height = (int)target.getHeight();
        if (!readTexture(target.getRender().getColorTexture(), frame.width, frame.height, frame.rgba))
            frame.rgba.clear();
        return frame;
    }

    // what Cavity did to a pixel: on minus off
    double cavityDelta(const CavityFrame& on, const CavityFrame& off, int x, int y){
        return on.luma(x, y) - off.luma(x, y);
    }

    // the most brightened (or darkened) pixel within r of (x, y)
    double cavityWindowMax(const CavityFrame& on, const CavityFrame& off, int x, int y, int r){
        double best = -255.0;
        for (int j = y - r; j <= y + r; j++)
            for (int i = x - r; i <= x + r; i++)
                best = std::max(best, cavityDelta(on, off, i, j));
        return best;
    }

    double cavityWindowMin(const CavityFrame& on, const CavityFrame& off, int x, int y, int r){
        double best = 255.0;
        for (int j = y - r; j <= y + r; j++)
            for (int i = x - r; i <= x + r; i++)
                best = std::min(best, cavityDelta(on, off, i, j));
        return best;
    }

    // the largest channel difference over the whole frame, its mean, and the share of pixels whose
    // largest channel moved by more than 8
    struct CavityDifference {
        int maxDiff = 0;
        double meanDiff = 0.0;
        double changed = 0.0;
    };

    CavityDifference cavityDifferenceOf(const CavityFrame& a, const CavityFrame& b){
        CavityDifference d;
        if (!a.valid() || a.rgba.size() != b.rgba.size()){
            d.maxDiff = 255;
            d.meanDiff = 255.0;
            d.changed = 1.0;
            return d;
        }
        double sum = 0.0;
        size_t changed = 0;
        for (size_t i = 0; i + 3 < a.rgba.size(); i += 4){
            int largest = 0;
            for (int c = 0; c < 3; c++)
                largest = std::max(largest, std::abs((int)a.rgba[i + c] - (int)b.rgba[i + c]));
            d.maxDiff = std::max(d.maxDiff, largest);
            sum += largest;
            changed += largest > 8 ? 1 : 0;
        }
        const double pixels = (double)(a.rgba.size() / 4);
        d.meanDiff = sum / pixels;
        d.changed = (double)changed / pixels;
        return d;
    }

    // what Cavity did over the whole frame: the sum of the brightening and of the darkening
    // (levels times pixels)
    struct CavityEnergy {
        double light = 0.0;
        double dark = 0.0;
    };

    CavityEnergy cavityEnergyOf(const CavityFrame& on, const CavityFrame& off){
        CavityEnergy energy;
        if (!on.valid() || on.rgba.size() != off.rgba.size())
            return energy;
        for (int y = 0; y < on.height; y++){
            for (int x = 0; x < on.width; x++){
                const double d = cavityDelta(on, off, x, y);
                if (d > 0.0)
                    energy.light += d;
                else
                    energy.dark -= d;
            }
        }
        return energy;
    }

    // The mean of on - off over the rows y0..y1 at each column cx - half .. cx + half: the profile
    // across an edge that runs straight down the frame at cx
    std::vector<double> cavityColumnProfile(const CavityFrame& on, const CavityFrame& off, int cx, int y0, int y1, int half){
        std::vector<double> profile;
        const int rows = std::max(1, y1 - y0 + 1);
        for (int x = cx - half; x <= cx + half; x++){
            double sum = 0.0;
            for (int y = y0; y <= y1; y++)
                sum += cavityDelta(on, off, x, y);
            profile.push_back(sum / rows);
        }
        return profile;
    }

    // the width of the run around the profile's peak where it is at least share x peak
    int cavityPeakWidth(const std::vector<double>& profile, double share, double& peak){
        if (profile.empty()){
            peak = 0.0;
            return 0;
        }
        const size_t top = (size_t)(std::max_element(profile.begin(), profile.end()) - profile.begin());
        peak = profile[top];
        if (peak <= 0.0)
            return 0;
        size_t left = top, right = top;
        while (left > 0 && profile[left - 1] >= share * peak)
            left--;
        while (right + 1 < profile.size() && profile[right + 1] >= share * peak)
            right++;
        return (int)(right - left + 1);
    }

    // How much on - off varies down each column of the profile (the edge runs straight down it,
    // so a smooth bevel is the same in every row there): the mean of the columns' standard deviations
    double cavityColumnNoise(const CavityFrame& on, const CavityFrame& off, int cx, int y0, int y1, int half){
        double total = 0.0;
        int columns = 0;
        for (int x = cx - half; x <= cx + half; x++){
            double sum = 0.0, squares = 0.0;
            int n = 0;
            for (int y = y0; y <= y1; y++){
                const double d = cavityDelta(on, off, x, y);
                sum += d;
                squares += d * d;
                n++;
            }
            if (n < 2)
                continue;
            const double mean = sum / n;
            total += std::sqrt(std::max(0.0, squares / n - mean * mean));
            columns++;
        }
        return columns > 0 ? total / columns : 0.0;
    }

    // and across it: the mean absolute second difference of the profile (steps of a coarse kernel)
    double cavityProfileRoughness(const std::vector<double>& profile){
        if (profile.size() < 3)
            return 0.0;
        double sum = 0.0;
        for (size_t i = 1; i + 1 < profile.size(); i++)
            sum += std::abs(profile[i + 1] - 2.0 * profile[i] + profile[i - 1]);
        return sum / (double)(profile.size() - 2);
    }

    // A line detector along a row (dx = 1) or a column (dy = 1) of on - off: how far a pixel stands
    // out of the mean of its neighbours 3 pixels away on both sides, the largest over the span
    double cavityLineStrength(const CavityFrame& on, const CavityFrame& off, int x0, int y0, int dx, int dy, int count){
        double strongest = 0.0;
        for (int k = 3; k < count - 3; k++){
            const int x = x0 + k * dx, y = y0 + k * dy;
            const double centre = cavityDelta(on, off, x, y);
            const double around = 0.5 * (cavityDelta(on, off, x - 3 * dx, y - 3 * dy) + cavityDelta(on, off, x + 3 * dx, y + 3 * dy));
            strongest = std::max(strongest, std::abs(centre - around));
        }
        return strongest;
    }

    // A flat-shaded polygon of a hand-made mesh: convex, its points in order around it (either
    // way), and its outward unit normal, which every one of its vertices gets
    struct CavityPolygon {
        std::vector<Vector3> points;
        Vector3 normal;
    };

    // fills the Mesh's single submesh with the polygons, wound counter-clockwise seen from their
    // normal (the engine's front faces), with the attributes a Primitive has
    void cavityFillPolygons(Scene& scene, Entity entity, const std::vector<CavityPolygon>& polygons){
        MeshComponent& mesh = scene.getComponent<MeshComponent>(entity);
        mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
        mesh.numSubmeshes = 1;
        mesh.buffer.clear();
        mesh.buffer.addAttribute(AttributeType::POSITION, 3);
        mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
        mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
        mesh.buffer.addAttribute(AttributeType::COLOR, 4);
        std::vector<uint16_t> indices;
        size_t base = 0;
        for (const CavityPolygon& polygon : polygons){
            std::vector<Vector3> points = polygon.points;
            const Vector3 turn = (points[1] - points[0]).crossProduct(points[2] - points[0]);
            if (turn.dotProduct(polygon.normal) < 0.0f)
                std::reverse(points.begin(), points.end());
            for (const Vector3& point : points){
                mesh.buffer.addVector3(AttributeType::POSITION, point);
                mesh.buffer.addVector2(AttributeType::TEXCOORD1, Vector2(0.0f, 0.0f));
                mesh.buffer.addVector3(AttributeType::NORMAL, polygon.normal);
                mesh.buffer.addVector4(AttributeType::COLOR, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
            }
            for (size_t k = 1; k + 1 < points.size(); k++){
                indices.push_back((uint16_t)base);
                indices.push_back((uint16_t)(base + k));
                indices.push_back((uint16_t)(base + k + 1));
            }
            base += points.size();
        }
        mesh.indices.clear();
        mesh.indices.setValues(0, mesh.indices.getAttribute(AttributeType::INDEX), indices.size(), (char*)indices.data(), sizeof(uint16_t));
        scene.getSystem<MeshSystem>()->calculateMeshAABB(mesh);
    }

    Vector3 cavityAxes(int i, float a, int j, float b, int k, float c){
        float v[3] = {0.0f, 0.0f, 0.0f};
        v[i] = a;
        v[j] = b;
        v[k] = c;
        return Vector3(v[0], v[1], v[2]);
    }

    // A cube of side 2h whose 12 edges are cut by 45-degree chamfers of width b, and its 8
    // corners by triangles: 26 flat faces
    std::vector<CavityPolygon> cavityBevelledCube(float h, float b){
        std::vector<CavityPolygon> polygons;
        const float in = h - b;
        for (int k = 0; k < 3; k++){
            const int u = (k + 1) % 3, v = (k + 2) % 3;
            for (float s : {-1.0f, 1.0f}){
                CavityPolygon face;
                face.points = {cavityAxes(k, s * h, u, in, v, in), cavityAxes(k, s * h, u, in, v, -in),
                               cavityAxes(k, s * h, u, -in, v, -in), cavityAxes(k, s * h, u, -in, v, in)};
                face.normal = cavityAxes(k, s, u, 0.0f, v, 0.0f);
                polygons.push_back(face);
            }
        }
        const float diagonal = 1.0f / std::sqrt(2.0f);
        for (int i = 0; i < 3; i++){
            for (int j = i + 1; j < 3; j++){
                const int k = 3 - i - j;
                for (float si : {-1.0f, 1.0f}){
                    for (float sj : {-1.0f, 1.0f}){
                        CavityPolygon chamfer;
                        chamfer.points = {cavityAxes(i, si * h, j, sj * in, k, in), cavityAxes(i, si * h, j, sj * in, k, -in),
                                          cavityAxes(i, si * in, j, sj * h, k, -in), cavityAxes(i, si * in, j, sj * h, k, in)};
                        chamfer.normal = cavityAxes(i, si * diagonal, j, sj * diagonal, k, 0.0f);
                        polygons.push_back(chamfer);
                    }
                }
            }
        }
        const float corner = 1.0f / std::sqrt(3.0f);
        for (float sx : {-1.0f, 1.0f}){
            for (float sy : {-1.0f, 1.0f}){
                for (float sz : {-1.0f, 1.0f}){
                    CavityPolygon triangle;
                    triangle.points = {Vector3(sx * h, sy * in, sz * in), Vector3(sx * in, sy * h, sz * in), Vector3(sx * in, sy * in, sz * h)};
                    triangle.normal = Vector3(sx * corner, sy * corner, sz * corner);
                    polygons.push_back(triangle);
                }
            }
        }
        return polygons;
    }

    // A low-poly rock: an icosahedron split once (80 faces), each vertex pushed in or out by a
    // smooth function of where it is (so a vertex shared by faces moves the same in all), flat shaded
    std::vector<CavityPolygon> cavityRock(float radius){
        const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
        std::vector<Vector3> v = {Vector3(-1, t, 0), Vector3(1, t, 0), Vector3(-1, -t, 0), Vector3(1, -t, 0),
                                  Vector3(0, -1, t), Vector3(0, 1, t), Vector3(0, -1, -t), Vector3(0, 1, -t),
                                  Vector3(t, 0, -1), Vector3(t, 0, 1), Vector3(-t, 0, -1), Vector3(-t, 0, 1)};
        for (Vector3& p : v)
            p.normalize();
        const int faces[20][3] = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
                                  {11, 10, 2}, {10, 7, 6}, {7, 1, 8}, {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8},
                                  {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
        auto surface = [radius](Vector3 p){
            p.normalize();
            const float bump = 1.0f + 0.14f * std::sin(p.x * 7.1f + p.y * 3.3f) * std::cos(p.z * 5.7f + p.x * 1.9f);
            return p * (radius * bump);
        };
        auto middle = [](const Vector3& a, const Vector3& b){
            return (a + b) * 0.5f;
        };
        std::vector<CavityPolygon> polygons;
        auto add = [&](const Vector3& a, const Vector3& b, const Vector3& c){
            CavityPolygon triangle;
            triangle.points = {surface(a), surface(b), surface(c)};
            Vector3 normal = (triangle.points[1] - triangle.points[0]).crossProduct(triangle.points[2] - triangle.points[0]);
            normal.normalize();
            const Vector3 centroid = (triangle.points[0] + triangle.points[1] + triangle.points[2]) * (1.0f / 3.0f);
            if (normal.dotProduct(centroid) < 0.0f)
                normal = normal * -1.0f;
            triangle.normal = normal;
            polygons.push_back(triangle);
        };
        for (const auto& face : faces){
            const Vector3& a = v[face[0]];
            const Vector3& b = v[face[1]];
            const Vector3& c = v[face[2]];
            const Vector3 ab = middle(a, b), bc = middle(b, c), ca = middle(c, a);
            add(a, ab, ca);
            add(b, bc, ab);
            add(c, ca, bc);
            add(ab, bc, ca);
        }
        return polygons;
    }

    // A step seen from +z: a floor (y = 0, z 0..1), a riser facing the camera (z = 0, y 0..0.5) and
    // a top (y = 0.5, z -0.8..0), each 1.2 wide: a concave crease along (x, 0, 0) and a convex edge
    // along (x, 0.5, 0), both horizontal on the screen of a camera on the z axis
    std::vector<CavityPolygon> cavityStep(){
        const float w = 0.6f;
        return {
            {{Vector3(-w, 0.0f, 0.0f), Vector3(w, 0.0f, 0.0f), Vector3(w, 0.0f, 1.0f), Vector3(-w, 0.0f, 1.0f)}, Vector3(0.0f, 1.0f, 0.0f)},
            {{Vector3(-w, 0.0f, 0.0f), Vector3(w, 0.0f, 0.0f), Vector3(w, 0.5f, 0.0f), Vector3(-w, 0.5f, 0.0f)}, Vector3(0.0f, 0.0f, 1.0f)},
            {{Vector3(-w, 0.5f, -0.8f), Vector3(w, 0.5f, -0.8f), Vector3(w, 0.5f, 0.0f), Vector3(-w, 0.5f, 0.0f)}, Vector3(0.0f, 1.0f, 0.0f)},
        };
    }

    // the built-in Lit without lighting: one colour on every face, so only Cavity moves a pixel
    Material cavityUnlit(float r, float g, float b){
        Material material;
        material.baseColorFactor = Vector4(r, g, b, 1.0f);
        material.metallicFactor = 0.0f;
        material.roughnessFactor = 1.0f;
        material.lighting = MaterialLighting::NONE;
        return material;
    }

    Material cavityLibrary(const char* shader, float grey){
        Material material;
        material.shader = shader;
        material.metallicFactor = 0.0f;
        material.roughnessFactor = 1.0f;
        ShaderUniforms::set(material.shaderParams, "baseColor", Vector4(grey, grey, grey, 1.0f));
        return material;
    }

    PostProcessingSettings cavityOn(CavityType type){
        PostProcessingSettings settings;
        settings.cavityEnabled = true;
        settings.cavityType = type;
        return settings;
    }

    // A 4 x 4 image of one colour (a TextureData made from memory points at its pixels)
    TextureData cavitySolidImage(uint8_t r, uint8_t g, uint8_t b){
        static std::vector<uint8_t> pixels;
        pixels.clear();
        for (int i = 0; i < 16; i++)
            pixels.insert(pixels.end(), {r, g, b, 255});
        return TextureData(4, 4, (unsigned int)pixels.size(), ColorFormat::RGBA, 4, pixels.data());
    }

    // what a frame is made of (AC4): its passes in order with their colour attachments, its draws,
    // the variants its meshes drew with, the main thread's allocations in a steady frame, its pixels
    struct CavityFrameCost {
        std::vector<std::string> passes;
        uint32_t draws = 0;
        std::vector<std::string> shaders;
        uint64_t allocations = 0;
        uint64_t image = 0;     // FNV-1a of the target's pixels
    };

    std::string cavityDescribe(const CavityFrameCost& cost){
        std::ostringstream out;
        out << "draws " << cost.draws << ", allocations " << cost.allocations << ", image " << cost.image << "\n";
        for (const std::string& pass : cost.passes)
            out << "  pass " << pass << "\n";
        for (const std::string& shader : cost.shaders)
            out << "  shader " << shader << "\n";
        return out.str();
    }

    // the rock, the smooth sphere and the bevelled cube of the goldens on a floor, lit by a sun
    struct CavityShowcase {
        Primitive floor;
        Mesh rock;
        Primitive sphere;
        Mesh cube;
        Light sun;

        explicit CavityShowcase(Scene& scene): floor(&scene), rock(&scene), sphere(&scene), cube(&scene), sun(&scene){
            floor.createPlane(8.0f, 8.0f);
            floor.setMaterial(litMaterial(0.55f, 0.55f, 0.55f));
            cavityFillPolygons(scene, rock.getEntity(), cavityRock(0.55f));
            rock.setMaterial(litMaterial(0.55f, 0.45f, 0.35f));
            rock.setPosition(-1.55f, 0.48f, 0.1f);
            sphere.createSphere(0.55f, 24, 16);
            sphere.setMaterial(litMaterial(0.35f, 0.5f, 0.7f));
            sphere.setPosition(0.0f, 0.55f, 0.0f);
            cavityFillPolygons(scene, cube.getEntity(), cavityBevelledCube(0.45f, 0.08f));
            cube.setMaterial(litMaterial(0.7f, 0.6f, 0.3f));
            cube.setPosition(1.55f, 0.45f, 0.0f);
            cube.setRotation(0.0f, 30.0f, 0.0f);
            sun.setType(LightType::DIRECTIONAL);
            sun.setDirection(-0.4f, -1.0f, -0.6f);
            sun.setIntensity(1.5f);
            sun.setShadows(false);
        }

        static Material litMaterial(float r, float g, float b){
            Material material;
            material.baseColorFactor = Vector4(r, g, b, 1.0f);
            material.metallicFactor = 0.0f;
            material.roughnessFactor = 0.8f;
            return material;
        }
    };

    class CavityGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        bool viewChanged = false;

        void SetUp() override{
            scene.setBackgroundColor(0.1f, 0.1f, 0.12f, 1.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            frontView();
            camera.setFarClip(100.0f);
            camera.activate();
            Engine::setScene(&scene);
        }

        void TearDown() override{
            Engine::removeScene(&scene);
            if (viewChanged && GpuEnvironment::isReady())
                GpuEnvironment::resetView();
        }

        // the camera of most tests: on the z axis, a little above, looking at the unit cube's middle
        void frontView(){
            camera.setPosition(0.0f, 1.4f, 3.4f);
            camera.setTarget(0.0f, 0.5f, 0.0f);
        }

        void setView(int width, int height){
            GpuEnvironment::setView(width, height, width, height, Scaling::STRETCH);
            viewChanged = true;
        }

        RenderSystem& renderSystem(){
            return *scene.getSystem<RenderSystem>();
        }

        CavityStatus status(){
            return renderSystem().getCavityStatus();
        }

        // frames until everything is loaded and no mesh waits for a variant (turning Cavity on or
        // off may reload the meshes' depth programs), then a few more
        bool render(){
            Scene* current = &scene;
            const auto settled = [current](){
                if (!current->getSystem<RenderSystem>()->isAllLoaded())
                    return false;
                auto meshes = current->getComponentArray<MeshComponent>();
                for (size_t i = 0; i < meshes->size(); i++){
                    const MeshComponent& mesh = meshes->getComponentFromIndex(i);
                    if (mesh.needReload || mesh.needVariantReload)
                        return false;
                }
                return true;
            };
            const bool ready = GpuEnvironment::runFrames(settled, 600);
            GpuEnvironment::drawFrames(2);
            return ready && settled();
        }

        CavityFrame frameWith(const PostProcessingSettings& settings){
            scene.setPostProcessing(settings);
            EXPECT_TRUE(render()) << "the scene never finished loading";
            return cavityReadTarget();
        }

        CavityFrame frameOff(){
            return frameWith(PostProcessingSettings());
        }

        // the target pixel a world point is drawn at (rows top first)
        bool project(const Vector3& world, int& x, int& y){
            const Vector4 clip = camera.getViewProjectionMatrix() * Vector4(world.x, world.y, world.z, 1.0f);
            if (clip.w <= 0.0f)
                return false;
            const float width = (float)GpuEnvironment::target().getWidth();
            const float height = (float)GpuEnvironment::target().getHeight();
            x = (int)std::lround((clip.x / clip.w * 0.5f + 0.5f) * width);
            y = (int)std::lround((1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * height);
            return x >= 0 && y >= 0 && x < (int)width && y < (int)height;
        }

        // The largest change within 3 pixels of the outline of a box turned 45 degrees about y
        // (its side corners, half a diagonal from its centre), over the middle 60% of its height,
        // seen by a level camera (the corners are vertical on screen)
        double silhouetteChange(const CavityFrame& on, const CavityFrame& off, const Vector3& centre, float halfDiagonal, float halfHeight){
            double worst = 0.0;
            for (float side : {-1.0f, 1.0f}){
                int x = 0, top = 0, bottom = 0, unused = 0;
                EXPECT_TRUE(project(centre + Vector3(side * halfDiagonal, 0.6f * halfHeight, 0.0f), x, top));
                EXPECT_TRUE(project(centre + Vector3(side * halfDiagonal, -0.6f * halfHeight, 0.0f), unused, bottom));
                for (int y = top; y <= bottom; y++)
                    for (int i = -3; i <= 3; i++)
                        worst = std::max(worst, std::abs(cavityDelta(on, off, x + i, y)));
            }
            return worst;
        }

        // the drawn frame's Cavity status says it drew once
        void expectDrawn(const char* what){
            const CavityStatus drawn = status();
            EXPECT_TRUE(drawn.active) << what << ": Cavity was not drawn";
            EXPECT_EQ(drawn.draws, 1u) << what;
        }

        // A unit cube standing on y = 0, turned 45 degrees: its front vertical edge (0, y, 0.7071)
        // runs straight down the middle of the front view's frame, between two faces in view
        static constexpr float CUBE_EDGE_Z = 0.70710678f;
        void makeFrontCube(Primitive& cube, const Material& material){
            cube.createBox(1.0f, 1.0f, 1.0f);
            cube.setPosition(0.0f, 0.5f, 0.0f);
            cube.setRotation(0.0f, 45.0f, 0.0f);
            cube.setMaterial(material);
        }

        // what Cavity did at the front cube's edge (the most brightened pixel near its middle)
        // and at the centres of its two faces in view (the larger change of the two)
        struct CubeReading {
            double ridge = 0.0;
            double faces = 0.0;
        };
        CubeReading readFrontCube(const CavityFrame& on, const CavityFrame& off){
            CubeReading reading;
            int x = 0, y = 0;
            EXPECT_TRUE(project(Vector3(0.0f, 0.5f, CUBE_EDGE_Z), x, y));
            reading.ridge = cavityWindowMax(on, off, x, y, 4);
            for (float side : {-1.0f, 1.0f}){
                EXPECT_TRUE(project(Vector3(side * 0.35355f, 0.5f, 0.35355f), x, y));
                reading.faces = std::max(reading.faces, std::abs(cavityDelta(on, off, x, y)));
            }
            return reading;
        }

        // the frame as drawn now, for AC4
        CavityFrameCost recordCost(){
            // a reader asks for the passes' details: the frames after record them
            SystemRender::getPassList(SystemRender::FRAME_STATS_FRAME, false);
            GpuEnvironment::drawFrames(2);
            uint64_t least = UINT64_MAX;
            for (int i = 0; i < 5; i++){
                testsupport::ThreadAllocationCount count;
                Engine::systemDraw();
                least = std::min(least, count.count());
            }
            CavityFrameCost cost;
            cost.allocations = testsupport::countsAllocations() ? least : 0;
            cost.draws = Engine::getFrameStats().drawCalls;
            const PassList list = SystemRender::getPassList(SystemRender::FRAME_STATS_FRAME, false);
            for (const PassRecord& pass : list.passes)
                cost.passes.push_back(std::string(pass.label) + " (" + std::to_string(pass.numColors) + " colour)");
            const CavityFrame frame = cavityReadTarget();
            uint64_t hash = 1469598103934665603ull;
            for (uint8_t byte : frame.rgba){
                hash ^= byte;
                hash *= 1099511628211ull;
            }
            cost.image = hash;
            std::set<std::string> shaders;
            auto meshes = scene.getComponentArray<MeshComponent>();
            for (size_t i = 0; i < meshes->size(); i++){
                const MeshComponent& mesh = meshes->getComponentFromIndex(i);
                for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
                    shaders.insert(ShaderPool::getShaderStr(ShaderType::MESH, mesh.submeshes[s].shaderProperties));
                    if (mesh.submeshes[s].depthShaderProperties != 0)
                        shaders.insert(ShaderPool::getShaderStr(ShaderType::DEPTH, mesh.submeshes[s].depthShaderProperties));
                }
            }
            cost.shaders.assign(shaders.begin(), shaders.end());
            return cost;
        }

        // the colour attachments of the frame's sampled depth pre-pass (0 when there is none)
        int prepassColors(){
            SystemRender::getPassList(SystemRender::FRAME_STATS_FRAME, false);
            GpuEnvironment::drawFrames(2);
            const PassList list = SystemRender::getPassList(SystemRender::FRAME_STATS_FRAME, false);
            for (const PassRecord& pass : list.passes){
                if (std::string(pass.label) == "depth pre-pass")
                    return pass.numColors;
            }
            return 0;
        }

        // the showcase from above and in front, for the goldens
        void showcaseView(){
            camera.setPosition(0.0f, 2.0f, 5.2f);
            camera.setTarget(0.0f, 0.4f, 0.0f);
        }

        // AC1: the golden of the showcase with these settings, which must differ from Cavity off
        void expectShowcaseGolden(const PostProcessingSettings& settings, const char* golden){
            PostProcessingSettings off = settings;
            off.cavityEnabled = false;
            const CavityFrame without = frameWith(off);
            const CavityFrame with = frameWith(settings);
            expectDrawn(golden);
            EXPECT_GOLDEN(golden);
            const CavityDifference difference = cavityDifferenceOf(with, without);
            EXPECT_GT(difference.changed, 0.002) << golden << ": Cavity changed almost nothing (mean " << difference.meanDiff << ")";
        }
    };

}

// AC5: with the default settings (Both) a unit cube's convex edge is brighter than its faces, whose
// centres keep their colour; a step's inner corner is darker, its outer edge brighter. The cube's
// edge is vertical on screen and the step's are horizontal, so the left-right and the up-down taps
// both count, and a backend whose screen-up is flipped swaps the step's ridge and valley (R3)
TEST_F(CavityGpu, DefaultsBrightenCubeEdgesAndDarkenInnerCorners){
    JASPER_REQUIRE_GPU();
    Primitive cube(&scene);
    makeFrontCube(cube, cavityUnlit(0.3f, 0.3f, 0.3f));
    Mesh step(&scene);
    cavityFillPolygons(scene, step.getEntity(), cavityStep());
    step.setMaterial(cavityUnlit(0.3f, 0.3f, 0.3f));
    step.setVisible(false);

    const CavityFrame cubeOff = frameOff();
    const CavityFrame cubeOn = frameWith(cavityOn(CavityType::BOTH));
    expectDrawn("cube");
    const CubeReading reading = readFrontCube(cubeOn, cubeOff);
    EXPECT_GE(reading.ridge, CAVITY_VISIBLE) << "the cube's convex edge is not brighter";
    EXPECT_LE(reading.faces, CAVITY_NEUTRAL) << "the centre of a flat face changed";

    cube.setVisible(false);
    step.setVisible(true);
    const CavityFrame stepOff = frameOff();
    const CavityFrame stepOn = frameWith(cavityOn(CavityType::BOTH));
    expectDrawn("step");
    int x = 0, y = 0;
    ASSERT_TRUE(project(Vector3(0.0f, 0.0f, 0.0f), x, y));
    EXPECT_LE(cavityWindowMin(stepOn, stepOff, x, y, 4), -CAVITY_VISIBLE) << "the inner corner is not darker";
    ASSERT_TRUE(project(Vector3(0.0f, 0.5f, 0.0f), x, y));
    EXPECT_GE(cavityWindowMax(stepOn, stepOff, x, y, 4), CAVITY_VISIBLE) << "the step's outer edge is not brighter";
    ASSERT_TRUE(project(Vector3(0.0f, 0.25f, 0.0f), x, y));
    EXPECT_LE(std::abs(cavityDelta(stepOn, stepOff, x, y)), CAVITY_NEUTRAL) << "the riser's centre changed";
    ASSERT_TRUE(project(Vector3(0.0f, 0.0f, 0.55f), x, y));
    EXPECT_LE(std::abs(cavityDelta(stepOn, stepOff, x, y)), CAVITY_NEUTRAL) << "the floor's centre changed";
    EXPECT_NO_ENGINE_ERRORS();
}

// R3, R4: each type on its own brightens the convex edge and leaves the faces; the status reports
// the type and the World samples as drawn (the default 8, none for Screen)
TEST_F(CavityGpu, EachTypeBrightensTheEdgeAndReportsItself){
    JASPER_REQUIRE_GPU();
    Primitive cube(&scene);
    makeFrontCube(cube, cavityUnlit(0.3f, 0.3f, 0.3f));
    const CavityFrame off = frameOff();
    const struct { CavityType type; const char* name; int samples; } types[] = {
        {CavityType::SCREEN, "Screen", 0}, {CavityType::WORLD, "World", 8}, {CavityType::BOTH, "Both", 8}};
    for (const auto& c : types){
        SCOPED_TRACE(c.name);
        const CavityFrame on = frameWith(cavityOn(c.type));
        expectDrawn(c.name);
        const CavityStatus drawn = status();
        EXPECT_EQ(drawn.type, c.type);
        EXPECT_EQ(drawn.worldSamples, c.samples);
        const CubeReading reading = readFrontCube(on, off);
        EXPECT_GE(reading.ridge, CAVITY_VISIBLE);
        EXPECT_LE(reading.faces, CAVITY_NEUTRAL);
    }
    EXPECT_NO_ENGINE_ERRORS();
}

// AC2 (D4): a smooth-shaded low-poly sphere in Screen mode with SSR off (the pre-pass normals)
// shows no line along its triangle edges: along rows and columns through its middle no pixel
// stands out of its neighbours. The faceted rock beside it, flat shaded, does show its edges
// (the detector's red control, and flat meshes keep flat normals)
TEST_F(CavityGpu, SmoothSphereShowsNoTriangleEdges){
    JASPER_REQUIRE_GPU();
    camera.setPosition(0.0f, 0.0f, 5.0f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    Primitive sphere(&scene);
    sphere.createSphere(1.0f, 24, 14);
    sphere.setPosition(1.1f, 0.0f, 0.0f);
    sphere.setMaterial(cavityUnlit(0.3f, 0.3f, 0.3f));
    Mesh rock(&scene);
    cavityFillPolygons(scene, rock.getEntity(), cavityRock(0.9f));
    rock.setPosition(-1.3f, 0.0f, 0.0f);
    rock.setMaterial(cavityUnlit(0.3f, 0.3f, 0.3f));

    const CavityFrame off = frameOff();
    const CavityFrame on = frameWith(cavityOn(CavityType::SCREEN));
    expectDrawn("Screen");
    EXPECT_STREQ(status().normals, "prepass");

    // the middle of a disc of the given centre and on-screen radius, along rows and columns at
    // a quarter and a half of the radius from its centre, 60% of the chord wide
    auto strongestLine = [&](const Vector3& centre, float worldRadius){
        int cx = 0, cy = 0, ex = 0, ey = 0;
        EXPECT_TRUE(project(centre, cx, cy));
        EXPECT_TRUE(project(centre + Vector3(worldRadius, 0.0f, 0.0f), ex, ey));
        const int r = std::abs(ex - cx);
        double strongest = 0.0;
        for (float share : {-0.5f, -0.25f, 0.0f, 0.25f, 0.5f}){
            const int offset = (int)std::lround(share * r);
            const int half = (int)std::lround(0.6 * std::sqrt((double)r * r - (double)offset * offset));
            strongest = std::max(strongest, cavityLineStrength(on, off, cx - half, cy + offset, 1, 0, 2 * half + 1));
            strongest = std::max(strongest, cavityLineStrength(on, off, cx + offset, cy - half, 0, 1, 2 * half + 1));
        }
        return strongest;
    };
    const double sphereLines = strongestLine(Vector3(1.1f, 0.0f, 0.0f), 1.0f);
    const double rockLines = strongestLine(Vector3(-1.3f, 0.0f, 0.0f), 0.85f);
    std::printf("[measured] strongest line: smooth sphere %.2f, faceted rock %.2f\n", sphereLines, rockLines);
    EXPECT_LE(sphereLines, 4.0) << "the smooth sphere shows lines (its triangle edges)";
    EXPECT_GE(rockLines, 8.0) << "the faceted rock shows no lines along its edges";
    EXPECT_NO_ENGINE_ERRORS();
}

// AC3 (R3): in Screen mode no line appears where two objects meet. Both cases have other normals
// on the two sides of the boundary (the front box is turned 45 degrees, its faces against the back
// box's front): a box whose back corner touches the other box's front, and one well in front of
// it. Within 3 pixels of the front box's silhouette nothing changes; its own front edge, one mesh's
// convex edge at the same distance, still brightens (the red control)
TEST_F(CavityGpu, ScreenDrawsNoLineBetweenTouchingOrOverlappingObjects){
    JASPER_REQUIRE_GPU();
    // level with the boxes' middle: the silhouette's corners are vertical on screen
    camera.setPosition(0.0f, 0.5f, 4.0f);
    camera.setTarget(0.0f, 0.5f, 0.0f);
    Primitive back(&scene);
    back.createBox(2.4f, 1.6f, 0.4f);
    back.setPosition(0.0f, 0.5f, -0.6f);
    back.setMaterial(cavityUnlit(0.3f, 0.3f, 0.3f));
    Primitive front(&scene);
    front.createBox(0.5f, 0.5f, 0.5f);
    front.setRotation(0.0f, 45.0f, 0.0f);
    front.setMaterial(cavityUnlit(0.6f, 0.6f, 0.6f));
    const float halfDiagonal = 0.25f * std::sqrt(2.0f);

    // touching: the back corner on the back box's front (z = -0.4); then 0.8 in front of it
    const struct { const char* name; float z; } cases[] = {{"touching", -0.4f + halfDiagonal}, {"in front", 0.4f + halfDiagonal}};
    for (const auto& c : cases){
        SCOPED_TRACE(c.name);
        front.setPosition(0.0f, 0.5f, c.z);
        const CavityFrame off = frameOff();
        const CavityFrame on = frameWith(cavityOn(CavityType::SCREEN));
        expectDrawn(c.name);
        const Vector3 centre(0.0f, 0.5f, c.z);
        const double outline = silhouetteChange(on, off, centre, halfDiagonal, 0.25f);
        int x = 0, y = 0;
        ASSERT_TRUE(project(centre + Vector3(0.0f, 0.0f, halfDiagonal), x, y));
        const double ridge = cavityWindowMax(on, off, x, y, 3);
        std::printf("[measured] %s: silhouette change %.1f, front edge ridge %.1f\n", c.name, outline, ridge);
        EXPECT_LE(outline, CAVITY_NEUTRAL) << "a line where the two boxes meet";
        EXPECT_GE(ridge, CAVITY_VISIBLE) << "the front box's own edge shows no line";
    }
    EXPECT_NO_ENGINE_ERRORS();
}

// R3 (the depth-step mask): a box half a metre in front of a wall shows no line at its outline,
// whether at 15 m (a 3% depth gap) or at 5 m (10%); its own convex front edge, at the same
// distance, still shows one (the red control)
TEST_F(CavityGpu, ScreenDrawsNoLineAtASmallDepthGap){
    JASPER_REQUIRE_GPU();
    camera.setPosition(0.0f, 1.0f, 0.0f);
    camera.setTarget(0.0f, 1.0f, -10.0f);
    Primitive wall(&scene);
    wall.createWall(40.0f, 20.0f);
    wall.setMaterial(cavityUnlit(0.3f, 0.3f, 0.3f));
    Primitive box(&scene);
    box.createBox(0.6f, 0.6f, 0.6f);
    box.setRotation(0.0f, 45.0f, 0.0f);
    box.setMaterial(cavityUnlit(0.6f, 0.6f, 0.6f));
    const float halfDiagonal = 0.3f * std::sqrt(2.0f);

    for (float distance : {15.0f, 5.0f}){
        SCOPED_TRACE(distance);
        wall.setPosition(0.0f, 1.0f, -distance);
        // its side corners (the outline) half a metre in front of the wall
        const Vector3 centre(0.4f, 1.0f, -distance + 0.5f);
        box.setPosition(centre.x, centre.y, centre.z);
        const CavityFrame off = frameOff();
        const CavityFrame on = frameWith(cavityOn(CavityType::SCREEN));
        expectDrawn("depth gap");
        const double outline = silhouetteChange(on, off, centre, halfDiagonal, 0.3f);
        int x = 0, y = 0;
        ASSERT_TRUE(project(centre + Vector3(0.0f, 0.0f, halfDiagonal), x, y));
        const double ridge = cavityWindowMax(on, off, x, y, 2);
        std::printf("[measured] %.0f m: outline change %.1f, front edge ridge %.1f\n", distance, outline, ridge);
        EXPECT_LE(outline, CAVITY_NEUTRAL) << "a line at the outline of a box half a metre in front of the wall";
        EXPECT_GE(ridge, CAVITY_VISIBLE) << "the box's own convex edge shows no line";
    }
    EXPECT_NO_ENGINE_ERRORS();
}

// AC6 (R3): Screen lines keep their apparent width (their width over the frame's height) from
// 720p to 2160p; they neither vanish at 4K nor smear at 720p. Line Width 2 keeps the 720p taps
// (1.33 px) off the 1-pixel minimum
TEST_F(CavityGpu, ScreenLinesKeepTheirApparentWidthFrom720pTo2160p){
    JASPER_REQUIRE_GPU();
    Primitive cube(&scene);
    makeFrontCube(cube, cavityUnlit(0.3f, 0.3f, 0.3f));
    PostProcessingSettings settings = cavityOn(CavityType::SCREEN);
    settings.cavityScreenWidth = 2.0f;

    double apparent[2] = {0.0, 0.0};
    double peaks[2] = {0.0, 0.0};
    int widths[2] = {0, 0};
    const int heights[2] = {720, 2160};
    for (int k = 0; k < 2; k++){
        SCOPED_TRACE(heights[k]);
        setView(heights[k] * 16 / 9, heights[k]);
        const CavityFrame off = frameOff();
        const CavityFrame on = frameWith(settings);
        expectDrawn("Screen");
        int cx = 0, y0 = 0, y1 = 0, unused = 0;
        ASSERT_TRUE(project(Vector3(0.0f, 0.65f, CUBE_EDGE_Z), cx, y0));
        ASSERT_TRUE(project(Vector3(0.0f, 0.35f, CUBE_EDGE_Z), unused, y1));
        const std::vector<double> profile = cavityColumnProfile(on, off, cx, y0, y1, heights[k] / 20);
        widths[k] = cavityPeakWidth(profile, 0.5, peaks[k]);
        apparent[k] = (double)widths[k] / heights[k];
    }
    std::printf("[measured] Screen line: 720p %d px (peak %.1f), 2160p %d px (peak %.1f)\n", widths[0], peaks[0], widths[1], peaks[1]);
    EXPECT_GE(peaks[0], CAVITY_VISIBLE) << "no line at 720p";
    EXPECT_GE(peaks[1], CAVITY_VISIBLE) << "the line vanishes at 2160p";
    EXPECT_LE(widths[0], 720 / 50) << "the line smears at 720p";
    ASSERT_GT(apparent[0], 0.0);
    const double ratio = apparent[1] / apparent[0];
    EXPECT_GE(ratio, 0.6) << "the line is thinner at 2160p";
    EXPECT_LE(ratio, 1.67) << "the line is thicker at 2160p";
    EXPECT_NO_ENGINE_ERRORS();
}

// AC7 (R4): World 4, 8 and 16 samples give the same bevel width (the kernel's mean radius does not
// depend on the count), and 16 is smoother than 4: less variation down the bevel (the edge runs
// straight down the frame) and fewer steps across it. No pixel cap, so the bevel is ~20 px wide
TEST_F(CavityGpu, WorldSampleCountsKeepTheBevelWidth){
    JASPER_REQUIRE_GPU();
    Primitive cube(&scene);
    makeFrontCube(cube, cavityUnlit(0.3f, 0.3f, 0.3f));
    const CavityFrame off = frameOff();

    const int counts[3] = {4, 8, 16};
    int widths[3] = {0, 0, 0};
    double peaks[3] = {0.0, 0.0, 0.0};
    double roughness[3] = {0.0, 0.0, 0.0};
    for (int k = 0; k < 3; k++){
        SCOPED_TRACE(counts[k]);
        PostProcessingSettings settings = cavityOn(CavityType::WORLD);
        settings.cavityWorldRadius = 0.15f;
        settings.cavityMaxPixelRadius = 0.0f;
        settings.cavityWorldSamples = counts[k];
        const CavityFrame on = frameWith(settings);
        expectDrawn("World");
        EXPECT_EQ(status().worldSamples, counts[k]);
        int cx = 0, y0 = 0, y1 = 0, unused = 0;
        ASSERT_TRUE(project(Vector3(0.0f, 0.7f, CUBE_EDGE_Z), cx, y0));
        ASSERT_TRUE(project(Vector3(0.0f, 0.3f, CUBE_EDGE_Z), unused, y1));
        const int half = 60;
        const std::vector<double> profile = cavityColumnProfile(on, off, cx, y0, y1, half);
        widths[k] = cavityPeakWidth(profile, 0.5, peaks[k]);
        roughness[k] = cavityColumnNoise(on, off, cx, y0, y1, half) + cavityProfileRoughness(profile);
    }
    std::printf("[measured] World bevel width 4/8/16: %d/%d/%d px, peak %.1f/%.1f/%.1f, roughness %.2f/%.2f/%.2f\n",
                widths[0], widths[1], widths[2], peaks[0], peaks[1], peaks[2], roughness[0], roughness[1], roughness[2]);
    for (int k = 0; k < 3; k++)
        EXPECT_GE(peaks[k], CAVITY_VISIBLE) << counts[k] << " samples drew no bevel";
    const double tolerance = std::max(3.0, 0.25 * widths[1]);
    EXPECT_LE(std::abs(widths[0] - widths[1]), tolerance) << "4 samples change the bevel's width";
    EXPECT_LE(std::abs(widths[2] - widths[1]), tolerance) << "16 samples change the bevel's width";
    EXPECT_LE(roughness[2], 0.9 * roughness[0] + 0.3) << "16 samples are not smoother than 4";
    EXPECT_LE(roughness[2], roughness[1] + 0.3) << "16 samples are rougher than 8";
    EXPECT_NO_ENGINE_ERRORS();
}

// AC8: Cavity multiplies whatever the material drew: the built-in Lit, Toon, Unlit and Fake Lit
// cubes all get a brighter top-front edge (a horizontal edge: the up-down taps)
TEST_F(CavityGpu, LitToonUnlitAndLibraryShadersShowCavity){
    JASPER_REQUIRE_GPU();
    camera.setPosition(0.0f, 1.8f, 6.5f);
    camera.setTarget(0.0f, 0.5f, 0.0f);
    Light sun(&scene);
    sun.setType(LightType::DIRECTIONAL);
    sun.setDirection(-0.3f, -1.0f, -0.5f);
    sun.setIntensity(1.0f);
    sun.setShadows(false);
    Material lit = cavityUnlit(0.3f, 0.3f, 0.3f);
    lit.lighting = MaterialLighting::AUTO;
    const struct { const char* name; Material material; float x; } cubes[] = {
        {"Lit", lit, -2.4f},
        {"Toon", cavityLibrary("library:toon", 0.3f), -0.8f},
        {"Unlit", cavityLibrary("library:unlit", 0.3f), 0.8f},
        {"Fake Lit", cavityLibrary("library:fake_lit", 0.3f), 2.4f},
    };
    std::vector<std::unique_ptr<Primitive>> primitives;
    for (const auto& c : cubes){
        auto cube = std::make_unique<Primitive>(&scene);
        cube->createBox(1.0f, 1.0f, 1.0f);
        cube->setPosition(c.x, 0.5f, 0.0f);
        cube->setMaterial(c.material);
        primitives.push_back(std::move(cube));
    }
    const CavityFrame off = frameOff();
    const CavityFrame on = frameWith(cavityOn(CavityType::BOTH));
    expectDrawn("materials");
    for (const auto& c : cubes){
        int x = 0, y = 0;
        ASSERT_TRUE(project(Vector3(c.x, 1.0f, 0.5f), x, y)) << c.name;
        EXPECT_GE(cavityWindowMax(on, off, x, y, 4), CAVITY_VISIBLE) << c.name << " shows no cavity on its edge";
    }
    EXPECT_NO_ENGINE_ERRORS();
}

// AC8 (D1): what draws after Cavity keeps its pixels: an alpha-blended quad (opaque in colour) and
// a UI image over the cube's edge, and a half-transparent box over the background. The cube's
// edge still brightens where nothing covers it
TEST_F(CavityGpu, TransparentObjectsAndUIOverAnEdgeKeepTheirPixels){
    JASPER_REQUIRE_GPU();
    Primitive cube(&scene);
    makeFrontCube(cube, cavityUnlit(0.3f, 0.3f, 0.3f));

    // magenta, in the transparent queue, over the edge's upper part
    Primitive glass(&scene);
    glass.createWall(0.4f, 0.3f);
    glass.setPosition(0.0f, 0.78f, 1.2f);
    Material magenta = cavityUnlit(1.0f, 0.0f, 1.0f);
    magenta.blendMode = MaterialBlendMode::ALPHA;
    glass.setMaterial(magenta);
    glass.setTransparent(true);

    // green, a UI image over its lower part (either pivot covers the edge)
    Image hud(&scene);
    hud.setTexture("cavity-gpu-hud", cavitySolidImage(0, 255, 0));
    hud.setSize(1, 1);
    hud.setScale(0.4f);
    hud.setPosition(-0.1f, 0.15f, 1.2f);

    // cyan, half transparent, over the background beside the cube
    Primitive ghost(&scene);
    ghost.createBox(0.5f, 0.5f, 0.5f);
    ghost.setPosition(1.4f, 0.5f, 0.0f);
    Material cyan = cavityUnlit(0.0f, 0.8f, 0.8f);
    cyan.baseColorFactor.w = 0.5f;
    cyan.blendMode = MaterialBlendMode::ALPHA;
    ghost.setMaterial(cyan);
    ghost.setTransparent(true);

    const CavityFrame off = frameOff();
    const CavityFrame on = frameWith(cavityOn(CavityType::BOTH));
    expectDrawn("covered");

    int edgeX = 0, unused = 0, ghostLeft = 0;
    ASSERT_TRUE(project(Vector3(0.0f, 0.5f, CUBE_EDGE_Z), edgeX, unused));
    ASSERT_TRUE(project(Vector3(1.1f, 0.5f, 0.0f), ghostLeft, unused));
    auto isMagenta = [](const uint8_t* p){ return p[0] > 180 && p[2] > 180 && p[1] < 60; };
    auto isGreen = [](const uint8_t* p){ return p[1] > 180 && p[0] < 60 && p[2] < 60; };
    struct Region { const char* name; size_t pixels = 0; size_t overEdge = 0; double worst = 0.0; };
    Region glassRegion{"the alpha-blended quad"}, hudRegion{"the UI image"}, ghostRegion{"the half-transparent box"};
    for (int y = 1; y < off.height - 1; y++){
        for (int x = 1; x < off.width - 1; x++){
            Region* region = nullptr;
            // two pixels in from its border, so no colour mixed at its edge counts
            auto inside = [&](const std::function<bool(const uint8_t*)>& is){
                for (int j = -2; j <= 2; j++)
                    for (int i = -2; i <= 2; i++)
                        if (!is(off.at(x + i, y + j)))
                            return false;
                return true;
            };
            if (inside(isMagenta))
                region = &glassRegion;
            else if (inside(isGreen))
                region = &hudRegion;
            else if (x > ghostLeft && std::abs(off.luma(x, y) - off.luma(off.width - 1, y)) > 6.0)
                region = &ghostRegion;
            if (!region)
                continue;
            region->pixels++;
            region->overEdge += std::abs(x - edgeX) <= 2 ? 1 : 0;
            for (int c = 0; c < 3; c++)
                region->worst = std::max(region->worst, std::abs((double)on.at(x, y)[c] - (double)off.at(x, y)[c]));
        }
    }
    for (const Region* region : {&glassRegion, &hudRegion, &ghostRegion}){
        EXPECT_GT(region->pixels, 100u) << region->name << " is not in the frame";
        EXPECT_LE(region->worst, 1.0) << region->name << " changed with Cavity";
    }
    EXPECT_GT(glassRegion.overEdge, 0u) << "the quad does not cover the edge";
    EXPECT_GT(hudRegion.overEdge, 0u) << "the UI image does not cover the edge";

    // the edge between the two covers
    int x = 0, y = 0;
    ASSERT_TRUE(project(Vector3(0.0f, 0.02f, CUBE_EDGE_Z), x, y));
    int top = 0;
    ASSERT_TRUE(project(Vector3(0.0f, 0.98f, CUBE_EDGE_Z), x, top));
    double ridge = -255.0;
    for (int row = top; row <= y; row++){
        bool covered = false;
        for (int i = -3; i <= 3; i++){
            const uint8_t* p = off.at(x + i, row);
            covered = covered || isMagenta(p) || isGreen(p);
        }
        if (!covered)
            ridge = std::max(ridge, cavityWindowMax(on, off, x, row, 2));
    }
    EXPECT_GE(ridge, CAVITY_VISIBLE) << "the uncovered edge is not brighter";
    EXPECT_NO_ENGINE_ERRORS();
}

// AC8 (R6): a stencil-tested submesh (an orange quad drawn only inside a stencil mask) is not in
// the depth pre-pass; in front of the cube's edge it must not show that edge's cavity
TEST_F(CavityGpu, StencilTestedSubmeshShowsNoForeignCavity){
    JASPER_REQUIRE_GPU();
    Primitive cube(&scene);
    makeFrontCube(cube, cavityUnlit(0.3f, 0.3f, 0.3f));

    // the mask (stencil 16, no colour or depth), drawn first
    Primitive mask(&scene);
    mask.createWall(0.9f, 0.9f);
    mask.setPosition(0.0f, 0.5f, 1.25f);
    Material maskMaterial;
    maskMaterial.shader = "library:stencil_mask";
    maskMaterial.sortPriority = -1;
    mask.setMaterial(maskMaterial);

    // the reader: "visible inside the mask", flat, facing the camera, over the edge's middle
    Primitive reader(&scene);
    reader.createWall(0.5f, 0.5f);
    reader.setPosition(0.0f, 0.5f, 1.3f);
    Material readerMaterial = cavityUnlit(0.9f, 0.5f, 0.1f);
    readerMaterial.stencil.compare = MaterialStencilCompare::EQUAL;
    readerMaterial.stencil.ref = 16;
    readerMaterial.stencil.readMask = 0xF0;
    readerMaterial.stencil.writeMask = 0;
    readerMaterial.sortPriority = 1;
    reader.setMaterial(readerMaterial);

    const CavityFrame off = frameOff();
    const CavityFrame on = frameWith(cavityOn(CavityType::BOTH));
    expectDrawn("stencil");
    ASSERT_TRUE(scene.getComponent<MeshComponent>(reader.getEntity()).submeshes[0].stencilTested)
        << "the reader is not stencil tested";

    int left = 0, top = 0, right = 0, bottom = 0;
    ASSERT_TRUE(project(Vector3(-0.25f, 0.75f, 1.3f), left, top));
    ASSERT_TRUE(project(Vector3(0.25f, 0.25f, 1.3f), right, bottom));
    size_t orange = 0;
    double worst = 0.0;
    // inside it by more than the World bevel's 8-pixel cap, so its own silhouette does not count
    const int inset = 12;
    for (int y = top + inset; y <= bottom - inset; y++){
        for (int x = left + inset; x <= right - inset; x++){
            const uint8_t* p = off.at(x, y);
            orange += (p[0] > p[2] + 80) ? 1 : 0;
            worst = std::max(worst, std::abs(cavityDelta(on, off, x, y)));
        }
    }
    EXPECT_GT(orange, (size_t)((right - left - 2 * inset) * (bottom - top - 2 * inset) * 9 / 10)) << "the reader is not drawn inside the mask";
    EXPECT_LE(worst, 3.0) << "the stencil-tested quad shows the cavity of the edge behind it";

    // the edge above and below it still brightens
    int x = 0, y = 0, high = 0;
    ASSERT_TRUE(project(Vector3(0.0f, 0.04f, CUBE_EDGE_Z), x, y));
    ASSERT_TRUE(project(Vector3(0.0f, 0.96f, CUBE_EDGE_Z), x, high));
    double ridge = -255.0;
    for (int row = high; row <= y; row++){
        if (row >= top - inset && row <= bottom + inset)
            continue;
        ridge = std::max(ridge, cavityWindowMax(on, off, x, row, 2));
    }
    EXPECT_GE(ridge, CAVITY_VISIBLE) << "the edge outside the quad is not brighter";
    EXPECT_NO_ENGINE_ERRORS();
}

// AC8 (R2): a mesh whose depth program is a project's fork of depth.frag from before Cavity (no
// normals output) gets neutral or depth-rebuilt cavity, never garbage: its faces stay, nothing of
// it goes dark (a convex cube has no valley) or black, and the fork compiles and draws the pre-pass
TEST_F(CavityGpu, ForkedDepthShaderGetsNoGarbage){
    JASPER_REQUIRE_GPU();
    Primitive cube(&scene);
    makeFrontCube(cube, cavityUnlit(0.45f, 0.45f, 0.45f));
    cube.setCustomDepthShader("cavity/forked_depth");

    const CavityFrame off = frameOff();
    const CavityFrame on = frameWith(cavityOn(CavityType::BOTH));
    expectDrawn("forked depth");
    EXPECT_NE(scene.getComponent<MeshComponent>(cube.getEntity()).submeshes[0].customDepthShaderId, 0)
        << "the pre-pass does not draw the cube with its fork";
    const CubeReading reading = readFrontCube(on, off);
    EXPECT_LE(reading.faces, 3.0) << "the faces of the forked mesh changed";

    const double background = off.luma(0, 0);
    size_t pixels = 0, darkened = 0, black = 0;
    for (int y = 0; y < off.height; y++){
        for (int x = 0; x < off.width; x++){
            if (std::abs(off.luma(x, y) - background) < 10.0)
                continue;
            pixels++;
            darkened += cavityDelta(on, off, x, y) < -16.0 ? 1 : 0;
            black += on.luma(x, y) < 8.0 ? 1 : 0;
        }
    }
    ASSERT_GT(pixels, 1000u) << "the cube is not in the frame";
    EXPECT_LE((double)darkened / (double)pixels, 0.01) << darkened << " of " << pixels << " pixels of a convex mesh went dark";
    EXPECT_EQ(black, 0u) << "black pixels on the forked mesh";
    EXPECT_NO_ENGINE_ERRORS();
}

// R2, AC8: with SSR on Cavity reads the G-buffer's normals with its depth; its factor (the debug
// view, R8) is the one the pre-pass normals give with SSR off, to the encoding's precision
TEST_F(CavityGpu, SsrGBufferNormalsMatchThePrepassNormals){
    JASPER_REQUIRE_GPU();
    Primitive cube(&scene);
    makeFrontCube(cube, cavityUnlit(0.3f, 0.3f, 0.3f));
    PostProcessingSettings debug = cavityOn(CavityType::BOTH);
    debug.cavityDebug = true;

    const CavityFrame prepass = frameWith(debug);
    expectDrawn("SSR off");
    EXPECT_STREQ(status().normals, "prepass");

    scene.setSSREnabled(true);
    const CavityFrame gbuffer = frameWith(debug);
    expectDrawn("SSR on");
    EXPECT_STREQ(status().normals, "gbuffer");
    EXPECT_FALSE(status().addedPrepass) << "SSR's G-buffer is the depth, no pre-pass is needed";
    const CavityDifference difference = cavityDifferenceOf(gbuffer, prepass);
    EXPECT_LE(difference.maxDiff, 8) << "the two normal sources give another factor (mean " << difference.meanDiff << ")";
    EXPECT_LE(difference.meanDiff, 0.5);

    int x = 0, y = 0, fx = 0, fy = 0;
    ASSERT_TRUE(project(Vector3(0.0f, 0.5f, CUBE_EDGE_Z), x, y));
    ASSERT_TRUE(project(Vector3(0.35355f, 0.5f, 0.35355f), fx, fy));
    double edge = 0.0;
    for (int j = -4; j <= 4; j++)
        for (int i = -4; i <= 4; i++)
            edge = std::max(edge, gbuffer.luma(x + i, y + j));
    EXPECT_GE(edge - gbuffer.luma(fx, fy), CAVITY_VISIBLE) << "no ridge from the G-buffer's normals";
    EXPECT_NO_ENGINE_ERRORS();
}

// R2, AC8: with SSR on, a visible mesh whose library depth program displaces it (Spin and Bob:
// the engine refuses the G-buffer's depth then) sends Cavity to the pre-pass for depth and normals
// both; hidden, Cavity reads the G-buffer again. Its motion is held still, so the frame is the
// one SSR off draws
TEST_F(CavityGpu, DisplacingLibraryShaderMovesCavityToThePrepassNormals){
    JASPER_REQUIRE_GPU();
    Primitive cube(&scene);
    makeFrontCube(cube, cavityUnlit(0.3f, 0.3f, 0.3f));
    Primitive pickup(&scene);
    pickup.createBox(0.5f, 0.5f, 0.5f);
    pickup.setPosition(1.3f, 0.25f, 0.0f);
    Material spin;
    spin.shader = "library:spin_bob";
    ShaderUniforms::set(spin.shaderParams, "baseColor", Vector4(0.3f, 0.3f, 0.3f, 1.0f));
    ShaderUniforms::set(spin.shaderParams, "spinSpeed", Vector4(0.0f, 0.0f, 0.0f, 0.0f));
    ShaderUniforms::set(spin.shaderParams, "bobHeight", Vector4(0.0f, 0.0f, 0.0f, 0.0f));
    ShaderUniforms::set(spin.shaderParams, "phaseVariation", Vector4(0.0f, 0.0f, 0.0f, 0.0f));
    pickup.setMaterial(spin);
    PostProcessingSettings debug = cavityOn(CavityType::BOTH);
    debug.cavityDebug = true;

    const CavityFrame without = frameWith(debug);
    EXPECT_STREQ(status().normals, "prepass");

    scene.setSSREnabled(true);
    const CavityFrame refused = frameWith(debug);
    expectDrawn("refused G-buffer depth");
    EXPECT_STREQ(status().normals, "prepass") << "Cavity read the G-buffer's normals with the pre-pass's depth";
    const CavityDifference difference = cavityDifferenceOf(refused, without);
    EXPECT_LE(difference.maxDiff, 8) << "mean " << difference.meanDiff;
    EXPECT_LE(difference.meanDiff, 0.5);

    pickup.setVisible(false);
    frameWith(debug);
    expectDrawn("pickup hidden");
    EXPECT_STREQ(status().normals, "gbuffer");
    EXPECT_NO_ENGINE_ERRORS();
}

// R4, AC1: an orthographic camera gets the same ridges from both types and keeps the faces
TEST_F(CavityGpu, WorksWithAnOrthographicCamera){
    JASPER_REQUIRE_GPU();
    const float aspect = (float)GpuEnvironment::target().getWidth() / (float)GpuEnvironment::target().getHeight();
    camera.setOrtho(-1.2f * aspect, 1.2f * aspect, -1.2f, 1.2f, 0.1f, 50.0f);
    frontView();
    Primitive cube(&scene);
    makeFrontCube(cube, cavityUnlit(0.3f, 0.3f, 0.3f));
    const CavityFrame off = frameOff();
    for (CavityType type : {CavityType::SCREEN, CavityType::WORLD}){
        SCOPED_TRACE(type == CavityType::SCREEN ? "Screen" : "World");
        const CavityFrame on = frameWith(cavityOn(type));
        expectDrawn("orthographic");
        const CubeReading reading = readFrontCube(on, off);
        EXPECT_GE(reading.ridge, CAVITY_VISIBLE);
        EXPECT_LE(reading.faces, CAVITY_NEUTRAL);
    }
    EXPECT_NO_ENGINE_ERRORS();
}

// AC1 (spec section 3: the colour pass is the only multisampled one): MSAA 4x multiplies as one
// sample does: the cube's ridge, the step's crease and a tilted cube (sloped faces: depth varies
// across a pixel's samples) brighten and darken as much as without MSAA, and the faces stay
TEST_F(CavityGpu, WorksWithMsaa4x){
    JASPER_REQUIRE_GPU();
    Primitive cube(&scene);
    makeFrontCube(cube, cavityUnlit(0.3f, 0.3f, 0.3f));
    Mesh step(&scene);
    cavityFillPolygons(scene, step.getEntity(), cavityStep());
    step.setMaterial(cavityUnlit(0.3f, 0.3f, 0.3f));
    Primitive tilted(&scene);
    tilted.createBox(1.0f, 1.0f, 1.0f);
    tilted.setPosition(0.0f, 0.6f, 0.0f);
    tilted.setRotation(30.0f, 45.0f, 0.0f);
    tilted.setMaterial(cavityUnlit(0.3f, 0.3f, 0.3f));

    struct Reading {
        double ridge = 0.0, faces = 0.0, crease = 0.0;
        CavityEnergy cube, tilted;
    };
    auto measure = [&](int samples){
        Reading reading;
        PostProcessingSettings off;
        off.msaaSamples = samples;
        PostProcessingSettings on = cavityOn(CavityType::BOTH);
        on.msaaSamples = samples;
        cube.setVisible(true);
        step.setVisible(false);
        tilted.setVisible(false);
        CavityFrame without = frameWith(off);
        CavityFrame with = frameWith(on);
        expectDrawn("cube");
        const CubeReading cubeReading = readFrontCube(with, without);
        reading.ridge = cubeReading.ridge;
        reading.faces = cubeReading.faces;
        reading.cube = cavityEnergyOf(with, without);
        cube.setVisible(false);
        step.setVisible(true);
        without = frameWith(off);
        with = frameWith(on);
        expectDrawn("step");
        int x = 0, y = 0;
        EXPECT_TRUE(project(Vector3(0.0f, 0.0f, 0.0f), x, y));
        reading.crease = cavityWindowMin(with, without, x, y, 4);
        step.setVisible(false);
        tilted.setVisible(true);
        without = frameWith(off);
        with = frameWith(on);
        expectDrawn("tilted");
        reading.tilted = cavityEnergyOf(with, without);
        return reading;
    };
    const Reading one = measure(1);
    const Reading four = measure(4);
    std::printf("[measured] ridge %.1f / %.1f, crease %.1f / %.1f, cube light %.0f / %.0f dark %.0f / %.0f, "
                "tilted light %.0f / %.0f dark %.0f / %.0f (1 sample / MSAA 4x)\n",
                one.ridge, four.ridge, one.crease, four.crease, one.cube.light, four.cube.light, one.cube.dark, four.cube.dark,
                one.tilted.light, four.tilted.light, one.tilted.dark, four.tilted.dark);
    EXPECT_GE(one.ridge, CAVITY_VISIBLE);
    EXPECT_LE(four.faces, CAVITY_NEUTRAL);
    // a level of 255 or a tenth, whichever is more: what a resolve's rounding and the
    // silhouettes' coverage may move
    auto similar = [](double a, double b){ return std::abs(a - b) <= std::max(3.0, 0.1 * std::abs(b)); };
    EXPECT_TRUE(similar(four.ridge, one.ridge)) << "the ridge with MSAA 4x: " << four.ridge << ", without: " << one.ridge;
    EXPECT_TRUE(similar(four.crease, one.crease)) << "the crease with MSAA 4x: " << four.crease << ", without: " << one.crease;
    // the whole frame's brightening and darkening (levels times pixels)
    auto similarEnergy = [](double a, double b){ return std::abs(a - b) <= 0.12 * std::abs(b) + 50.0; };
    EXPECT_TRUE(similarEnergy(four.cube.light, one.cube.light)) << "cube brightening " << four.cube.light << " vs " << one.cube.light;
    EXPECT_TRUE(similarEnergy(four.tilted.light, one.tilted.light)) << "tilted cube brightening " << four.tilted.light << " vs " << one.tilted.light;
    EXPECT_TRUE(similarEnergy(four.tilted.dark, one.tilted.dark)) << "tilted cube darkening " << four.tilted.dark << " vs " << one.tilted.dark;
    EXPECT_NO_ENGINE_ERRORS();
}

// R8: the debug view shows the factor alone as grey, 0.5 (128) neutral: a red cube's face centre
// is neutral grey, its edge lighter; the step's crease darker
TEST_F(CavityGpu, DebugViewShowsTheFactorAsGrey){
    JASPER_REQUIRE_GPU();
    Primitive cube(&scene);
    makeFrontCube(cube, cavityUnlit(0.8f, 0.1f, 0.1f));
    PostProcessingSettings debug = cavityOn(CavityType::BOTH);
    debug.cavityDebug = true;
    const CavityFrame frame = frameWith(debug);
    expectDrawn("debug");

    int x = 0, y = 0;
    ASSERT_TRUE(project(Vector3(0.35355f, 0.5f, 0.35355f), x, y));
    const uint8_t* face = frame.at(x, y);
    EXPECT_LE(std::abs((int)face[0] - (int)face[1]), 3) << "the face shows its colour, not the factor";
    EXPECT_LE(std::abs((int)face[1] - (int)face[2]), 3);
    EXPECT_NEAR(frame.luma(x, y), 128.0, 4.0) << "a flat face's factor is not neutral grey";
    ASSERT_TRUE(project(Vector3(0.0f, 0.5f, CUBE_EDGE_Z), x, y));
    double edge = 0.0;
    for (int j = -4; j <= 4; j++)
        for (int i = -4; i <= 4; i++)
            edge = std::max(edge, frame.luma(x + i, y + j));
    EXPECT_GE(edge, 128.0 + CAVITY_VISIBLE) << "the edge's factor is not above neutral";

    cube.setVisible(false);
    Mesh step(&scene);
    cavityFillPolygons(scene, step.getEntity(), cavityStep());
    step.setMaterial(cavityUnlit(0.8f, 0.1f, 0.1f));
    const CavityFrame crease = frameWith(debug);
    ASSERT_TRUE(project(Vector3(0.0f, 0.0f, 0.0f), x, y));
    double darkest = 255.0;
    for (int j = -4; j <= 4; j++)
        for (int i = -4; i <= 4; i++)
            darkest = std::min(darkest, crease.luma(x + i, y + j));
    EXPECT_LE(darkest, 128.0 - CAVITY_VISIBLE) << "the crease's factor is not below neutral";
    EXPECT_NO_ENGINE_ERRORS();
}

// Agent UX (get_post_processing_status): the status says what the last frame drew, where the
// normals came from and whether Cavity alone asked for the sampled depth pre-pass
TEST_F(CavityGpu, StatusReportsWhatWasDrawn){
    JASPER_REQUIRE_GPU();
    Primitive cube(&scene);
    makeFrontCube(cube, cavityUnlit(0.3f, 0.3f, 0.3f));

    frameOff();
    CavityStatus drawn = status();
    EXPECT_FALSE(drawn.active);
    EXPECT_EQ(drawn.draws, 0u);
    EXPECT_EQ(drawn.worldSamples, 0);
    EXPECT_STREQ(drawn.normals, "none");
    EXPECT_FALSE(drawn.addedPrepass);

    frameWith(cavityOn(CavityType::SCREEN));
    drawn = status();
    EXPECT_TRUE(drawn.active);
    EXPECT_EQ(drawn.draws, 1u);
    EXPECT_EQ(drawn.type, CavityType::SCREEN);
    EXPECT_EQ(drawn.worldSamples, 0);
    EXPECT_STREQ(drawn.normals, "prepass");
    EXPECT_TRUE(drawn.addedPrepass) << "nothing else asked for the pre-pass";

    PostProcessingSettings world = cavityOn(CavityType::WORLD);
    world.cavityWorldSamples = 16;
    frameWith(world);
    drawn = status();
    EXPECT_EQ(drawn.type, CavityType::WORLD);
    EXPECT_EQ(drawn.worldSamples, 16);

    PostProcessingSettings both = cavityOn(CavityType::BOTH);
    both.cavityWorldSamples = 4;
    frameWith(both);
    drawn = status();
    EXPECT_EQ(drawn.type, CavityType::BOTH);
    EXPECT_EQ(drawn.worldSamples, 4);

    // SSAO's pre-pass is there anyway: Cavity adds its normals, not the pass
    scene.setSSAOEnabled(true);
    frameWith(both);
    drawn = status();
    EXPECT_STREQ(drawn.normals, "prepass");
    EXPECT_FALSE(drawn.addedPrepass) << "SSAO already asked for the pre-pass";
    scene.setSSAOEnabled(false);

    // SSR's G-buffer: no pre-pass at all
    scene.setSSREnabled(true);
    frameWith(both);
    drawn = status();
    EXPECT_STREQ(drawn.normals, "gbuffer");
    EXPECT_FALSE(drawn.addedPrepass);
    EXPECT_EQ(prepassColors(), 0) << "a depth pre-pass while Cavity reads SSR's G-buffer";
    EXPECT_NO_ENGINE_ERRORS();
}

// AC4: off costs nothing. Fields set but Cavity disabled draw the frame of default settings (its
// passes and their attachments, draws, variants, allocations, pixels); on, it adds the pre-pass
// with a normals attachment and its draw; off again, the frame is the default one once more
TEST_F(CavityGpu, DisabledCostsNothing){
    JASPER_REQUIRE_GPU();
    showcaseView();
    CavityShowcase showcase(scene);

    frameOff();
    const CavityFrameCost baseline = recordCost();
    std::printf("[S100] the frame with default settings:\n%s", cavityDescribe(baseline).c_str());
    ASSERT_FALSE(baseline.passes.empty()) << "no pass was recorded for the frame";
    ASSERT_GT(baseline.draws, 0u);

    PostProcessingSettings disabled;
    disabled.cavityEnabled = false;
    disabled.cavityType = CavityType::WORLD;
    disabled.cavityWorldSamples = 16;
    disabled.cavityWorldRadius = 2.0f;
    disabled.cavityMaxPixelRadius = 0.0f;
    disabled.cavityScreenWidth = 4.0f;
    disabled.cavityDebug = true;
    frameWith(disabled);
    const CavityFrameCost set = recordCost();
    EXPECT_EQ(set.passes, baseline.passes) << cavityDescribe(set);
    EXPECT_EQ(set.draws, baseline.draws);
    EXPECT_EQ(set.shaders, baseline.shaders) << cavityDescribe(set);
    EXPECT_EQ(set.image, baseline.image) << "other pixels with Cavity's fields set but disabled";
    if (testsupport::countsAllocations()){
        EXPECT_LE(set.allocations, baseline.allocations);
    }
    EXPECT_FALSE(status().active);
    EXPECT_EQ(status().draws, 0u);

    frameWith(cavityOn(CavityType::BOTH));
    const CavityFrameCost on = recordCost();
    expectDrawn("on");
    EXPECT_TRUE(status().addedPrepass);
    EXPECT_EQ(prepassColors(), 2) << "the pre-pass has no normals attachment\n" << cavityDescribe(on);
    EXPECT_GT(on.draws, baseline.draws);
    EXPECT_NE(on.image, baseline.image);

    frameOff();
    const CavityFrameCost again = recordCost();
    EXPECT_EQ(again.passes, baseline.passes) << cavityDescribe(again);
    EXPECT_EQ(again.draws, baseline.draws);
    EXPECT_EQ(again.image, baseline.image) << "Cavity left something behind when turned off";
    EXPECT_FALSE(status().active);
    EXPECT_EQ(status().draws, 0u);
    EXPECT_FALSE(status().addedPrepass);
    EXPECT_STREQ(status().normals, "none");
    EXPECT_NO_ENGINE_ERRORS();
}

// AC4, R2: with SSAO asking for the pre-pass, Cavity off leaves it as it was (one colour
// attachment, the depth); on, the same pass gains the normals attachment
TEST_F(CavityGpu, PrepassGainsTheNormalsAttachmentOnlyWithCavity){
    JASPER_REQUIRE_GPU();
    Primitive cube(&scene);
    makeFrontCube(cube, cavityUnlit(0.3f, 0.3f, 0.3f));
    scene.setSSAOEnabled(true);
    frameOff();
    EXPECT_EQ(prepassColors(), 1);
    frameWith(cavityOn(CavityType::SCREEN));
    expectDrawn("SSAO");
    EXPECT_EQ(prepassColors(), 2);
    frameOff();
    EXPECT_EQ(prepassColors(), 1) << "the normals attachment stayed after Cavity was turned off";
    EXPECT_NO_ENGINE_ERRORS();
}

// R1, AC1: a scene rendered below the output's size (Render Scale 0.85) or at a fixed resolution,
// with or without SSAO's pre-pass, gets Cavity along the cube's whole edge (every row brightened:
// no band or dropout) and the faces stay. With SSAO's pre-pass the frame's Cavity is the one
// without it pixel for pixel (a pre-pass of another size than the colour pass leaves pixels out),
// on the front cube and on a tilted one (sloped faces)
TEST_F(CavityGpu, ScaledAndFixedResolutionsApplyAlongTheWholeEdge){
    JASPER_REQUIRE_GPU();
    Primitive cube(&scene);
    makeFrontCube(cube, cavityUnlit(0.3f, 0.3f, 0.3f));
    Primitive tilted(&scene);
    tilted.createBox(1.0f, 1.0f, 1.0f);
    tilted.setPosition(0.0f, 0.6f, 0.0f);
    tilted.setRotation(30.0f, 45.0f, 0.0f);
    tilted.setMaterial(cavityUnlit(0.3f, 0.3f, 0.3f));
    tilted.setVisible(false);
    const struct { const char* name; float scale; bool ssao; bool fixed; } cases[] = {
        {"Render Scale 0.85", 0.85f, false, false},
        {"Render Scale 0.85, SSAO", 0.85f, true, false},
        {"fixed 480 x 270", 1.0f, false, true},
        {"fixed 480 x 270, SSAO", 1.0f, true, true},
    };
    // on - off of each case, per pixel, for the front cube and the tilted one
    std::vector<double> deltas[2][4];
    for (int shape = 0; shape < 2; shape++){
        cube.setVisible(shape == 0);
        tilted.setVisible(shape == 1);
        for (int k = 0; k < 4; k++){
            const auto& c = cases[k];
            SCOPED_TRACE(std::string(c.name) + (shape == 0 ? ", front cube" : ", tilted cube"));
            scene.setSSAOEnabled(c.ssao);
            scene.setFixedResolutionEnabled(c.fixed);
            if (c.fixed)
                scene.setFixedResolutionSize(480, 270);
            PostProcessingSettings off;
            off.renderScale = c.scale;
            PostProcessingSettings on = cavityOn(CavityType::BOTH);
            on.renderScale = c.scale;
            const CavityFrame without = frameWith(off);
            const CavityFrame with = frameWith(on);
            expectDrawn(c.name);
            for (int y = 0; y < with.height; y++)
                for (int x = 0; x < with.width; x++)
                    deltas[shape][k].push_back(cavityDelta(with, without, x, y));
            if (shape != 0)
                continue;
            int x = 0, top = 0, bottom = 0, unused = 0;
            ASSERT_TRUE(project(Vector3(0.0f, 0.9f, CUBE_EDGE_Z), x, top));
            ASSERT_TRUE(project(Vector3(0.0f, 0.1f, CUBE_EDGE_Z), unused, bottom));
            int dropouts = 0, rows = 0;
            double weakest = 255.0;
            for (int y = top; y <= bottom; y++){
                const double ridge = cavityWindowMax(with, without, x, y, 3);
                weakest = std::min(weakest, ridge);
                dropouts += ridge < CAVITY_VISIBLE ? 1 : 0;
                rows++;
            }
            const CubeReading reading = readFrontCube(with, without);
            std::printf("[measured] %s: %d of %d edge rows without cavity, weakest %.1f, faces %.1f\n",
                        c.name, dropouts, rows, weakest, reading.faces);
            EXPECT_EQ(dropouts, 0) << "rows of the edge Cavity left out";
            EXPECT_LE(reading.faces, CAVITY_NEUTRAL);
        }
    }
    // SSAO's pre-pass gives the Cavity the scene's own pre-pass gives, pixel for pixel
    for (int shape = 0; shape < 2; shape++){
        for (int k : {1, 3}){
            SCOPED_TRACE(std::string(cases[k].name) + (shape == 0 ? ", front cube" : ", tilted cube"));
            const std::vector<double>& with = deltas[shape][k];
            const std::vector<double>& without = deltas[shape][k - 1];
            ASSERT_EQ(with.size(), without.size());
            size_t moved = 0, touched = 0;
            double worst = 0.0;
            for (size_t i = 0; i < with.size(); i++){
                const double d = std::abs(with[i] - without[i]);
                worst = std::max(worst, d);
                moved += d > 4.0 ? 1 : 0;
                touched += std::abs(without[i]) > 4.0 ? 1 : 0;
            }
            std::printf("[measured] %s (%s) against without SSAO: %zu of the %zu pixels Cavity moves differ by more than 4, worst %.1f\n",
                        cases[k].name, shape == 0 ? "front cube" : "tilted cube", moved, touched, worst);
            ASSERT_GT(touched, 100u) << "Cavity moved almost nothing without SSAO";
            EXPECT_LE((double)moved, 0.02 * (double)touched) << "SSAO's pre-pass gives another Cavity";
        }
    }
    EXPECT_NO_ENGINE_ERRORS();
}

// R2: with SSR on, opaque library shaders whose depth program draws what the built-in one does
// (Unlit, Toon, Fake Lit: no displacement, no clip) keep Cavity on the G-buffer's normals, with no
// pre-pass of its own, and show it
TEST_F(CavityGpu, SsrKeepsTheGBufferForLibraryShadersThatDoNotDisplace){
    JASPER_REQUIRE_GPU();
    Light sun(&scene);
    sun.setType(LightType::DIRECTIONAL);
    sun.setDirection(-0.3f, -1.0f, -0.5f);
    sun.setShadows(false);
    scene.setSSREnabled(true);
    camera.setPosition(0.0f, 1.8f, 4.0f);
    camera.setTarget(0.0f, 0.5f, 0.0f);
    const char* shaders[] = {"library:unlit", "library:toon", "library:fake_lit"};
    std::vector<std::unique_ptr<Primitive>> cubes;
    for (const char* shader : shaders){
        auto cube = std::make_unique<Primitive>(&scene);
        cube->createBox(1.0f, 1.0f, 1.0f);
        cube->setPosition(0.0f, 0.5f, 0.0f);
        cube->setMaterial(cavityLibrary(shader, 0.3f));
        cube->setVisible(false);
        cubes.push_back(std::move(cube));
    }
    for (size_t k = 0; k < cubes.size(); k++){
        SCOPED_TRACE(shaders[k]);
        for (size_t j = 0; j < cubes.size(); j++)
            cubes[j]->setVisible(j == k);
        const CavityFrame off = frameOff();
        const CavityFrame on = frameWith(cavityOn(CavityType::BOTH));
        expectDrawn(shaders[k]);
        const CavityStatus drawn = status();
        std::printf("[measured] %s with SSR: normals %s, added pre-pass %d\n", shaders[k], drawn.normals, drawn.addedPrepass ? 1 : 0);
        EXPECT_STREQ(drawn.normals, "gbuffer") << shaders[k] << " sent Cavity off SSR's G-buffer";
        EXPECT_FALSE(drawn.addedPrepass);
        int x = 0, y = 0;
        ASSERT_TRUE(project(Vector3(0.0f, 1.0f, 0.5f), x, y));
        EXPECT_GE(cavityWindowMax(on, off, x, y, 4), CAVITY_VISIBLE) << shaders[k] << " shows no cavity on its edge";
    }
    EXPECT_NO_ENGINE_ERRORS();
}

// R2: a project's depth fork whose file starts with a comment (GLSL allows one before #version)
// is still the program of every depth pass with Cavity on: it cuts its cube out of them all, so
// the cube casts no shadow and gets no cavity (the pre-pass sees the floor behind it), and it
// compiles without an error. Its twin with the built-in depth program, in the same frame, is the
// control: its shadow is there. (The fork is set before the first frame: a depth program swapped
// on a still scene keeps the cached shadow of the old one wherever the reload completes within one
// update, as on Vulkan, an engine bug older than Cavity)
TEST_F(CavityGpu, ForkWithACommentBeforeVersionKeepsItsDepthProgram){
    JASPER_REQUIRE_GPU();
    Primitive ground(&scene);
    ground.createPlane(8.0f, 8.0f);
    ground.setMaterial(CavityShowcase::litMaterial(0.6f, 0.6f, 0.6f));
    Primitive forked(&scene), builtin(&scene);
    const float forkedX = -0.7f, builtinX = 0.7f;
    for (Primitive* cube : {&forked, &builtin}){
        cube->createBox(0.6f, 0.6f, 0.6f);
        cube->setRotation(0.0f, 45.0f, 0.0f);
        cube->setMaterial(cavityUnlit(0.3f, 0.3f, 0.3f));
    }
    forked.setPosition(forkedX, 1.0f, 0.0f);
    builtin.setPosition(builtinX, 1.0f, 0.0f);
    forked.setCustomDepthShader("cavity/forked_depth.vert|cavity/commented_depth.frag");
    Light sun(&scene);
    sun.setType(LightType::DIRECTIONAL);
    sun.setDirection(0.3f, -1.0f, 0.6f);
    sun.setIntensity(1.5f);
    sun.setShadows(true);
    // each cube's shadow (its centre along the light) and a lit floor point clear of both
    const Vector3 forkedShadow(forkedX + 0.3f, 0.0f, 0.6f);
    const Vector3 builtinShadow(builtinX + 0.3f, 0.0f, 0.6f);
    const Vector3 lit(0.3f, 0.0f, 1.4f);
    int fx = 0, fy = 0, bx = 0, by = 0, lx = 0, ly = 0;

    const CavityFrame off = frameOff();
    ASSERT_TRUE(project(forkedShadow, fx, fy));
    ASSERT_TRUE(project(builtinShadow, bx, by));
    ASSERT_TRUE(project(lit, lx, ly));
    ASSERT_GT(off.luma(lx, ly) - off.luma(bx, by), 10.0) << "no shadow of the built-in cube to compare with";
    EXPECT_LT(off.luma(lx, ly) - off.luma(fx, fy), 4.0) << "the fork does not cut the cube out of the shadow pass";

    const CavityFrame on = frameWith(cavityOn(CavityType::BOTH));
    expectDrawn("commented fork");
    EXPECT_NE(scene.getComponent<MeshComponent>(forked.getEntity()).submeshes[0].customDepthShaderId, 0)
        << "Cavity's depth variant fell back to the built-in program";
    std::printf("[measured] shadow points: built-in cube %.1f / %.1f, forked cube %.1f / %.1f (Cavity off / on; lit floor %.1f)\n",
                off.luma(bx, by), on.luma(bx, by), off.luma(fx, fy), on.luma(fx, fy), on.luma(lx, ly));
    EXPECT_GT(on.luma(lx, ly) - on.luma(bx, by), 10.0) << "the built-in cube's shadow went with Cavity on";
    EXPECT_LT(on.luma(lx, ly) - on.luma(fx, fy), 4.0) << "with Cavity the cube casts the shadow its fork cuts out";
    int x = 0, y = 0;
    ASSERT_TRUE(project(Vector3(forkedX, 1.0f, 0.3f * std::sqrt(2.0f)), x, y));
    const double edge = std::max(cavityWindowMax(on, off, x, y, 4), -cavityWindowMin(on, off, x, y, 4));
    EXPECT_LE(edge, CAVITY_NEUTRAL) << "the cube its depth fork cuts out of the pre-pass gets cavity";
    // (the built-in twin does get it: Cavity drew where a pre-pass has the cube)
    ASSERT_TRUE(project(Vector3(builtinX, 1.0f, 0.3f * std::sqrt(2.0f)), x, y));
    EXPECT_GE(cavityWindowMax(on, off, x, y, 4), CAVITY_VISIBLE) << "the built-in cube's edge shows no cavity";
    EXPECT_NO_ENGINE_ERRORS();
}

// R2, R4: World mode on features about a pixel wide whose depth program writes no normal (the
// pre-Cavity fork: the normal is rebuilt from depth, with only the background around them):
// bounded, never black or darkened past a quarter
TEST_F(CavityGpu, WorldModeOnPixelWideFeaturesWithoutNormalsStaysBounded){
    JASPER_REQUIRE_GPU();
    Primitive pole(&scene), wire(&scene);
    pole.createBox(0.012f, 1.6f, 0.012f);
    pole.setPosition(-0.6f, 0.8f, 0.6f);
    wire.createBox(1.4f, 0.012f, 0.012f);
    wire.setPosition(0.3f, 1.2f, 0.6f);
    for (Primitive* thin : {&pole, &wire}){
        thin->setMaterial(cavityUnlit(0.5f, 0.5f, 0.5f));
        thin->setCustomDepthShader("cavity/forked_depth");
    }
    const CavityFrame off = frameOff();
    const CavityFrame on = frameWith(cavityOn(CavityType::WORLD));
    expectDrawn("thin features");
    const double background = off.luma(0, 0);
    size_t pixels = 0, darkened = 0, black = 0;
    double lowest = 10.0;
    for (int y = 0; y < off.height; y++){
        for (int x = 0; x < off.width; x++){
            if (off.luma(x, y) - background < 10.0)
                continue;
            pixels++;
            const double ratio = on.luma(x, y) / std::max(1.0, off.luma(x, y));
            lowest = std::min(lowest, ratio);
            darkened += ratio < 0.75 ? 1 : 0;
            black += on.luma(x, y) < 8.0 ? 1 : 0;
        }
    }
    std::printf("[measured] thin features: %zu pixels, %zu darkened past a quarter, %zu black, lowest ratio %.2f\n",
                pixels, darkened, black, lowest);
    ASSERT_GT(pixels, 200u) << "the pole and the wire are not in the frame";
    EXPECT_EQ(darkened, 0u) << "a feature without normals went dark";
    EXPECT_EQ(black, 0u);
    EXPECT_NO_ENGINE_ERRORS();
}

// R2 (the shadows never change): turning Cavity on leaves the shadow passes as they were: as many
// draws, triangles and pipelines, and the same shadow on the floor (its pixels, where Cavity is
// neutral: flat floor away from the cube). The cube moves a hair each frame, so the passes redraw.
// (Which program a shadow draw ran is not readable here: depth programs carry no label)
TEST_F(CavityGpu, ShadowPassesDrawTheSameWithCavity){
    JASPER_REQUIRE_GPU();
    Primitive ground(&scene);
    ground.createPlane(8.0f, 8.0f);
    ground.setMaterial(CavityShowcase::litMaterial(0.6f, 0.6f, 0.6f));
    Primitive cube(&scene);
    cube.createBox(0.6f, 0.6f, 0.6f);
    cube.setPosition(0.0f, 1.0f, 0.0f);
    cube.setMaterial(CavityShowcase::litMaterial(0.5f, 0.4f, 0.3f));
    Light sun(&scene);
    sun.setType(LightType::DIRECTIONAL);
    sun.setDirection(0.3f, -1.0f, 0.6f);
    sun.setIntensity(1.5f);
    sun.setShadows(true);

    auto shadowPasses = [&](){
        std::vector<std::string> record;
        cube.setPosition(0.001f, 1.0f, 0.0f);
        GpuEnvironment::drawFrames(1);
        cube.setPosition(0.0f, 1.0f, 0.0f);
        GpuEnvironment::drawFrames(1);
        const PassList list = SystemRender::getPassList(SystemRender::FRAME_STATS_FRAME, false);
        for (const PassRecord& pass : list.passes){
            const std::string label = pass.label;
            if (label.find("shadow") == std::string::npos || label == "shadow atlas view")
                continue;
            record.push_back(label + ": " + std::to_string(pass.draws) + " draws, " + std::to_string(pass.triangles) +
                             " triangles, " + std::to_string(pass.pipelines) + " pipelines, " + std::to_string(pass.bindings) + " bindings");
        }
        return record;
    };
    // the shadow's middle (the cube's centre along the light) and a band across it
    const Vector3 shadow(0.3f, 0.0f, 0.6f);
    const CavityFrame offFrame = frameOff();
    const std::vector<std::string> off = shadowPasses();
    const CavityFrame onFrame = frameWith(cavityOn(CavityType::BOTH));
    expectDrawn("shadows");
    const std::vector<std::string> on = shadowPasses();
    for (const std::string& line : off)
        std::printf("[measured] Cavity off: %s\n", line.c_str());
    for (const std::string& line : on)
        std::printf("[measured] Cavity on: %s\n", line.c_str());
    ASSERT_FALSE(off.empty()) << "no shadow pass drew";
    EXPECT_EQ(on, off) << "Cavity changed the shadow passes";
    int sx = 0, sy = 0;
    ASSERT_TRUE(project(shadow, sx, sy));
    double worst = 0.0;
    for (int j = -6; j <= 6; j++)
        for (int i = -6; i <= 6; i++)
            worst = std::max(worst, std::abs(cavityDelta(onFrame, offFrame, sx + i, sy + j)));
    std::printf("[measured] shadow pixels: off %.1f, on %.1f, worst change %.1f\n", offFrame.luma(sx, sy), onFrame.luma(sx, sy), worst);
    EXPECT_LE(worst, CAVITY_NEUTRAL) << "the cube's shadow changed with Cavity";
    EXPECT_NO_ENGINE_ERRORS();
}

// AC1: goldens of the faceted rock, the smooth sphere and the bevelled cube, lit, on a floor;
// each fails with the effect off
TEST_F(CavityGpu, MatchesTheScreenGolden){
    JASPER_REQUIRE_GPU();
    showcaseView();
    CavityShowcase showcase(scene);
    expectShowcaseGolden(cavityOn(CavityType::SCREEN), "cavity_screen");
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(CavityGpu, MatchesTheWorldGolden){
    JASPER_REQUIRE_GPU();
    showcaseView();
    CavityShowcase showcase(scene);
    expectShowcaseGolden(cavityOn(CavityType::WORLD), "cavity_world");
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(CavityGpu, MatchesTheBothGolden){
    JASPER_REQUIRE_GPU();
    showcaseView();
    CavityShowcase showcase(scene);
    expectShowcaseGolden(cavityOn(CavityType::BOTH), "cavity_both");
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(CavityGpu, MatchesTheOrthographicGolden){
    JASPER_REQUIRE_GPU();
    const float aspect = (float)GpuEnvironment::target().getWidth() / (float)GpuEnvironment::target().getHeight();
    camera.setOrtho(-1.8f * aspect, 1.8f * aspect, -1.8f, 1.8f, 0.1f, 50.0f);
    showcaseView();
    CavityShowcase showcase(scene);
    expectShowcaseGolden(cavityOn(CavityType::BOTH), "cavity_both_ortho");
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(CavityGpu, MatchesTheMsaa4Golden){
    JASPER_REQUIRE_GPU();
    showcaseView();
    CavityShowcase showcase(scene);
    PostProcessingSettings settings = cavityOn(CavityType::BOTH);
    settings.msaaSamples = 4;
    expectShowcaseGolden(settings, "cavity_both_msaa4");
    EXPECT_NO_ENGINE_ERRORS();
}

TEST_F(CavityGpu, MatchesTheSsrNormalsGolden){
    JASPER_REQUIRE_GPU();
    showcaseView();
    CavityShowcase showcase(scene);
    scene.setSSREnabled(true);
    expectShowcaseGolden(cavityOn(CavityType::BOTH), "cavity_both_ssr");
    EXPECT_STREQ(status().normals, "gbuffer");
    EXPECT_NO_ENGINE_ERRORS();
}

// AC8 (R2): a terrain, whose normal its own programs compute, gets neutral or plausible cavity,
// never garbage: gentle hills far from the camera barely change, and nothing goes black
class CavityTerrainGpu : public TerrainGpuFixture {};

TEST_F(CavityTerrainGpu, TerrainGetsNoGarbage){
    JASPER_REQUIRE_GPU();
    makeTerrain(12.0f, [](float x, float z){
        return 0.3f + 0.15f * std::sin(x * 0.08f) * std::cos(z * 0.07f);
    });
    scene.setPostProcessing(PostProcessingSettings());
    ASSERT_TRUE(render());
    std::vector<uint8_t> off;
    ASSERT_TRUE(readFrame(off));
    PostProcessingSettings settings;
    settings.cavityEnabled = true;
    scene.setPostProcessing(settings);
    ASSERT_TRUE(render());
    const CavityStatus drawn = scene.getSystem<RenderSystem>()->getCavityStatus();
    EXPECT_TRUE(drawn.active) << "Cavity was not drawn";
    EXPECT_EQ(drawn.draws, 1u);
    std::vector<uint8_t> on;
    ASSERT_TRUE(readFrame(on));
    ASSERT_EQ(on.size(), off.size());

    size_t pixels = 0, moved = 0, black = 0;
    double sum = 0.0;
    for (size_t i = 0; i + 3 < off.size(); i += 4){
        // the green terrain, not the blue background
        if (off[i + 1] <= off[i + 2] + 5)
            continue;
        pixels++;
        const double d = ((double)on[i] + on[i + 1] + on[i + 2] - off[i] - off[i + 1] - off[i + 2]) / 3.0;
        sum += std::abs(d);
        moved += std::abs(d) > 24.0 ? 1 : 0;
        black += (on[i] + on[i + 1] + on[i + 2]) < 24 ? 1 : 0;
    }
    ASSERT_GT(pixels, off.size() / 4 / 10) << "the terrain is not in the frame";
    EXPECT_LE(sum / (double)pixels, 3.0) << "the terrain's pixels moved a lot on average";
    EXPECT_LE((double)moved / (double)pixels, 0.005) << moved << " of " << pixels << " terrain pixels moved by more than 24";
    EXPECT_EQ(black, 0u) << "black pixels on the terrain";
    EXPECT_NO_ENGINE_ERRORS();
}

}  // namespace CavityGpuTest_cpp
