// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Vertex Animation baker (vertex animation spec 4.1-4.5, 4.7): sources normalised to clips,
// sampling against CpuSkin, Auto decisions, precision, validation kinds, the estimate, the
// existing-file rule, CreateVatCmd with its place modes, staleness and Rebake, cancel. A headless
// project (AppSettings in a temporary config directory + Project::loadProject) with the engine
// started on the editor's Platform, as CpuSkinTest does. Models: T1c's animated hero and T-pose
// hero (tools/fixtures/pose-animation), and tools/fixtures/vertex-animation (rigid_arm: a rigid
// two-part hierarchy, morph_blob: a morph target; make_vat_fixtures.py writes them).

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include "AppSettings.h"
#include "Engine.h"
#include "Out.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/CreateVatCmd.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/ModelLoadCmd.h"
#include "util/EntityPath.h"
#include "util/FileUtils.h"
#include "animation/PreviewOwner.h"
#include "command/type/PropertyCmd.h"
#include "vat/VatBakeSession.h"
#include "vat/VatCrowd.h"
#include "vat/VatDocument.h"
#include "vat/VatImportWrite.h"

#include "Scene.h"
#include "animation/AnimationClipIO.h"
#include "animation/ModelSubClips.h"
#include "component/AnimationPlayerComponent.h"
#include "component/InstancedMeshComponent.h"
#include "component/LatticeComponent.h"
#include "component/LatticeModifierComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "component/VertexAnimatorComponent.h"
#include "lattice/LatticeRender.h"
#include "pool/AnimationClipPool.h"
#include "pool/TextureDataPool.h"
#include "pool/VertexAnimationPool.h"
#include "render/CpuSkin.h"
#include "subsystem/ActionSystem.h"
#include "subsystem/MeshSystem.h"

#include "json.hpp"
#include "tiny_gltf.h"

#include "support/PosixCompat.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::vat;
using jasperengine::vat::VatClip;
using jasperengine::vat::VatData;
using jasperengine::vat::VatLayout;
using jasperengine::vat::VatMode;
using jasperengine::vat::VatPayload;
using jasperengine::vat::VatPrecision;
namespace fs = std::filesystem;

namespace {

    // ---------------------------------------------------------------- headless project

    struct Headless {
        bool ready = false;
        fs::path dir;
        std::unique_ptr<Project> project;
    };

    fs::path repoFile(const fs::path& relative) {
        return (fs::path(JASPER_TEST_FIXTURES_DIR) / ".." / ".." / ".." / relative).lexically_normal();
    }

