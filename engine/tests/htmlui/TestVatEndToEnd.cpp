// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The GPU half of the Vertex Animation end-to-end proofs (vertex animation spec 8): the files the
// real baker and importers wrote (editor/tests/VatEndToEndTest.cpp, the "bundle": this binary
// can't link the editor, the editor test binary has no GPU; tools/vat_e2e.sh runs both) loaded
// from disk like a game loads them (Model = <name>.vat.glb, Vertex Animator = <name>.vat through
// VertexAnimationPool) and drawn by the engine with the same camera, light and Lit materials as
// their sources:
// - render parity: the hero (tools/fixtures/pose-animation hero_animated.gltf) skinned on the GPU
//   and posed at time t by an Animation Player ("hero_animated.gltf#Walk", the runtime path)
//   against the VAT hero at the same t (VertexAnimator play + setTime), Vertices and Bones bakes,
//   Walk and Idle, at samples, between samples (smooth stretches, and Walk's keys, which fall
//   between two 30 fps samples: the worst interpolation error) and across the loop seams, from two
//   views (a 3/4 front view and the side, so motion along either view direction shows); a crowd
//   of 4 instances with their own clips and times against 4 skinned heroes at theirs; the rigid
//   hierarchy (rigid_arm, Auto -> Bones) and the morph target (morph_blob, Auto -> Vertices)
//   against their sources.
// - imports: the synthetic Houdini soft and rigid exports' silhouettes against a CPU raster of the
//   ground truth triangles at sample times and between samples.
// The times ("cases") come from the editor half with the error it measured for each: the bake
// decoded on the CPU exactly as vat.glsl does, against the source posed by its clip and skinned
// by CpuSkin (largest vertex distance in mm, largest normal angle). The comparison is a
// local-displacement test: every pixel of each image has a pixel within R pixels in the other
// image whose colour is within C (max channel, of 255). R is that distance in pixels at the
// model's nearest depth plus one pixel for rasterization; C covers the shading change of that
// normal angle (colourToleranceFor); where the angle is too large for colours to discriminate
// (only Walk on its keys) the silhouettes are compared instead. A few sliver-face pixels are
// allowed (kSliverPixels). Negative controls (the source a quarter of Walk's cycle later, two
// crowd instances' times swapped, another time's ground truth) must fail the same test, so the
// tolerance can't hide a wrong time. The engine can't read the depth buffer back here: the second
// view stands in for depth, and the CPU half checks positions and normals.
// Without the bundle the tests skip and say how to write it.

#include "TestGoldenCommon.h"

#include "Scene.h"
#include "component/InstancedMeshComponent.h"
#include "component/MeshComponent.h"
#include "component/VertexAnimatorComponent.h"
#include "object/AnimationPlayer.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Model.h"
#include "object/VertexAnimator.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/VertexAnimatorSystem.h"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

    const int screenWidth = 640;
    const int screenHeight = 360;

    fs::path repoRoot(){
        return fs::weakly_canonical(fs::path(JASPER_HTMLUI_TEST_SOURCE_DIR) / ".." / ".." / "..");
    }

    // the same rule as editor/tests/VatEndToEndTest.cpp's bundleDirectory()
    fs::path bundleDirectory(){
        if (const char* env = std::getenv("JASPER_VAT_E2E_DIR"); env && *env)
            return fs::path(env);
        uint64_t hash = 1469598103934665603ull;
        for (unsigned char c : repoRoot().generic_string()){
            hash ^= c;
            hash *= 1099511628211ull;
        }
        char name[64];
        std::snprintf(name, sizeof(name), "jasperengine-vat-e2e-%016llx", (unsigned long long)hash);
        return fs::temp_directory_path() / name;
    }

    // <name>.json of the bundle (null when absent)
    Json bundleMeta(const std::string& name){
        std::ifstream file(bundleDirectory() / (name + ".json"));
        if (!file)
            return Json();
        std::stringstream text;
        text << file.rdbuf();
        return Json::parse(text.str(), nullptr, false);
    }

    std::string bundleFile(const std::string& file){
        return (bundleDirectory() / file).generic_string();
    }

