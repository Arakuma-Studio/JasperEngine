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

#include "AppSettings.h"
#include "Engine.h"
#include "Out.h"
#include "Platform.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/CreateVatCmd.h"
#include "command/type/ModelLoadCmd.h"
#include "util/EntityPath.h"
#include "util/FileUtils.h"
#include "vat/VatBakeSession.h"

#include "Scene.h"
#include "animation/AnimationClipIO.h"
#include "component/AnimationPlayerComponent.h"
#include "component/LatticeComponent.h"
#include "component/LatticeModifierComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "component/VertexAnimatorComponent.h"
#include "lattice/LatticeRender.h"
#include "pool/AnimationClipPool.h"
#include "pool/VertexAnimationPool.h"
#include "render/CpuSkin.h"
#include "subsystem/ActionSystem.h"
#include "subsystem/MeshSystem.h"

#include <unistd.h>

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
        std::unique_ptr<editor::Platform> platform;
    };

    fs::path repoFile(const fs::path& relative) {
        return (fs::path(JASPER_EDITOR_TEST_FIXTURES) / ".." / ".." / ".." / relative).lexically_normal();
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
            out.platform.reset(new editor::Platform(out.project.get()));
            static char arg0[] = "jasperengine-editor-tests";
            static char* argv[] = {arg0, nullptr};
            Engine::systemInit(1, argv, out.platform.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            out.ready = out.project->loadProject(out.dir, false);
            Engine::setAsyncLoading(false);
            return out;
        }();
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
            for (int i = 0; i < 4; i++) out[i] = halfToFloat(half[i]);
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
    clipFrames(0.95f, ClipWrapMode::Loop, 30.0f, frames, fps);
    EXPECT_EQ(frames, 29);
    EXPECT_NEAR((float)frames / fps, 0.95f, 1e-6f);
    clipFrames(0.0f, ClipWrapMode::Once, 30.0f, frames, fps);
    EXPECT_EQ(frames, 1);
}

TEST(VatBakerTypes, HalfFloatsRoundToNearestEven) {
    for (float value : {0.0f, 1.0f, -2.5f, 0.000123f, 65504.0f, 1e-7f}) {
        const float back = halfToFloat(floatToHalf(value));
        EXPECT_NEAR(back, value, std::fabs(value) * 0.0005f + 6e-8f) << value;
    }
    EXPECT_EQ(floatToHalf(1.0f), 0x3c00);
    EXPECT_EQ(floatToHalf(-2.0f), 0xc000);
    EXPECT_EQ(floatToHalf(1.0f + 1.0f / 2048.0f), 0x3c00) << "a tie rounds to even";
    EXPECT_EQ(floatToHalf(1.0f + 3.0f / 2048.0f), 0x3c02);
    EXPECT_TRUE(std::isinf(halfToFloat(floatToHalf(1e6f))));
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
    // bone_limit: Bones forced with 129 parts moving apart
    {
        VatBakeInput input;
        for (int i = 0; i < 129; i++) input.renderers.push_back(quadRenderer("P" + std::to_string(i)));
        input.mode = VatModeChoice::Bones;
        input.clips.push_back(movingClip("Scatter", 129, 0.2f, 10.0f, ClipWrapMode::Loop, [](size_t r, float t) {
            return Matrix4::translateMatrix(Vector3((float)r * t, 0.0f, 0.0f));
        }));
        EXPECT_FALSE(computeVatBake(input, out, error));
        EXPECT_TRUE(hasKind(out.report.warnings, "bone_limit")) << kinds(out.report.warnings);
        // Auto takes Vertices and says why
        input.mode = VatModeChoice::Auto;
        ASSERT_TRUE(computeVatBake(input, out, error)) << error;
        EXPECT_EQ(out.data.mode, VatMode::Vertices);
        EXPECT_NE(out.report.modeReason.find("129 bones"), std::string::npos) << out.report.modeReason;
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
        EXPECT_EQ(result->mesh.vertexCount(), 2801u) << "the three primitives share their vertices";
        EXPECT_TRUE(data.bounds.valid);
        if (mode == VatModeChoice::Vertices) {
            EXPECT_EQ(data.mode, VatMode::Vertices);
            EXPECT_EQ(data.layout.vertices, 2801);
            EXPECT_EQ(data.layout.texelsPerSample(VatMode::Vertices), 2);
            EXPECT_EQ(result->report.fetchesPerVertex, 4);
        } else {
            EXPECT_EQ(data.mode, VatMode::Bones);
            EXPECT_EQ(data.layout.precision, VatPrecision::Float);
            EXPECT_GT(data.layout.bones, 10);
            EXPECT_LE(data.layout.bones, 43);
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
        EXPECT_EQ(result->mesh.materials[0].name, "Red");
        EXPECT_NEAR(result->mesh.materials[1].baseColor[2], 0.8f, 1e-6f);
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
        const VatBakeReport& report = session.result()->report;
        EXPECT_EQ(estimate.vertices, report.vertices);
        if (mode == VatModeChoice::Bones) EXPECT_EQ(estimate.bones, report.bones);
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
        if (issue.kind == "too_large") EXPECT_GE(issue.fixes.size(), 3u);
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
    // replace needs a GameObject; crowd waits on the crowd placement
    VatBakeSpec replace = modelSpec("hero.gltf", {"Walk"});
    replace.place = VatPlace::Replace;
    EXPECT_TRUE(hasErrors(check(replace)));
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

    // the same source again: rewritten, GUID kept
    std::unique_ptr<VatBakeSession> again = bake(modelSpec("hero.gltf", {"Walk"}));
    VatWriteResult rewritten;
    ASSERT_TRUE(again->write(rewritten, error)) << error;
    EXPECT_EQ(rewritten.vat, "assets/models/hero.vat");
    EXPECT_TRUE(rewritten.rewritten);
    EXPECT_EQ(rewritten.guid, written.guid);

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
    EXPECT_NE(session.progress().stage.find("Posing clip 1 of 1: Walk"), std::string::npos) << session.progress().stage;
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
