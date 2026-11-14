// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The editor half of the Vertex Animation end-to-end proofs (vertex animation spec 8): the real
// baker (VatBakeSession: source -> ActionSystem posing -> CpuSkin -> encode -> write, one undo
// step) and the real importers (importVat + encodeVatImport) write their .vat / .vat.glb pairs
// into the end-to-end bundle, a directory the GPU half (engine/tests/htmlui/TestVatEndToEnd.cpp,
// jasperengine-htmlui-tests of the standalone engine build) loads and renders against the
// skinned / rigid / morph sources and against the importers' ground truth. The editor test
// binary has no GPU and the GPU test binary can't link the editor, so the bundle is the
// hand-over; tools/vat_e2e.sh runs both halves in order.
// Bundle: $JASPER_VAT_E2E_DIR, else <temp>/jasperengine-vat-e2e-<FNV-1a of the repository root>
// (both halves derive the same name). Each asset comes with <name>.json: what the GPU half needs
// to know (mode, clips, the report, the source model) and the times it renders: for bakes each
// with the error measured here (the bake decoded as vat.glsl does against the source posed by its
// clip and skinned by CpuSkin: the largest vertex distance and normal angle), for imports with the
// ground truth triangles.
//   hero_vertices / hero_bones   tools/fixtures/pose-animation hero_animated.gltf, Walk, Idle and Wave
//                                (also the baked hero of tools/performance/vat_crowd.py)
//   rigid_arm                    tools/fixtures/vertex-animation rigid_arm.gltf Swing, Auto (Bones)
//   morph_blob                   morph_blob.gltf Pulse, Auto (Vertices: morph targets)
//   houdini_soft / houdini_rigid the synthetic Houdini VAT 3 exports of VatImportFixtures.h

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include "vat/VatImportFixtures.h"

#include "AppSettings.h"
#include "Engine.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/ModelLoadCmd.h"
#include "vat/VatBakeSession.h"
#include "vat/import/VatImport.h"

#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "lattice/LatticeRender.h"
#include "pool/AnimationClipPool.h"
#include "render/CpuSkin.h"
#include "subsystem/ActionSystem.h"
#include "subsystem/MeshSystem.h"

#include "json.hpp"