#define REQUIRE_BUNDLE(meta, name)                                                                                                  \
    const Json meta = bundleMeta(name);                                                                                             \
    if (meta.is_null() || meta.is_discarded())                                                                                      \
        GTEST_SKIP() << "no end-to-end bundle at " << bundleDirectory().string() << ": run tools/vat_e2e.sh, or "                 \
                     << "jasperengine-editor-tests --gtest_filter='VatEndToEnd.*' first (JASPER_VAT_E2E_DIR picks another directory)"

    // ---- images

    struct Image {
        std::vector<uint8_t> rgba;
        int width = 0;
        int height = 0;

        const uint8_t* at(int x, int y) const{ return &rgba[(size_t(y) * width + x) * 4]; }
        bool ink(int x, int y) const{
            const uint8_t* p = at(x, y);
            return std::max({std::abs(p[0] - goldenBackground), std::abs(p[1] - goldenBackground), std::abs(p[2] - goldenBackground)}) > 24;
        }
        size_t inkCount() const{
            size_t count = 0;
            for (int y = 0; y < height; y++)
                for (int x = 0; x < width; x++)
                    count += ink(x, y) ? 1 : 0;
            return count;
        }
    };

    int colorDistance(const uint8_t* a, const uint8_t* b){
        return std::max({std::abs(a[0] - b[0]), std::abs(a[1] - b[1]), std::abs(a[2] - b[2])});
    }

    struct Comparison {
        size_t inkA = 0, inkB = 0;
        size_t silhouetteXor = 0;       // pixels ink in one image only
        size_t differing = 0;           // pixels whose colours differ by more than C in place
        double meanDifference = 0.0;    // over the union of the silhouettes, in place
        size_t unmatched = 0;           // pixels with no pixel within R in the other image within C
        int radius = 0;
        int tolerance = 0;

        std::string text() const{
            std::ostringstream out;
            out << "ink " << inkA << " / " << inkB << ", silhouette xor " << silhouetteXor << ", in place: " << differing
                << " pixels over " << tolerance << ", mean difference " << meanDifference << "; unmatched within " << radius
                << " px: " << unmatched;
            return out.str();
        }
    };

    // the local-displacement test (see the top of the file) and in-place statistics
    Comparison compareImages(const Image& a, const Image& b, int radius, int tolerance){
        Comparison c;
        c.radius = radius;
        c.tolerance = tolerance;
        double sum = 0.0;
        size_t union_ = 0;
        auto matched = [&](const Image& from, const Image& to, int x, int y){
            const uint8_t* p = from.at(x, y);
            for (int dy = -radius; dy <= radius; dy++){
                for (int dx = -radius; dx <= radius; dx++){
                    const int qx = x + dx, qy = y + dy;
                    if (qx < 0 || qy < 0 || qx >= to.width || qy >= to.height)
                        continue;
                    if (colorDistance(p, to.at(qx, qy)) <= tolerance)
                        return true;
                }
            }
            return false;
        };
        for (int y = 0; y < a.height; y++){
            for (int x = 0; x < a.width; x++){
                const bool ia = a.ink(x, y), ib = b.ink(x, y);
                c.inkA += ia;
                c.inkB += ib;
                c.silhouetteXor += ia != ib;
                const int d = colorDistance(a.at(x, y), b.at(x, y));
                if (ia || ib){
                    union_++;
                    sum += d;
                }
                if (d > tolerance){
                    c.differing++;
                    if (!matched(a, b, x, y))
                        c.unmatched++;
                    if (!matched(b, a, x, y))
                        c.unmatched++;
                }
            }
        }
        c.meanDifference = union_ ? sum / double(union_) : 0.0;
        return c;
    }

    // the smallest C that leaves at most `allowed` pixels unmatched within `radius` (printed next to
    // each comparison: the measured margin under the tolerance used)
    int neededTolerance(const Image& a, const Image& b, int radius, size_t allowed){
        int lo = 0, hi = 255;
        while (lo < hi){
            const int mid = (lo + hi) / 2;
            if (compareImages(a, b, radius, mid).unmatched <= allowed)
                hi = mid;
            else
                lo = mid + 1;
        }
        return lo;
    }

    // a failed comparison's two images as PNGs in <bundle>/failures (for a person to look at)
    void dumpPair(const std::string& label, const Image& reference, const Image& baked){
        const fs::path dir = bundleDirectory() / "failures";
        fs::create_directories(dir);
        std::string file = label;
        for (char& c : file){
            if (!std::isalnum((unsigned char)c) && c != '.' && c != '-')
                c = '_';
        }
        writePng((dir / (file + ".reference.png")).string(), reference.rgba, reference.width, reference.height);
        writePng((dir / (file + ".vat.png")).string(), baked.rgba, baked.width, baked.height);
        std::printf("[ dump     ] %s\n", (dir / file).c_str());
    }

    // ---- the stage: a perspective camera, a directional light and ambient light, no shadows

    struct ViewSpec {
        Vector3 eye;
        Vector3 target;
        float fov = 40.0f;
    };

    struct Stage {
        SceneFixture fixture;
        Camera camera;
        ViewSpec view;
        bool checkedHiding = false;

        Stage(): camera(&fixture.getScene()){
            camera.setPerspective(40.0f, float(screenWidth) / float(screenHeight), 0.1f, 100.0f);
            camera.activate();
            scene().setGlobalIllumination(0.35f);
            Light& light = fixture.add<Light>();
            light.setType(LightType::DIRECTIONAL);
            light.setDirection(-0.4f, -0.6f, -0.7f);
            light.setIntensity(2.5f);
            light.setShadows(false);
            // the animators' clock stays at 0: times are set with setTime / setInstanceTime
            scene().getSystem<VertexAnimatorSystem>()->setPaused(true);
        }

        Scene& scene(){ return fixture.getScene(); }

        void look(const ViewSpec& spec){
            view = spec;
            camera.setPerspective(spec.fov, float(screenWidth) / float(screenHeight), 0.1f, 100.0f);
            camera.setPosition(spec.eye);
            camera.setTarget(spec.target);
        }

        Model& addModel(const std::string& path, const Vector3& position = Vector3(0, 0, 0)){
            Model& model = fixture.add<Model>();
            EXPECT_TRUE(model.loadModel(path)) << path;
            model.setPosition(position);
            return model;
        }

        // a source model whose Animation Player holds every clip it is asked to play (speed 0:
        // the state stays at the time it is set to)
        Model& addSource(const std::string& path, const std::vector<std::string>& clips, const Vector3& position = Vector3(0, 0, 0)){
            Model& model = addModel(path, position);
            AnimationPlayer player(&scene(), model.getEntity());
            player.setPlayAutomatically(false);
            player.setSpeed(0.0f);
            for (const std::string& clip : clips)
                EXPECT_TRUE(player.addClip(path + "#" + clip, clip)) << clip;
            return model;
        }

        void pose(Model& source, const std::string& clip, float time){
            AnimationPlayer player(&scene(), source.getEntity());
            player.play(clip);
            player.getState(clip).setTime(time);
        }

        // a Model of the .vat.glb with its Vertex Animator playing nothing yet
        Model& addVat(const Json& meta, const Vector3& position = Vector3(0, 0, 0)){
            Model& model = addModel(bundleFile(meta["mesh"]), position);
            VertexAnimator animator(&scene(), model.getEntity());
            animator.setPlayAutomatically(false);
            animator.setRandomStart(false);
            animator.setAnimation(bundleFile(meta["vat"]));
            return model;
        }

        void play(Model& vat, const std::string& clip, float time){
            VertexAnimator animator(&scene(), vat.getEntity());
            animator.play(clip);
            animator.setTime(time);
        }

        // frames until every model, animation and shader variant is loaded and every animator
        // plays on the GPU, then a few more
        bool settle(){
            Scene* current = &scene();
            int extra = 0;
            return GpuEnvironment::runFrames([current, &extra](){
                if (!current->getSystem<RenderSystem>()->isAllLoaded())
                    return false;
                auto meshes = current->getComponentArray<MeshComponent>();
                for (size_t i = 0; i < meshes->size(); i++){
                    const MeshComponent& mesh = meshes->getComponentFromIndex(i);
                    if (mesh.numSubmeshes == 0)
                        continue;
                    if (!mesh.loaded || mesh.needReload || mesh.needVariantReload || mesh.vat.variantActive != mesh.vat.variant)
                        return false;
                }
                auto animators = current->getComponentArray<VertexAnimatorComponent>();
                for (size_t i = 0; i < animators->size(); i++){
                    const VertexAnimatorComponent& animator = animators->getComponentFromIndex(i);
                    if (!animator.started || !animator.gpuActive)
                        return false;
                }
                return ++extra > 3;
            }, 900);
        }

        Image capture(){
            settle();
            Screen screen;
            Image image;
            EXPECT_TRUE(screen.read());
            image.rgba = screen.rgba;
            image.width = screen.width;
            image.height = screen.height;
            return image;
        }

        // pixels per metre at `point` (projected 10 cm up and right)
        float pixelsPerMetre(const Vector3& point){
            Vector2 a = project(point), b = project(point + Vector3(0, 0.1f, 0));
            const float up = std::hypot(a.x - b.x, a.y - b.y) * 10.0f;
            const Vector3 right = (view.target - view.eye).crossProduct(Vector3(0, 1, 0)).normalized();
            b = project(point + right * 0.1f);
            return std::max(up, std::hypot(a.x - b.x, a.y - b.y) * 10.0f);
        }

        // a world point on the screen (pixels, rows from the top)
        Vector2 project(const Vector3& p){
            const Vector4 clip = camera.getViewProjectionMatrix() * Vector4(p.x, p.y, p.z, 1.0f);
            return Vector2((clip.x / clip.w * 0.5f + 0.5f) * screenWidth, (0.5f - clip.y / clip.w * 0.5f) * screenHeight);
        }
    };

    // R for a model error of `errorMm` at `pxPerMetre`: the error in pixels, rounded up, plus one
    // pixel for rasterization (a sub-pixel move flips edge pixels)
    int radiusFor(double errorMm, float pxPerMetre){
        return 1 + int(std::ceil(errorMm / 1000.0 * pxPerMetre - 1e-3));
    }

    // C for a normal error of `normalDegrees` (the largest angle between a baked normal and the
    // skinned one, measured by the editor half): Lambert shading of a base colour <= 1 lit at 2.5
    // moves by at most 2.5 x the angle (radians), twice that after the sRGB encoding of values
    // above 0.1 (ambient light keeps every lit value above it); 8 levels on top cover the rounding
    // of two pipelines (the skin matrix product against rest + offset, half-float frames).
    const double kLightIntensity = 2.5;
    const double kSrgbSlope = 2.0;
    int colourToleranceFor(double normalDegrees){
        return 8 + int(std::ceil(255.0 * kLightIntensity * kSrgbSlope * normalDegrees * 3.14159265358979 / 180.0 - 1e-3));
    }
    // Above this normal error colours are not compared, silhouettes are: a face turned 2 degrees
    // (Walk between two samples on a key, where linear interpolation cuts the corner of the curve:
    // 18 mm, the bake's own reported worst case) shades up to ~45 levels differently, and a colour
    // tolerance that wide no longer tells a wrong pose from a right one at this size.
    const double kColourLimitDegrees = 0.5;
    // Unmatched pixels allowed per image: sliver faces (triangles seen edge-on, thinner than a
    // pixel and lit unlike their neighbours) that a sub-millimetre move covers or uncovers, one
    // pixel each (at most 2 measured per image); a wrong pose leaves 50 and more
    const size_t kSliverPixels = 4;

    // ink masks of two images compared within `radius` (silhouettes only: colours don't matter)
    Comparison compareMasks(const Image& a, const Image& b, int radius){
        Image ma = a, mb = b;
        for (Image* m : {&ma, &mb}){
            for (int y = 0; y < m->height; y++){
                for (int x = 0; x < m->width; x++){
                    uint8_t* px = &m->rgba[(size_t(y) * m->width + x) * 4];
                    const uint8_t value = m->ink(x, y) ? 255 : uint8_t(goldenBackground);
                    px[0] = px[1] = px[2] = value;
                }
            }
        }
        return compareImages(ma, mb, radius, 0);
    }

    // one time of the editor half's cases (<name>.json "cases")
    struct ParityCase {
        std::string what;
        std::string clip;
        float time = 0.0f;
        double errorMm = 0.0;
        double normalDegrees = 0.0;
        bool crowd = false;
    };

    std::vector<ParityCase> casesOf(const Json& meta){
        std::vector<ParityCase> cases;
        for (const Json& row : meta["cases"]){
            ParityCase c;
            c.what = row["what"];
            c.clip = row["clip"];
            c.time = row["time"];
            c.errorMm = row["error_mm"];
            c.normalDegrees = row["normal_error_deg"];
            c.crowd = row.value("crowd", false);
            cases.push_back(c);
        }
        return cases;
    }

    // how a case is compared at a scale
    struct Check {
        int radius = 1;
        int tolerance = 0;
        bool colour = true;

        Comparison compare(const Image& a, const Image& b) const{
            return colour ? compareImages(a, b, radius, tolerance) : compareMasks(a, b, radius);
        }
        std::string text() const{
            return colour ? "colours within " + std::to_string(tolerance) + " levels" : "silhouettes only";
        }
        // the unmatched pixels a wrong pose at least makes (measured: 130 and more for the
        // closest wrong times here, a quarter of Walk's cycle off)
        size_t wrongAtLeast() const{ return 50; }
    };

    Check checkFor(const ParityCase& c, float pxPerMetre){
        Check check;
        check.radius = radiusFor(c.errorMm, pxPerMetre);
        check.colour = c.normalDegrees <= kColourLimitDegrees;
        check.tolerance = colourToleranceFor(c.normalDegrees);
        return check;
    }

    // the hero in the middle of the screen: a 3/4 front view and the left side
    const ViewSpec heroFront{Vector3(1.6f, 1.4f, 3.2f), Vector3(0.0f, 0.9f, 0.0f)};
    const ViewSpec heroSide{Vector3(-3.6f, 1.2f, 0.4f), Vector3(0.0f, 0.9f, 0.0f)};

    // one source and its bake, posed at the same time, drawn alone each, compared from each view;
    // with a `wrongOffset`, the source that much later must fail the same comparison
    void expectParity(Stage& s, Model& source, Model& vat, const Json& meta, const ParityCase& c, const std::vector<ViewSpec>& views,
                      const Vector3& nearest, float wrongOffset){
        SCOPED_TRACE(c.what + " (" + c.clip + " at " + std::to_string(c.time) + " s)");
        s.pose(source, c.clip, c.time);
        s.play(vat, c.clip, c.time);
        if (!s.checkedHiding){
            // each image holds one model: hidden, both draw nothing
            s.look(views[0]);
            source.setVisible(false);
            vat.setVisible(false);
            EXPECT_EQ(s.capture().inkCount(), 0u) << "hiding the models empties the screen";
            s.checkedHiding = true;
        }
        for (size_t v = 0; v < views.size(); v++){
            SCOPED_TRACE("view " + std::to_string(v));
            s.look(views[v]);
            source.setVisible(true);
            vat.setVisible(false);
            const Image reference = s.capture();
            source.setVisible(false);
            vat.setVisible(true);
            const Image baked = s.capture();
            const Check check = checkFor(c, s.pixelsPerMetre(nearest));
            const Comparison result = check.compare(reference, baked);
            std::printf("[ parity   ] %s %s %.4f view %zu (%.3f mm, %.3f deg: %s): %s (needs C %d)\n", meta["vat"].get<std::string>().c_str(),
                        c.clip.c_str(), c.time, v, c.errorMm, c.normalDegrees, check.text().c_str(), result.text().c_str(),
                        neededTolerance(reference, baked, check.radius, kSliverPixels));
            EXPECT_GT(result.inkA, 2000u) << "the model is on the screen";
            EXPECT_LE(result.unmatched, kSliverPixels) << check.text() << ": " << result.text();
            EXPECT_LT(double(result.silhouetteXor), (check.colour ? 0.04 : 0.06) * double(result.inkA)) << result.text();
            if (result.unmatched)
                dumpPair(meta["vat"].get<std::string>() + "-" + c.clip + "-" + std::to_string(c.time) + "-view" + std::to_string(v), reference, baked);
            if (wrongOffset != 0.0f && v == 0){
                s.pose(source, c.clip, c.time + wrongOffset);
                source.setVisible(true);
                vat.setVisible(false);
                const Image off = s.capture();
                const Comparison wrong = check.compare(off, baked);
                EXPECT_GT(wrong.unmatched, check.wrongAtLeast()) << "the source " << wrongOffset << " s later is caught: " << wrong.text();
                s.pose(source, c.clip, c.time);
            }
        }
        source.setVisible(true);
        vat.setVisible(true);
    }

    void heroParity(const char* name){
        REQUIRE_BUNDLE(meta, name);
        Stage s;
        const std::vector<ParityCase> cases = casesOf(meta);
        ASSERT_GE(cases.size(), 8u);
        // one source per clip: a player keeps what a stopped clip animated and the next one
        // doesn't (Unity's legacy rule), while each baked clip starts from the rest pose
        std::map<std::string, Model*> sources;
        for (const ParityCase& c : cases){
            if (!sources.count(c.clip))
                sources[c.clip] = &s.addSource(meta["source"], {c.clip});
        }
        Model& vat = s.addVat(meta);
        ASSERT_TRUE(s.settle());
        const MeshComponent& mesh = vat.getComponent<MeshComponent>();
        EXPECT_EQ(mesh.vat.variantActive, meta["mode"] == "bones" ? VatVariant::BONES : VatVariant::VERTICES);
        // the nearest parts of the hero to the cameras are ~0.5 m in front of its middle
        const Vector3 nearest(0.25f, 0.9f, 0.45f);
        for (const ParityCase& c : cases){
            for (auto& entry : sources)
                entry.second->setVisible(false);
            // a quarter of Walk's cycle later: the legs elsewhere
            expectParity(s, *sources[c.clip], vat, meta, c, {heroFront, heroSide}, nearest, c.clip == "Walk" ? 0.25f : 0.0f);
        }
    }

    // columns [x0, x1) of an image, full height
    Image crop(const Image& image, int x0, int x1){
        Image out;
        x0 = std::clamp(x0, 0, image.width);
        x1 = std::clamp(x1, x0, image.width);
        out.width = x1 - x0;
        out.height = image.height;
        for (int y = 0; y < image.height; y++)
            out.rgba.insert(out.rgba.end(), image.at(x0, y), image.at(x0, y) + size_t(out.width) * 4);
        return out;
    }

    void crowdParity(const char* name){
        REQUIRE_BUNDLE(meta, name);
        Stage s;
        // the 4 crowd cases (two Walk, two Idle), one instance each, 1.9 m apart (the T-posed arms
        // span 1.6 m), every other one 0.5 m back; a skinned hero at each place posed at that
        // instance's time
        std::vector<ParityCase> members;
        for (const ParityCase& c : casesOf(meta))
            if (c.crowd)
                members.push_back(c);
        ASSERT_EQ(members.size(), 4u);
        ASSERT_EQ(members[0].clip, "Walk");
        ASSERT_EQ(members[1].clip, "Walk");
        std::vector<Vector3> positions;
        for (int i = 0; i < 4; i++)
            positions.push_back(Vector3(-2.85f + 1.9f * float(i), 0.0f, (i % 2) ? -0.5f : 0.0f));
        std::vector<Model*> sources;
        for (int i = 0; i < 4; i++)
            sources.push_back(&s.addSource(meta["source"], {members[size_t(i)].clip}, positions[size_t(i)]));
        Model& crowd = s.addVat(meta);
        crowd.createInstancedMesh();
        for (const Vector3& p : positions)
            crowd.addInstance(p);
        ASSERT_TRUE(s.settle());
        VertexAnimator animator(&s.scene(), crowd.getEntity());
        ASSERT_EQ(animator.getInstanceCount(), 4);
        for (int i = 0; i < 4; i++){
            s.pose(*sources[size_t(i)], members[size_t(i)].clip, members[size_t(i)].time);
            animator.playInstance(i, members[size_t(i)].clip);
            animator.setInstanceTime(i, members[size_t(i)].time);
        }
        s.look({Vector3(0.0f, 2.4f, 6.4f), Vector3(0.0f, 0.9f, -0.25f), 40.0f});
        for (Model* source : sources)
            source->setVisible(true);
        crowd.setVisible(false);
        const Image reference = s.capture();
        for (Model* source : sources)
            source->setVisible(false);
        crowd.setVisible(true);
        const Image baked = s.capture();

        // each instance in its own columns, with its own case's check
        int columns[4][2];
        std::vector<Check> checks;
        for (int i = 0; i < 4; i++){
            const ParityCase& c = members[size_t(i)];
            SCOPED_TRACE("instance " + std::to_string(i) + ": " + c.clip + " at " + std::to_string(c.time) + " s");
            const Vector3 centre = positions[size_t(i)] + Vector3(0.0f, 0.9f, 0.0f);
            columns[i][0] = int(std::floor(s.project(centre - Vector3(0.95f, 0.0f, 0.0f)).x));
            columns[i][1] = int(std::ceil(s.project(centre + Vector3(0.95f, 0.0f, 0.0f)).x));
            checks.push_back(checkFor(c, s.pixelsPerMetre(centre + Vector3(0.0f, 0.0f, 0.45f))));
            const Image a = crop(reference, columns[i][0], columns[i][1]);
            const Image b = crop(baked, columns[i][0], columns[i][1]);
            const Comparison result = checks.back().compare(a, b);
            std::printf("[ crowd    ] %s instance %d (%s %.4f, %s): %s (needs C %d)\n", name, i, c.clip.c_str(), c.time, checks.back().text().c_str(),
                        result.text().c_str(), neededTolerance(a, b, checks.back().radius, kSliverPixels));
            EXPECT_GT(result.inkA, 1000u) << "the instance is on the screen";
            EXPECT_LE(result.unmatched, kSliverPixels) << checks.back().text() << ": " << result.text();
            EXPECT_LT(double(result.silhouetteXor), (checks.back().colour ? 0.04 : 0.06) * double(result.inkA)) << result.text();
            if (result.unmatched)
                dumpPair(std::string(name) + "-crowd-" + std::to_string(i), a, b);
        }
        // each instance plays its own state: instances 0 and 1 (both Walk) with their times swapped
        // are caught in both columns
        animator.setInstanceTime(0, members[1].time);
        animator.setInstanceTime(1, members[0].time);
        const Image swapped = s.capture();
        for (int i = 0; i < 2; i++){
            const Comparison wrong = checks[size_t(i)].compare(crop(reference, columns[i][0], columns[i][1]), crop(swapped, columns[i][0], columns[i][1]));
            EXPECT_GT(wrong.unmatched, checks[size_t(i)].wrongAtLeast()) << "instance " << i << " at the other's time is caught: " << wrong.text();
        }
    }

    // ---- CPU raster of ground-truth triangles (both faces), into an ink mask

    Image rasterize(Stage& s, const std::vector<double>& triangles){
        Image image;
        image.width = screenWidth;
        image.height = screenHeight;
        image.rgba.assign(size_t(screenWidth) * screenHeight * 4, uint8_t(goldenBackground));
        for (size_t t = 0; t + 8 < triangles.size(); t += 9){
            Vector2 p[3];
            for (int k = 0; k < 3; k++)
                p[k] = s.project(Vector3(float(triangles[t + k * 3]), float(triangles[t + k * 3 + 1]), float(triangles[t + k * 3 + 2])));
            const float area = (p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[2].x - p[0].x) * (p[1].y - p[0].y);
            if (std::fabs(area) < 1e-6f)
                continue;
            const int x0 = std::max(0, int(std::floor(std::min({p[0].x, p[1].x, p[2].x}))));
            const int x1 = std::min(screenWidth - 1, int(std::ceil(std::max({p[0].x, p[1].x, p[2].x}))));
            const int y0 = std::max(0, int(std::floor(std::min({p[0].y, p[1].y, p[2].y}))));
            const int y1 = std::min(screenHeight - 1, int(std::ceil(std::max({p[0].y, p[1].y, p[2].y}))));
            for (int y = y0; y <= y1; y++){
                for (int x = x0; x <= x1; x++){
                    const float cx = x + 0.5f, cy = y + 0.5f;
                    float w[3];
                    for (int k = 0; k < 3; k++){
                        const Vector2& a = p[(k + 1) % 3];
                        const Vector2& b = p[(k + 2) % 3];
                        w[k] = ((b.x - a.x) * (cy - a.y) - (b.y - a.y) * (cx - a.x)) / area;
                    }
                    if (w[0] >= 0.0f && w[1] >= 0.0f && w[2] >= 0.0f){
                        uint8_t* px = &image.rgba[(size_t(y) * screenWidth + x) * 4];
                        px[0] = px[1] = px[2] = 255;
                    }
                }
            }
        }
        return image;
    }

    void importSilhouettes(const char* name){
        REQUIRE_BUNDLE(meta, name);
        Stage s;
        // frame the ground truth of every sample
        Vector3 lo(1e9f, 1e9f, 1e9f), hi(-1e9f, -1e9f, -1e9f);
        for (const Json& sample : meta["samples"]){
            const std::vector<double> t = sample["triangles"];
            for (size_t i = 0; i + 2 < t.size(); i += 3){
                lo = Vector3(std::min(lo.x, float(t[i])), std::min(lo.y, float(t[i + 1])), std::min(lo.z, float(t[i + 2])));
                hi = Vector3(std::max(hi.x, float(t[i])), std::max(hi.y, float(t[i + 1])), std::max(hi.z, float(t[i + 2])));
            }
        }
        const Vector3 centre = (lo + hi) * 0.5f;
        const float radius = (hi - lo).length() * 0.5f;
        const Vector3 direction = Vector3(0.45f, 0.75f, 1.0f).normalized();
        // the bounding sphere just fits the 40 degree field of view
        const float distance = radius / std::sin(20.0f * 3.14159265f / 180.0f);
        s.look({centre + direction * distance, centre, 40.0f});
        Model& vat = s.addVat(meta);
        ASSERT_TRUE(s.settle());
        const std::string clip = meta["clips"][0]["name"];
        const float pxPerMetre = s.pixelsPerMetre(centre + direction * radius);
        for (const Json& sample : meta["samples"]){
            const float time = sample["time"];
            SCOPED_TRACE(std::string(name) + " at " + std::to_string(time) + " s");
            s.play(vat, clip, time);
            const Image rendered = s.capture();
            const Image truth = rasterize(s, sample["triangles"].get<std::vector<double>>());
            const int r = radiusFor(sample["tolerance_mm"].get<double>(), pxPerMetre);
            const Comparison result = compareMasks(truth, rendered, r);
            std::printf("[ import   ] %s %.4f: %s\n", name, time, result.text().c_str());
            EXPECT_GT(result.inkA, 150u) << "the truth is on the screen";
            EXPECT_EQ(result.unmatched, 0u) << result.text();
            EXPECT_LT(double(result.silhouetteXor), 0.06 * double(result.inkA)) << result.text();
        }
        // the negative control: the truth of another sample doesn't match
        const Json& first = meta["samples"][0];
        const Json& last = meta["samples"][meta["samples"].size() - 1];
        s.play(vat, clip, first["time"].get<float>());
        const Image rendered = s.capture();
        const Comparison wrong = compareMasks(rasterize(s, last["triangles"].get<std::vector<double>>()), rendered, 1);
        EXPECT_GT(wrong.unmatched, 100u) << "another time's truth is caught: " << wrong.text();
    }

    class VatEndToEndTest : public ::testing::Test {
    protected:
        void SetUp() override{
            JASPER_REQUIRE_GPU();
            GpuEnvironment::setView(screenWidth, screenHeight, screenWidth, screenHeight, Scaling::FITWIDTH);
        }
    };

}