    Headless& headless() {
        static Headless h = []() {
            Headless out;
            const fs::path root = fs::temp_directory_path() / ("jasper-vat-baker-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            fs::create_directories(out.dir / "assets" / "models");
            std::ofstream(out.dir / "project.yaml") << "name: vatbaker\nassetsDir: assets\n";
            AppSettings::initialize();
            out.project.reset(new Project());
            // the engine runs already (TestMain, its TestSystem): this project's folders are its paths
            editor::test::useProjectPaths(out.project.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            out.ready = out.project->loadProject(out.dir, false);
            Engine::setAsyncLoading(false);
            return out;
        }();
        // every test of the project (the paths end with each test)
        if (h.project) editor::test::useProjectPaths(h.project.get());
        return h;
    }

    Project* project() { return headless().project.get(); }
    const fs::path& root() { return headless().dir; }
    CommandHistory* history(uint32_t sceneId) { return CommandHandle::get(sceneId); }
    Scene* sceneOf(uint32_t sceneId) { return project()->getScene(sceneId)->scene; }

    std::string readAll(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    void writeAll(const fs::path& path, const std::string& content) {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << content;
    }

    // a fresh assets/models per test, with every fixture model
    void resetAssets() {
        const fs::path models = root() / "assets" / "models";
        fs::remove_all(models);
        fs::remove_all(root() / "assets" / "anim");
        fs::create_directories(models);
        const fs::path pose = fs::path("tools") / "fixtures" / "pose-animation" / "assets" / "models";
        const fs::path vat = fs::path("tools") / "fixtures" / "vertex-animation" / "assets" / "models";
        fs::copy_file(repoFile(pose / "hero_animated.gltf"), models / "hero.gltf", fs::copy_options::overwrite_existing);
        fs::copy_file(repoFile(pose / "hero_tpose.gltf"), models / "tpose.gltf", fs::copy_options::overwrite_existing);
        fs::copy_file(repoFile(vat / "rigid_arm.gltf"), models / "rigid_arm.gltf", fs::copy_options::overwrite_existing);
        fs::copy_file(repoFile(vat / "morph_blob.gltf"), models / "morph_blob.gltf", fs::copy_options::overwrite_existing);
        for (const char* name : {"hero.gltf", "tpose.gltf", "rigid_arm.gltf", "morph_blob.gltf"}) {
            MeshSystem::invalidateModelFile((models / name).string());
        }
        AnimationClipPool::clear();
    }

    uint32_t newScene() {
        Project* p = project();
        if (SceneProject* current = p->getSelectedScene()) current->isModified = false;
        p->createNewScene("Vat", SceneType::SCENE_3D);
        return p->getSelectedSceneId();
    }

    Entity loadModel(uint32_t sceneId, const std::string& assetsRelative, const std::string& name = "Hero") {
        CreateEntityCmd* create = new CreateEntityCmd(project(), sceneId, name, EntityCreationType::MODEL);
        create->setQuiet(true);
        history(sceneId)->addCommandNoMerge(create);
        const Entity entity = create->getEntity();
        history(sceneId)->addCommandNoMerge(new ModelLoadCmd(project(), sceneId, entity, assetsRelative));
        EXPECT_FALSE(sceneOf(sceneId)->getComponent<ModelComponent>(entity).loadedFilename.empty()) << assetsRelative;
        return entity;
    }

    VatBakeSpec modelSpec(const std::string& model, std::vector<std::string> clips = {}) {
        VatBakeSpec spec;
        spec.source.kind = VatSourceKind::Model;
        spec.source.path = "assets/models/" + model;
        spec.clips = std::move(clips);
        return spec;
    }

    bool hasKind(const std::vector<VatIssue>& issues, const std::string& kind, bool* error = nullptr) {
        for (const VatIssue& issue : issues) {
            if (issue.kind == kind) {
                if (error) *error = issue.error;
                return true;
            }
        }
        return false;
    }

    std::string kinds(const std::vector<VatIssue>& issues) {
        std::string out;
        for (const VatIssue& issue : issues) out += issue.kind + "(" + issue.message + ") ";
        return out;
    }

    // ---------------------------------------------------------------- reference poses

    // every vertex of every mesh under `root`: its rest (identity skin, root space) and its
    // position posed by `clip` at `time` (CpuSkin, root space)
    struct RefVertex {
        Vector3 rest;
        Vector3 posed;
    };

    Vector3 apply(const Matrix4& m, const Vector3& p) {
        return Vector3(m.get(0, 0) * p.x + m.get(1, 0) * p.y + m.get(2, 0) * p.z + m.get(3, 0),
                       m.get(0, 1) * p.x + m.get(1, 1) * p.y + m.get(2, 1) * p.z + m.get(3, 1),
                       m.get(0, 2) * p.x + m.get(1, 2) * p.y + m.get(2, 2) * p.z + m.get(3, 2));
    }

    std::vector<RefVertex> referencePose(Scene* scene, Entity rootEntity, const std::shared_ptr<const AnimationClip>& clip, float time) {
        std::vector<RefVertex> out;
        cpuskin::updateTransforms(scene);
        const Matrix4 rootInverse = scene->getComponent<Transform>(rootEntity).modelMatrix.inverse();
        // rest first
        std::vector<Entity> meshes;
        std::vector<std::vector<Vector3>> rests;
        auto transforms = scene->getComponentArray<Transform>();
        for (size_t i = 0; i < transforms->size(); i++) {
            const Entity entity = transforms->getEntity(i);
            Entity walk = entity;
            bool under = false;
            for (int d = 0; walk != NULL_ENTITY && d < 1000; d++) {
                if (walk == rootEntity) { under = true; break; }
                walk = scene->getComponent<Transform>(walk).parent;
            }
            const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
            if (!under || !mesh || mesh->numSubmeshes == 0) continue;
            cpuskin::CpuSkinSource source;
            std::string error;
            EXPECT_TRUE(cpuskin::buildSource(*mesh, source, &error)) << error;
            int maxBone = 0;
            for (const auto& ids : source.boneIds) for (int id : ids) maxBone = std::max(maxBone, id);
            std::vector<Matrix4> identity((size_t)maxBone + 1);
            cpuskin::CpuSkinResult rest;
            EXPECT_TRUE(cpuskin::cpuDeform(source, identity, mesh->morphWeights, nullptr, rest, &error)) << error;
            const Matrix4 toRoot = rootInverse * scene->getComponent<Transform>(entity).modelMatrix;
            std::vector<Vector3> r;
            for (const Vector3& p : rest.positions) r.push_back(apply(toRoot, p));
            meshes.push_back(entity);
            rests.push_back(r);
        }
        anim::ClipPreviewSession session;
        session.root = rootEntity;
        ActionSystem::PlayerPreview preview;
        ActionSystem::PlayerPreviewState state;
        state.clip = clip;
        state.time = time;
        state.wrap = ClipWrapMode::ClampForever;
        preview.states.push_back(state);
        scene->getSystem<ActionSystem>()->previewPlayer(rootEntity, preview, session);
        cpuskin::updateTransforms(scene);
        LatticeRender::prepare(scene);
        for (size_t m = 0; m < meshes.size(); m++) {
            cpuskin::CpuSkinResult posed;
            std::string error;
            EXPECT_TRUE(cpuskin::deformEntity(scene, meshes[m], posed, &error)) << error;
            const Matrix4 toRoot = rootInverse * scene->getComponent<Transform>(meshes[m]).modelMatrix;
            for (size_t v = 0; v < posed.positions.size(); v++) out.push_back({rests[m][v], apply(toRoot, posed.positions[v])});
        }
        scene->getSystem<ActionSystem>()->restorePlayerPreview(session);
        cpuskin::updateTransforms(scene);
        LatticeRender::prepare(scene);
        return out;
    }

    std::string restKey(const Vector3& p) {
        char text[96];
        std::snprintf(text, sizeof(text), "%.4f,%.4f,%.4f", std::round(p.x * 1e4) / 1e4, std::round(p.y * 1e4) / 1e4, std::round(p.z * 1e4) / 1e4);
        return text;
    }

    float distance(const Vector3& a, const Vector3& b) {
        const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // a payload texel as floats
    void texel(const VatPayload& payload, uint64_t index, float out[4]) {
        if (payload.precision == VatPrecision::Float) {
            std::memcpy(out, payload.bytes.data() + index * 16, 16);
        } else {
            uint16_t half[4];
            std::memcpy(half, payload.bytes.data() + index * 8, 8);
            for (int i = 0; i < 4; i++) out[i] = editor::vat::halfToFloat(half[i]);
        }
    }

    // the baked mesh posed at a global frame, as the GPU rebuilds it
    std::vector<Vector3> decodeFrame(const VatBakeResult& result, int globalFrame) {
        const VatData& data = result.data;
        const VatMeshData& mesh = result.mesh;
        const VatPayload& payload = result.payload();
        std::vector<Vector3> out;
        for (size_t v = 0; v < mesh.vertexCount(); v++) {
            const Vector3 rest(mesh.positions[v * 3], mesh.positions[v * 3 + 1], mesh.positions[v * 3 + 2]);
            if (data.mode == VatMode::Vertices) {
                float t[4];
                texel(payload, jasperengine::vat::vatTexelIndex(VatMode::Vertices, data.layout, globalFrame, (int)mesh.vatIndex[v], 0), t);
                out.push_back(Vector3(rest.x + t[0], rest.y + t[1], rest.z + t[2]));
            } else {
                Vector3 sum(0, 0, 0);
                for (int k = 0; k < 4; k++) {
                    const float w = mesh.weights[v * 4 + k];
                    if (w == 0.0f) continue;
                    float q[4], ts[4];
                    texel(payload, jasperengine::vat::vatTexelIndex(VatMode::Bones, data.layout, globalFrame, mesh.joints[v * 4 + k], 0), q);
                    texel(payload, jasperengine::vat::vatTexelIndex(VatMode::Bones, data.layout, globalFrame, mesh.joints[v * 4 + k], 1), ts);
                    const Vector3 p = applyBone(q, ts, ts[3], rest);
                    sum = Vector3(sum.x + w * p.x, sum.y + w * p.y, sum.z + w * p.z);
                }
                out.push_back(sum);
            }
        }
        return out;
    }

    // the largest distance between the baked mesh at `globalFrame` and the reference pose
    float frameError(const VatBakeResult& result, int globalFrame, const std::vector<RefVertex>& reference) {
        std::map<std::string, std::vector<Vector3>> byRest;
        for (const RefVertex& v : reference) byRest[restKey(v.rest)].push_back(v.posed);
        const std::vector<Vector3> decoded = decodeFrame(result, globalFrame);
        float worst = 0.0f;
        for (size_t v = 0; v < decoded.size(); v++) {
            const Vector3 rest(result.mesh.positions[v * 3], result.mesh.positions[v * 3 + 1], result.mesh.positions[v * 3 + 2]);
            auto it = byRest.find(restKey(rest));
            if (it == byRest.end()) {
                ADD_FAILURE() << "baked vertex " << v << " has no source vertex at its rest";
                return 1e9f;
            }
            float best = 1e9f;
            for (const Vector3& p : it->second) best = std::min(best, distance(p, decoded[v]));
            worst = std::max(worst, best);
        }
        return worst;
    }

    std::unique_ptr<VatBakeSession> bake(const VatBakeSpec& spec) {
        auto session = std::make_unique<VatBakeSession>(project());
        session->setSpec(spec);
        const bool ok = session->run();
        EXPECT_TRUE(ok) << session->error() << " / " << kinds(session->failureIssues());
        return session;
    }

    // ---------------------------------------------------------------- synthetic sources

    // one rigid renderer: a unit quad (two triangles, 4 vertices) with normals
    VatBakeRenderer quadRenderer(const std::string& name) {
        VatBakeRenderer renderer;
        renderer.name = name;
        cpuskin::CpuSkinSource& source = renderer.source;
        source.positions = {Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(1, 1, 0), Vector3(0, 1, 0)};
        source.normals.assign(4, Vector3(0, 0, 1));
        source.tangents.assign(4, Vector4(0, 0, 0, 0));
        source.uv0.assign(4, Vector2(0, 0));
        source.uv1.assign(4, Vector2(0, 0));
        source.colors.assign(4, Vector4(1, 1, 1, 1));
        source.boneIds.assign(4, {0, 0, 0, 0});
        source.boneWeights.assign(4, Vector4(0, 0, 0, 0));
        source.vertexSubmesh.assign(4, 0);
        source.vertexIndex = {0, 1, 2, 3};
        source.canonical = {0, 1, 2, 3};
        cpuskin::CpuSkinSubmesh submesh;
        submesh.vertexCount = 4;
        submesh.indices = {0, 1, 2, 0, 2, 3};
        submesh.hasNormal = true;
        source.submeshes.push_back(submesh);
        renderer.submeshMaterial = {-1};
        return renderer;
    }

    // a clip whose frame f moves each renderer by `move(renderer, time)`
    VatBakeClipInput movingClip(const std::string& name, size_t renderers, float length, float fps, ClipWrapMode wrap,
                                const std::function<Matrix4(size_t, float)>& move) {
        VatBakeClipInput clip;
        clip.name = name;
        clip.wrap = wrap;
        clip.length = length;
        clipFrames(length, wrap, fps, clip.frames, clip.fps);
        auto sample = [&](float time) {
            VatSampleState state;
            for (size_t r = 0; r < renderers; r++) {
                VatRendererState rs;
                rs.toRoot = move(r, time);
                state.renderers.push_back(rs);
            }
            return state;
        };
        for (int f = 0; f < clip.frames; f++) clip.samples.push_back(sample(clipSampleTime(length, wrap, clip.frames, clip.fps, f)));
        clip.midFrames = midFramesFor(clip.frames, wrap);
        for (int f : clip.midFrames) {
            const float t0 = clipSampleTime(length, wrap, clip.frames, clip.fps, f);
            const float t1 = f + 1 >= clip.frames ? length : clipSampleTime(length, wrap, clip.frames, clip.fps, f + 1);
            clip.mids.push_back(sample((t0 + t1) * 0.5f));
        }
        return clip;
    }

}

// ==================================================================== pure parts

TEST(VatBakerTypes, ClipFramesFollowTheLoopSeamRule) {
    int frames = 0;
    float fps = 0.0f;
    clipFrames(1.0f, ClipWrapMode::Loop, 30.0f, frames, fps);
    EXPECT_EQ(frames, 30);
    EXPECT_FLOAT_EQ(fps, 30.0f);
    EXPECT_FLOAT_EQ(clipSampleTime(1.0f, ClipWrapMode::Loop, frames, fps, 29), 29.0f / 30.0f) << "no duplicate end frame";
    clipFrames(1.0f, ClipWrapMode::Once, 30.0f, frames, fps);
    EXPECT_EQ(frames, 31);
    EXPECT_FLOAT_EQ(clipSampleTime(1.0f, ClipWrapMode::Once, frames, fps, 30), 1.0f) << "the last frame is the length";
    // a length that is no whole number of samples: fps adjusts so sample i is at i / fps
    clipFrames(0.9f, ClipWrapMode::Loop, 30.0f, frames, fps);
    EXPECT_EQ(frames, 27);
    EXPECT_NEAR(fps, 30.0f, 1e-3f);
    clipFrames(0.97f, ClipWrapMode::Loop, 30.0f, frames, fps);
    EXPECT_EQ(frames, 29);
    EXPECT_NEAR((float)frames / fps, 0.97f, 1e-6f);
    clipFrames(0.0f, ClipWrapMode::Once, 30.0f, frames, fps);
    EXPECT_EQ(frames, 1);
}

TEST(VatBakerTypes, HalfFloatsRoundToNearestEven) {
    for (float value : {0.0f, 1.0f, -2.5f, 0.000123f, 65504.0f, 1e-7f}) {
        const float back = editor::vat::halfToFloat(editor::vat::floatToHalf(value));
        EXPECT_NEAR(back, value, std::fabs(value) * 0.0005f + 6e-8f) << value;
    }
    EXPECT_EQ(editor::vat::floatToHalf(1.0f), 0x3c00);
    EXPECT_EQ(editor::vat::floatToHalf(-2.0f), 0xc000);
    EXPECT_EQ(editor::vat::floatToHalf(1.0f + 1.0f / 2048.0f), 0x3c00) << "a tie rounds to even";
    EXPECT_EQ(editor::vat::floatToHalf(1.0f + 3.0f / 2048.0f), 0x3c02);
    EXPECT_TRUE(std::isinf(editor::vat::halfToFloat(editor::vat::floatToHalf(1e6f))));
}

TEST(VatBakerTypes, BoneDecompositionRoundTripsAndKeepsMirrors) {
    const Matrix4 matrix = Matrix4::translateMatrix(Vector3(1, 2, 3)) * Quaternion(0.9238795f, 0.0f, 0.3826834f, 0.0f).getRotationMatrix() *
                           Matrix4::scaleMatrix(1.5f);
    float q[4], t[3], s;
    decomposeBone(matrix, q, t, s);
    EXPECT_NEAR(s, 1.5f, 1e-5f);
    const Vector3 p(0.3f, -0.7f, 2.0f);
    EXPECT_LT(distance(applyBone(q, t, s, p), apply(matrix, p)), 1e-5f);
    // a mirror is a rotation with a negative uniform scale
    const Matrix4 mirror = matrix * Matrix4::scaleMatrix(Vector3(-1, -1, -1));
    decomposeBone(mirror, q, t, s);
    EXPECT_NEAR(s, -1.5f, 1e-5f);
    EXPECT_LT(distance(applyBone(q, t, s, p), apply(mirror, p)), 1e-5f);
    // the tangent frame maps +X, +Z to the tangent and the normal
    float frame[4];
    const float origin[3] = {0, 0, 0};
    tangentFrame(Vector3(0, 1, 0), Vector3(1, 0, 0), frame);
    const Vector3 z = applyBone(frame, origin, 1.0f, Vector3(0, 0, 1));
    const Vector3 x = applyBone(frame, origin, 1.0f, Vector3(1, 0, 0));
    EXPECT_LT(distance(z, Vector3(0, 1, 0)), 1e-6f);
    EXPECT_LT(distance(x, Vector3(1, 0, 0)), 1e-6f);
}

TEST(VatBakerTypes, TooLargeFixesAreOrderedByEffect) {
    VatSizeInputs size;
    size.mode = VatMode::Vertices;
    size.vertices = 20000;
    size.bones = 40;
    size.normals = true;
    size.fps = 30.0f;
    for (const char* name : {"Walk", "Run", "Idle"}) {
        VatClipEstimate clip;
        clip.name = name;
        clip.length = 2.0f;
        clip.wrap = ClipWrapMode::Loop;
        size.clips.push_back(clip);
    }
    ASSERT_GT(sizeTexels(size), presetLimits(VatPreset::MobileWeb).maxTexels);
    const VatIssue issue = tooLargeIssue(size);
    EXPECT_EQ(issue.kind, "too_large");
    EXPECT_TRUE(issue.error);
    ASSERT_GE(issue.fixes.size(), 4u);
    // Bones mode shrinks it most, the desktop preset least
    EXPECT_EQ(issue.fixes.front().setting, "mode");
    std::vector<std::string> settings;
    for (const VatFix& fix : issue.fixes) settings.push_back(fix.setting);
    EXPECT_NE(std::find(settings.begin(), settings.end(), "fps"), settings.end());
    EXPECT_NE(std::find(settings.begin(), settings.end(), "normals"), settings.end());
    EXPECT_NE(std::find(settings.begin(), settings.end(), "clips"), settings.end());
    EXPECT_NE(std::find(settings.begin(), settings.end(), "preset"), settings.end());
    // the fps fix is the highest whole rate that fits
    for (const VatFix& fix : issue.fixes) {
        if (fix.setting != "fps") continue;
        VatSizeInputs lower = size;
        lower.fps = (float)std::atoi(fix.value.c_str());
        EXPECT_LE(sizeTexels(lower), presetLimits(VatPreset::MobileWeb).maxTexels);
        lower.fps += 1.0f;
        EXPECT_GT(sizeTexels(lower), presetLimits(VatPreset::MobileWeb).maxTexels);
    }
}

TEST(VatBakerTypes, ClipsMatchByNameThenInOrderPreferringLength) {
    std::vector<VatClip> old(3);
    old[0].name = "Walk"; old[0].frames = 30; old[0].fps = 30;
    old[1].name = "Stroll"; old[1].frames = 60; old[1].fps = 30;      // renamed Run (2 s)
    old[2].name = "Idle"; old[2].frames = 45; old[2].fps = 30;
    // Wave (1 s) is new, Run (2 s) was renamed
    const std::vector<int> match = matchClips(old, {"Walk", "Wave", "Run", "Idle"}, {1.0f, 1.0f, 2.0f, 1.5f});
    EXPECT_EQ(match, (std::vector<int>{0, -1, 1, 2}));
}

// ==================================================================== compute on synthetic input

TEST(VatBakerCompute, RigidPartsBakeAsOneBoneEachAndMergeEqualOnes) {
    VatBakeInput input;
    input.renderers = {quadRenderer("A"), quadRenderer("B"), quadRenderer("C")};
    // A and C stay, B turns: A and C share one (identity) bone
    input.clips.push_back(movingClip("Turn", 3, 1.0f, 10.0f, ClipWrapMode::Loop, [](size_t r, float t) {
        return r == 1 ? Matrix4::rotateZMatrix(t * 3.0f) : Matrix4();
    }));
    VatBakeOutput out;
    std::string error;
    ASSERT_TRUE(computeVatBake(input, out, error)) << error;
    EXPECT_EQ(out.data.mode, VatMode::Bones) << out.report.modeReason;
    EXPECT_EQ(out.data.layout.bones, 2);
    EXPECT_EQ(out.report.fetchesPerVertex, 4) << "weight-1 rigid pieces";
    EXPECT_EQ(out.mesh.vertexCount(), 12u);
    EXPECT_EQ(out.mesh.submeshes.size(), 1u) << "one submesh per material (all default)";
    EXPECT_NE(out.report.modeReason.find("rigid parts only"), std::string::npos) << out.report.modeReason;
}

TEST(VatBakerCompute, PerVertexErrorFollowsTheMeasurements) {
    std::string error;
    // a rigid piece sliding linearly: Bones reproduce it at the frames and between them (~0)
    {
        VatBakeInput input;
        input.renderers = {quadRenderer("Slide")};
        input.mode = VatModeChoice::Bones;
        input.clips.push_back(movingClip("Slide", 1, 1.0f, 10.0f, ClipWrapMode::Once, [](size_t, float t) {
            return Matrix4::translateMatrix(Vector3(t * 0.5f, 0.0f, 0.0f));
        }));
        VatBakeOutput out;
        ASSERT_TRUE(computeVatBake(input, out, error)) << error;
        ASSERT_EQ(out.data.mode, VatMode::Bones);
        ASSERT_EQ(out.vertexErrorMm.size(), out.mesh.vertexCount());
        for (float e : out.vertexErrorMm) EXPECT_LT(e, 1e-3f);
    }
    // an orbiting piece: the error is between the frames, it grows as the sample rate falls, and
    // the worst vertex's error is the report's max (in both modes)
    auto turning = [&](float fps, VatModeChoice mode) {
        VatBakeInput input;
        input.renderers = {quadRenderer("Turn")};
        input.mode = mode;
        input.precision = VatPrecisionChoice::Half;
        input.clips.push_back(movingClip("Turn", 1, 1.0f, fps, ClipWrapMode::Loop, [](size_t, float t) {
            // an orbit (1 m around the origin, once per loop) turning as it goes: neither the
            // offsets' lerp nor the translation's lerp follows the arc between frames
            const float a = t * 6.2831853f;
            return Matrix4::translateMatrix(Vector3(std::cos(a), std::sin(a), 0.0f)) * Matrix4::rotateZMatrix(t * 360.0f);
        }));
        VatBakeOutput out;
        EXPECT_TRUE(computeVatBake(input, out, error)) << error;
        return out;
    };
    for (VatModeChoice mode : {VatModeChoice::Vertices, VatModeChoice::Bones}) {
        const VatBakeOutput fine = turning(30.0f, mode);
        const VatBakeOutput coarse = turning(6.0f, mode);
        for (const VatBakeOutput* out : {&fine, &coarse}) {
            ASSERT_EQ(out->vertexErrorMm.size(), out->mesh.vertexCount());
            const float worst = *std::max_element(out->vertexErrorMm.begin(), out->vertexErrorMm.end());
            EXPECT_FLOAT_EQ(worst, out->report.maxErrorMm) << modeChoiceName(mode);
            ASSERT_EQ(out->report.clips.size(), 1u);
            EXPECT_FLOAT_EQ(worst, out->report.clips[0].maxErrorMm) << modeChoiceName(mode);
            for (float e : out->vertexErrorMm) EXPECT_GT(e, 0.0f) << "every vertex leaves the arc between frames";
        }
        const float fineWorst = *std::max_element(fine.vertexErrorMm.begin(), fine.vertexErrorMm.end());
        const float coarseWorst = *std::max_element(coarse.vertexErrorMm.begin(), coarse.vertexErrorMm.end());
        EXPECT_GT(coarseWorst, fineWorst * 4.0f) << modeChoiceName(mode) << ": a lower rate, a larger error between frames";
    }
    // half precision far from the origin: the frames' quantization is in it too
    {
        VatBakeInput input;
        input.renderers = {quadRenderer("Far")};
        input.mode = VatModeChoice::Vertices;
        input.precision = VatPrecisionChoice::Half;
        input.clips.push_back(movingClip("Far", 1, 1.0f, 10.0f, ClipWrapMode::Once, [](size_t, float t) {
            return Matrix4::translateMatrix(Vector3(100.0f + t * 0.1f, 0.0f, 0.0f));
        }));
        VatBakeOutput out;
        ASSERT_TRUE(computeVatBake(input, out, error)) << error;
        const float worst = *std::max_element(out.vertexErrorMm.begin(), out.vertexErrorMm.end());
        EXPECT_GE(worst, out.report.quantizationErrorMm);
        EXPECT_GT(worst, 1.0f) << "half floats 100 m from rest";
        EXPECT_FLOAT_EQ(worst, out.report.maxErrorMm);
    }
}

TEST(VatBakerCompute, HeatmapColoursRunGreenYellowRed) {
    float c[4];
    heatmapColor(0.0f, 2.0f, c);
    EXPECT_FLOAT_EQ(c[0], 0.0f);
    EXPECT_FLOAT_EQ(c[1], 1.0f);
    heatmapColor(2.0f, 2.0f, c);
    EXPECT_FLOAT_EQ(c[0], 1.0f);
    EXPECT_FLOAT_EQ(c[1], 1.0f);
    heatmapColor(1.0f, 2.0f, c);
    EXPECT_FLOAT_EQ(c[0], 0.5f);
    heatmapColor(4.0f, 2.0f, c);
    EXPECT_FLOAT_EQ(c[0], 1.0f);
    EXPECT_FLOAT_EQ(c[1], 0.0f);
    heatmapColor(100.0f, 2.0f, c);
    EXPECT_FLOAT_EQ(c[1], 0.0f) << "clamped at red";
    EXPECT_FLOAT_EQ(c[2], 0.0f);
    EXPECT_FLOAT_EQ(c[3], 1.0f);
}

TEST(VatBakerCompute, PrecisionAutoSwitchesToFloatFarFromRest) {
    VatBakeInput input;
    input.renderers = {quadRenderer("Far")};
    input.mode = VatModeChoice::Vertices;
    // the clip plays 100 m away from the rest pose: half offsets lose ~50 mm there
    input.clips.push_back(movingClip("Away", 1, 1.0f, 10.0f, ClipWrapMode::Loop, [](size_t, float t) {
        return Matrix4::translateMatrix(Vector3(100.0f + t * 0.1f, 0.0f, 0.0f));
    }));
    VatBakeOutput out;
    std::string error;
    ASSERT_TRUE(computeVatBake(input, out, error)) << error;
    EXPECT_EQ(out.data.layout.precision, VatPrecision::Float);
    EXPECT_TRUE(hasKind(out.report.warnings, "precision_switched")) << kinds(out.report.warnings);
    EXPECT_GT(out.halfErrorMm, out.report.toleranceMm);
    // the same near rest stays half
    input.clips[0] = movingClip("Near", 1, 1.0f, 10.0f, ClipWrapMode::Loop, [](size_t, float t) {
        return Matrix4::translateMatrix(Vector3(t * 0.1f, 0.0f, 0.0f));
    });
    ASSERT_TRUE(computeVatBake(input, out, error)) << error;
    EXPECT_EQ(out.data.layout.precision, VatPrecision::Half);
    EXPECT_FALSE(hasKind(out.report.warnings, "precision_switched"));
    // forced half stays half
    input.precision = VatPrecisionChoice::Half;
    input.clips[0] = movingClip("Away", 1, 1.0f, 10.0f, ClipWrapMode::Loop, [](size_t, float t) {
        return Matrix4::translateMatrix(Vector3(100.0f + t * 0.1f, 0.0f, 0.0f));
    });
    ASSERT_TRUE(computeVatBake(input, out, error)) << error;
    EXPECT_EQ(out.data.layout.precision, VatPrecision::Half);
}

TEST(VatBakerCompute, ValidationKindsOfTheBake) {
    std::string error;
    VatBakeOutput out;
    // no_animation
    {
        VatBakeInput input;
        input.renderers = {quadRenderer("A")};
        EXPECT_FALSE(computeVatBake(input, out, error));
        EXPECT_TRUE(hasKind(out.report.warnings, "no_animation"));
    }
    // clip_limit
    {
        VatBakeInput input;
        input.renderers = {quadRenderer("A")};
        for (int i = 0; i < jasperengine::vat::VAT_MAX_CLIPS + 1; i++) {
            input.clips.push_back(movingClip("C" + std::to_string(i), 1, 0.0f, 10.0f, ClipWrapMode::Loop, [](size_t, float) { return Matrix4(); }));
        }
        EXPECT_FALSE(computeVatBake(input, out, error));
        EXPECT_TRUE(hasKind(out.report.warnings, "clip_limit"));
    }
    // bone_limit: Bones forced on a skin of 65536 joints moving apart
    {
        const int joints = jasperengine::vat::VAT_MAX_BONES + 1;
        VatBakeInput input;
        VatBakeRenderer renderer = quadRenderer("Swarm");
        cpuskin::CpuSkinSource& source = renderer.source;
        // one triangle per three vertices, each vertex on its own joint
        const size_t vertices = (size_t)joints * 3;
        source.positions.assign(vertices, Vector3(0, 0, 0));
        for (size_t v = 0; v < vertices; v++) source.positions[v] = Vector3((float)(v % 3), (float)(v % 2), 0.0f);
        source.normals.assign(vertices, Vector3(0, 0, 1));
        source.tangents.assign(vertices, Vector4(0, 0, 0, 0));
        source.uv0.assign(vertices, Vector2(0, 0));
        source.uv1.assign(vertices, Vector2(0, 0));
        source.colors.assign(vertices, Vector4(1, 1, 1, 1));
        source.boneIds.resize(vertices);
        source.boneWeights.assign(vertices, Vector4(1, 0, 0, 0));
        source.vertexSubmesh.assign(vertices, 0);
        source.vertexIndex.resize(vertices);
        source.canonical.resize(vertices);
        source.submeshes[0].indices.resize(vertices);
        source.submeshes[0].vertexCount = (uint32_t)vertices;
        source.submeshes[0].hasSkinning = true;
        for (size_t v = 0; v < vertices; v++){
            source.boneIds[v] = {(int)(v / 3), 0, 0, 0};
            source.vertexIndex[v] = (uint32_t)v;
            source.canonical[v] = (uint32_t)v;
            source.submeshes[0].indices[v] = (uint32_t)v;
        }
        input.renderers.push_back(renderer);
        input.mode = VatModeChoice::Bones;
        VatBakeClipInput clip;
        clip.name = "Scatter";
        clip.wrap = ClipWrapMode::Once;
        clip.frames = 1;
        clip.fps = 10.0f;
        VatSampleState sample;
        VatRendererState state;
        for (int j = 0; j < joints; j++) state.skin.push_back(Matrix4::translateMatrix(Vector3((float)j, 0, 0)));
        sample.renderers.push_back(state);
        clip.samples.push_back(sample);
        input.clips.push_back(clip);
        EXPECT_FALSE(computeVatBake(input, out, error));
        EXPECT_TRUE(hasKind(out.report.warnings, "bone_limit")) << kinds(out.report.warnings);
        // Auto takes Vertices and says why
        input.mode = VatModeChoice::Auto;
        ASSERT_TRUE(computeVatBake(input, out, error)) << error;
        EXPECT_EQ(out.data.mode, VatMode::Vertices);
        EXPECT_NE(out.report.modeReason.find(std::to_string(joints) + " bones"), std::string::npos) << out.report.modeReason;
    }
    // many rigid parts stay Bones
    {
        VatBakeInput input;
        for (int i = 0; i < 300; i++) input.renderers.push_back(quadRenderer("P" + std::to_string(i)));
        input.clips.push_back(movingClip("Scatter", 300, 0.2f, 10.0f, ClipWrapMode::Loop, [](size_t r, float t) {
            return Matrix4::translateMatrix(Vector3((float)r * t, 0.0f, 0.0f));
        }));
        ASSERT_TRUE(computeVatBake(input, out, error)) << error;
        EXPECT_EQ(out.data.mode, VatMode::Bones) << out.report.modeReason;
        EXPECT_EQ(out.data.layout.bones, 300);
    }
    // too_large: Vertices over the preset's texels
    {
        VatBakeInput input;
        input.renderers = {quadRenderer("A")};
        input.mode = VatModeChoice::Vertices;
        // 4 vertices x 2 texels x 600000 samples (the size is checked before any sample is deformed)
        VatBakeClipInput big;
        big.name = "Long";
        big.length = 2000.0f;
        big.frames = 600000;
        big.fps = 300.0f;
        VatSampleState still;
        VatRendererState rs;
        still.renderers.push_back(rs);
        big.samples.assign(600000, still);
        input.clips.push_back(big);
        EXPECT_FALSE(computeVatBake(input, out, error));
        EXPECT_TRUE(hasKind(out.report.warnings, "too_large")) << kinds(out.report.warnings);
    }
    // events_dropped, large_interpolation_error, mixed_rest
    {
        VatBakeInput input;
        input.renderers = {quadRenderer("A")};
        input.mode = VatModeChoice::Vertices;
        // a fast spin sampled at 2 fps: far off between frames
        VatBakeClipInput clip = movingClip("Spin", 1, 2.0f, 2.0f, ClipWrapMode::Loop, [](size_t, float t) {
            return Matrix4::rotateZMatrix(t * 2.5f);
        });
        ClipEventData inside;
        inside.time = 0.5f;
        inside.function = "onStep";
        ClipEventData past = inside;
        past.time = 5.0f;
        clip.events = {inside, past};
        input.clips.push_back(clip);
        ASSERT_TRUE(computeVatBake(input, out, error)) << error;
        EXPECT_TRUE(hasKind(out.report.warnings, "events_dropped"));
        ASSERT_EQ(out.data.clips[0].events.size(), 1u);
        EXPECT_EQ(out.data.clips[0].events[0].function, "onStep");
        EXPECT_TRUE(hasKind(out.report.warnings, "large_interpolation_error")) << kinds(out.report.warnings);
        EXPECT_GT(out.report.clips[0].interpolationErrorMm, out.report.toleranceMm);
        // a skinned quad whose bind pose collapsed to a point: each vertex's own bone places it,
        // so the first frame is the rest
        cpuskin::CpuSkinSource& source = input.renderers[0].source;
        const Vector3 corners[4] = {source.positions[0], source.positions[1], source.positions[2], source.positions[3]};
        for (size_t v = 0; v < 4; v++){
            source.positions[v] = Vector3(0, 0, 0);
            source.boneIds[v] = {(int)v, 0, 0, 0};
            source.boneWeights[v] = Vector4(1, 0, 0, 0);
        }
        source.submeshes[0].hasSkinning = true;
        input.clips[0] = movingClip("Unfold", 1, 1.0f, 10.0f, ClipWrapMode::Loop, [](size_t, float) { return Matrix4(); });
        for (std::vector<VatSampleState>* list : {&input.clips[0].samples, &input.clips[0].mids}){
            for (size_t i = 0; i < list->size(); i++){
                std::vector<Matrix4>& skin = (*list)[i].renderers[0].skin;
                for (size_t v = 0; v < 4; v++) skin.push_back(Matrix4::translateMatrix(corners[v] * (1.0f + 0.1f * (float)i)));
            }
        }
        ASSERT_TRUE(computeVatBake(input, out, error)) << error;
        EXPECT_TRUE(hasKind(out.report.warnings, "mixed_rest")) << kinds(out.report.warnings);
        EXPECT_EQ(out.mesh.positions[3], 1.0f) << "the rest is the first frame (vertex 1 at x = 1)";
    }
}

TEST(VatBakerCompute, ALoopThatDoesntEndWhereItStartsIsNotToldToRaiseFps) {
    // the loop seam is measured even past VAT_MAX_MID_SAMPLES pairs
    for (int frames : {10, 33, 64, 65, 90, 400}) {
        const std::vector<int> mids = midFramesFor(frames, ClipWrapMode::Loop);
        ASSERT_FALSE(mids.empty());
        EXPECT_EQ(mids.back(), frames - 1) << frames << " frames";
        EXPECT_LE(mids.size(), (size_t)VAT_MAX_MID_SAMPLES);
        for (size_t i = 1; i < mids.size(); i++) EXPECT_LT(mids[i - 1], mids[i]);
    }
    EXPECT_EQ(midFramesFor(90, ClipWrapMode::Once).back(), 87) << "no seam: spread over the 89 pairs";

    auto bake = [](const std::function<Matrix4(size_t, float)>& move, float length) {
        VatBakeInput input;
        input.renderers = {quadRenderer("A")};
        input.mode = VatModeChoice::Vertices;
        input.clips.push_back(movingClip("Slide", 1, length, 30.0f, ClipWrapMode::Loop, move));
        VatBakeOutput out;
        std::string error;
        EXPECT_TRUE(computeVatBake(input, out, error)) << error;
        return out;
    };
    auto issueOf = [](const VatBakeOutput& out) {
        for (const VatIssue& issue : out.report.warnings)
            if (issue.kind == "large_interpolation_error") return issue;
        return VatIssue();
    };
    auto hasFix = [](const VatIssue& issue, const std::string& setting) {
        for (const VatFix& fix : issue.fixes)
            if (fix.setting == setting) return true;
        return false;
    };
    {
        // a straight slide of 1 m over 3 s (90 frames): exact between frames, but looping it
        // jumps back 1 m between its last frame and its first: no fps fixes that
        const VatBakeOutput out = bake([](size_t, float t) { return Matrix4::translateMatrix(Vector3(t / 3.0f, 0, 0)); }, 3.0f);
        const VatIssue issue = issueOf(out);
        ASSERT_EQ(issue.kind, "large_interpolation_error") << kinds(out.report.warnings);
        EXPECT_NEAR(out.report.clips[0].interpolationErrorMm, 500.0f, 20.0f) << "half the jump, at the seam";
        EXPECT_NE(issue.message.find("doesn't end where it starts"), std::string::npos) << issue.message;
        EXPECT_NE(issue.fix.find("once or clamp_forever"), std::string::npos) << issue.fix;
        // the fix names each surface's way: the MCP clips objects and bake-vat's flags
        EXPECT_NE(issue.fix.find("wrap: \"once\""), std::string::npos) << issue.fix;
        EXPECT_NE(issue.fix.find("--clip-wrap"), std::string::npos) << issue.fix;
        EXPECT_NE(issue.fix.find("--clip-interpolate"), std::string::npos) << issue.fix;
        EXPECT_FALSE(hasFix(issue, "fps")) << "a higher rate doesn't help at the seam";
        EXPECT_TRUE(hasFix(issue, "interpolate"));
    }
    {
        // a fast spin that loops seamlessly (two turns in 1 s, 24 degrees a frame): off between every
        // pair of frames alike, so the fix is a higher rate
        const VatBakeOutput out = bake([](size_t, float t) { return Matrix4::rotateZMatrix(t * 720.0f); }, 1.0f);
        const VatIssue issue = issueOf(out);
        ASSERT_EQ(issue.kind, "large_interpolation_error") << kinds(out.report.warnings);
        EXPECT_NEAR(out.report.clips[0].interpolationErrorMm, 1414.2f * (1.0f - std::cos(12.0f * 3.14159265f / 180.0f)), 1.0f)
            << "the chord of 24 degrees at the far corner";
        EXPECT_EQ(issue.message.find("doesn't end where it starts"), std::string::npos) << issue.message;
        EXPECT_TRUE(hasFix(issue, "fps"));
        EXPECT_NE(issue.fix.find("interpolate: false"), std::string::npos) << issue.fix;
        EXPECT_NE(issue.fix.find("--clip-interpolate"), std::string::npos) << issue.fix;
    }
}

TEST(VatBakerCompute, CancelStopsTheBake) {
    VatBakeInput input;
    input.renderers = {quadRenderer("A")};
    input.clips.push_back(movingClip("Turn", 1, 1.0f, 10.0f, ClipWrapMode::Loop, [](size_t, float t) { return Matrix4::rotateZMatrix(t); }));
    std::atomic<bool> cancel{true};
    VatBakeOutput out;
    std::string error;
    EXPECT_FALSE(computeVatBake(input, out, error, &cancel));
    EXPECT_EQ(error, "cancelled");
}

// ==================================================================== the hero

class VatBaker_ : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(headless().ready);
        resetAssets();
    }
};

TEST_F(VatBaker_, HeroBakesInBothModesAndMatchesCpuSkin) {
    const uint32_t sceneId = newScene();
    const Entity hero = loadModel(sceneId, "models/hero.gltf");
    Scene* scene = sceneOf(sceneId);
    const std::shared_ptr<const AnimationClip> walk = AnimationClipPool::getOrLoad("models/hero.gltf#Walk");
    const std::shared_ptr<const AnimationClip> idle = AnimationClipPool::getOrLoad("models/hero.gltf#Idle");
    ASSERT_TRUE(walk && idle);

    for (VatModeChoice mode : {VatModeChoice::Vertices, VatModeChoice::Bones}) {
        VatBakeSpec spec = modelSpec("hero.gltf", {"Walk", "Idle"});
        spec.mode = mode;
        std::unique_ptr<VatBakeSession> session = bake(spec);
        const VatBakeResult* result = session->result();
        ASSERT_TRUE(result);
        const VatData& data = result->data;
        ASSERT_EQ(data.clips.size(), 2u);
        EXPECT_EQ(data.clips[0].name, "Walk");
        EXPECT_EQ(data.clips[0].wrap, ClipWrapMode::Loop) << "model clips bake as loops";
        EXPECT_EQ(data.clips[0].frames, 30) << "Walk is 1 s: no duplicate end frame";
        EXPECT_EQ(data.clips[1].start, 30);
        EXPECT_EQ(data.clips[1].frames, 60);
        EXPECT_EQ(result->mesh.submeshes.size(), 3u) << "three materials";
        EXPECT_EQ(result->mesh.vertexCount(), (size_t)session->estimate().vertices) << "the three primitives share their vertices";
        EXPECT_LE(result->mesh.vertexCount(), 2801u);
        EXPECT_TRUE(data.bounds.valid);
        if (mode == VatModeChoice::Vertices) {
            EXPECT_EQ(data.mode, VatMode::Vertices);
            EXPECT_EQ(data.layout.vertices, (int)result->mesh.vertexCount());
            EXPECT_EQ(data.layout.texelsPerSample(VatMode::Vertices), 2);
            EXPECT_EQ(result->report.fetchesPerVertex, 4);
        } else {
            EXPECT_EQ(data.mode, VatMode::Bones);
            EXPECT_EQ(data.layout.precision, VatPrecision::Float);
            // the joints Walk and Idle move, and one bone for every joint they leave at rest
            EXPECT_GT(data.layout.bones, 5);
            EXPECT_LT(data.layout.bones, 43);
            EXPECT_EQ(data.layout.bones, session->estimate().bones);
            EXPECT_EQ(result->report.fetchesPerVertex, 16);
        }
        // decoded frames against CpuSkin of the same pose
        const float bound = mode == VatModeChoice::Vertices ? result->report.quantizationErrorMm / 1000.0f + 2e-5f : 1e-3f;
        for (int frame : {0, 7, 29}) {
            const float time = clipSampleTime(walk->getLength(), ClipWrapMode::Loop, data.clips[0].frames, data.clips[0].fps, frame);
            EXPECT_LE(frameError(*result, data.clips[0].start + frame, referencePose(scene, hero, walk, time)), bound)
                << modeChoiceName(mode) << " Walk frame " << frame;
        }
        for (int frame : {0, 33, 59}) {
            const float time = clipSampleTime(idle->getLength(), ClipWrapMode::Loop, data.clips[1].frames, data.clips[1].fps, frame);
            EXPECT_LE(frameError(*result, data.clips[1].start + frame, referencePose(scene, hero, idle, time)), bound)
                << modeChoiceName(mode) << " Idle frame " << frame;
        }
        // the preview asset is in the pool
        EXPECT_EQ(VertexAnimationPool::get(result->previewKey), result->preview);
    }
}

TEST_F(VatBaker_, OnceClipsIncludeTheEndFrame) {
    const uint32_t sceneId = newScene();
    const Entity hero = loadModel(sceneId, "models/hero.gltf");
    VatBakeSpec spec = modelSpec("hero.gltf", {"Walk"});
    spec.mode = VatModeChoice::Vertices;
    spec.clipOverrides.push_back({"Walk", ClipWrapMode::Once, 0.0f});
    std::unique_ptr<VatBakeSession> session = bake(spec);
    const VatBakeResult* result = session->result();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->data.clips[0].frames, 31);
    EXPECT_EQ(result->data.clips[0].wrap, ClipWrapMode::Once);
    const std::shared_ptr<const AnimationClip> walk = AnimationClipPool::getOrLoad("models/hero.gltf#Walk");
    EXPECT_LE(frameError(*result, 30, referencePose(sceneOf(sceneId), hero, walk, walk->getLength())),
              result->report.quantizationErrorMm / 1000.0f + 2e-5f);
}

TEST_F(VatBaker_, AutoPicksTheModeAndSaysWhy) {
    {
        std::unique_ptr<VatBakeSession> session = bake(modelSpec("hero.gltf", {"Walk"}));
        ASSERT_TRUE(session->result());
        EXPECT_EQ(session->result()->data.mode, VatMode::Bones) << session->result()->report.modeReason;
        EXPECT_NE(session->result()->report.modeReason.find("skin and rigid parts only"), std::string::npos);
        // the estimate guessed the same
        EXPECT_EQ(session->estimate().mode, VatMode::Bones);
    }
    {
        std::unique_ptr<VatBakeSession> session = bake(modelSpec("morph_blob.gltf"));
        ASSERT_TRUE(session->result());
        EXPECT_EQ(session->result()->data.mode, VatMode::Vertices);
        EXPECT_NE(session->result()->report.modeReason.find("morph targets"), std::string::npos) << session->result()->report.modeReason;
        EXPECT_EQ(session->estimate().mode, VatMode::Vertices);
        // the centre vertex rose 0.4 at the half
        const VatBakeResult* result = session->result();
        float highest = 0.0f;
        for (const Vector3& p : decodeFrame(*result, result->data.clips[0].frames / 2)) highest = std::max(highest, p.y);
        EXPECT_NEAR(highest, 0.4f, 1e-3f);
    }
    {
        std::unique_ptr<VatBakeSession> session = bake(modelSpec("rigid_arm.gltf", {"Swing"}));
        ASSERT_TRUE(session->result());
        const VatBakeResult* result = session->result();
        EXPECT_EQ(result->data.mode, VatMode::Bones) << result->report.modeReason;
        EXPECT_EQ(result->data.layout.bones, 2);
        EXPECT_EQ(result->report.fetchesPerVertex, 4);
        EXPECT_EQ(result->mesh.submeshes.size(), 2u);
        ASSERT_EQ(result->mesh.materials.size(), 2u);
        EXPECT_NEAR(result->mesh.materials[0].baseColor[0], 0.8f, 1e-6f) << "red";
        EXPECT_NEAR(result->mesh.materials[1].baseColor[2], 0.8f, 1e-6f) << "blue";
    }
    {
        std::unique_ptr<VatBakeSession> session = bake(modelSpec("rigid_arm.gltf", {"Stretch"}));
        ASSERT_TRUE(session->result());
        EXPECT_EQ(session->result()->data.mode, VatMode::Vertices);
        EXPECT_NE(session->result()->report.modeReason.find("nonuniform_bone_scale"), std::string::npos)
            << session->result()->report.modeReason;
    }
}

TEST_F(VatBaker_, EstimateAgreesWithTheBake) {
    for (VatModeChoice mode : {VatModeChoice::Vertices, VatModeChoice::Bones}) {
        VatBakeSpec spec = modelSpec("hero.gltf", {"Walk", "Idle"});
        spec.mode = mode;
        spec.fps = 24.0f;
        VatBakeSession session(project());
        session.setSpec(spec);
        EXPECT_FALSE(hasErrors(session.check())) << kinds(session.check());
        const VatEstimate estimate = session.estimate();
        ASSERT_TRUE(session.run()) << session.error();
        const VatReport& report = session.result()->report;
        EXPECT_EQ(estimate.vertices, report.vertices);
        if (mode == VatModeChoice::Bones) {
            EXPECT_EQ(estimate.bones, report.bones);
        }
        EXPECT_EQ(estimate.width, report.width);
        EXPECT_EQ(estimate.height, report.height);
        EXPECT_EQ(estimate.bytes, report.bytes);
        ASSERT_EQ(estimate.clips.size(), report.clips.size());
        for (size_t i = 0; i < report.clips.size(); i++) {
            EXPECT_EQ(estimate.clips[i].frames, report.clips[i].frames);
            EXPECT_EQ(estimate.clips[i].bytes, report.clips[i].bytes);
        }
        EXPECT_EQ(estimate.fetchesPerVertex, report.fetchesPerVertex);
    }
}

TEST_F(VatBaker_, CheckFindsEveryProblemBeforeSampling) {
    auto check = [](const VatBakeSpec& spec) {
        VatBakeSession session(project());
        session.setSpec(spec);
        return session.check();
    };
    bool error = false;
    EXPECT_TRUE(hasKind(check(modelSpec("missing.gltf")), "invalid_source", &error));
    EXPECT_TRUE(error);
    EXPECT_TRUE(hasKind(check(modelSpec("tpose.gltf")), "no_animation", &error)) << "a model without animations";
    EXPECT_TRUE(error);
    EXPECT_TRUE(hasKind(check(modelSpec("hero.gltf", {"Moonwalk"})), "invalid_settings", &error));
    VatBakeSpec zero = modelSpec("hero.gltf");
    zero.fps = 0.0f;
    EXPECT_TRUE(hasKind(check(zero), "invalid_settings"));
    VatBakeSpec socket = modelSpec("hero.gltf", {"Walk"});
    socket.sockets = {"Hero/NoSuchBone"};
    EXPECT_TRUE(hasKind(check(socket), "socket_not_found", &error));
    EXPECT_TRUE(error);
    // too big for mobile: every clip at 240 fps in Vertices mode, with the fixes
    VatBakeSpec big = modelSpec("hero.gltf");
    big.mode = VatModeChoice::Vertices;
    big.fps = 240.0f;
    const std::vector<VatIssue> bigIssues = check(big);
    EXPECT_TRUE(hasKind(bigIssues, "too_large", &error)) << kinds(bigIssues);
    EXPECT_TRUE(error);
    for (const VatIssue& issue : bigIssues) {
        if (issue.kind == "too_large") {
            EXPECT_GE(issue.fixes.size(), 3u);
        }
    }
    // float Vertices of Walk and Idle at 240 fps: fits, but over the 32 MB of mobile
    VatBakeSpec heavy = modelSpec("hero.gltf", {"Walk", "Idle"});
    heavy.mode = VatModeChoice::Vertices;
    heavy.precision = VatPrecisionChoice::Float;
    heavy.fps = 240.0f;
    EXPECT_TRUE(hasKind(check(heavy), "memory_high", &error)) << kinds(check(heavy));
    EXPECT_FALSE(error);
    // an explicit output that exists
    writeAll(root() / "assets" / "models" / "taken.vat", "{}");
    VatBakeSpec taken = modelSpec("hero.gltf", {"Walk"});
    taken.output = "assets/models/taken.vat";
    EXPECT_TRUE(hasKind(check(taken), "output_exists", &error));
    EXPECT_TRUE(error);
    taken.overwrite = true;
    EXPECT_FALSE(hasKind(check(taken), "output_exists"));
    // replace needs a GameObject; a crowd needs a count and an area
    VatBakeSpec replace = modelSpec("hero.gltf", {"Walk"});
    replace.place = VatPlace::Replace;
    EXPECT_TRUE(hasErrors(check(replace)));
    VatBakeSpec crowd = modelSpec("hero.gltf", {"Walk"});
    crowd.place = VatPlace::Crowd;
    EXPECT_FALSE(hasErrors(check(crowd))) << kinds(check(crowd));
    crowd.crowd.count = 0;
    EXPECT_TRUE(hasKind(check(crowd), "invalid_settings", &error)) << kinds(check(crowd));
    EXPECT_TRUE(error);
    crowd.crowd.count = 10;
    crowd.crowd.area = 0.0f;
    EXPECT_TRUE(hasKind(check(crowd), "invalid_settings")) << kinds(check(crowd));
}

TEST_F(VatBaker_, AnimFilesTheirEventsUnboundTracksAndNoMesh) {
    const fs::path anim = root() / "assets" / "anim";
    // a clip of the hero's Walk keys plus a track no node answers and two events
    ClipData walk;
    std::vector<ClipWarning> warnings;
    ASSERT_TRUE(anim::buildModelSubClip("models/hero.gltf#Walk", walk, warnings, nullptr));
    walk.guid = "0123456789abcdef0123456789abcdef";
    walk.wrap = ClipWrapMode::Loop;
    ClipTrackData ghost;
    ghost.path = "Hero/Tail";
    ghost.channel = ClipChannel::Position;
    ClipKey key;
    key.value = {0, 0, 0};
    ghost.keys.push_back(key);
    walk.tracks.push_back(ghost);
    ClipEventData step;
    step.time = 0.25f;
    step.function = "onFootstep";
    step.args.push_back(AnimationEvent::stringArg("left"));
    walk.events.push_back(step);
    anim::normalize(walk);
    writeAll(anim / "walk.anim", anim::writeClip(walk));

    VatBakeSpec spec;
    spec.source.kind = VatSourceKind::Anim;
    spec.source.path = "assets/models/hero.gltf";
    spec.source.anims = {"assets/anim/walk.anim"};
    VatBakeSession session(project());
    session.setSpec(spec);
    const std::vector<VatIssue> issues = session.check();
    EXPECT_TRUE(hasKind(issues, "unbound_tracks")) << kinds(issues);
    EXPECT_FALSE(hasErrors(issues)) << kinds(issues);
    ASSERT_TRUE(session.run()) << session.error();
    const VatBakeResult* result = session.result();
    ASSERT_EQ(result->data.clips.size(), 1u);
    EXPECT_EQ(result->data.clips[0].name, "walk");
    ASSERT_EQ(result->data.clips[0].events.size(), 1u) << "events come from the .anim";
    EXPECT_EQ(result->data.clips[0].events[0].function, "onFootstep");
    EXPECT_TRUE(hasKind(result->report.warnings, "unbound_tracks"));
    ASSERT_EQ(result->data.recipe.sources.size(), 1u);
    EXPECT_EQ(result->data.recipe.sources[0].kind, "anim");
    EXPECT_EQ(result->data.recipe.sources[0].clips, std::vector<std::string>{"assets/anim/walk.anim"});
    bool hashedAnim = false, hashedModel = false;
    for (const auto& hash : result->data.recipe.sources[0].hashes) {
        hashedAnim |= hash.first == "assets/anim/walk.anim" && hash.second.compare(0, 5, "sha1:") == 0;
        hashedModel |= hash.first == "assets/models/hero.gltf";
    }
    EXPECT_TRUE(hashedAnim && hashedModel);

    // a GameObject with nothing to draw
    const uint32_t sceneId = newScene();
    Scene* scene = sceneOf(sceneId);
    CreateEntityCmd* create = new CreateEntityCmd(project(), sceneId, "Empty", EntityCreationType::GAMEOBJECT);
    create->setQuiet(true);
    history(sceneId)->addCommandNoMerge(create);
    VatBakeSpec empty;
    empty.source.kind = VatSourceKind::Anim;
    empty.source.sceneId = sceneId;
    empty.source.path = EntityPath::of(*project()->getScene(sceneId), create->getEntity());
    empty.source.anims = {"assets/anim/walk.anim"};
    VatBakeSession nothing(project());
    nothing.setSpec(empty);
    const std::vector<VatIssue> emptyIssues = nothing.check();
    EXPECT_TRUE(hasKind(emptyIssues, "no_animation") || hasKind(emptyIssues, "no_mesh")) << kinds(emptyIssues);
    (void)scene;
}

// ==================================================================== writing, undo, staleness, rebake

TEST_F(VatBaker_, WrittenFilesLoadAndFollowTheExistingFileRule) {
    std::unique_ptr<VatBakeSession> session = bake(modelSpec("hero.gltf", {"Walk", "Idle"}));
    VatWriteResult written;
    std::string error;
    ASSERT_TRUE(session->write(written, error)) << error;
    EXPECT_EQ(written.vat, "assets/models/hero.vat");
    EXPECT_EQ(written.mesh, "assets/models/hero.vat.glb");
    EXPECT_FALSE(written.rewritten);
    const fs::path vatFile = root() / written.vat;
    ASSERT_TRUE(fs::exists(vatFile));
    ASSERT_TRUE(fs::exists(root() / written.mesh));

    // the pool loads the files
    std::string loadError;
    std::shared_ptr<const jasperengine::vat::VertexAnimation> asset = VertexAnimationPool::getOrLoad(vatFile.string(), &loadError);
    ASSERT_TRUE(asset) << loadError;
    EXPECT_EQ(asset->getData().clips.size(), 2u);
    EXPECT_EQ(asset->getData().guid, written.guid);
    EXPECT_EQ(asset->getPayload().bytes, session->result()->payload().bytes);
    // and the mesh loads through MeshSystem with its _VAT_INDEX rows
    const uint32_t sceneId = newScene();
    const Entity model = loadModel(sceneId, "models/hero.vat.glb", "Loaded");
    Scene* scene = sceneOf(sceneId);
    size_t vertices = 0;
    for (const auto& node : scene->getComponent<ModelComponent>(model).meshNodesMapping) {
        if (const MeshComponent* mesh = scene->findComponent<MeshComponent>(node.second)) {
            cpuskin::CpuSkinSource source;
            if (mesh->numSubmeshes > 0 && cpuskin::buildSource(*mesh, source, nullptr)) {
                for (size_t v = 0; v < source.vertexCount(); v++) vertices += source.canonical[v] == v;
            }
        }
    }
    if (const MeshComponent* own = scene->findComponent<MeshComponent>(model)) {
        cpuskin::CpuSkinSource source;
        if (own->numSubmeshes > 0 && cpuskin::buildSource(*own, source, nullptr)) {
            for (size_t v = 0; v < source.vertexCount(); v++) vertices += source.canonical[v] == v;
        }
    }
    EXPECT_EQ(vertices, session->result()->mesh.vertexCount());

    // the same source and clips again: rewritten, GUID kept
    std::unique_ptr<VatBakeSession> again = bake(modelSpec("hero.gltf", {"Idle", "Walk"}));
    VatWriteResult rewritten;
    ASSERT_TRUE(again->write(rewritten, error)) << error;
    EXPECT_EQ(rewritten.vat, "assets/models/hero.vat");
    EXPECT_TRUE(rewritten.rewritten);
    EXPECT_EQ(rewritten.guid, written.guid);
    // other clips of the same model: another asset
    std::unique_ptr<VatBakeSession> walkOnly = bake(modelSpec("hero.gltf", {"Walk"}));
    VatWriteResult other;
    ASSERT_TRUE(walkOnly->write(other, error)) << error;
    EXPECT_EQ(other.vat, "assets/models/hero 2.vat");
    EXPECT_FALSE(other.rewritten);
    EXPECT_NE(other.guid, written.guid);

    // another source with that default name: a free name
    fs::copy_file(root() / "assets/models/rigid_arm.gltf", root() / "assets/models/hero_copy.gltf");
    writeAll(root() / "assets/models/rigid_arm.vat", readAll(vatFile));      // a hero .vat under the arm's name
    std::unique_ptr<VatBakeSession> arm = bake(modelSpec("rigid_arm.gltf", {"Swing"}));
    VatWriteResult free;
    ASSERT_TRUE(arm->write(free, error)) << error;
    EXPECT_EQ(free.vat, "assets/models/rigid_arm 2.vat");
    EXPECT_FALSE(free.rewritten);
    EXPECT_NE(free.guid, written.guid);

    // files only: one project undo step
    CommandHistory* projectHistory = project()->getProjectCommandHistory();
    ASSERT_TRUE(projectHistory->undo());
    EXPECT_FALSE(fs::exists(root() / "assets/models/rigid_arm 2.vat"));
    EXPECT_FALSE(fs::exists(root() / "assets/models/rigid_arm 2.vat.glb"));
    ASSERT_TRUE(projectHistory->redo());
    EXPECT_TRUE(fs::exists(root() / "assets/models/rigid_arm 2.vat"));

    // an explicit existing path is refused without overwrite
    VatBakeSpec explicitSpec = modelSpec("rigid_arm.gltf", {"Swing"});
    explicitSpec.output = "assets/models/hero.vat";
    VatBakeSession refused(project());
    refused.setSpec(explicitSpec);
    EXPECT_FALSE(refused.run());
    EXPECT_TRUE(hasKind(refused.failureIssues(), "output_exists"));
}

TEST_F(VatBaker_, PlaceBesideAndReplaceAreOneUndoStep) {
    const uint32_t sceneId = newScene();
    const Entity hero = loadModel(sceneId, "models/hero.gltf");
    Scene* scene = sceneOf(sceneId);
    scene->getComponent<Transform>(hero).position = Vector3(2.0f, 0.0f, -1.0f);
    AnimationPlayerComponent player;
    AnimationPlayerClip entry;
    entry.clip = "models/hero.gltf#Walk";
    player.clips.push_back(entry);
    scene->addComponent<AnimationPlayerComponent>(hero, player);
    cpuskin::updateTransforms(scene);

    for (VatPlace place : {VatPlace::Beside, VatPlace::Replace}) {
        VatBakeSpec spec;
        spec.source.kind = VatSourceKind::Entity;
        spec.source.sceneId = sceneId;
        spec.source.path = EntityPath::of(*project()->getScene(sceneId), hero);
        spec.place = place;
        spec.output = place == VatPlace::Beside ? "assets/models/beside.vat" : "assets/models/replace.vat";
        std::unique_ptr<VatBakeSession> session = bake(spec);
        ASSERT_TRUE(session->result());
        EXPECT_EQ(session->result()->data.clips[0].name, "Walk");
        VatWriteResult written;
        std::string error;
        ASSERT_TRUE(session->write(written, error)) << error;
        ASSERT_EQ(written.sceneId, sceneId);
        const Entity placed = written.entity;
        ASSERT_NE(placed, NULL_ENTITY);
        EXPECT_EQ(scene->getEntityName(placed), "Hero (VAT)");
        EXPECT_EQ(scene->getComponent<Transform>(placed).position, Vector3(2.0f, 0.0f, -1.0f));
        EXPECT_EQ(scene->getComponent<ModelComponent>(placed).filename, place == VatPlace::Beside ? "models/beside.vat.glb" : "models/replace.vat.glb");
        const VertexAnimatorComponent* animator = scene->findComponent<VertexAnimatorComponent>(placed);
        ASSERT_TRUE(animator);
        EXPECT_EQ(animator->animation, place == VatPlace::Beside ? "models/beside.vat" : "models/replace.vat");
        EXPECT_EQ(animator->guid, written.guid);
        EXPECT_EQ(project()->getScene(sceneId)->selectedEntities, std::vector<Entity>{placed});
        EXPECT_EQ(scene->isEntityActive(hero), place != VatPlace::Replace);

        // one undo takes back the GameObject, the source's state and the files
        ASSERT_TRUE(history(sceneId)->undo());
        EXPECT_FALSE(scene->isEntityCreated(placed) && scene->findComponent<VertexAnimatorComponent>(placed));
        EXPECT_TRUE(scene->isEntityActive(hero));
        EXPECT_FALSE(fs::exists(root() / written.vat));
        EXPECT_FALSE(fs::exists(root() / written.mesh));
        // redo puts everything back
        ASSERT_TRUE(history(sceneId)->redo());
        EXPECT_TRUE(fs::exists(root() / written.vat));
        ASSERT_TRUE(scene->findComponent<VertexAnimatorComponent>(placed));
        EXPECT_EQ(scene->isEntityActive(hero), place != VatPlace::Replace);
        // undo keeps a file edited since
        const std::string edited = readAll(root() / written.vat) + " ";
        writeAll(root() / written.vat, edited);
        ASSERT_TRUE(history(sceneId)->undo());
        EXPECT_TRUE(fs::exists(root() / written.vat)) << "changed after the bake: kept";
        EXPECT_FALSE(fs::exists(root() / written.mesh));
        fs::remove(root() / written.vat);
    }
}

TEST_F(VatBaker_, PlaceCrowdMakesTheGameObjectACrowdInOneUndoStep) {
    const uint32_t sceneId = newScene();
    const Entity arm = loadModel(sceneId, "models/rigid_arm.gltf", "Arm");
    Scene* scene = sceneOf(sceneId);
    scene->getComponent<Transform>(arm).position = Vector3(4.0f, 1.0f, 0.0f);
    AnimationPlayerComponent player;
    AnimationPlayerClip entry;
    entry.clip = "models/rigid_arm.gltf#Swing";
    player.clips.push_back(entry);
    scene->addComponent<AnimationPlayerComponent>(arm, player);
    cpuskin::updateTransforms(scene);

    VatBakeSpec spec;
    spec.source.kind = VatSourceKind::Entity;
    spec.source.sceneId = sceneId;
    spec.source.path = EntityPath::of(*project()->getScene(sceneId), arm);
    spec.place = VatPlace::Crowd;
    spec.crowd.count = 150;
    spec.crowd.area = 6.0f;
    spec.crowd.seed = 7;
    spec.output = "assets/models/crowd.vat";
    std::unique_ptr<VatBakeSession> session = bake(spec);
    ASSERT_TRUE(session->result());
    VatWriteResult written;
    std::string error;
    ASSERT_TRUE(session->write(written, error)) << error;
    const Entity placed = written.entity;
    ASSERT_NE(placed, NULL_ENTITY);
    EXPECT_EQ(scene->getEntityName(placed), "Arm (VAT)");
    EXPECT_EQ(scene->getComponent<Transform>(placed).position, Vector3(4.0f, 1.0f, 0.0f));
    const InstancedMeshComponent* instanced = scene->findComponent<InstancedMeshComponent>(placed);
    ASSERT_TRUE(instanced);
    ASSERT_EQ(instanced->instances.size(), 150u) << "every instance placed (no minimum spacing)";
    EXPECT_GE(instanced->maxInstances, 150u);
    std::vector<Vector3> positions;
    for (const InstanceData& instance : instanced->instances) {
        EXPECT_LE(std::fabs(instance.position.x), 3.0f + 1e-4f);
        EXPECT_LE(std::fabs(instance.position.z), 3.0f + 1e-4f);
        EXPECT_NEAR(instance.position.y, 0.0f, 1e-5f) << "on the GameObject's ground plane";
        positions.push_back(instance.position);
    }
    // the same layout as VatCrowd's for these settings (seeded, deterministic)
    CrowdSettings settings;
    settings.count = 150;
    settings.width = settings.depth = 6.0f;
    settings.minSpacing = 0.0f;
    settings.seed = 7;
    const CrowdLayout layout = layoutCrowd(settings, nullptr);
    ASSERT_EQ(layout.instances.size(), positions.size());
    for (size_t i = 0; i < positions.size(); i++) EXPECT_EQ(layout.instances[i].position, positions[i]);
    const VertexAnimatorComponent* animator = scene->findComponent<VertexAnimatorComponent>(placed);
    ASSERT_TRUE(animator);
    EXPECT_EQ(animator->animation, "models/crowd.vat");
    EXPECT_TRUE(animator->randomStart) << "out of step";
    EXPECT_EQ(project()->getScene(sceneId)->selectedEntities, std::vector<Entity>{placed});

    // one undo step: the crowd GameObject and the files
    ASSERT_TRUE(history(sceneId)->undo());
    EXPECT_FALSE(scene->isEntityCreated(placed) && scene->findComponent<InstancedMeshComponent>(placed));
    EXPECT_FALSE(fs::exists(root() / written.vat));
    ASSERT_TRUE(history(sceneId)->redo());
    ASSERT_TRUE(scene->findComponent<InstancedMeshComponent>(placed));
    EXPECT_EQ(scene->getComponent<InstancedMeshComponent>(placed).instances.size(), 150u);
    EXPECT_TRUE(fs::exists(root() / written.vat));
    ASSERT_TRUE(history(sceneId)->undo());

    // a crowd from a file source goes to the selected scene, at its origin
    VatBakeSpec fileSpec = modelSpec("rigid_arm.gltf", {"Swing"});
    fileSpec.place = VatPlace::Crowd;
    fileSpec.crowd.count = 3;
    fileSpec.output = "assets/models/crowd2.vat";
    std::unique_ptr<VatBakeSession> fileSession = bake(fileSpec);
    VatWriteResult fileWritten;
    ASSERT_TRUE(fileSession->write(fileWritten, error)) << error;
    ASSERT_NE(fileWritten.entity, NULL_ENTITY);
    EXPECT_EQ(fileWritten.sceneId, project()->getSelectedSceneId());
    ASSERT_TRUE(scene->findComponent<InstancedMeshComponent>(fileWritten.entity));
    EXPECT_EQ(scene->getComponent<InstancedMeshComponent>(fileWritten.entity).instances.size(), 3u);
    // the crowd builder refuses settings it can't use (the check refuses them first)
    std::vector<Command*> commands;
    VatCrowd bad;
    bad.count = MAX_CROWD + 1;
    EXPECT_FALSE(crowdPlacement(bad)(project(), sceneId, fileWritten.entity, commands, error));
    EXPECT_TRUE(commands.empty());
    EXPECT_NE(error.find("crowd count"), std::string::npos) << error;
}

TEST_F(VatBaker_, StalenessAndRebakeKeepEdits) {
    VatBakeSpec spec = modelSpec("hero.gltf", {"Walk", "Idle"});
    spec.sockets = {"Hero/mixamorig:Hips"};
    std::unique_ptr<VatBakeSession> session = bake(spec);
    ASSERT_EQ(session->result()->sockets.sockets, 1);
    VatWriteResult written;
    std::string error;
    ASSERT_TRUE(session->write(written, error)) << error;
    EXPECT_TRUE(checkVatStaleness(project(), written.vat).empty()) << kinds(checkVatStaleness(project(), written.vat));

    // hand edits: rename Walk, its speed, wrap, interpolate and an event; rename the socket
    const fs::path vatFile = root() / written.vat;
    jasperengine::vat::VatParseResult parsed = jasperengine::vat::parseVat(readAll(vatFile));
    ASSERT_TRUE(parsed.ok());
    VatData edited = *parsed.data;
    edited.clips[0].name = "Stroll";
    edited.clips[0].speed = 1.5f;
    edited.clips[0].interpolate = false;
    edited.clips[1].wrap = ClipWrapMode::PingPong;
    ClipEventData event;
    event.time = 0.5f;
    event.function = "onStep";
    edited.clips[0].events.push_back(event);
    edited.sockets[0].name = "Pelvis";
    writeAll(vatFile, jasperengine::vat::writeVat(edited));

    // the source changes: stale
    writeAll(root() / "assets/models/hero.gltf", readAll(root() / "assets/models/hero.gltf") + "\n");
    std::vector<VatIssue> stale = checkVatStaleness(project(), written.vat);
    EXPECT_TRUE(hasKind(stale, "vat-source-changed")) << kinds(stale);
    MeshSystem::invalidateModelFile((root() / "assets/models/hero.gltf").string());

    VatWriteResult rebaked;
    ASSERT_TRUE(rebakeVat(project(), written.vat, rebaked, error)) << error;
    EXPECT_EQ(rebaked.vat, written.vat);
    EXPECT_EQ(rebaked.guid, written.guid);
    parsed = jasperengine::vat::parseVat(readAll(vatFile));
    ASSERT_TRUE(parsed.ok()) << parsed.error;
    const VatData& now = *parsed.data;
    ASSERT_EQ(now.clips.size(), 2u);
    EXPECT_EQ(now.clips[0].name, "Stroll");
    EXPECT_FLOAT_EQ(now.clips[0].speed, 1.5f);
    EXPECT_FALSE(now.clips[0].interpolate);
    ASSERT_EQ(now.clips[0].events.size(), 1u);
    EXPECT_EQ(now.clips[0].events[0].function, "onStep");
    EXPECT_EQ(now.clips[1].name, "Idle");
    EXPECT_EQ(now.clips[1].wrap, ClipWrapMode::PingPong);
    EXPECT_EQ(now.clips[1].frames, 61) << "a ping-pong clip keeps its end frame";
    ASSERT_EQ(now.sockets.size(), 1u);
    EXPECT_EQ(now.sockets[0].name, "Pelvis");
    EXPECT_TRUE(checkVatStaleness(project(), written.vat).empty()) << kinds(checkVatStaleness(project(), written.vat));

    // a clip removed by hand stays removed
    edited = now;
    edited.clips.erase(edited.clips.begin() + 1);
    writeAll(vatFile, jasperengine::vat::writeVat(edited));
    ASSERT_TRUE(rebakeVat(project(), written.vat, rebaked, error)) << error;
    parsed = jasperengine::vat::parseVat(readAll(vatFile));
    ASSERT_TRUE(parsed.ok());
    ASSERT_EQ(parsed.data->clips.size(), 1u);
    EXPECT_EQ(parsed.data->clips[0].name, "Stroll");

    // missing source, broken mesh
    fs::remove(root() / "assets/models/hero.gltf");
    stale = checkVatStaleness(project(), written.vat);
    EXPECT_TRUE(hasKind(stale, "vat-source-missing")) << kinds(stale);
    writeAll(root() / written.mesh, "not a glb");
    stale = checkVatStaleness(project(), written.vat);
    EXPECT_TRUE(hasKind(stale, "vat-mesh-mismatch")) << kinds(stale);
    // a generated field changed by hand
    std::string text = readAll(vatFile);
    const size_t at = text.find("\"layout_hash\": \"");
    ASSERT_NE(at, std::string::npos);
    text[at + 16] = text[at + 16] == 'a' ? 'b' : 'a';
    writeAll(vatFile, text);
    stale = checkVatStaleness(project(), written.vat);
    EXPECT_TRUE(hasKind(stale, "vat-layout-mismatch") || hasKind(stale, "vat-mesh-mismatch")) << kinds(stale);
}

TEST_F(VatBaker_, CancelMidRunLeavesTheSceneAtRestAndWritesNothing) {
    const uint32_t sceneId = newScene();
    const Entity hero = loadModel(sceneId, "models/hero.gltf");
    Scene* scene = sceneOf(sceneId);
    AnimationPlayerComponent player;
    AnimationPlayerClip entry;
    entry.clip = "models/hero.gltf#Walk";
    player.clips.push_back(entry);
    scene->addComponent<AnimationPlayerComponent>(hero, player);
    // every Transform before
    std::map<Entity, Vector3> positions;
    std::map<Entity, Quaternion> rotations;
    auto transforms = scene->getComponentArray<Transform>();
    for (size_t i = 0; i < transforms->size(); i++) {
        positions[transforms->getEntity(i)] = transforms->getComponentFromIndex(i).position;
        rotations[transforms->getEntity(i)] = transforms->getComponentFromIndex(i).rotation;
    }
    VatBakeSpec spec;
    spec.source.kind = VatSourceKind::Entity;
    spec.source.sceneId = sceneId;
    spec.source.path = EntityPath::of(*project()->getScene(sceneId), hero);
    spec.output = "assets/models/cancelled.vat";
    spec.fps = 120.0f;
    VatBakeSession session(project());
    session.setSpec(spec);
    ASSERT_TRUE(session.start()) << session.error();
    EXPECT_FALSE(session.advance(0.001)) << "a slice of one sample";
    EXPECT_EQ(session.state(), VatBakeState::Posing);
    EXPECT_NE(session.progress().stage.find("Posing clip 1 of "), std::string::npos) << session.progress().stage;
    EXPECT_GT(session.progress().fraction, 0.0f);
    session.cancel();
    EXPECT_EQ(session.state(), VatBakeState::Cancelled);
    for (size_t i = 0; i < transforms->size(); i++) {
        const Entity entity = transforms->getEntity(i);
        EXPECT_EQ(transforms->getComponentFromIndex(i).position, positions[entity]) << scene->getEntityName(entity);
        EXPECT_EQ(transforms->getComponentFromIndex(i).rotation, rotations[entity]) << scene->getEntityName(entity);
    }
    VatWriteResult written;
    std::string error;
    EXPECT_FALSE(session.write(written, error));
    EXPECT_FALSE(fs::exists(root() / "assets/models/cancelled.vat"));

    // an edit between two slices fails the bake
    VatBakeSession edited(project());
    edited.setSpec(spec);
    ASSERT_TRUE(edited.start());
    EXPECT_FALSE(edited.advance(0.001));
    scene->getComponent<Transform>(hero).position = Vector3(5, 0, 0);
    EXPECT_TRUE(edited.advance(0.001));
    EXPECT_EQ(edited.state(), VatBakeState::Failed);
    EXPECT_NE(edited.error().find("changed during the bake"), std::string::npos) << edited.error();
}

TEST_F(VatBaker_, LatticeTracksBakeAsVertices) {
    const uint32_t sceneId = newScene();
    Scene* scene = sceneOf(sceneId);
    // a GameObject holding a box and a lattice around it, with a clip moving one lattice point
    CreateEntityCmd* rootCmd = new CreateEntityCmd(project(), sceneId, "Jelly", EntityCreationType::GAMEOBJECT);
    rootCmd->setQuiet(true);
    history(sceneId)->addCommandNoMerge(rootCmd);
    const Entity jelly = rootCmd->getEntity();
    CreateEntityCmd* boxCmd = new CreateEntityCmd(project(), sceneId, "Box", EntityCreationType::BOX, jelly);
    boxCmd->setQuiet(true);
    history(sceneId)->addCommandNoMerge(boxCmd);
    const Entity box = boxCmd->getEntity();
    const Entity cage = scene->createUserEntity();
    scene->setEntityName(cage, "Cage");
    scene->addComponent<Transform>(cage, {});
    scene->addEntityChild(jelly, cage, false);
    scene->getComponent<Transform>(cage).scale = Vector3(1.5f, 1.5f, 1.5f);
    LatticeComponent lattice;
    scene->addComponent<LatticeComponent>(cage, lattice);
    LatticeModifierComponent modifier;
    LatticeBinding binding;
    binding.lattice = cage;
    modifier.bindings.push_back(binding);
    scene->addComponent<LatticeModifierComponent>(box, modifier);
    project()->getScene(sceneId)->entities.push_back(cage);

    ClipData clip;
    clip.guid = "fedcba9876543210fedcba9876543210";
    clip.wrap = ClipWrapMode::Loop;
    ClipTrackData track;
    track.path = "Cage";
    track.channel = ClipChannel::Lattice;
    track.points = {26};
    for (int i = 0; i < 3; i++) {
        ClipKey key;
        key.time = 0.5f * (float)i;
        key.value = {0.0f, i == 1 ? 0.5f : 0.0f, 0.0f};
        track.keys.push_back(key);
    }
    clip.tracks.push_back(track);
    anim::normalize(clip);
    writeAll(root() / "assets/anim/jelly.anim", anim::writeClip(clip));

    VatBakeSpec spec;
    spec.source.kind = VatSourceKind::Anim;
    spec.source.sceneId = sceneId;
    spec.source.path = EntityPath::of(*project()->getScene(sceneId), jelly);
    spec.source.anims = {"assets/anim/jelly.anim"};
    VatBakeSession session(project());
    session.setSpec(spec);
    const std::vector<VatIssue> issues = session.check();
    ASSERT_FALSE(hasErrors(issues)) << kinds(issues);
    EXPECT_EQ(session.estimate().mode, VatMode::Vertices) << session.estimate().modeReason;
    ASSERT_TRUE(session.run()) << session.error() << kinds(session.failureIssues());
    const VatBakeResult* result = session.result();
    EXPECT_EQ(result->data.mode, VatMode::Vertices);
    EXPECT_NE(result->report.modeReason.find("lattice"), std::string::npos) << result->report.modeReason;
    // the corner near point 26 moved at the half, not at the start
    const std::vector<Vector3> start = decodeFrame(*result, 0);
    const std::vector<Vector3> half = decodeFrame(*result, result->data.clips[0].frames / 2);
    float moved = 0.0f;
    for (size_t v = 0; v < start.size(); v++) moved = std::max(moved, distance(start[v], half[v]));
    EXPECT_GT(moved, 0.05f);
    // the scene is back at rest
    EXPECT_EQ(scene->getComponent<LatticeComponent>(cage).offsets[26], Vector3(0, 0, 0));
}

TEST_F(VatBaker_, SceneSourcesConvertTheirAnimationsAndReportMaterials) {
    const uint32_t sceneId = newScene();
    const Entity arm = loadModel(sceneId, "models/rigid_arm.gltf", "Arm");
    Scene* scene = sceneOf(sceneId);
    // the model's entity Animations (Swing, Stretch) are the GameObject's clips
    VatBakeSpec spec;
    spec.source.kind = VatSourceKind::Entity;
    spec.source.sceneId = sceneId;
    spec.source.path = EntityPath::of(*project()->getScene(sceneId), arm);
    spec.clips = {"Swing"};
    // one part draws with a library shader, the other with a project fork
    int part = 0;
    for (const auto& node : scene->getComponent<ModelComponent>(arm).meshNodesMapping) {
        MeshComponent* mesh = scene->findComponent<MeshComponent>(node.second);
        if (!mesh || mesh->numSubmeshes == 0) continue;
        mesh->submeshes[0].material.shader = part++ == 0 ? "library:toon" : "shaders/old_fork.shader";
    }
    ASSERT_EQ(part, 2);
    {
        VatBakeSession session(project());
        session.setSpec(spec);
        const std::vector<VatIssue> issues = session.check();
        EXPECT_FALSE(hasErrors(issues)) << kinds(issues);
        EXPECT_TRUE(hasKind(issues, "material_simplified")) << kinds(issues);
        EXPECT_TRUE(hasKind(issues, "materials_not_vat_ready")) << kinds(issues);
        bool swing = false, stretch = false;
        for (const VatSourceClip& clip : session.sourceClips()) {
            swing |= clip.name == "Swing";
            stretch |= clip.name == "Stretch";
        }
        EXPECT_TRUE(swing && stretch) << "entity Animations become clips";
        ASSERT_TRUE(session.run()) << session.error();
        EXPECT_EQ(session.result()->data.mode, VatMode::Bones) << session.result()->report.modeReason;
        EXPECT_TRUE(hasKind(session.result()->report.warnings, "materials_not_vat_ready"));
        EXPECT_EQ(session.result()->data.recipe.sources[0].kind, "entity");
        EXPECT_EQ(session.result()->data.recipe.sources[0].clips, std::vector<std::string>{"Swing"});
    }
    // a playing scene refuses
    project()->getScene(sceneId)->playState = ScenePlayState::PLAYING;
    {
        VatBakeSession session(project());
        session.setSpec(spec);
        bool error = false;
        EXPECT_TRUE(hasKind(session.check(), "scene_playing", &error));
        EXPECT_TRUE(error);
    }
    project()->getScene(sceneId)->playState = ScenePlayState::STOPPED;

    // a GameObject that moves but draws nothing
    ClipData move;
    move.guid = "00112233445566778899aabbccddeeff";
    move.wrap = ClipWrapMode::Loop;
    ClipTrackData track;
    track.path = "";
    track.channel = ClipChannel::Position;
    for (int i = 0; i < 2; i++) {
        ClipKey key;
        key.time = (float)i;
        key.value = {(float)i, 0, 0};
        track.keys.push_back(key);
    }
    move.tracks.push_back(track);
    anim::normalize(move);
    writeAll(root() / "assets/anim/move.anim", anim::writeClip(move));
    CreateEntityCmd* create = new CreateEntityCmd(project(), sceneId, "Mover", EntityCreationType::GAMEOBJECT);
    create->setQuiet(true);
    history(sceneId)->addCommandNoMerge(create);
    VatBakeSpec mover;
    mover.source.kind = VatSourceKind::Anim;
    mover.source.sceneId = sceneId;
    mover.source.path = EntityPath::of(*project()->getScene(sceneId), create->getEntity());
    mover.source.anims = {"assets/anim/move.anim"};
    VatBakeSession nothing(project());
    nothing.setSpec(mover);
    bool error = false;
    EXPECT_TRUE(hasKind(nothing.check(), "no_mesh", &error)) << kinds(nothing.check());
    EXPECT_TRUE(error);
}

TEST_F(VatBaker_, PoseAnimationsBakeThroughTheirClip) {
    auto pose = [](const std::string& name, float raise) {
        return "{\"name\": \"" + name + "\", \"hold\": 0.3, \"grounded\": true, \"lift\": 0, \"transition\": null, "
               "\"offset\": {\"left\": 0, \"forward\": 0}, \"angles\": {\"left_upper_arm.raise\": " + std::to_string(raise) + "}}";
    };
    writeAll(root() / "assets/anim/raise.poseanim",
             "{\"format\": \"jasperengine.poseanim\", \"version\": 1, \"preview_model\": \"models/hero.gltf\", "
             "\"frame_rate\": 30, \"loop\": true, "
             "\"feel\": {\"transition\": {\"curve\": \"spring\", \"duration\": 0.2, \"overshoot\": 0.25, \"anticipation\": 0.06}, "
             "\"step_rate\": 0, \"overlap\": 0.05, \"looseness\": 0.2, \"weights\": {\"spine\": 1, \"head\": 1, \"arms\": 1, \"legs\": 1}}, "
             "\"humanize\": {\"enabled\": false, \"joint_limits\": true, \"moving_holds\": 0.08, \"inbetweens\": 0.4, "
             "\"life\": 0.6, \"seed\": 1, \"head_follow\": 0.5, \"torso_follow\": 0.4, \"springiness\": 0.5}, "
             "\"poses\": [" + pose("Rest", 0) + ", " + pose("Raise", 70) + "]}\n");
    VatBakeSpec spec;
    spec.source.kind = VatSourceKind::Pose;
    spec.source.path = "assets/models/hero.gltf";
    spec.source.anims = {"assets/anim/raise.poseanim"};
    std::unique_ptr<VatBakeSession> session = bake(spec);
    const VatBakeResult* result = session->result();
    ASSERT_TRUE(result);
    ASSERT_EQ(result->data.clips.size(), 1u);
    EXPECT_EQ(result->data.clips[0].name, "raise");
    EXPECT_EQ(result->data.clips[0].wrap, ClipWrapMode::Loop);
    EXPECT_EQ(result->data.recipe.sources[0].kind, "pose");
    bool hashed = false;
    for (const auto& hash : result->data.recipe.sources[0].hashes) hashed |= hash.first == "assets/anim/raise.poseanim";
    EXPECT_TRUE(hashed);
    // the arm rises: the frames differ
    const std::vector<Vector3> first = decodeFrame(*result, 0);
    float moved = 0.0f;
    for (int frame = 1; frame < result->data.clips[0].frames; frame++) {
        const std::vector<Vector3> later = decodeFrame(*result, frame);
        for (size_t v = 0; v < first.size(); v++) moved = std::max(moved, distance(first[v], later[v]));
    }
    EXPECT_GT(moved, 0.2f) << "the left arm raised 70 degrees";
    // its default output is next to the pose animation
    std::string vatPath;
    bool rewrite = true;
    std::string error;
    ASSERT_TRUE(session->plannedOutput(vatPath, rewrite, error)) << error;
    EXPECT_EQ(vatPath, "assets/anim/raise.vat");
    EXPECT_FALSE(rewrite);
}

// ==================================================================== review fixes (FB)

namespace {