#include "support/PosixCompat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::vat;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

    fs::path repoRoot() {
        return fs::weakly_canonical(fs::path(JASPER_TEST_FIXTURES_DIR) / ".." / ".." / "..");
    }

    // the same rule as TestVatEndToEnd.cpp's bundleDirectory()
    fs::path bundleDirectory() {
        if (const char* env = std::getenv("JASPER_VAT_E2E_DIR"); env && *env) return fs::path(env);
        uint64_t hash = 1469598103934665603ull;
        for (unsigned char c : repoRoot().generic_string()) {
            hash ^= c;
            hash *= 1099511628211ull;
        }
        char name[64];
        std::snprintf(name, sizeof(name), "jasperengine-vat-e2e-%016llx", (unsigned long long)hash);
        return fs::temp_directory_path() / name;
    }

    struct Headless {
        bool ready = false;
        fs::path dir;
        std::unique_ptr<Project> project;
    };

    Headless& headless() {
        static Headless h = []() {
            Headless out;
            const fs::path root = fs::temp_directory_path() / ("jasper-vat-e2e-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            const fs::path models = out.dir / "assets" / "models";
            fs::create_directories(models);
            std::ofstream(out.dir / "project.yaml") << "name: vate2e\nassetsDir: assets\n";
            fs::copy_file(repoRoot() / "tools/fixtures/pose-animation/assets/models/hero_animated.gltf", models / "hero.gltf",
                          fs::copy_options::overwrite_existing);
            for (const char* name : {"rigid_arm.gltf", "morph_blob.gltf"}) {
                fs::copy_file(repoRoot() / "tools/fixtures/vertex-animation/assets/models" / name, models / name, fs::copy_options::overwrite_existing);
            }
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

    void writeText(const fs::path& path, const std::string& text) {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << text;
    }

    Json clipsJson(const jasperengine::vat::VatData& data, const VatReport& report) {
        Json clips = Json::array();
        for (size_t c = 0; c < data.clips.size(); c++) {
            const jasperengine::vat::VatClip& clip = data.clips[c];
            Json row = {{"name", clip.name}, {"start", clip.start}, {"frames", clip.frames}, {"fps", clip.fps},
                        {"interpolate", clip.interpolate}, {"length", clip.length()}};
            for (const VatClipReport& r : report.clips) {
                if (r.name == clip.name) {
                    row["max_error_mm"] = r.maxErrorMm;
                    row["interpolation_error_mm"] = r.interpolationErrorMm;
                }
            }
            clips.push_back(row);
        }
        return clips;
    }

    // ---- what the GPU should draw, measured on the CPU

    struct D3 {
        double x = 0, y = 0, z = 0;
        D3() = default;
        D3(double a, double b, double c) : x(a), y(b), z(c) {}
        explicit D3(const Vector3& v) : x(v.x), y(v.y), z(v.z) {}
        D3 operator+(const D3& o) const { return {x + o.x, y + o.y, z + o.z}; }
        D3 operator-(const D3& o) const { return {x - o.x, y - o.y, z - o.z}; }
        D3 operator*(double s) const { return {x * s, y * s, z * s}; }
    };
    double dot(const D3& a, const D3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    D3 cross(const D3& a, const D3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
    double length(const D3& a) { return std::sqrt(dot(a, a)); }
    D3 normalized(const D3& a) {
        const double l = length(a);
        return l > 0 ? a * (1.0 / l) : a;
    }
    double angleDegrees(const D3& a, const D3& b) {
        return std::atan2(length(cross(normalized(a), normalized(b))), dot(normalized(a), normalized(b))) * 180.0 / 3.14159265358979323846;
    }

    D3 transformPoint(const Matrix4& m, const Vector3& p) {
        return {m.get(0, 0) * p.x + m.get(1, 0) * p.y + m.get(2, 0) * p.z + m.get(3, 0), m.get(0, 1) * p.x + m.get(1, 1) * p.y + m.get(2, 1) * p.z + m.get(3, 1),
                m.get(0, 2) * p.x + m.get(1, 2) * p.y + m.get(2, 2) * p.z + m.get(3, 2)};
    }
    D3 transformDirection(const Matrix4& m, const Vector3& d) {
        return normalized(D3(m.get(0, 0) * d.x + m.get(1, 0) * d.y + m.get(2, 0) * d.z, m.get(0, 1) * d.x + m.get(1, 1) * d.y + m.get(2, 1) * d.z,
                             m.get(0, 2) * d.x + m.get(1, 2) * d.y + m.get(2, 2) * d.z));
    }

    // a source vertex: rest (identity skin) and posed (CpuSkin of the scene posed by the clip, the
    // GPU skinning's formula), positions and normals in the root's space
    struct RefVertex {
        D3 rest, restNormal, posed, posedNormal;
    };

    std::vector<RefVertex> referencePose(Scene* scene, Entity root, const std::shared_ptr<const AnimationClip>& clip, float time) {
        std::vector<RefVertex> out;
        cpuskin::updateTransforms(scene);
        const Matrix4 rootInverse = scene->getComponent<Transform>(root).modelMatrix.inverse();
        std::vector<Entity> meshes;
        std::vector<std::vector<RefVertex>> rests;
        auto transforms = scene->getComponentArray<Transform>();
        for (size_t i = 0; i < transforms->size(); i++) {
            const Entity entity = transforms->getEntity(i);
            bool under = false;
            for (Entity walk = entity; walk != NULL_ENTITY; walk = scene->getComponent<Transform>(walk).parent) {
                if (walk == root) {
                    under = true;
                    break;
                }
            }
            const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
            if (!under || !mesh || mesh->numSubmeshes == 0) continue;
            cpuskin::CpuSkinSource source;
            std::string error;
            EXPECT_TRUE(cpuskin::buildSource(*mesh, source, &error)) << error;
            int maxBone = 0;
            for (const auto& ids : source.boneIds)
                for (int id : ids) maxBone = std::max(maxBone, id);
            const std::vector<Matrix4> identity((size_t)maxBone + 1);
            cpuskin::CpuSkinResult rest;
            EXPECT_TRUE(cpuskin::cpuDeform(source, identity, mesh->morphWeights, nullptr, rest, &error)) << error;
            const Matrix4 toRoot = rootInverse * scene->getComponent<Transform>(entity).modelMatrix;
            std::vector<RefVertex> r(rest.positions.size());
            for (size_t v = 0; v < r.size(); v++) {
                r[v].rest = transformPoint(toRoot, rest.positions[v]);
                r[v].restNormal = transformDirection(toRoot, rest.normals[v]);
            }
            meshes.push_back(entity);
            rests.push_back(r);
        }
        anim::ClipPreviewSession session;
        session.root = root;
        ActionSystem::PlayerPreview preview;
        ActionSystem::PlayerPreviewState state;
        state.clip = clip;
        state.time = time;
        state.wrap = ClipWrapMode::ClampForever;
        preview.states.push_back(state);
        scene->getSystem<ActionSystem>()->previewPlayer(root, preview, session);
        cpuskin::updateTransforms(scene);
        LatticeRender::prepare(scene);
        for (size_t m = 0; m < meshes.size(); m++) {
            cpuskin::CpuSkinResult posed;
            std::string error;
            EXPECT_TRUE(cpuskin::deformEntity(scene, meshes[m], posed, &error)) << error;
            const Matrix4 toRoot = rootInverse * scene->getComponent<Transform>(meshes[m]).modelMatrix;
            for (size_t v = 0; v < posed.positions.size(); v++) {
                RefVertex r = rests[m][v];
                r.posed = transformPoint(toRoot, posed.positions[v]);
                r.posedNormal = transformDirection(toRoot, posed.normals[v]);
                out.push_back(r);
            }
        }
        scene->getSystem<ActionSystem>()->restorePlayerPreview(session);
        cpuskin::updateTransforms(scene);
        LatticeRender::prepare(scene);
        return out;
    }

    struct Q {
        double x = 0, y = 0, z = 0, w = 1;
    };
    D3 rotate(const Q& q, const D3& v) {
        const D3 u(q.x, q.y, q.z);
        return v + cross(u, cross(u, v) + v * q.w) * 2.0;
    }
    // vat.glsl's vatNlerp: q1 on q0's side, mixed, normalised
    Q nlerp(Q a, Q b, double t) {
        if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0) b = {-b.x, -b.y, -b.z, -b.w};
        Q q{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
        const double l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        return {q.x / l, q.y / l, q.z / l, q.w / l};
    }

    std::array<double, 4> texel(const jasperengine::vat::VatPayload& payload, uint64_t index) {
        std::array<double, 4> out{};
        if (payload.precision == jasperengine::vat::VatPrecision::Float) {
            float f[4];
            std::memcpy(f, payload.bytes.data() + index * 16, 16);
            for (int i = 0; i < 4; i++) out[i] = f[i];
        } else {
            uint16_t h[4];
            std::memcpy(h, payload.bytes.data() + index * 8, 8);
            for (int i = 0; i < 4; i++) out[i] = editor::vat::halfToFloat(h[i]);
        }
        return out;
    }

    // every baked vertex at `time` of clip `clipIndex` as vat.glsl computes it (one track, no
    // crossfade): positions and normals, in the order of the baked mesh
    void decodeAt(const VatBakeResult& result, int clipIndex, float time, std::vector<D3>& positions, std::vector<D3>& normals) {
        using jasperengine::vat::VatMode;
        const jasperengine::vat::VatData& data = result.data;
        const VatMeshData& mesh = result.mesh;
        const jasperengine::vat::VatPayload& payload = result.payload();
        const jasperengine::vat::VatFrameSample f = jasperengine::vat::vatSample(data.clips[(size_t)clipIndex], time);
        positions.clear();
        normals.clear();
        for (size_t v = 0; v < mesh.vertexCount(); v++) {
            const D3 rest(mesh.positions[v * 3], mesh.positions[v * 3 + 1], mesh.positions[v * 3 + 2]);
            const D3 restNormal(mesh.normals[v * 3], mesh.normals[v * 3 + 1], mesh.normals[v * 3 + 2]);
            if (data.mode == VatMode::Vertices) {
                const int element = (int)mesh.vatIndex[v];
                const auto o0 = texel(payload, jasperengine::vat::vatTexelIndex(VatMode::Vertices, data.layout, f.frame0, element, 0));
                const auto o1 = texel(payload, jasperengine::vat::vatTexelIndex(VatMode::Vertices, data.layout, f.frame1, element, 0));
                positions.push_back(rest + D3(o0[0] + (o1[0] - o0[0]) * f.alpha, o0[1] + (o1[1] - o0[1]) * f.alpha, o0[2] + (o1[2] - o0[2]) * f.alpha));
                if (data.layout.normals) {
                    const auto q0 = texel(payload, jasperengine::vat::vatTexelIndex(VatMode::Vertices, data.layout, f.frame0, element, 1));
                    const auto q1 = texel(payload, jasperengine::vat::vatTexelIndex(VatMode::Vertices, data.layout, f.frame1, element, 1));
                    normals.push_back(rotate(nlerp({q0[0], q0[1], q0[2], q0[3]}, {q1[0], q1[1], q1[2], q1[3]}, f.alpha), D3(0, 0, 1)));
                } else {
                    normals.push_back(restNormal);
                }
            } else {
                D3 p, n;
                double weightSum = 0;
                for (int k = 0; k < 4; k++) {
                    const double w = mesh.weights[v * 4 + k];
                    if (w == 0) continue;
                    const int bone = mesh.joints[v * 4 + k];
                    const auto q0 = texel(payload, jasperengine::vat::vatTexelIndex(VatMode::Bones, data.layout, f.frame0, bone, 0));
                    const auto q1 = texel(payload, jasperengine::vat::vatTexelIndex(VatMode::Bones, data.layout, f.frame1, bone, 0));
                    const auto t0 = texel(payload, jasperengine::vat::vatTexelIndex(VatMode::Bones, data.layout, f.frame0, bone, 1));
                    const auto t1 = texel(payload, jasperengine::vat::vatTexelIndex(VatMode::Bones, data.layout, f.frame1, bone, 1));
                    const Q q = nlerp({q0[0], q0[1], q0[2], q0[3]}, {q1[0], q1[1], q1[2], q1[3]}, f.alpha);
                    double ts[4];
                    for (int i = 0; i < 4; i++) ts[i] = t0[i] + (t1[i] - t0[i]) * f.alpha;
                    p = p + (rotate(q, rest * ts[3]) + D3(ts[0], ts[1], ts[2])) * w;
                    n = n + rotate(q, restNormal) * (ts[3] * w);
                    weightSum += w;
                }
                positions.push_back(weightSum > 0 ? p * (1.0 / weightSum) : rest);
                normals.push_back(weightSum > 0 ? normalized(n) : restNormal);
            }
        }
    }

    std::string restKey(const D3& p) {
        char text[96];
        std::snprintf(text, sizeof(text), "%.4f,%.4f,%.4f", std::round(p.x * 1e4) / 1e4, std::round(p.y * 1e4) / 1e4, std::round(p.z * 1e4) / 1e4);
        return text;
    }

    // the largest distance (mm) and normal angle (degrees) between what the GPU draws for the bake
    // at `time` and the source posed at `time`; each baked vertex against the source vertex at its
    // rest position with the closest rest normal
    void measureAt(Scene* scene, Entity root, const VatBakeResult& result, int clipIndex, const std::shared_ptr<const AnimationClip>& clip, float time,
                   double& errorMm, double& normalDegrees) {
        const std::vector<RefVertex> reference = referencePose(scene, root, clip, time);
        std::map<std::string, std::vector<const RefVertex*>> byRest;
        for (const RefVertex& r : reference) byRest[restKey(r.rest)].push_back(&r);
        std::vector<D3> positions, normals;
        decodeAt(result, clipIndex, time, positions, normals);
        errorMm = 0;
        normalDegrees = 0;
        for (size_t v = 0; v < positions.size(); v++) {
            const D3 rest(result.mesh.positions[v * 3], result.mesh.positions[v * 3 + 1], result.mesh.positions[v * 3 + 2]);
            const D3 restNormal(result.mesh.normals[v * 3], result.mesh.normals[v * 3 + 1], result.mesh.normals[v * 3 + 2]);
            auto it = byRest.find(restKey(rest));
            if (it == byRest.end()) {
                ADD_FAILURE() << "baked vertex " << v << " has no source vertex at its rest position";
                return;
            }
            const RefVertex* best = it->second[0];
            for (const RefVertex* r : it->second)
                if (angleDegrees(r->restNormal, restNormal) < angleDegrees(best->restNormal, restNormal)) best = r;
            errorMm = std::max(errorMm, length(best->posed - positions[v]) * 1000.0);
            normalDegrees = std::max(normalDegrees, angleDegrees(best->posedNormal, normals[v]));
        }
    }

    // a time the GPU half renders: a clip at `frames` of its samples (x.5: between two)
    struct CaseSpec {
        std::string clip;
        double frames = 0;
        std::string what;
        bool crowd = false;     // one of the 4 crowd instances (heroes)
    };

    uint32_t newScene() {
        Project* p = headless().project.get();
        if (SceneProject* current = p->getSelectedScene()) current->isModified = false;
        p->createNewScene("VatE2E", SceneType::SCENE_3D);
        return p->getSelectedSceneId();
    }

    Entity loadModel(uint32_t sceneId, const std::string& assetsRelative) {
        Project* p = headless().project.get();
        CreateEntityCmd* create = new CreateEntityCmd(p, sceneId, "Source", EntityCreationType::MODEL);
        create->setQuiet(true);
        CommandHandle::get(sceneId)->addCommandNoMerge(create);
        const Entity entity = create->getEntity();
        CommandHandle::get(sceneId)->addCommandNoMerge(new ModelLoadCmd(p, sceneId, entity, assetsRelative));
        EXPECT_FALSE(p->getScene(sceneId)->scene->getComponent<ModelComponent>(entity).loadedFilename.empty()) << assetsRelative;
        return entity;
    }

    // A bake written through the session (one undo step on the project history) and copied into
    // the bundle as <name>.vat, <name>.vat.glb (+ images a glTF embeds) with <name>.json: its
    // clips, its report, and every case the GPU half renders with the error measured here for it
    // (error_mm, normal_error_deg: the bake decoded as vat.glsl does against the source posed by
    // its clip and skinned by CpuSkin)
    void bakeIntoBundle(const std::string& name, VatBakeSpec spec, const std::string& model, const std::string& sourceFile,
                        jasperengine::vat::VatMode expectedMode, const std::vector<CaseSpec>& cases) {
        SCOPED_TRACE(name);
        ASSERT_TRUE(headless().ready);
        spec.output = "assets/e2e/" + name + ".vat";
        spec.overwrite = true;
        VatBakeSession session(headless().project.get());
        session.setSpec(spec);
        ASSERT_TRUE(session.run()) << session.error() << " / " << issuesText(session.failureIssues());
        const VatBakeResult* result = session.result();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->data.mode, expectedMode) << result->report.modeReason;

        const uint32_t sceneId = newScene();
        Scene* scene = headless().project->getScene(sceneId)->scene;
        const Entity root = loadModel(sceneId, "models/" + model);
        Json measured = Json::array();
        for (const CaseSpec& c : cases) {
            int clipIndex = -1;
            for (size_t i = 0; i < result->data.clips.size(); i++)
                if (result->data.clips[i].name == c.clip) clipIndex = (int)i;
            ASSERT_GE(clipIndex, 0) << c.clip;
            const jasperengine::vat::VatClip& clip = result->data.clips[(size_t)clipIndex];
            const float time = float(c.frames / clip.fps);
            const std::shared_ptr<const AnimationClip> source = AnimationClipPool::getOrLoad("models/" + model + "#" + c.clip);
            ASSERT_TRUE(source) << c.clip;
            double errorMm = 0, normalDegrees = 0;
            measureAt(scene, root, *result, clipIndex, source, time, errorMm, normalDegrees);
            const bool sample = std::fabs(c.frames - std::round(c.frames)) < 1e-6;
            // the bake's own report bounds it: quantization at samples, interpolation between them
            const VatClipReport* row = nullptr;
            for (const VatClipReport& r : result->report.clips)
                if (r.name == c.clip) row = &r;
            ASSERT_TRUE(row);
            EXPECT_LE(errorMm, (sample ? result->report.quantizationErrorMm : row->maxErrorMm) + 0.05) << c.what;
            std::printf("[ e2e      ] %s %s at %.4f s (%s): %.3f mm, normals %.3f degrees\n", name.c_str(), c.clip.c_str(), time, c.what.c_str(),
                        errorMm, normalDegrees);
            measured.push_back({{"clip", c.clip}, {"time", time}, {"sample", sample}, {"what", c.what}, {"crowd", c.crowd},
                                {"error_mm", errorMm}, {"normal_error_deg", normalDegrees}});
        }

        VatWriteResult written;
        std::string error;
        ASSERT_TRUE(session.write(written, error)) << error;
        const fs::path bundle = bundleDirectory();
        fs::create_directories(bundle);
        for (const std::string& file : written.files) {
            const fs::path from = headless().dir / file;
            ASSERT_TRUE(fs::exists(from)) << file;
            fs::copy_file(from, bundle / from.filename(), fs::copy_options::overwrite_existing);
        }
        ASSERT_TRUE(fs::exists(bundle / (name + ".vat")));
        ASSERT_TRUE(fs::exists(bundle / (name + ".vat.glb")));
        const Json meta = {{"vat", name + ".vat"},
                           {"mesh", name + ".vat.glb"},
                           {"mode", result->data.mode == jasperengine::vat::VatMode::Bones ? "bones" : "vertices"},
                           {"source", (repoRoot() / sourceFile).generic_string()},
                           {"clips", clipsJson(result->data, result->report)},
                           {"report", reportJson(result->report)},
                           {"cases", measured}};
        writeText(bundle / (name + ".json"), meta.dump(2));
    }

    VatBakeSpec modelSpec(const std::string& model, std::vector<std::string> clips, VatModeChoice mode) {
        VatBakeSpec spec;
        spec.source.kind = VatSourceKind::Model;
        spec.source.path = "assets/models/" + model;
        spec.clips = std::move(clips);
        spec.mode = mode;
        return spec;
    }

    // an import encoded as its files into the bundle, with the ground truth triangles at `times`
    // (truth(time) -> world-space triangle corners, 9 floats a triangle) in <name>.json
    void importIntoBundle(const std::string& name, vatimportfixtures::ev::VatImportResult& r, const std::vector<double>& times,
                          const std::function<std::vector<double>(double)>& truth, const std::function<double(double)>& toleranceMm) {
        SCOPED_TRACE(name);
        ASSERT_TRUE(r.ok()) << vatImportIssuesText(r.errors);
        const fs::path bundle = bundleDirectory();
        fs::create_directories(bundle);
        VatEncodedFiles files;
        std::string error;
        ASSERT_TRUE(encodeVatImport(r, (bundle / (name + ".vat")).string(), files, &error)) << error;
        writeText(bundle / (name + ".vat"), files.vatText);
        writeText(bundle / (name + ".vat.glb"), std::string(files.glbBytes.begin(), files.glbBytes.end()));
        Json samples = Json::array();
        for (double t : times) samples.push_back({{"time", t}, {"tolerance_mm", toleranceMm(t)}, {"triangles", truth(t)}});
        const Json meta = {{"vat", name + ".vat"},
                           {"mesh", name + ".vat.glb"},
                           {"mode", r.data.mode == jasperengine::vat::VatMode::Bones ? "bones" : "vertices"},
                           {"clips", clipsJson(r.data, r.report)},
                           {"report", Json::parse(vatImportReportJson(r.report))},
                           {"samples", samples}};
        writeText(bundle / (name + ".json"), meta.dump(2));
    }

}

TEST(VatEndToEnd, BakesTheRenderParityBundle) {
    using jasperengine::vat::VatMode;
    const std::string hero = "tools/fixtures/pose-animation/assets/models/hero_animated.gltf";
    // Walk's keys are at 0, 0.25, 0.5, 0.75 s: at 30 fps each falls between two samples (7.5,
    // 15, 22.5), where linear interpolation cuts the corner its curve makes (the worst error);
    // between other samples the motion is nearly straight
    const std::vector<CaseSpec> heroCases = {
        {"Walk", 0.0, "first sample"},
        {"Walk", 3.0, "a sample", true},
        {"Walk", 7.0, "a sample"},
        {"Walk", 7.5, "between samples, on a key"},
        {"Walk", 12.5, "between samples", true},
        {"Walk", 22.5, "between samples, on a key"},
        {"Walk", 29.5, "across the loop seam"},
        {"Idle", 12.0, "a sample", true},
        {"Idle", 30.0, "a sample"},
        {"Idle", 30.5, "between samples"},
        {"Idle", 59.5, "across the loop seam", true},
        {"Wave", 10.0, "a sample"},
        {"Wave", 10.5, "between samples"},
    };
    bakeIntoBundle("hero_vertices", modelSpec("hero.gltf", {"Walk", "Idle", "Wave"}, VatModeChoice::Vertices), "hero.gltf", hero,
                   VatMode::Vertices, heroCases);
    bakeIntoBundle("hero_bones", modelSpec("hero.gltf", {"Walk", "Idle", "Wave"}, VatModeChoice::Bones), "hero.gltf", hero,
                   VatMode::Bones, heroCases);
    // Auto: a rigid hierarchy bakes as bones, morph targets as vertices
    bakeIntoBundle("rigid_arm", modelSpec("rigid_arm.gltf", {"Swing"}, VatModeChoice::Auto), "rigid_arm.gltf",
                   "tools/fixtures/vertex-animation/assets/models/rigid_arm.gltf", VatMode::Bones,
                   {{"Swing", 8.0, "a sample"}, {"Swing", 8.5, "between samples"}, {"Swing", 15.0, "the turn's peak"},
                    {"Swing", 29.5, "across the loop seam"}});
    bakeIntoBundle("morph_blob", modelSpec("morph_blob.gltf", {"Pulse"}, VatModeChoice::Auto), "morph_blob.gltf",
                   "tools/fixtures/vertex-animation/assets/models/morph_blob.gltf", VatMode::Vertices,
                   {{"Pulse", 10.0, "a sample"}, {"Pulse", 15.0, "the peak"}, {"Pulse", 15.5, "between samples"}});
    std::printf("[ e2e      ] bundle written to %s\n", bundleDirectory().c_str());
}

TEST(VatEndToEnd, ImportsTheHoudiniBundle) {
    using namespace vatimportfixtures;
    const fs::path root = fs::temp_directory_path() / ("jasper-vat-e2e-houdini-" + std::to_string(::getpid()));
    fs::remove_all(root);

    {
        // soft body: the 3 x 3 grid waving (8 triangles), HDR, a Unity-target material
        const SoftTruth truth = writeSoft(root / "soft", "Wave", SoftOptions());
        ev::VatImportSpec spec;
        spec.path = (root / "soft" / "geo" / "Wave_mesh.fbx").string();
        ev::VatImportResult r = ev::importVat(spec);
        ASSERT_TRUE(r.ok()) << issues(r);
        ASSERT_EQ(r.data.clips.size(), 1u);
        const jasperengine::vat::VatClip clip = r.data.clips[0];
        const int tris[8][3] = {{0, 1, 4}, {0, 4, 3}, {1, 2, 5}, {1, 5, 4}, {3, 4, 7}, {3, 7, 6}, {4, 5, 8}, {4, 8, 7}};
        // the decoded truth at a clip time: a sample, or between two (interpolated linearly, or
        // the earlier one when the clip doesn't interpolate)
        auto at = [&](double time) {
            const double f = time * clip.fps;
            const int f0 = std::min((int)std::floor(f + 1e-6), (int)truth.pos.size() - 1);
            const int f1 = std::min(f0 + 1, (int)truth.pos.size() - 1);
            const double a = clip.interpolate ? std::max(0.0, f - f0) : 0.0;
            std::vector<double> out;
            for (const auto& t : tris) {
                for (int k = 0; k < 3; k++) {
                    const V3 p = truth.pos[f0][t[k]] * (1.0 - a) + truth.pos[f1][t[k]] * a;
                    out.insert(out.end(), {p.x, p.y, p.z});
                }
            }
            return out;
        };
        const double fps = clip.fps;
        importIntoBundle("houdini_soft", r, {0.0, 2.0 / fps, 2.5 / fps, 5.0 / fps}, at, [&](double) { return r.report.quantizationErrorMm; });
    }
    {
        // rigid body: three tetrahedra spinning up to 140 degrees a frame (upsampled at import)
        const RigidTruth truth = writeRigid(root / "rigid", "Shatter", false);
        ev::VatImportSpec spec;
        spec.path = (root / "rigid" / "geo" / "Shatter_mesh.fbx").string();
        ev::VatImportResult r = ev::importVat(spec);
        ASSERT_TRUE(r.ok()) << issues(r);
        ASSERT_EQ(r.data.mode, jasperengine::vat::VatMode::Bones);
        const int faces[4][3] = {{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}};
        // Houdini's exact motion at a time (frames of the export: time * its fps)
        auto at = [&](double time) {
            std::vector<double> out;
            for (size_t b = 0; b < truth.pieces.size(); b++) {
                for (const auto& face : faces) {
                    for (int k = 0; k < 3; k++) {
                        const V3 p = truth.point((int)b * 4 + face[k], time * truth.fps);
                        out.insert(out.end(), {p.x, p.y, p.z});
                    }
                }
            }
            return out;
        };
        const double fps = truth.fps;
        // the samples are exact; between them the GPU's nlerp / lerp is within the import's
        // measured interpolation error
        auto tolerance = [&](double time) {
            const double f = time * fps * r.report.upsample;
            return std::fabs(f - std::round(f)) < 1e-6 ? 0.0 : (double)r.report.interpolationErrorMm;
        };
        importIntoBundle("houdini_rigid", r, {0.0, 1.0 / fps, 1.5 / fps, 3.25 / fps}, at, tolerance);
    }
    fs::remove_all(root);
}