TEST_F(VatEndToEndTest, HeroVerticesMatchesTheSkinnedHero){
    heroParity("hero_vertices");
}

TEST_F(VatEndToEndTest, HeroBonesMatchesTheSkinnedHero){
    heroParity("hero_bones");
}

TEST_F(VatEndToEndTest, CrowdOfFourVerticesMatchesFourSkinnedHeroes){
    crowdParity("hero_vertices");
}

TEST_F(VatEndToEndTest, CrowdOfFourBonesMatchesFourSkinnedHeroes){
    crowdParity("hero_bones");
}

TEST_F(VatEndToEndTest, RigidHierarchyAutoBonesMatchesItsSource){
    REQUIRE_BUNDLE(meta, "rigid_arm");
    EXPECT_EQ(meta["mode"], "bones");
    Stage s;
    Model& source = s.addSource(meta["source"], {"Swing"});
    Model& vat = s.addVat(meta);
    ASSERT_TRUE(s.settle());
    EXPECT_EQ(vat.getComponent<MeshComponent>().vat.variantActive, VatVariant::BONES);
    const ViewSpec front{Vector3(0.9f, 1.2f, 2.6f), Vector3(0.2f, 0.6f, 0.0f)};
    const ViewSpec top{Vector3(0.2f, 3.2f, 0.6f), Vector3(0.2f, 0.6f, 0.0f)};
    const std::vector<ParityCase> cases = casesOf(meta);
    ASSERT_GE(cases.size(), 4u);
    for (const ParityCase& c : cases)
        expectParity(s, source, vat, meta, c, {front, top}, Vector3(0.3f, 0.6f, 0.3f), 0.25f);
}