    // the images of a glb (MeshSystem::parseGLTFFile: kept encoded) and which image each
    // material's base colour samples (-1: none)
    struct GlbImages {
        std::vector<std::string> images;
        std::vector<int> baseColorImage;
        std::vector<std::string> materialNames;
        std::string json;
    };

    GlbImages glbImages(const fs::path& file) {
        GlbImages out;
        tinygltf::Model model;
        std::string error;
        EXPECT_TRUE(MeshSystem::parseGLTFFile(file.string(), model, &error)) << error;
        for (const tinygltf::Image& image : model.images) {
            EXPECT_GE(image.bufferView, 0) << "inside the glb";
            EXPECT_TRUE(image.uri.empty()) << image.uri;
            out.images.push_back(std::string(image.image.begin(), image.image.end()));
        }
        for (const tinygltf::Material& material : model.materials) {
            const int texture = material.pbrMetallicRoughness.baseColorTexture.index;
            out.baseColorImage.push_back(texture >= 0 && (size_t)texture < model.textures.size() ? model.textures[(size_t)texture].source : -1);
            out.materialNames.push_back(material.name);
        }
        const std::string bytes = readAll(file);
        // the JSON chunk: after the 12-byte header and the chunk's 8-byte header
        if (bytes.size() > 20) {
            uint32_t length = 0;
            std::memcpy(&length, bytes.data() + 12, 4);
            out.json = bytes.substr(20, std::min<size_t>(length, bytes.size() - 20));
        }
        return out;
    }

    // every submesh material at or under a model's entity
    std::vector<const Material*> materialsOf(Scene* scene, Entity model) {
        std::vector<const Material*> out;
        std::vector<Entity> meshes = {model};
        for (const auto& node : scene->getComponent<ModelComponent>(model).meshNodesMapping) meshes.push_back(node.second);
        for (Entity entity : meshes) {
            if (const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)) {
                for (unsigned int i = 0; i < mesh->numSubmeshes; i++) out.push_back(&mesh->submeshes[i].material);
            }
        }
        return out;
    }

    // a PNG's width (IHDR)
    unsigned int pngWidth(const std::string& png) {
        if (png.size() < 24) return 0;
        const unsigned char* b = reinterpret_cast<const unsigned char*>(png.data()) + 16;
        return (unsigned int)b[0] << 24 | (unsigned int)b[1] << 16 | (unsigned int)b[2] << 8 | (unsigned int)b[3];
    }

    // the encoded first image of a fixture model
    std::string sourceImage(const std::string& model) {
        tinygltf::Model gltf;
        std::string error;
        EXPECT_TRUE(MeshSystem::parseGLTFFile((root() / "assets/models" / model).string(), gltf, &error)) << error;
        if (gltf.images.empty()) return "";
        return std::string(gltf.images[0].image.begin(), gltf.images[0].image.end());
    }