TEST_F(VatEndToEndTest, MorphTargetAutoVerticesMatchesItsSource){
    REQUIRE_BUNDLE(meta, "morph_blob");
    EXPECT_EQ(meta["mode"], "vertices");
    Stage s;
    Model& source = s.addSource(meta["source"], {"Pulse"});
    Model& vat = s.addVat(meta);
    ASSERT_TRUE(s.settle());
    EXPECT_EQ(vat.getComponent<MeshComponent>().vat.variantActive, VatVariant::VERTICES);
    const ViewSpec oblique{Vector3(1.1f, 1.3f, 1.6f), Vector3(0.0f, 0.1f, 0.0f)};
    const ViewSpec side{Vector3(2.2f, 0.25f, 0.0f), Vector3(0.0f, 0.15f, 0.0f)};
    const std::vector<ParityCase> cases = casesOf(meta);
    ASSERT_GE(cases.size(), 3u);
    // the centre's rise at the peak against the rest: Pulse's weight 0 -> 1 -> 0
    for (const ParityCase& c : cases)
        expectParity(s, source, vat, meta, c, {oblique, side}, Vector3(0.3f, 0.2f, 0.4f), c.time < 0.4f ? 0.15f : 0.0f);
}

TEST_F(VatEndToEndTest, HoudiniSoftImportMatchesItsGroundTruth){
    importSilhouettes("houdini_soft");
}

TEST_F(VatEndToEndTest, HoudiniRigidImportMatchesItsGroundTruth){
    importSilhouettes("houdini_rigid");
}