    unsigned int sourceImageWidth(const std::string& model) { return pngWidth(sourceImage(model)); }

    std::string base64(const std::string& bytes) {
        static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        size_t i = 0;
        for (; i + 2 < bytes.size(); i += 3) {
            const uint32_t v = (uint32_t)(unsigned char)bytes[i] << 16 | (uint32_t)(unsigned char)bytes[i + 1] << 8 | (unsigned char)bytes[i + 2];
            out += alphabet[v >> 18 & 63]; out += alphabet[v >> 12 & 63]; out += alphabet[v >> 6 & 63]; out += alphabet[v & 63];
        }
        if (i + 1 == bytes.size()) {
            const uint32_t v = (uint32_t)(unsigned char)bytes[i] << 16;
            out += alphabet[v >> 18 & 63]; out += alphabet[v >> 12 & 63]; out += "==";
        } else if (i + 2 == bytes.size()) {
            const uint32_t v = (uint32_t)(unsigned char)bytes[i] << 16 | (uint32_t)(unsigned char)bytes[i + 1] << 8;
            out += alphabet[v >> 18 & 63]; out += alphabet[v >> 12 & 63]; out += alphabet[v >> 6 & 63]; out += '=';
        }
        return out;
    }

    // the arm as a GameObject playing Swing (an Animation Player state)
    Entity armWithPlayer(uint32_t sceneId, const std::string& name = "Arm") {
        const Entity arm = loadModel(sceneId, "models/rigid_arm.gltf", name);
        Scene* scene = sceneOf(sceneId);
        AnimationPlayerComponent player;
        AnimationPlayerClip entry;
        entry.clip = "models/rigid_arm.gltf#Swing";
        player.clips.push_back(entry);
        scene->addComponent<AnimationPlayerComponent>(arm, player);
        cpuskin::updateTransforms(scene);
        return arm;
    }

    VatBakeSpec entitySpec(uint32_t sceneId, Entity entity) {
        VatBakeSpec spec;
        spec.source.kind = VatSourceKind::Entity;
        spec.source.sceneId = sceneId;
        spec.source.path = EntityPath::of(*project()->getScene(sceneId), entity);
        return spec;
    }

    // the scene's mesh vertex positions of a bake (its rest)
    std::vector<float> restOf(const VatBakeSpec& spec) {
        std::unique_ptr<VatBakeSession> session = bake(spec);
        return session->result() ? session->result()->mesh.positions : std::vector<float>();
    }

    float largestDifference(const std::vector<float>& a, const std::vector<float>& b) {
        if (a.size() != b.size()) return 1e9f;
        float worst = 0.0f;
        for (size_t i = 0; i < a.size(); i++) worst = std::max(worst, std::fabs(a[i] - b[i]));
        return worst;
    }

    // a command that counts its deletions (and fails when told to)
    struct CountedCmd : public Command {
        std::map<int, int>* deleted;
        int id;
        bool ok;
        CountedCmd(std::map<int, int>* deleted, int id, bool ok) : deleted(deleted), id(id), ok(ok) {}
        ~CountedCmd() override { (*deleted)[id]++; }
        bool execute() override { return ok; }
        void undo() override {}
        std::string describe() const override { return "Counted " + std::to_string(id); }
        const char* typeName() const override { return "CountedCmd"; }
    };

    // two open scenes (createNewScene closes the one before): the second selected
    uint32_t secondScene(uint32_t& first, SceneType type = SceneType::SCENE_3D) {
        first = newScene();
        std::string error;
        const uint32_t second = project()->createSceneNow("Second", type, {}, false, UnsavedChoice::Discard, &error);
        EXPECT_NE(second, NULL_PROJECT_SCENE) << error;
        return second;
    }

    bool hashed(const VatData& data, const std::string& file) {
        for (const auto& hash : data.recipe.sources[0].hashes) {
            if (hash.first == file) return true;
        }
        return false;
    }

    // an .anim moving one node of a hierarchy along x over `length` seconds
    void writeMoveClip(const fs::path& file, const std::string& path, float length) {
        ClipData move;
        move.guid = "0011223344556677889900aabbccddee";
        move.wrap = ClipWrapMode::Loop;
        ClipTrackData track;
        track.path = path;
        track.channel = ClipChannel::Position;
        for (int i = 0; i < 2; i++) {
            ClipKey key;
            key.time = (float)i * length;
            key.value = {(float)i * 0.5f, 0, 0};
            track.keys.push_back(key);
        }
        move.tracks.push_back(track);
        anim::normalize(move);
        writeAll(file, anim::writeClip(move));
    }

}

TEST_F(VatBaker_, EmbeddedTexturesAreStoredInsideTheGlb) {
    std::unique_ptr<VatBakeSession> session = bake(modelSpec("morph_blob.gltf"));
    const VatBakeResult* result = session->result();
    ASSERT_TRUE(result);
    ASSERT_EQ(result->embeddedImages.size(), 1u);
    EXPECT_EQ(result->embeddedImages[0].name, "Checker");
    EXPECT_EQ(result->embeddedImages[0].mimeType, "image/png");
    EXPECT_EQ(result->embeddedImages[0].bytes.compare(0, 4, "\x89PNG"), 0);
    ASSERT_EQ(result->mesh.materials.size(), 1u);
    EXPECT_EQ(result->mesh.materials[0].baseColorTexture, "embedded:0");
    EXPECT_EQ(result->mesh.materials[0].name, "Green") << "the source glTF material's name";
    EXPECT_FALSE(result->mesh.uv0.empty());
    // the preview as files outside the assets (a Model can load it), gone with the session
    std::string previewVat, previewMesh, previewError;
    ASSERT_TRUE(session->writePreviewFiles(previewVat, previewMesh, previewError)) << previewError;
    EXPECT_EQ(previewVat, result->previewKey);
    EXPECT_TRUE(fs::exists(previewMesh));
    size_t previewFiles = 0;
    for (const auto& entry : fs::directory_iterator(fs::path(previewMesh).parent_path())) previewFiles += entry.is_regular_file();
    EXPECT_EQ(previewFiles, 2u) << "the .vat and its glb, no image file";
    EXPECT_TRUE(jasperengine::vat::parseVat(readAll(previewVat)).ok());
    EXPECT_EQ(fs::path(previewVat).lexically_relative(root()).generic_string().compare(0, 13, ".jasperengine"), 0);
    VatWriteResult written;
    std::string error;
    ASSERT_TRUE(session->write(written, error)) << error;
    EXPECT_EQ(written.files, (std::vector<std::string>{"assets/models/morph_blob.vat", "assets/models/morph_blob.vat.glb"}));
    EXPECT_FALSE(fs::exists(root() / "assets/models/morph_blob_Checker.png"));
    const GlbImages glb = glbImages(root() / written.mesh);
    ASSERT_EQ(glb.images.size(), 1u);
    EXPECT_EQ(glb.images[0], result->embeddedImages[0].bytes) << "the image is a buffer view of the glb";
    // the engine loads the glb's material with its texture decoded
    const uint32_t sceneId = newScene();
    const Entity loaded = loadModel(sceneId, "models/morph_blob.vat.glb", "Loaded");
    const std::vector<const Material*> materials = materialsOf(sceneOf(sceneId), loaded);
    ASSERT_EQ(materials.size(), 1u);
    const Texture& texture = materials[0]->baseColorTexture;
    EXPECT_FALSE(texture.empty());
    EXPECT_NE(texture.getId().find("|gltf-image|"), std::string::npos) << texture.getId();
    std::shared_ptr<std::array<TextureData, 6>> data = TextureDataPool::get(texture.getId());
    ASSERT_TRUE(data) << "decoded into the texture pool";
    EXPECT_EQ(data->at(0).getWidth(), sourceImageWidth("morph_blob.gltf"));
    ASSERT_TRUE(project()->getProjectCommandHistory()->undo());
    EXPECT_FALSE(fs::exists(root() / written.mesh));
    session.reset();
    EXPECT_FALSE(fs::exists(previewMesh)) << "the preview files go with the session";
}

// spec 4.4 / review 1: a GameObject source closed or deleted after the bake (or while it encodes)
// fails cleanly with invalid_source "bake again", in every place mode
TEST_F(VatBaker_, AClosedSceneOrDeletedSourceIsRefusedNotACrash) {
    for (VatPlace place : {VatPlace::Beside, VatPlace::Replace, VatPlace::Crowd}) {
        SCOPED_TRACE(placeName(place));
        // the scene closed between the bake and write()
        {
            uint32_t keep = 0;
            const uint32_t sceneId = secondScene(keep);
            VatBakeSpec spec = entitySpec(sceneId, armWithPlayer(sceneId));
            spec.place = place;
            spec.crowd.count = 3;
            std::unique_ptr<VatBakeSession> session = bake(spec);
            ASSERT_TRUE(session->result());
            std::string error;
            ASSERT_TRUE(project()->closeSceneNow(sceneId, UnsavedChoice::Discard, &error)) << error;
            VatWriteResult written;
            VatIssue issue;
            EXPECT_FALSE(session->write(written, error, &issue));
            EXPECT_EQ(issue.kind, "invalid_source");
            EXPECT_EQ(issue.fix, "bake again");
            EXPECT_NE(error.find("scene was closed"), std::string::npos) << error;
            EXPECT_FALSE(fs::exists(root() / "assets/models/Arm.vat"));
        }
        // the source GameObject deleted between the bake and write()
        {
            const uint32_t sceneId = newScene();
            const Entity arm = armWithPlayer(sceneId);
            VatBakeSpec spec = entitySpec(sceneId, arm);
            spec.place = place;
            spec.crowd.count = 3;
            std::unique_ptr<VatBakeSession> session = bake(spec);
            ASSERT_TRUE(session->result());
            sceneOf(sceneId)->destroyEntity(arm);
            VatWriteResult written;
            VatIssue issue;
            std::string error;
            EXPECT_FALSE(session->write(written, error, &issue));
            EXPECT_EQ(issue.kind, "invalid_source");
            EXPECT_NE(error.find("GameObject was deleted"), std::string::npos) << error;
            EXPECT_NE(error.find("bake again"), std::string::npos) << error;
        }
    }
    // files only need nothing of the scene: written after it closed
    {
        uint32_t keep = 0;
        const uint32_t sceneId = secondScene(keep);
        std::unique_ptr<VatBakeSession> session = bake(entitySpec(sceneId, armWithPlayer(sceneId)));
        ASSERT_TRUE(session->result());
        std::string error;
        ASSERT_TRUE(project()->closeSceneNow(sceneId, UnsavedChoice::Discard, &error)) << error;
        VatWriteResult written;
        EXPECT_TRUE(session->write(written, error)) << error;
        EXPECT_TRUE(fs::exists(root() / written.vat));
    }
    // closed while the worker encodes: the bake fails (nothing kept to write)
    {
        uint32_t keep = 0;
        const uint32_t sceneId = secondScene(keep);
        VatBakeSpec spec = entitySpec(sceneId, armWithPlayer(sceneId));
        spec.place = VatPlace::Beside;
        VatBakeSession session(project());
        session.setSpec(spec);
        ASSERT_TRUE(session.start()) << session.error();
        while (session.state() == VatBakeState::Posing) session.advance(1000.0);
        ASSERT_EQ(session.state(), VatBakeState::Encoding);
        std::string error;
        ASSERT_TRUE(project()->closeSceneNow(sceneId, UnsavedChoice::Discard, &error)) << error;
        while (!session.advance(1000.0)) {}
        EXPECT_EQ(session.state(), VatBakeState::Failed);
        ASSERT_FALSE(session.failureIssues().empty());
        EXPECT_EQ(session.failureIssues().back().kind, "invalid_source");
        EXPECT_EQ(session.failureIssues().back().fix, "bake again");
        VatWriteResult written;
        EXPECT_FALSE(session.write(written, error));
    }
    // deleted between two posing slices
    {
        const uint32_t sceneId = newScene();
        const Entity arm = armWithPlayer(sceneId);
        VatBakeSpec spec = entitySpec(sceneId, arm);
        spec.fps = 120.0f;
        VatBakeSession session(project());
        session.setSpec(spec);
        ASSERT_TRUE(session.start());
        EXPECT_FALSE(session.advance(0.001));
        sceneOf(sceneId)->destroyEntity(arm);
        EXPECT_TRUE(session.advance(0.001));
        EXPECT_EQ(session.state(), VatBakeState::Failed);
        ASSERT_FALSE(session.failureIssues().empty());
        EXPECT_EQ(session.failureIssues().back().kind, "invalid_source");
    }
}

// review 2: a scene command that fails is deleted once (CreateVatCmd owns it)
TEST_F(VatBaker_, CreateVatCmdDeletesAFailedSceneCommandOnce) {
    const uint32_t sceneId = newScene();
    const fs::path vat = root() / "assets/models/cmd.vat";
    // a Model that can't load (a missing FBX: prepareModelFile fails)
    {
        VatPlacement placement;
        placement.mode = VatPlacement::Mode::Beside;
        placement.name = "Missing (VAT)";
        placement.model = "models/missing.fbx";
        placement.animation = "models/cmd.vat";
        auto* command = new CreateVatCmd(project(), sceneId, {{vat, "{}"}}, placement);
        auto failure = std::make_shared<std::string>();
        command->reportFailureTo(failure);
        EXPECT_FALSE(history(sceneId)->addCommand(command));
        EXPECT_NE(failure->find("could not load 'models/missing.fbx'"), std::string::npos) << *failure;
        EXPECT_FALSE(fs::exists(vat)) << "the files are put back";
    }
    // a crowd command refused: it and the ones after it deleted once each
    {
        std::map<int, int> deleted;
        VatPlacement placement;
        placement.mode = VatPlacement::Mode::Crowd;
        placement.name = "Crowd (VAT)";
        placement.model = "models/rigid_arm.gltf";
        placement.animation = "models/cmd.vat";
        placement.crowd = [&deleted](Project*, uint32_t, Entity, std::vector<Command*>& commands, std::string&) {
            commands.push_back(new CountedCmd(&deleted, 0, true));
            commands.push_back(new CountedCmd(&deleted, 1, false));
            commands.push_back(new CountedCmd(&deleted, 2, true));
            return true;
        };
        auto* command = new CreateVatCmd(project(), sceneId, {{vat, "{}"}}, placement);
        auto failure = std::make_shared<std::string>();
        command->reportFailureTo(failure);
        EXPECT_FALSE(history(sceneId)->addCommand(command));
        EXPECT_NE(failure->find("refused a crowd change (command 2)"), std::string::npos) << *failure;
        EXPECT_EQ(deleted, (std::map<int, int>{{0, 1}, {1, 1}, {2, 1}}));
        EXPECT_FALSE(fs::exists(vat));
    }
}

// review 3: another clip of the same model is another asset; the same clips rewrite theirs
TEST_F(VatBaker_, AnotherClipOfTheSameModelTakesAFreeName) {
    std::string error;
    VatWriteResult swing;
    ASSERT_TRUE(bake(modelSpec("rigid_arm.gltf", {"Swing"}))->write(swing, error)) << error;
    EXPECT_EQ(swing.vat, "assets/models/rigid_arm.vat");
    std::unique_ptr<VatBakeSession> stretchSession = bake(modelSpec("rigid_arm.gltf", {"Stretch"}));
    std::string planned;
    bool rewrite = true;
    ASSERT_TRUE(stretchSession->plannedOutput(planned, rewrite, error)) << error;
    EXPECT_EQ(planned, "assets/models/rigid_arm 2.vat");
    EXPECT_FALSE(rewrite);
    VatWriteResult stretch;
    ASSERT_TRUE(stretchSession->write(stretch, error)) << error;
    EXPECT_EQ(stretch.vat, "assets/models/rigid_arm 2.vat");
    jasperengine::vat::VatParseResult first = jasperengine::vat::parseVat(readAll(root() / swing.vat));
    jasperengine::vat::VatParseResult second = jasperengine::vat::parseVat(readAll(root() / stretch.vat));
    ASSERT_TRUE(first.ok() && second.ok());
    ASSERT_EQ(first.data->clips.size(), 1u);
    EXPECT_EQ(first.data->clips[0].name, "Swing") << "the earlier bake is untouched";
    ASSERT_EQ(second.data->clips.size(), 1u);
    EXPECT_EQ(second.data->clips[0].name, "Stretch") << "the new clip keeps its name";
    // the same clip again (before a bake too): the existing file is rewritten
    VatBakeSession again(project());
    again.setSpec(modelSpec("rigid_arm.gltf", {"Swing"}));
    ASSERT_TRUE(again.plannedOutput(planned, rewrite, error)) << error;
    EXPECT_EQ(planned, "assets/models/rigid_arm.vat");
    EXPECT_TRUE(rewrite);
    // the name-only match of a plain re-bake: a renamed clip keeps nothing of another
    EXPECT_EQ(matchClips(first.data->clips, {"Stretch"}, {first.data->clips[0].length()}, false), std::vector<int>{-1});
    EXPECT_EQ(matchClips(first.data->clips, {"Stretch"}, {first.data->clips[0].length()}, true), std::vector<int>{0});
}

// reviews 4-6: images a source holds inside (buffer view, data: URI) go inside the .vat.glb,
// same-named ones stay apart, and no file next to the .vat is touched
TEST_F(VatBaker_, EmbeddedImagesFromDataUrisAndSameNamesStayInTheGlb) {
    const std::string png = sourceImage("morph_blob.gltf");
    ASSERT_EQ(png.compare(0, 4, "\x89PNG"), 0);
    // morph_blob with its image as a data: URI
    {
        nlohmann::json gltf = nlohmann::json::parse(readAll(root() / "assets/models/morph_blob.gltf"));
        gltf["images"][0] = {{"name", "Checker"}, {"uri", "data:image/png;base64," + base64(png)}};
        writeAll(root() / "assets/models/blob_uri.gltf", gltf.dump());
        MeshSystem::invalidateModelFile((root() / "assets/models/blob_uri.gltf").string());
        std::unique_ptr<VatBakeSession> session = bake(modelSpec("blob_uri.gltf"));
        ASSERT_TRUE(session->result());
        EXPECT_FALSE(hasKind(session->result()->report.warnings, "material_simplified")) << kinds(session->result()->report.warnings);
        ASSERT_EQ(session->result()->embeddedImages.size(), 1u) << "the data: URI image is kept";
        EXPECT_EQ(session->result()->embeddedImages[0].bytes, png);
        VatWriteResult written;
        std::string error;
        ASSERT_TRUE(session->write(written, error)) << error;
        const GlbImages glb = glbImages(root() / written.mesh);
        ASSERT_EQ(glb.images.size(), 1u);
        EXPECT_EQ(glb.images[0], png);
    }
    // two materials with different images of the same name
    {
        nlohmann::json gltf = nlohmann::json::parse(readAll(root() / "assets/models/rigid_arm.gltf"));
        const std::string other = png + std::string("\0\0\0\0", 4);    // trailing bytes: another file, still a PNG
        gltf["images"] = nlohmann::json::array({{{"name", "Skin"}, {"uri", "data:image/png;base64," + base64(png)}},
                                                {{"name", "Skin"}, {"uri", "data:image/png;base64," + base64(other)}}});
        gltf["textures"] = nlohmann::json::array({{{"source", 0}}, {{"source", 1}}});
        gltf["materials"][0]["pbrMetallicRoughness"]["baseColorTexture"] = {{"index", 0}};
        gltf["materials"][1]["pbrMetallicRoughness"]["baseColorTexture"] = {{"index", 1}};
        writeAll(root() / "assets/models/skins.gltf", gltf.dump());
        MeshSystem::invalidateModelFile((root() / "assets/models/skins.gltf").string());
        std::unique_ptr<VatBakeSession> session = bake(modelSpec("skins.gltf", {"Swing"}));
        ASSERT_TRUE(session->result());
        ASSERT_EQ(session->result()->embeddedImages.size(), 2u);
        VatWriteResult written;
        std::string error;
        ASSERT_TRUE(session->write(written, error)) << error;
        EXPECT_EQ(written.files.size(), 2u) << "the .vat and its glb only";
        const GlbImages glb = glbImages(root() / written.mesh);
        ASSERT_EQ(glb.images.size(), 2u);
        ASSERT_EQ(glb.baseColorImage.size(), 2u);
        ASSERT_EQ(glb.materialNames, (std::vector<std::string>{"Red", "Blue"}));
        ASSERT_GE(glb.baseColorImage[0], 0);
        ASSERT_GE(glb.baseColorImage[1], 0);
        EXPECT_EQ(glb.images[(size_t)glb.baseColorImage[0]], png) << "Red samples the first Skin";
        EXPECT_EQ(glb.images[(size_t)glb.baseColorImage[1]], other) << "Blue samples the second";
    }
    // a user texture named as the old extraction would have named it: never touched
    {
        const fs::path user = root() / "assets/models/morph_blob_Checker.png";
        writeAll(user, "the user's own texture");
        std::string error;
        VatWriteResult written;
        ASSERT_TRUE(bake(modelSpec("morph_blob.gltf"))->write(written, error)) << error;
        VatWriteResult rebaked;
        ASSERT_TRUE(rebakeVat(project(), written.vat, rebaked, error)) << error;
        EXPECT_EQ(readAll(user), "the user's own texture");
        for (const std::string& file : rebaked.files) EXPECT_EQ(file.find(".png"), std::string::npos) << file;
        size_t pngs = 0;
        for (const auto& entry : fs::directory_iterator(root() / "assets/models")) pngs += entry.path().extension() == ".png";
        EXPECT_EQ(pngs, 1u) << "only the user's";
    }
}

// review 7: a clip removed by hand is not a layout mismatch (the stored hash is the glb's)
TEST_F(VatBaker_, AClipRemovedByHandIsNotStale) {
    std::string error;
    VatWriteResult written;
    ASSERT_TRUE(bake(modelSpec("hero.gltf", {"Walk", "Idle"}))->write(written, error)) << error;
    jasperengine::vat::VatParseResult parsed = jasperengine::vat::parseVat(readAll(root() / written.vat));
    ASSERT_TRUE(parsed.ok());
    VatData edited = *parsed.data;
    edited.clips.erase(edited.clips.begin());
    writeAll(root() / written.vat, jasperengine::vat::writeVat(edited));
    const std::vector<VatIssue> stale = checkVatStaleness(project(), written.vat);
    EXPECT_TRUE(stale.empty()) << kinds(stale);
    VertexAnimationPool::invalidate((root() / written.vat).string());
    std::string loadError;
    EXPECT_TRUE(VertexAnimationPool::getOrLoad((root() / written.vat).string(), &loadError)) << loadError;
}

// review 8: a GameObject bake hashes the files it read (models under the root), not the scene
TEST_F(VatBaker_, GameObjectBakesHashTheFilesTheyReadNotTheScene) {
    const uint32_t sceneId = newScene();
    Scene* scene = sceneOf(sceneId);
    CreateEntityCmd* group = new CreateEntityCmd(project(), sceneId, "Group", EntityCreationType::GAMEOBJECT);
    group->setQuiet(true);
    history(sceneId)->addCommandNoMerge(group);
    CreateEntityCmd* child = new CreateEntityCmd(project(), sceneId, "Limb", EntityCreationType::MODEL, group->getEntity());
    child->setQuiet(true);
    history(sceneId)->addCommandNoMerge(child);
    history(sceneId)->addCommandNoMerge(new ModelLoadCmd(project(), sceneId, child->getEntity(), "models/rigid_arm.gltf"));
    ASSERT_FALSE(scene->getComponent<ModelComponent>(child->getEntity()).loadedFilename.empty());
    writeMoveClip(root() / "assets/anim/limb.anim", "Limb", 1.0f);
    std::string error;
    ASSERT_TRUE(project()->saveSceneNow(sceneId, root() / "assets/scenes/hashes.scene", &error)) << error;

    VatBakeSpec spec;
    spec.source.kind = VatSourceKind::Anim;
    spec.source.sceneId = sceneId;
    spec.source.path = "Group";
    spec.source.anims = {"assets/anim/limb.anim"};
    std::unique_ptr<VatBakeSession> session = bake(spec);
    ASSERT_TRUE(session->result());
    const VatData& data = session->result()->data;
    EXPECT_EQ(data.recipe.sources[0].scene, "assets/scenes/hashes.scene");
    EXPECT_TRUE(hashed(data, "assets/models/rigid_arm.gltf")) << "the model on a child object";
    EXPECT_TRUE(hashed(data, "assets/anim/limb.anim"));
    EXPECT_FALSE(hashed(data, "assets/scenes/hashes.scene"));
    VatWriteResult written;
    ASSERT_TRUE(session->write(written, error)) << error;
    EXPECT_TRUE(checkVatStaleness(project(), written.vat).empty());

    // the scene edited and saved: not stale
    scene->getComponent<Transform>(group->getEntity()).position = Vector3(3, 0, 0);
    ASSERT_TRUE(project()->saveSceneNow(sceneId, {}, &error)) << error;
    EXPECT_TRUE(checkVatStaleness(project(), written.vat).empty()) << kinds(checkVatStaleness(project(), written.vat));
    // the child's model changed: stale
    writeAll(root() / "assets/models/rigid_arm.gltf", readAll(root() / "assets/models/rigid_arm.gltf") + "\n");
    std::vector<VatIssue> stale = checkVatStaleness(project(), written.vat);
    ASSERT_TRUE(hasKind(stale, "vat-source-changed")) << kinds(stale);
    EXPECT_NE(stale[0].message.find("rigid_arm.gltf"), std::string::npos) << stale[0].message;
    std::string closeError;
    project()->closeSceneNow(sceneId, UnsavedChoice::Discard, &closeError);
}

// review 9: what a glTF material carries is carried (names, unlit, foliage), the rest reported
TEST_F(VatBaker_, MaterialsKeepWhatGltfCarriesAndReportTheRest) {
    const uint32_t sceneId = newScene();
    const Entity arm = armWithPlayer(sceneId);
    Scene* scene = sceneOf(sceneId);
    std::vector<Material*> parts;
    for (const auto& node : scene->getComponent<ModelComponent>(arm).meshNodesMapping) {
        MeshComponent* mesh = scene->findComponent<MeshComponent>(node.second);
        if (mesh && mesh->numSubmeshes > 0) parts.push_back(&mesh->submeshes[0].material);
    }
    ASSERT_EQ(parts.size(), 2u);
    Material& red = parts[0]->baseColorFactor.x > 0.5f ? *parts[0] : *parts[1];
    red.lighting = MaterialLighting::NONE;
    red.foliageTransmission = 0.4f;
    red.sortPriority = 3;
    red.blendMode = MaterialBlendMode::ADDITIVE;
    red.stencil.compare = MaterialStencilCompare::ALWAYS;
    red.extraPass.edit().shader = "library:outline";
    VatBakeSpec spec = entitySpec(sceneId, arm);
    spec.output = "assets/models/materials.vat";
    std::unique_ptr<VatBakeSession> session = bake(spec);
    ASSERT_TRUE(session->result());
    const VatMeshData& mesh = session->result()->mesh;
    ASSERT_EQ(mesh.materials.size(), 2u);
    std::vector<std::string> names = {mesh.materials[0].name, mesh.materials[1].name};
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names, (std::vector<std::string>{"Blue", "Red"})) << "the source glTF materials' names";
    const VatMaterialData& baked = mesh.materials[0].name == "Red" ? mesh.materials[0] : mesh.materials[1];
    EXPECT_TRUE(baked.unlit);
    EXPECT_FLOAT_EQ(baked.foliageTransmission, 0.4f);
    std::string notes;
    for (const VatIssue& issue : session->result()->report.warnings) {
        if (issue.kind == "material_simplified") notes += issue.message + "\n";
    }
    for (const char* lost : {"sort priority (3)", "blend mode", "stencil", "extra pass (library:outline)"}) {
        EXPECT_NE(notes.find(lost), std::string::npos) << lost << " in:\n" << notes;
    }
    // unlit is carried (KHR_materials_unlit loads as Lighting None): no advice to set it by hand
    EXPECT_EQ(notes.find("unlit"), std::string::npos) << notes;
    EXPECT_EQ(notes.find("Lighting"), std::string::npos) << notes;
    EXPECT_NE(notes.find("'Red'"), std::string::npos) << notes;
    EXPECT_EQ(notes.find("'Blue'"), std::string::npos) << "nothing lost on Blue:\n" << notes;
    VatWriteResult written;
    std::string error;
    ASSERT_TRUE(session->write(written, error)) << error;
    const GlbImages glb = glbImages(root() / written.mesh);
    EXPECT_NE(glb.json.find("KHR_materials_unlit"), std::string::npos);
    EXPECT_NE(glb.json.find("jasperengine_foliage_transmission"), std::string::npos);
    // the engine reads the foliage transmission back
    const uint32_t loadScene = newScene();
    const Entity loaded = loadModel(loadScene, "models/materials.vat.glb", "Loaded");
    bool foliage = false;
    int unlit = 0, lit = 0;
    for (const Material* material : materialsOf(sceneOf(loadScene), loaded)) {
        foliage |= std::fabs(material->foliageTransmission - 0.4f) < 1e-6f;
        // and draws Red unlit, Blue as before
        if (material->baseColorFactor.x > 0.5f) {
            EXPECT_EQ(material->lighting, MaterialLighting::NONE);
            unlit++;
        } else {
            EXPECT_EQ(material->lighting, MaterialLighting::AUTO);
            lit++;
        }
    }
    EXPECT_TRUE(foliage);
    EXPECT_EQ(unlit, 1);
    EXPECT_EQ(lit, 1);
}

// unlit is the lighting the source draws with, resolved against its shader: library:unlit on
// Auto is unlit, a None that library:toon does not list draws toon's per pixel default
TEST_F(VatBaker_, UnlitIsTheLightingTheSourceDrawsWith) {
    const uint32_t sceneId = newScene();
    const Entity arm = armWithPlayer(sceneId);
    Scene* scene = sceneOf(sceneId);
    std::vector<Material*> parts;
    for (const auto& node : scene->getComponent<ModelComponent>(arm).meshNodesMapping) {
        MeshComponent* mesh = scene->findComponent<MeshComponent>(node.second);
        if (mesh && mesh->numSubmeshes > 0) parts.push_back(&mesh->submeshes[0].material);
    }
    ASSERT_EQ(parts.size(), 2u);
    Material& red = parts[0]->baseColorFactor.x > 0.5f ? *parts[0] : *parts[1];
    Material& blue = parts[0]->baseColorFactor.x > 0.5f ? *parts[1] : *parts[0];
    red.shader = "library:unlit";
    red.lighting = MaterialLighting::AUTO;
    blue.shader = "library:toon";
    blue.lighting = MaterialLighting::NONE;
    VatBakeSpec spec = entitySpec(sceneId, arm);
    spec.output = "assets/models/unlit_source.vat";
    std::unique_ptr<VatBakeSession> session = bake(spec);
    ASSERT_TRUE(session->result());
    const VatMeshData& mesh = session->result()->mesh;
    ASSERT_EQ(mesh.materials.size(), 2u);
    for (const VatMaterialData& baked : mesh.materials) {
        EXPECT_EQ(baked.unlit, baked.name == "Red") << baked.name;
    }
    for (const VatIssue& issue : session->result()->report.warnings) {
        EXPECT_EQ(issue.message.find("Lighting to None"), std::string::npos) << issue.message;
    }
    VatWriteResult written;
    std::string error;
    ASSERT_TRUE(session->write(written, error)) << error;
    const uint32_t loadScene = newScene();
    const Entity loaded = loadModel(loadScene, "models/unlit_source.vat.glb", "Loaded");
    int unlit = 0;
    for (const Material* material : materialsOf(sceneOf(loadScene), loaded)) {
        const bool isRed = material->baseColorFactor.x > 0.5f;
        EXPECT_EQ(material->lighting, isRed ? MaterialLighting::NONE : MaterialLighting::AUTO) << (isRed ? "Red" : "Blue");
        unlit += material->lighting == MaterialLighting::NONE;
    }
    EXPECT_EQ(unlit, 1);
}

// review 10: a source outside the assets folder bakes under it by default (the same folder
// under assets/), so the default output is one an explicit output and placement accept
TEST_F(VatBaker_, ASourceOutsideAssetsBakesUnderAssets) {
    writeMoveClip(root() / "anim/outside.anim", "", 1.0f);
    VatBakeSpec spec;
    spec.source.kind = VatSourceKind::Anim;
    spec.source.path = "assets/models/rigid_arm.gltf";
    spec.source.anims = {"anim/outside.anim"};
    spec.place = VatPlace::Beside;
    newScene();
    std::unique_ptr<VatBakeSession> session = bake(spec);
    ASSERT_TRUE(session->result());
    std::string planned, error;
    bool rewrite = true;
    ASSERT_TRUE(session->plannedOutput(planned, rewrite, error)) << error;
    EXPECT_EQ(planned, "assets/anim/outside.vat");
    VatWriteResult written;
    ASSERT_TRUE(session->write(written, error)) << error;
    EXPECT_EQ(written.vat, "assets/anim/outside.vat");
    EXPECT_TRUE(fs::exists(root() / "assets/anim/outside.vat.glb"));
    EXPECT_FALSE(fs::exists(root() / "anim/outside.vat"));
    ASSERT_NE(written.entity, NULL_ENTITY) << "placed: the files are assets";
    EXPECT_EQ(sceneOf(written.sceneId)->getComponent<VertexAnimatorComponent>(written.entity).animation, "anim/outside.vat");
    fs::remove_all(root() / "anim");
}

// review 11: the rest pose is captured after previews of the source stop, and a scene edited
// between check() and start() is checked again
TEST_F(VatBaker_, TheRestIsCapturedWithoutPreviewsAndAfterEdits) {
    const uint32_t sceneId = newScene();
    const Entity arm = armWithPlayer(sceneId);
    Scene* scene = sceneOf(sceneId);
    VatBakeSpec spec = entitySpec(sceneId, arm);
    spec.mode = VatModeChoice::Bones;
    const std::vector<float> rest = restOf(spec);
    ASSERT_FALSE(rest.empty());

    // an Animation-window-like preview posing the arm at the middle of Swing
    anim::ClipPreviewSession preview;
    preview.root = arm;
    ActionSystem::PlayerPreview pose;
    ActionSystem::PlayerPreviewState state;
    state.clip = AnimationClipPool::getOrLoad("models/rigid_arm.gltf#Swing");
    ASSERT_TRUE(state.clip);
    state.time = state.clip->getLength() * 0.5f;
    state.wrap = ClipWrapMode::ClampForever;
    pose.states.push_back(state);
    bool stopped = false;
    const PreviewOwner::Token token = PreviewOwner::acquire(sceneId, arm, scene, "Animation window", [&]() {
        scene->getSystem<ActionSystem>()->restorePlayerPreview(preview);
        cpuskin::updateTransforms(scene);
        stopped = true;
    });
    scene->getSystem<ActionSystem>()->previewPlayer(arm, pose, preview);
    cpuskin::updateTransforms(scene);
    VatBakeSession session(project());
    session.setSpec(spec);
    EXPECT_FALSE(hasErrors(session.check())) << kinds(session.check());
    EXPECT_TRUE(stopped) << "check() stops the preview before it captures the rest";
    ASSERT_TRUE(session.run()) << session.error();
    EXPECT_LE(largestDifference(session.result()->mesh.positions, rest), 1e-5f);
    PreviewOwner::release(token);

    // an edit between check() and start() (MCP checks in the call, starts a tick later)
    Entity part = NULL_ENTITY;
    for (const auto& node : scene->getComponent<ModelComponent>(arm).meshNodesMapping) part = node.second;
    ASSERT_NE(part, NULL_ENTITY);
    VatBakeSession later(project());
    later.setSpec(spec);
    EXPECT_FALSE(hasErrors(later.check()));
    const Quaternion turned(0.9238795f, 0.0f, 0.0f, 0.3826834f);
    history(sceneId)->addCommandNoMerge(new PropertyCmd<Quaternion>(project(), sceneId, part, ComponentType::Transform, "rotation", turned));
    ASSERT_TRUE(later.run()) << later.error();
    const std::vector<float> edited = restOf(spec);
    EXPECT_GT(largestDifference(edited, rest), 1e-3f) << "the edit moved the rest";
    EXPECT_LE(largestDifference(later.result()->mesh.positions, edited), 1e-5f) << "start() captured the edited rest";
}

// review 12: Rebake keeps the recipe's fps (a longer source clip is not resampled at the old rate)
TEST_F(VatBaker_, RebakeKeepsTheRecipeFps) {
    const fs::path clip = root() / "assets/anim/slide.anim";
    writeMoveClip(clip, "", 1.04f);
    VatBakeSpec spec;
    spec.source.kind = VatSourceKind::Anim;
    spec.source.path = "assets/models/rigid_arm.gltf";
    spec.source.anims = {"assets/anim/slide.anim"};
    std::string error;
    VatWriteResult written;
    ASSERT_TRUE(bake(spec)->write(written, error)) << error;
    jasperengine::vat::VatParseResult parsed = jasperengine::vat::parseVat(readAll(root() / written.vat));
    ASSERT_TRUE(parsed.ok());
    ASSERT_EQ(parsed.data->clips[0].frames, 31) << "round(1.04 x 30)";
    // the source clip gets longer: 3 s at the recipe's 30 fps is 90 frames (not 3 s at the
    // 29.8 fps the 1.04 s clip was sampled at)
    writeMoveClip(clip, "", 3.0f);
    VatWriteResult rebaked;
    ASSERT_TRUE(rebakeVat(project(), written.vat, rebaked, error)) << error;
    parsed = jasperengine::vat::parseVat(readAll(root() / written.vat));
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed.data->clips[0].frames, 90);
    EXPECT_FLOAT_EQ(parsed.data->clips[0].fps, 30.0f);
    // a clip baked at its own rate keeps that rate
    writeMoveClip(clip, "", 1.04f);
    spec.clipOverrides = {{"slide", ClipWrapMode::Default, 60.0f}};
    spec.output = "assets/anim/fast.vat";
    VatWriteResult fast;
    ASSERT_TRUE(bake(spec)->write(fast, error)) << error;
    writeMoveClip(clip, "", 3.0f);
    ASSERT_TRUE(rebakeVat(project(), fast.vat, rebaked, error)) << error;
    parsed = jasperengine::vat::parseVat(readAll(root() / fast.vat));
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed.data->clips[0].frames, 180) << "3 s at 60 fps";
}

// review 13: a non-loop wrap edited in the file survives a re-bake of the same clips and a Rebake
TEST_F(VatBaker_, AllowedWrapEditsSurviveRebakes) {
    VatBakeSpec spec = modelSpec("hero.gltf", {"Walk"});
    spec.clipOverrides = {{"Walk", ClipWrapMode::Once, 0.0f}};
    std::string error;
    VatWriteResult written;
    ASSERT_TRUE(bake(spec)->write(written, error)) << error;
    std::shared_ptr<VatDocument> document = VatDocument::open(project(), written.vat, &error);
    ASSERT_TRUE(document) << error;
    VatData data = document->data();
    data.clips[0].wrap = ClipWrapMode::PingPong;
    ASSERT_TRUE(document->commit(data, "Wrap", "", &error)) << error;
    data.clips[0].wrap = ClipWrapMode::Loop;
    EXPECT_FALSE(document->commit(data, "Wrap", "", &error));
    EXPECT_NE(error.find("loop clips are sampled without a duplicate end frame"), std::string::npos) << error;
    // the same clips baked again (not a Rebake): the edit stays
    VatWriteResult again;
    ASSERT_TRUE(bake(spec)->write(again, error)) << error;
    ASSERT_EQ(again.vat, written.vat);
    jasperengine::vat::VatParseResult parsed = jasperengine::vat::parseVat(readAll(root() / written.vat));
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed.data->clips[0].wrap, ClipWrapMode::PingPong);
    // and a Rebake
    VatWriteResult rebaked;
    ASSERT_TRUE(rebakeVat(project(), written.vat, rebaked, error)) << error;
    parsed = jasperengine::vat::parseVat(readAll(root() / written.vat));
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed.data->clips[0].wrap, ClipWrapMode::PingPong);
    EXPECT_EQ(parsed.data->clips[0].frames, 31) << "non-loop rows keep the end frame";
}

// review 15: a crowd's area is in metres whatever the source's scale and rotation
TEST_F(VatBaker_, ACrowdOfAScaledSourceKeepsItsAreaInMetres) {
    const uint32_t sceneId = newScene();
    const Entity arm = armWithPlayer(sceneId);
    Scene* scene = sceneOf(sceneId);
    const Quaternion tilted(0.7071068f, 0.7071068f, 0.0f, 0.0f);
    scene->getComponent<Transform>(arm).position = Vector3(2.0f, 0.0f, 1.0f);
    scene->getComponent<Transform>(arm).rotation = tilted;
    scene->getComponent<Transform>(arm).scale = Vector3(0.01f, 0.01f, 0.01f);
    cpuskin::updateTransforms(scene);
    VatBakeSpec spec = entitySpec(sceneId, arm);
    spec.place = VatPlace::Crowd;
    spec.crowd.count = 40;
    spec.crowd.area = 10.0f;
    spec.crowd.seed = 3;
    std::unique_ptr<VatBakeSession> session = bake(spec);
    VatWriteResult written;
    std::string error;
    ASSERT_TRUE(session->write(written, error)) << error;
    const Transform& placed = scene->getComponent<Transform>(written.entity);
    EXPECT_EQ(placed.position, Vector3(2.0f, 0.0f, 1.0f));
    EXPECT_EQ(placed.scale, Vector3(1.0f, 1.0f, 1.0f));
    EXPECT_EQ(placed.rotation, Quaternion());
    const InstancedMeshComponent& crowd = scene->getComponent<InstancedMeshComponent>(written.entity);
    ASSERT_EQ(crowd.instances.size(), 40u);
    CrowdSettings settings;
    settings.count = 40;
    settings.width = settings.depth = 10.0f;
    settings.minSpacing = 0.0f;
    settings.seed = 3;
    const CrowdLayout layout = layoutCrowd(settings, nullptr);
    float spread = 0.0f;
    for (size_t i = 0; i < crowd.instances.size(); i++) {
        const InstanceData& instance = crowd.instances[i];
        spread = std::max(spread, std::max(std::fabs(instance.position.x), std::fabs(instance.position.z)));
        EXPECT_EQ(instance.position, layout.instances[i].position);
        EXPECT_NEAR(instance.scale.x, 0.01f * layout.instances[i].scale.x, 1e-7f);
        EXPECT_NEAR(instance.scale.y, 0.01f * layout.instances[i].scale.y, 1e-7f);
        const Quaternion expected = layout.instances[i].rotation * tilted;
        EXPECT_NEAR(std::fabs(instance.rotation.w * expected.w + instance.rotation.x * expected.x + instance.rotation.y * expected.y +
                              instance.rotation.z * expected.z), 1.0f, 1e-5f) << "yaw, then the source's rotation";
    }
    EXPECT_GT(spread, 2.5f) << "spread over the 10 m area, not 0.1 m";
    EXPECT_LE(spread, 5.0f + 1e-4f);
}

// review 16: a 2D scene takes no Vertex Animation (bake check, write and import)
TEST_F(VatBaker_, TwoDScenesRefuseThePlacement) {
    uint32_t threeD = 0;
    const uint32_t flat = secondScene(threeD, SceneType::SCENE_2D);
    ASSERT_NE(flat, threeD);
    ASSERT_EQ(project()->getScene(flat)->sceneType, SceneType::SCENE_2D);
    VatBakeSpec spec = modelSpec("rigid_arm.gltf", {"Swing"});
    spec.place = VatPlace::Beside;
    {
        VatBakeSession session(project());
        session.setSpec(spec);
        bool error = false;
        EXPECT_TRUE(hasKind(session.check(), "invalid_settings", &error)) << kinds(session.check());
        EXPECT_TRUE(error);
        EXPECT_FALSE(session.run());
    }
    // checked with the 3D scene selected, written with the 2D one
    project()->setSelectedSceneId(threeD);
    std::unique_ptr<VatBakeSession> session = bake(spec);
    ASSERT_TRUE(session->result());
    project()->setSelectedSceneId(flat);
    VatWriteResult written;
    VatIssue issue;
    std::string error;
    EXPECT_FALSE(session->write(written, error, &issue));
    EXPECT_EQ(issue.kind, "invalid_settings");
    EXPECT_NE(error.find("3D scene"), std::string::npos) << error;
    EXPECT_FALSE(fs::exists(root() / "assets/models/rigid_arm.vat"));
    // an import placed into it is refused before anything is written
    VatImportResult result;
    VatImportDetection detection;
    detection.assetName = "Wave";
    VatImportWriteOptions options;
    options.place = VatPlace::Beside;
    EXPECT_FALSE(writeVatImport(project(), result, detection, options, written, error));
    EXPECT_NE(error.find("3D scene"), std::string::npos) << error;
    // a GameObject source of a 2D scene can still bake its files
    project()->setSelectedSceneId(threeD);
}

TEST_F(VatBaker_, AGameObjectBakesAgainAfterBakesPlacementsAndADeleteUndone) {
    const uint32_t sceneId = newScene();
    const Entity hero = loadModel(sceneId, "models/hero.gltf");
    Scene* scene = sceneOf(sceneId);
    VatBakeSpec spec;
    spec.source.kind = VatSourceKind::Entity;
    spec.source.sceneId = sceneId;
    spec.source.path = EntityPath::of(*project()->getScene(sceneId), hero);
    spec.clips = {"Walk"};
    spec.fps = 5.0f;
    // every bake of the same GameObject: the same layout, and the source's rest mesh unchanged
    VatReport first;
    std::vector<Vector3> rest;
    auto again = [&](const std::string& what, VatPlace place = VatPlace::None, const std::string& output = std::string()) {
        VatBakeSpec run = spec;
        run.place = place;
        run.output = output;
        VatBakeSession session(project());
        session.setSpec(run);
        if (!session.run()) {
            ADD_FAILURE() << what << ": " << session.error() << " / " << kinds(session.failureIssues());
            return false;
        }
        const VatReport& report = session.result()->report;
        if (first.vertices == 0) first = report;
        EXPECT_EQ(report.vertices, first.vertices) << what;
        EXPECT_EQ(report.bones, first.bones) << what;
        EXPECT_EQ(report.width, first.width) << what;
        EXPECT_EQ(report.height, first.height) << what;
        std::string error;
        {
            VatBakeTarget target;
            std::vector<VatIssue> issues;
            VatPoser poser;
            EXPECT_TRUE(resolveVatSource(project(), spec.source, target, issues)) << what << ": " << kinds(issues);
            EXPECT_TRUE(poser.begin(target, {}, issues, error)) << what << ": " << error << " " << kinds(issues);
            EXPECT_FALSE(poser.renderers.empty()) << what;
            if (!poser.renderers.empty()) {
                if (rest.empty()) rest = poser.renderers[0].source.positions;
                EXPECT_TRUE(poser.renderers[0].source.positions == rest) << what << ": the source's mesh is unchanged";
            }
        }
        // (replace then disables the source)
        if (place != VatPlace::None) {
            VatWriteResult written;
            EXPECT_TRUE(session.write(written, error)) << what << ": " << error;
        }
        return true;
    };
    ASSERT_TRUE(again("the first bake"));
    EXPECT_TRUE(again("the same GameObject again"));
    EXPECT_TRUE(again("place beside", VatPlace::Beside, "assets/models/again_beside.vat"));
    EXPECT_TRUE(again("after place beside"));
    EXPECT_TRUE(again("replace", VatPlace::Replace, "assets/models/again_replace.vat"));
    EXPECT_FALSE(scene->isEntityActive(hero)) << "replace disables the source";
    ASSERT_TRUE(history(sceneId)->undo());
    EXPECT_TRUE(scene->isEntityActive(hero));
    EXPECT_TRUE(again("after undoing replace"));
    ASSERT_TRUE(history(sceneId)->undo());
    EXPECT_TRUE(again("after undoing place beside"));

    // deleted, then brought back by undo: its Model builds the meshes in the next MeshSystem
    // update (until then they hold submeshes without vertex data, which read as "no POSITION")
    history(sceneId)->addCommandNoMerge(new DeleteEntityCmd(project(), sceneId, hero));
    ASSERT_FALSE(scene->isEntityCreated(hero));
    ASSERT_TRUE(history(sceneId)->undo());
    ASSERT_TRUE(scene->isEntityCreated(hero));
    ASSERT_TRUE(scene->getComponent<ModelComponent>(hero).needUpdateModel);
    EXPECT_TRUE(vatSourceLoading(project(), spec.source));
    {
        VatBakeSession session(project());
        session.setSpec(spec);
        bool error = false;
        const std::vector<VatIssue> issues = session.check();
        EXPECT_TRUE(hasKind(issues, "source_loading", &error)) << kinds(issues);
        EXPECT_TRUE(error);
        EXPECT_FALSE(hasKind(issues, "invalid_source")) << kinds(issues);
        EXPECT_FALSE(session.run());
    }
    // the refused bake changed nothing: the load is still the engine's to do
    EXPECT_TRUE(scene->getComponent<ModelComponent>(hero).needUpdateModel);
    scene->getSystem<MeshSystem>()->update(0);
    EXPECT_FALSE(vatSourceLoading(project(), spec.source));
    EXPECT_TRUE(again("after its deletion was undone and the model loaded"));
    EXPECT_TRUE(again("and once more"));
    // a file source never waits
    EXPECT_FALSE(vatSourceLoading(project(), modelSpec("hero.gltf").source));
}
